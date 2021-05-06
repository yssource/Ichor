#pragma once

#include <ichor/Service.h>

namespace Ichor {
    class IConnectionService {
    public:
        /**
         * Send function. Implementation defined synchronous/asynchronous behaviour.
         * @param msg message to send
         * @return true if succesfully (going to) send message, false otherwise
         */
        virtual bool send(std::pmr::vector<uint8_t>&& msg) = 0;

        /**
         * Sets priority with which to push incoming network events.
         * @param priority
         */
        virtual void setPriority(uint64_t priority) = 0;
        virtual uint64_t getPriority() = 0;

    protected:
        virtual ~IConnectionService() = default;
    };
}