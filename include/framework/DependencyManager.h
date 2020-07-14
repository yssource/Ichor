#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <spdlog/spdlog.h>
#include <concurrentqueue.h>
#include <framework/interfaces/IFrameworkLogger.h>
#include "Service.h"
#include "ServiceLifecycleManager.h"
#include "Events.h"
#include "framework/Callback.h"
#include "Filter.h"

using namespace std::chrono_literals;

namespace Cppelix {

    class DependencyManager;
    class CommunicationChannel;

    class [[nodiscard]] EventStackUniquePtr final {
    public:
        EventStackUniquePtr() = default;

        template <typename T, typename... Args>
        requires Derived<T, Event>
        static EventStackUniquePtr create(Args&&... args) {
            static_assert(sizeof(T) < 128, "size not big enough to hold T");
            EventStackUniquePtr ptr;
            new (ptr._buffer.data()) T(std::forward<Args>(args)...);
            ptr._type = T::TYPE;
            return ptr;
        }

        EventStackUniquePtr(const EventStackUniquePtr&) = delete;
        EventStackUniquePtr(EventStackUniquePtr&& other) noexcept {
            if(!_empty) {
                reinterpret_cast<Event *>(_buffer.data())->~Event();
            }
            _buffer = other._buffer;
            other._empty = true;
            _empty = false;
            _type = other._type;
        }


        EventStackUniquePtr& operator=(const EventStackUniquePtr&) = delete;
        EventStackUniquePtr& operator=(EventStackUniquePtr &&other) noexcept {
            if(!_empty) {
                reinterpret_cast<Event *>(_buffer.data())->~Event();
            }
            _buffer = other._buffer;
            other._empty = true;
            _empty = false;
            _type = other._type;
            return *this;
        }

        ~EventStackUniquePtr() {
            if(!_empty) {
                reinterpret_cast<Event *>(_buffer.data())->~Event();
            }
        }

        template <typename T>
        requires Derived<T, Event>
        [[nodiscard]] T* getT() noexcept {
            if(_empty) {
                throw std::runtime_error("empty");
            }

            return reinterpret_cast<T*>(_buffer.data());
        }

        [[nodiscard]] Event* get() noexcept {
            return reinterpret_cast<Event*>(_buffer.data());
        }

        [[nodiscard]] uint64_t getType() const noexcept {
            return _type;
        }
    private:

        std::array<uint8_t, 128> _buffer;
        uint64_t _type{0};
        bool _empty{true};
    };

    class [[nodiscard]] EventCompletionHandlerRegistration final {
    public:
        EventCompletionHandlerRegistration(DependencyManager *mgr, CallbackKey key) noexcept : _mgr(mgr), _key(key) {}
        EventCompletionHandlerRegistration() noexcept = default;
        ~EventCompletionHandlerRegistration();

        EventCompletionHandlerRegistration(const EventCompletionHandlerRegistration&) = delete;
        EventCompletionHandlerRegistration(EventCompletionHandlerRegistration&&) noexcept = default;
        EventCompletionHandlerRegistration& operator=(const EventCompletionHandlerRegistration&) = delete;
        EventCompletionHandlerRegistration& operator=(EventCompletionHandlerRegistration&&) noexcept = default;
    private:
        DependencyManager *_mgr{nullptr};
        CallbackKey _key{0, 0};
    };

    class [[nodiscard]] EventHandlerRegistration final {
    public:
        EventHandlerRegistration(DependencyManager *mgr, CallbackKey key) noexcept : _mgr(mgr), _key(key) {}
        EventHandlerRegistration() noexcept = default;
        ~EventHandlerRegistration();

        EventHandlerRegistration(const EventHandlerRegistration&) = delete;
        EventHandlerRegistration(EventHandlerRegistration&&) noexcept = default;
        EventHandlerRegistration& operator=(const EventHandlerRegistration&) = delete;
        EventHandlerRegistration& operator=(EventHandlerRegistration&&) noexcept = default;
    private:
        DependencyManager *_mgr{nullptr};
        CallbackKey _key{0, 0};
    };

    class [[nodiscard]] DependencyTrackerRegistration final {
    public:
        DependencyTrackerRegistration(DependencyManager *mgr, uint64_t interfaceNameHash) noexcept : _mgr(mgr), _interfaceNameHash(interfaceNameHash) {}
        DependencyTrackerRegistration() noexcept = default;
        ~DependencyTrackerRegistration();

