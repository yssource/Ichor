#ifdef ICHOR_USE_BOOST_BEAST

#include <ichor/DependencyManager.h>
#include <ichor/optional_bundles/network_bundle/http/HttpConnectionService.h>
#include "ichor/optional_bundles/network_bundle/NetworkEvents.h"


Ichor::HttpConnectionService::HttpConnectionService(DependencyRegister &reg, Properties props, DependencyManager *mng) : Service(std::move(props), mng) {
    reg.registerDependency<ILogger>(this, true);
    reg.registerDependency<IHttpContextService>(this, true);
}

Ichor::StartBehaviour Ichor::HttpConnectionService::start() {
    if(!_httpContextService->fibersShouldStop() && !_connecting && !_connected) {
        ICHOR_LOG_WARN(_logger, "starting svc {}", getServiceId());
        _quit = false;
        if (getProperties().contains("Priority")) {
            _priority = Ichor::any_cast<uint64_t>(getProperties().operator[]("Priority"));
        }

        if (!getProperties().contains("Port") || !getProperties().contains("Address")) {
            getManager()->pushPrioritisedEvent<UnrecoverableErrorEvent>(getServiceId(), _priority, 0,
                                                                        "Missing port or address when starting HttpConnectionService");
            return Ichor::StartBehaviour::FAILED_DO_NOT_RETRY;
        }

        if(getProperties().contains("NoDelay")) {
            _tcpNoDelay = Ichor::any_cast<bool>(getProperties().operator[]("NoDelay"));
        }

        auto address = net::ip::make_address(Ichor::any_cast<std::string &>(getProperties().operator[]("Address")));
        auto port = Ichor::any_cast<uint16_t>(getProperties().operator[]("Port"));

        _connecting = true;
        net::spawn(*_httpContextService->getContext(), [this, address = std::move(address), port](net::yield_context yield) {
            connect(tcp::endpoint{address, port}, std::move(yield));
        });
    }

    if(_httpContextService->fibersShouldStop()) {
        return Ichor::StartBehaviour::FAILED_DO_NOT_RETRY;
    }

    if(!_connected) {
        return Ichor::StartBehaviour::FAILED_AND_RETRY;
    }

    return Ichor::StartBehaviour::SUCCEEDED;
}

Ichor::StartBehaviour Ichor::HttpConnectionService::stop() {
    _quit = true;

    if(!close()) {
        return Ichor::StartBehaviour::FAILED_AND_RETRY;
    }

    _httpStream = nullptr;
    return Ichor::StartBehaviour::SUCCEEDED;
}

void Ichor::HttpConnectionService::addDependencyInstance(ILogger *logger, IService *) {
    _logger = logger;
}

void Ichor::HttpConnectionService::removeDependencyInstance(ILogger *logger, IService *) {
    _logger = nullptr;
}

void Ichor::HttpConnectionService::addDependencyInstance(IHttpContextService *httpContextService, IService *) {
    _httpContextService = httpContextService;
    ICHOR_LOG_TRACE(_logger, "Inserted httpContextService");
}

void Ichor::HttpConnectionService::removeDependencyInstance(IHttpContextService *httpContextService, IService *) {
    _httpContextService = nullptr;
    _quit = true;
    _connected = false;
}

void Ichor::HttpConnectionService::setPriority(uint64_t priority) {
    _priority.store(priority, std::memory_order_release);
}

uint64_t Ichor::HttpConnectionService::getPriority() {
    return _priority.load(std::memory_order_acquire);
}

