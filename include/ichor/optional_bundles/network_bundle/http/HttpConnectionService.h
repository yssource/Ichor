#pragma once

#ifdef USE_BOOST_BEAST

#include <ichor/optional_bundles/network_bundle/http/IHttpConnectionService.h>
#include <ichor/optional_bundles/logging_bundle/Logger.h>
#include <ichor/optional_bundles/timer_bundle/TimerService.h>
#include <boost/beast.hpp>
#include <boost/asio/spawn.hpp>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace Ichor {
    class HttpConnectionService final : public IHttpConnectionService, public Service<HttpConnectionService> {
    public:
        HttpConnectionService(DependencyRegister &reg, Properties props, DependencyManager *mng);
        ~HttpConnectionService() final = default;

        bool start() final;
        bool stop() final;

        void addDependencyInstance(ILogger *logger, IService *);
        void removeDependencyInstance(ILogger *logger, IService *);

        uint64_t sendAsync(HttpMethod method, std::string_view route, std::vector<HttpHeader, Ichor::PolymorphicAllocator<HttpHeader>> &&headers, std::vector<uint8_t, Ichor::PolymorphicAllocator<uint8_t>>&& msg) final;

        bool close() final;

        void setPriority(uint64_t priority) final;
        uint64_t getPriority() final;

    private:
        void fail(beast::error_code, char const* what);
        void connect(tcp::endpoint endpoint, net::yield_context yield);

        Ichor::unique_ptr<net::io_context> _httpContext{};
        Ichor::unique_ptr<beast::tcp_stream> _httpStream{};
        std::atomic<uint64_t> _priority{INTERNAL_EVENT_PRIORITY};
        bool _quit{};
        bool _connecting{};
        bool _connected{};
        int _attempts{};
        uint64_t _msgId{};
        ILogger *_logger{nullptr};
        Timer* _timerManager{nullptr};
    };
}

#endif