        DependencyTrackerRegistration(const DependencyTrackerRegistration&) = delete;
        DependencyTrackerRegistration(DependencyTrackerRegistration&&) noexcept = default;
        DependencyTrackerRegistration& operator=(const DependencyTrackerRegistration&) = delete;
        DependencyTrackerRegistration& operator=(DependencyTrackerRegistration&&) noexcept = default;
    private:
        DependencyManager *_mgr{nullptr};
        uint64_t _interfaceNameHash{0};
    };

    struct DependencyTrackerInfo final {
        DependencyTrackerInfo() = default;
        DependencyTrackerInfo(uint64_t _trackingServiceId, std::function<void(Event const * const)> _trackFunc) noexcept : trackingServiceId(_trackingServiceId), trackFunc(std::move(_trackFunc)) {}
        ~DependencyTrackerInfo() = default;
        uint64_t trackingServiceId;
        std::function<void(Event const * const)> trackFunc;
    };

    class DependencyManager final {
    public:
        DependencyManager() : _services(), _dependencyRequestTrackers(), _dependencyUndoRequestTrackers(), _completionCallbacks{}, _errorCallbacks{}, _logger(nullptr), _eventQueue{}, _producerToken{_eventQueue}, _consumerToken{_eventQueue}, _eventIdCounter{0}, _quit{false}, _communicationChannel(nullptr), _id(_managerIdCounter++) {}

        template<class Interface, class Impl, typename... Required, typename... Optional>
        requires Derived<Impl, Service> && Derived<Impl, Interface>
        [[nodiscard]]
        auto createServiceManager(RequiredList_t<Required...>, OptionalList_t<Optional...>, CppelixProperties properties = CppelixProperties{}) {
            auto cmpMgr = LifecycleManager<Interface, Impl, Required..., Optional...>::template create(_logger, "", std::move(properties), RequiredList<Required...>, OptionalList<Optional...>);

            if(_logger != nullptr) {
                LOG_DEBUG(_logger, "added ServiceManager<{}, {}>", typeName<Interface>(), typeName<Impl>());
            }

            cmpMgr->getService().injectDependencyManager(this);

            for(auto &[key, mgr] : _services) {
                if(mgr->getServiceState() == ServiceState::ACTIVE) {
                    auto filterProp = mgr->getProperties()->find("Filter");
                    const Filter *filter = nullptr;
                    if(filterProp != end(*mgr->getProperties())) {
                        filter = std::any_cast<const Filter>(&filterProp->second);
                    }

                    if(filter != nullptr && !filter->compareTo(cmpMgr)) {
                        continue;
                    }

                    cmpMgr->dependencyOnline(mgr);
                }
            }

            (_eventQueue.enqueue(_producerToken, EventStackUniquePtr::create<DependencyRequestEvent>(_eventIdCounter.fetch_add(1, std::memory_order_acq_rel), cmpMgr->serviceId(), cmpMgr, Dependency{typeNameHash<Optional>(), Optional::version, false}, cmpMgr->getProperties())), ...);
            (_eventQueue.enqueue(_producerToken, EventStackUniquePtr::create<DependencyRequestEvent>(_eventIdCounter.fetch_add(1, std::memory_order_acq_rel), cmpMgr->serviceId(), cmpMgr, Dependency{typeNameHash<Required>(), Required::version, true}, cmpMgr->getProperties())), ...);
            _eventQueue.enqueue(_producerToken, EventStackUniquePtr::create<StartServiceEvent>(_eventIdCounter.fetch_add(1, std::memory_order_acq_rel), 0, cmpMgr->serviceId()));

            _services.emplace(cmpMgr->serviceId(), cmpMgr);
            return &cmpMgr->getService();
        }

        template<class Interface, class Impl>
        requires Derived<Impl, Service> && Derived<Impl, Interface>
        [[nodiscard]]
        auto createServiceManager(CppelixProperties properties = {}) {
            auto cmpMgr = LifecycleManager<Interface, Impl>::create(_logger, "", std::move(properties));

            if constexpr (std::is_same<Interface, IFrameworkLogger>::value) {
                _logger = &cmpMgr->getService();
            }

            if(_logger != nullptr) {
                LOG_DEBUG(_logger, "added ServiceManager<{}, {}>", typeName<Interface>(), typeName<Impl>());
            }

            cmpMgr->getService().injectDependencyManager(this);

            _eventQueue.enqueue(_producerToken, EventStackUniquePtr::create<StartServiceEvent>(_eventIdCounter.fetch_add(1, std::memory_order_acq_rel), 0, cmpMgr->getService().getServiceId()));

            _services.emplace(cmpMgr->serviceId(), cmpMgr);
            return &cmpMgr->getService();
        }

