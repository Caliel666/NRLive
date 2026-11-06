#pragma once
#include <cstdint>

namespace xess_mfg_policy
{
enum class ModelRoute : uint32_t
{
    PortedMfg = 0, // AMD compatibility port; supports 2x through 6x.
    Native2x = 1   // Keep XeFG's native 2x model; intentionally limited to 2x.
};

constexpr ModelRoute normalizeModelRoute(uint32_t value) noexcept
{
    return value == static_cast<uint32_t>(ModelRoute::Native2x)
        ? ModelRoute::Native2x : ModelRoute::PortedMfg;
}

constexpr uint32_t maxMultiplier(ModelRoute route) noexcept
{
    return route == ModelRoute::Native2x ? 2u : 6u;
}

constexpr uint32_t clampMultiplier(uint32_t requested, ModelRoute route) noexcept
{
    if (requested < 2u) return 2u;
    const uint32_t maximum = maxMultiplier(route);
    return requested > maximum ? maximum : requested;
}

constexpr bool usesCompatibilityPort(ModelRoute route) noexcept
{
    return route == ModelRoute::PortedMfg;
}
}
