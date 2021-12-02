#pragma once

#ifdef USE_BOOST_BEAST

#include <ichor/optional_bundles/network_bundle/http/IHttpService.h>
#include <ichor/optional_bundles/logging_bundle/Logger.h>
#include <ichor/optional_bundles/timer_bundle/TimerService.h>
#include <boost/beast.hpp>
#include <boost/asio/spawn.hpp>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace Ichor {
    class HttpHostService final : public IHttpService, public Service<HttpHostService> {
    public:
        HttpHostService(DependencyRegister &reg, Properties props, DependencyManager *mng);
        ~HttpHostService() final = default;

        bool start() final;
        bool stop() final;

        void addDependencyInstance(ILogger *logger, IService *isvc);
        void removeDependencyInstance(ILogger *logger, IService *isvc);

        Ichor::unique_ptr<HttpRouteRegistration> addRoute(HttpMethod method, std::string_view route, std::function<HttpResponse(HttpRequest&)> handler) final;
        void removeRoute(HttpMethod method, std::string_view route) final;

        void setPriority(uint64_t priority) final;
        uint64_t getPriority() final;

    private:
        void fail(beast::error_code, char const* what);
        void listen(tcp::endpoint endpoint, net::yield_context yield);
        void read(tcp::socket socket, net::yield_context yield);

        Ichor::unique_ptr<net::io_context> _httpContext{};
        Ichor::unique_ptr<tcp::acceptor> _httpAcceptor{};
        Ichor::unique_ptr<beast::tcp_stream> _httpStream{};
        uint64_t _priority{INTERNAL_EVENT_PRIORITY};
        bool _quit{};
        ILogger *_logger{nullptr};
        Timer* _timerManager{nullptr};
        std::unordered_map<HttpMethod, std::unordered_map<std::string, std::function<HttpResponse(HttpRequest&)>, string_hash, std::equal_to<>>> _handlers{};
//        Ichor::unique_ptr<EventHandlerRegistration> _eventRegistration{};
    };
}

#endif