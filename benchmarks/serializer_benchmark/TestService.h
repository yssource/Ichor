#pragma once

#include <chrono>
#include <ichor/DependencyManager.h>
#include <ichor/optional_bundles/logging_bundle/Logger.h>
#include <ichor/Service.h>
#include <ichor/optional_bundles/serialization_bundle/ISerializationAdmin.h>
#include <ichor/LifecycleManager.h>
#include "../../examples/common/TestMsg.h"

using namespace Ichor;

class TestService final : public Service<TestService> {
public:
    TestService(DependencyRegister &reg, Properties props, DependencyManager *mng) : Service(std::move(props), mng) {
        reg.registerDependency<ILogger>(this, true);
        reg.registerDependency<ISerializationAdmin>(this, true);
    }
    ~TestService() final = default;
    StartBehaviour start() final {
        ICHOR_LOG_INFO(_logger, "TestService started with dependency");
        _doWorkRegistration = getManager()->registerEventCompletionCallbacks<DoWorkEvent>(this);
        getManager()->pushEvent<DoWorkEvent>(getServiceId());
        return Ichor::StartBehaviour::SUCCEEDED;
    }

    StartBehaviour stop() final {
        ICHOR_LOG_INFO(_logger, "TestService stopped with dependency");
        _doWorkRegistration.reset();
        return Ichor::StartBehaviour::SUCCEEDED;
    }

    void addDependencyInstance(ILogger *logger, IService *) {
        _logger = logger;
    }

    void removeDependencyInstance(ILogger *logger, IService *) {
        _logger = nullptr;
    }

    void addDependencyInstance(ISerializationAdmin *serializationAdmin, IService *) {
        _serializationAdmin = serializationAdmin;
        ICHOR_LOG_INFO(_logger, "Inserted serializationAdmin");
    }

    void removeDependencyInstance(ISerializationAdmin *serializationAdmin, IService *) {
        _serializationAdmin = nullptr;
        ICHOR_LOG_INFO(_logger, "Removed serializationAdmin");
    }

    void handleCompletion(DoWorkEvent const * const evt) {
        ICHOR_LOG_ERROR(_logger, "handling DoWorkEvent");
        TestMsg msg{20, "five hundred"};
        auto start = std::chrono::steady_clock::now();
        for(uint64_t i = 0; i < 1'000'000; i++) {
            auto res = _serializationAdmin->serialize<TestMsg>(msg);
            auto msg2 = _serializationAdmin->deserialize<TestMsg>(std::move(res));
            if(msg2->id != msg.id || msg2->val != msg.val) {
                ICHOR_LOG_ERROR(_logger, "serde incorrect!");
            }
        }
        auto end = std::chrono::steady_clock::now();
        ICHOR_LOG_INFO(_logger, "finished in {:L} µs", std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
        getManager()->pushEvent<QuitEvent>(getServiceId());
    }

    void handleError(DoWorkEvent const * const evt) {
        ICHOR_LOG_ERROR(_logger, "Error handling DoWorkEvent");
    }

private:
    ILogger *_logger{};
    ISerializationAdmin *_serializationAdmin{};
    EventCompletionHandlerRegistration _doWorkRegistration{};
};