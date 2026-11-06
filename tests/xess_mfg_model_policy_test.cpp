#include "xess_mfg_policy.h"
#include <iostream>

int main()
{
    using xess_mfg_policy::ModelRoute;
    using xess_mfg_policy::clampMultiplier;
    using xess_mfg_policy::maxMultiplier;
    using xess_mfg_policy::normalizeModelRoute;
    using xess_mfg_policy::usesCompatibilityPort;

    if (normalizeModelRoute(0) != ModelRoute::PortedMfg ||
        normalizeModelRoute(1) != ModelRoute::Native2x ||
        normalizeModelRoute(99) != ModelRoute::PortedMfg) {
        std::cerr << "XeMFG model route normalization failed\n";
        return 1;
    }
    if (!usesCompatibilityPort(ModelRoute::PortedMfg) ||
        usesCompatibilityPort(ModelRoute::Native2x)) {
        std::cerr << "XeMFG compatibility-port selection failed\n";
        return 2;
    }
    if (maxMultiplier(ModelRoute::PortedMfg) != 6u ||
        clampMultiplier(1u, ModelRoute::PortedMfg) != 2u ||
        clampMultiplier(4u, ModelRoute::PortedMfg) != 4u ||
        clampMultiplier(7u, ModelRoute::PortedMfg) != 6u) {
        std::cerr << "Ported MFG multiplier bounds failed\n";
        return 3;
    }
    if (maxMultiplier(ModelRoute::Native2x) != 2u ||
        clampMultiplier(1u, ModelRoute::Native2x) != 2u ||
        clampMultiplier(2u, ModelRoute::Native2x) != 2u ||
        clampMultiplier(3u, ModelRoute::Native2x) != 2u ||
        clampMultiplier(6u, ModelRoute::Native2x) != 2u) {
        std::cerr << "Native 2x model must never select an MFG multiplier\n";
        return 4;
    }
    return 0;
}
