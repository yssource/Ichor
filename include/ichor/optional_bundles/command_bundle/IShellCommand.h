#pragma once

#include <fmt/format.h>
#include <ichor/Common.h>
#include <ichor/Service.h>

namespace Ichor {
    class IShellCommand : virtual public IService {
    protected:
        virtual ~IShellCommand() = default;
    };
}