        template <typename EventT, typename... Args>
        requires Derived<EventT, Event>
        uint64_t pushEvent(uint64_t originatingServiceId, Args&&... args){
            static_assert(sizeof(EventT) < 128, "event type cannot be larger than 128 bytes");

            if(_quit.load(std::memory_order_acquire)) {
                return 0;
            }

            uint64_t eventId = _eventIdCounter.fetch_add(1, std::memory_order_acq_rel);
            _eventQueue.enqueue(EventStackUniquePtr::create<EventT>(std::forward<uint64_t>(eventId), std::forward<uint64_t>(originatingServiceId), std::forward<Args>(args)...));
            return eventId;
        }

        template <typename EventT, typename... Args>
        requires Derived<EventT, Event>
        uint64_t pushEventThreadUnsafe(uint64_t originatingServiceId, Args&&... args){
            static_assert(sizeof(EventT) < 128, "event type cannot be larger than 128 bytes");

            if(_quit.load(std::memory_order_acquire)) {
                return 0;
            }

            uint64_t eventId = _eventIdCounter.fetch_add(1, std::memory_order_acq_rel);
            _eventQueue.enqueue(_producerToken, EventStackUniquePtr::create<EventT>(std::forward<uint64_t>(eventId), std::forward<uint64_t>(originatingServiceId), std::forward<Args>(args)...));
            return eventId;
        }

        template <typename Interface, typename Impl>
        requires Derived<Impl, Service> && ImplementsTrackingHandlers<Impl, Interface>
        [[nodiscard]]
        std::unique_ptr<DependencyTrackerRegistration> registerDependencyTracker(uint64_t serviceId, Impl *impl) {
            auto requestTrackersForType = _dependencyRequestTrackers.find(typeNameHash<Interface>());
            auto undoRequestTrackersForType = _dependencyUndoRequestTrackers.find(typeNameHash<Interface>());

            DependencyTrackerInfo requestInfo{impl->getServiceId(), [impl](Event const * const evt){ impl->handleDependencyRequest(static_cast<Interface*>(nullptr), static_cast<DependencyRequestEvent const *>(evt)); }};
            DependencyTrackerInfo undoRequestInfo{impl->getServiceId(), [impl](Event const * const evt){ impl->handleDependencyUndoRequest(static_cast<Interface*>(nullptr), static_cast<DependencyUndoRequestEvent const *>(evt)); }};

            for(auto &[key, mgr] : _services) {
                auto const * depInfo = mgr->getDependencyInfo();

                if(depInfo == nullptr) {
                    continue;
                }

                for (const auto &dependency : depInfo->_dependencies) {
                    if(dependency.interfaceNameHash == typeNameHash<Interface>()) {
                        DependencyRequestEvent evt{0, mgr->serviceId(), mgr, Dependency{dependency.interfaceNameHash, dependency.interfaceVersion, dependency.required}, mgr->getProperties()};
                        requestInfo.trackFunc(&evt);
                    }
                }
            }

            if(requestTrackersForType == end(_dependencyRequestTrackers)) {
                _dependencyRequestTrackers.emplace(typeNameHash<Interface>(), std::vector<DependencyTrackerInfo>{requestInfo});
            } else {
                requestTrackersForType->second.emplace_back(std::move(requestInfo));
            }

            if(undoRequestTrackersForType == end(_dependencyUndoRequestTrackers)) {
                _dependencyUndoRequestTrackers.emplace(typeNameHash<Interface>(), std::vector<DependencyTrackerInfo>{undoRequestInfo});
            } else {
                undoRequestTrackersForType->second.emplace_back(std::move(undoRequestInfo));
            }

            // I think there's a bug in GCC 10.1, where if I don't make this a unique_ptr, the DependencyTrackerRegistration destructor immediately gets called for some reason.
            // Even if the result is stored in a variable at the caller site.
            return std::make_unique<DependencyTrackerRegistration>(this, typeNameHash<Interface>());
        }

        template <typename EventT, typename Impl>
        requires Derived<EventT, Event> && ImplementsEventCompletionHandlers<Impl, EventT>
        [[nodiscard]]
        std::unique_ptr<EventCompletionHandlerRegistration> registerEventCompletionCallbacks(uint64_t serviceId, Impl *impl) {
            CallbackKey key{serviceId, EventT::TYPE};
            _completionCallbacks.emplace(key, [impl](Event const * const evt){ impl->handleCompletion(static_cast<EventT const * const>(evt)); });
            _errorCallbacks.emplace(key, [impl](Event const * const evt){ impl->handleError(static_cast<EventT const * const>(evt)); });
            // I think there's a bug in GCC 10.1, where if I don't make this a unique_ptr, the EventCompletionHandlerRegistration destructor immediately gets called for some reason.
            // Even if the result is stored in a variable at the caller site.
            return std::make_unique<EventCompletionHandlerRegistration>(this, key);
        }

