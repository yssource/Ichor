#pragma once

#include <ichor/optional_bundles/network_bundle/IHostService.h>
#include <ichor/optional_bundles/logging_bundle/Logger.h>
#include <ichor/optional_bundles/network_bundle/tcp/TcpConnectionService.h>
#include <ichor/optional_bundles/timer_bundle/TimerService.h>

namespace Ichor {
    struct NewSocketEvent final : public Ichor::Event {
        NewSocketEvent(uint64_t _id, uint64_t _originatingService, uint64_t _priority, int _socket) noexcept : Event(TYPE, NAME, _id, _originatingService, _priority), socket(_socket) {}
        ~NewSocketEvent() final = default;

        int socket;
        static constexpr uint64_t TYPE = Ichor::typeNameHash<NewSocketEvent>();
        static constexpr std::string_view NAME = Ichor::typeName<NewSocketEvent>();
    };

    class TcpHostService final : public IHostService, public Service<TcpHostService> {
    public:
        TcpHostService(DependencyRegister &reg, Properties props, DependencyManager *mng);
        ~TcpHostService() final = default;

        StartBehaviour start() final;
        StartBehaviour stop() final;

        void addDependencyInstance(ILogger *logger, IService *isvc);
        void removeDependencyInstance(ILogger *logger, IService *isvc);

        void setPriority(uint64_t priority) final;
        uint64_t getPriority() final;

        Generator<bool> handleEvent(NewSocketEvent const * const evt);

    private:
        int _socket;
        int _bindFd;
        uint64_t _priority;
        bool _quit;
        ILogger *_logger{nullptr};
        Timer* _timerManager{nullptr};
        std::vector<TcpConnectionService*> _connections;
        EventHandlerRegistration _newSocketEventHandlerRegistration{};
    };
}