uint64_t Ichor::HttpConnectionService::sendAsync(Ichor::HttpMethod method, std::string_view route, std::vector<HttpHeader, Ichor::PolymorphicAllocator<HttpHeader>> &&headers, std::vector<uint8_t, Ichor::PolymorphicAllocator<uint8_t>> &&msg) {

    if(method == HttpMethod::get && !msg.empty()) {
        throw std::runtime_error("GET requests cannot have a body.");
    }

    ICHOR_LOG_DEBUG(_logger, "sending to {}", route);

    if(_quit || _httpContextService->fibersShouldStop()) {
        return 0;
    }

    auto msgId = ++_msgId;

    net::spawn(*_httpContextService->getContext(), [this, msgId, method, route, headers = std::forward<decltype(headers)>(headers), msg = std::forward<decltype(msg)>(msg)](net::yield_context yield) mutable {
        beast::error_code ec;
        http::request<http::vector_body<uint8_t, Ichor::PolymorphicAllocator<uint8_t>>, http::basic_fields<Ichor::PolymorphicAllocator<uint8_t>>> req{static_cast<http::verb>(method), route, 11, std::move(msg)};

        for(auto const &header : headers) {
            req.set(header.name, header.value);
        }
        req.set(http::field::host, Ichor::any_cast<std::string &>(getProperties().operator[]("Address")));
        req.prepare_payload();

        http::write(*_httpStream, req, ec);
        if (ec) {
            getManager()->pushEvent<FailedSendMessageEvent>(getServiceId(), std::move(msg), msgId);
            return fail(ec, "HttpConnectionService::sendAsync write");
        }

        // This buffer is used for reading and must be persisted
        beast::basic_flat_buffer b{Ichor::PolymorphicAllocator<uint8_t>{getMemoryResource()}};

        // Declare a container to hold the response
        http::response<http::vector_body<uint8_t, Ichor::PolymorphicAllocator<uint8_t>>, http::basic_fields<Ichor::PolymorphicAllocator<uint8_t>>> res;

        // Receive the HTTP response
        http::async_read(*_httpStream, b, res, yield[ec]);
        if(ec) {
            return fail(ec, "HttpConnectionService::sendAsync read");
        }

        ICHOR_LOG_WARN(_logger, "received HTTP response {}", std::string_view(reinterpret_cast<char*>(res.body().data()), res.body().size()));

        std::vector<HttpHeader, Ichor::PolymorphicAllocator<HttpHeader>> resHeaders{getMemoryResource()};
        resHeaders.reserve(std::distance(std::begin(res), std::end(res)));
        for(auto const &header : res) {
            resHeaders.emplace_back(header.name_string(), header.value());
        }

        // need to use move iterator instead of std::move directly, to prevent leaks.
        std::vector<uint8_t, Ichor::PolymorphicAllocator<uint8_t>> body{getMemoryResource()};
        body.insert(body.end(), std::make_move_iterator(res.body().begin()), std::make_move_iterator(res.body().end()));

        getManager()->pushPrioritisedEvent<HttpResponseEvent>(getServiceId(), getPriority(), msgId, HttpResponse{static_cast<HttpStatus>(res.result()), std::move(body), std::move(resHeaders)});
    });

    return msgId;
}

bool Ichor::HttpConnectionService::close() {
    if((_quit || _httpContextService->fibersShouldStop()) && !_stopping) {
        _stopping = true;
        net::spawn(*_httpContextService->getContext(), [this](net::yield_context yield) {
            beast::error_code ec;
            _httpStream->socket().shutdown(tcp::socket::shutdown_both, ec);

            _connected = false;
            if (ec && ec != beast::errc::not_connected) {
                fail(ec, "HttpConnectionService::close shutdown");
            }
        });
    }

    return !_connected;
}

void Ichor::HttpConnectionService::fail(beast::error_code ec, const char *what) {
    ICHOR_LOG_ERROR(_logger, "Boost.BEAST fail: {}, {}", what, ec.message());
    getManager()->pushPrioritisedEvent<StopServiceEvent>(getServiceId(), _priority.load(std::memory_order_acquire), getServiceId());
}

void Ichor::HttpConnectionService::connect(tcp::endpoint endpoint, net::yield_context yield) {
    beast::error_code ec;

    tcp::resolver resolver(*_httpContextService->getContext());
    _httpStream = Ichor::make_unique<beast::tcp_stream>(getMemoryResource(), *_httpContextService->getContext());

    auto const results = resolver.resolve(endpoint, ec);
    if(ec) {
        return fail(ec, "HttpConnectionService::connect resolve");
    }

    // Set the timeout.
    _httpStream->expires_after(30s);

    // Make the connection on the IP address we get from a lookup
    while(!_quit && !_httpContextService->fibersShouldStop() && _attempts < 5) {
//        ICHOR_LOG_WARN(_logger, "async_connect {}", _attempts);
        _httpStream->async_connect(results, yield[ec]);
//        ICHOR_LOG_WARN(_logger, "async_connect after {}", _attempts);
        if (ec) {
            _attempts++;
            net::steady_timer t{*_httpContextService->getContext()};
            t.expires_after(250ms);
            t.async_wait(yield);
        } else {
            break;
        }
    }

    _httpStream->socket().set_option(tcp::no_delay(_tcpNoDelay));

    if(ec) {
        _quit = true;
        _connecting = false;
        return fail(ec, "HttpConnectionService::connect connect");
    }

    _connecting = false;
    _connected = true;
}

#endif