        template <typename EventT, typename Impl>
        requires Derived<EventT, Event> && ImplementsEventHandlers<Impl, EventT>
        [[nodiscard]]
        std::unique_ptr<EventHandlerRegistration> registerEventHandler(uint64_t serviceId, Impl *impl) {
            auto existingHandlers = _eventCallbacks.find(EventT::TYPE);
            if(existingHandlers == end(_eventCallbacks)) {
                _eventCallbacks.emplace(EventT::TYPE, std::vector<std::pair<uint64_t, std::function<bool(Event const * const)>>>{std::pair<uint64_t, std::function<bool(Event const * const)>>{serviceId,
                        [impl](Event const * const evt){ return impl->handleEvent(static_cast<EventT const * const>(evt)); }}
                });
            } else {
                existingHandlers->second.emplace_back(std::pair<uint64_t, std::function<bool(Event const * const)>>{serviceId, [impl](Event const * const evt){ return impl->handleEvent(static_cast<EventT const * const>(evt)); }});
            }
            // I think there's a bug in GCC 10.1, where if I don't make this a unique_ptr, the EventHandlerRegistration destructor immediately gets called for some reason.
            // Even if the result is stored in a variable at the caller site.
            return std::make_unique<EventHandlerRegistration>(this, CallbackKey{serviceId, EventT::TYPE});
        }

        [[nodiscard]] uint64_t getId() const {
            return _id;
        }

        ///
        /// \return Potentially nullptr
        [[nodiscard]] CommunicationChannel* getCommunicationChannel() {
            return _communicationChannel;
        }

        void start();

    private:
        template <typename EventT>
        requires Derived<EventT, Event>
        void handleEventError(EventT const * const evt) const {
            if(evt->originatingService == 0) {
                return;
            }

            auto service = _services.find(evt->originatingService);
            if(service == end(_services) || service->second->getServiceState() != ServiceState::ACTIVE) {
                return;
            }

            auto callback = _errorCallbacks.find(CallbackKey{evt->originatingService, EventT::TYPE});
            if(callback == end(_errorCallbacks)) {
                return;
            }

            callback->second(evt);
        }

        void handleEventCompletion(Event const * const evt) const {
            if(evt->originatingService == 0) {
                return;
            }

            auto service = _services.find(evt->originatingService);
            if(service == end(_services) || service->second->getServiceState() != ServiceState::ACTIVE) {
                return;
            }

            auto callback = _completionCallbacks.find(CallbackKey{evt->originatingService, evt->type});
            if(callback == end(_completionCallbacks)) {
                return;
            }

            callback->second(evt);
        }

        void broadcastEvent(Event const * const evt) const {
            auto registeredListeners = _eventCallbacks.find(evt->type);
            if(registeredListeners == end(_eventCallbacks)) {
                return;
            }

            for(auto &[serviceId, callback] : registeredListeners->second) {
                auto service = _services.find(serviceId);
                if(service == end(_services) || service->second->getServiceState() != ServiceState::ACTIVE) {
                    continue;
                }

                if(callback(evt)) {
                    break;
                }
            }
        }

        void setCommunicationChannel(CommunicationChannel *channel) {
            _communicationChannel = channel;
        }

        std::unordered_map<uint64_t, std::shared_ptr<ILifecycleManager>> _services;
        std::unordered_map<uint64_t, std::vector<DependencyTrackerInfo>> _dependencyRequestTrackers;
        std::unordered_map<uint64_t, std::vector<DependencyTrackerInfo>> _dependencyUndoRequestTrackers;
        std::unordered_map<CallbackKey, std::function<void(Event const * const)>> _completionCallbacks;
        std::unordered_map<CallbackKey, std::function<void(Event const * const)>> _errorCallbacks;
        std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, std::function<bool(Event const * const)>>>> _eventCallbacks;
        IFrameworkLogger *_logger;
        moodycamel::ConcurrentQueue<EventStackUniquePtr> _eventQueue;
        moodycamel::ProducerToken _producerToken;
        moodycamel::ConsumerToken _consumerToken;
        std::atomic<uint64_t> _eventIdCounter;
        std::atomic<bool> _quit;
        CommunicationChannel *_communicationChannel;
        uint64_t _id;
        static std::atomic<uint64_t> _managerIdCounter;

        friend class EventCompletionHandlerRegistration;
        friend class CommunicationChannel;
    };
}