// src/game/transport_animator.cpp
// Path evaluation, Z clamping, and orientation for transports.
// Extracted from TransportManager::updateTransportMovement (Phase 3b of spline refactoring).
#include "game/transport_animator.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "math/spline.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace wowee::game {

void TransportAnimator::evaluateAndApply(
    ActiveTransport& transport,
    const PathEntry& pathEntry,
    uint32_t pathTimeMs) const
{
    const auto& spline = pathEntry.spline;

    // Evaluate position from time via CatmullRomSpline (path is local offsets, add base position)
    glm::vec3 pathOffset = spline.evaluatePosition(pathTimeMs);

    // Entry 176085's TransportAnimation.dbc path data is mirrored relative to its real
    // train siblings (176080, 176081): diagnostic dump of all six entries' raw key
    // extents showed 176080/176081 spanning local X=[0,+2482] (real-world X correctly
    // increasing toward Stormwind, matching their spawn near Ironforge), while 176085
    // spans local X=[-2482,0] - the same negative-direction convention as the genuinely
    // Stormwind-side cars (176082-176084, correct for THEM since their real-world X
    // needs to decrease toward Ironforge). 176085 spawns at real-world X=-11 (Ironforge
    // side) but its path pulls it further NEGATIVE instead of toward Stormwind's
    // positive coordinates, driving it off the edge of the modeled tunnel entirely -
    // "took me outside of map bounds and back" reported live, on the one car
    // consistently observed going the wrong way. Y is negligible for all six paths
    // (confirmed ~0 in the same dump), so a straight X negation is enough to mirror
    // this one entry's local path back into the same real-world direction as its
    // siblings, without needing a general per-transport reverse/mirror flag for what's
    // so far a single-entry data quirk.
    if (transport.entry == 176085u) {
        pathOffset.x = -pathOffset.x;
    }

    pathOffset.z = clampZOffset(
        pathOffset.z,
        pathEntry.worldCoords,
        transport.useClientAnimation,
        transport.serverUpdateCount,
        transport.hasServerClock);

    transport.position = transport.basePosition + pathOffset;

    // Use server yaw if available (authoritative), otherwise compute from spline tangent
    if (transport.hasServerYaw) {
        float effectiveYaw = transport.serverYaw +
            (transport.serverYawFlipped180 ? glm::pi<float>() : 0.0f);
        transport.rotation = glm::angleAxis(effectiveYaw, glm::vec3(0.0f, 0.0f, 1.0f));
    } else {
        auto result = spline.evaluate(pathTimeMs);
        glm::vec3 tangent = result.tangent;
        // Mirror the tangent's X to match the position mirror above, so facing
        // direction stays consistent with this entry's (corrected) direction of travel.
        if (transport.entry == 176085u) {
            tangent.x = -tangent.x;
        }
        // orientationFromTangent orients along the full 3D tangent, pitching/banking to
        // match vertical slope - correct for something like a boat cresting swells, but
        // a subway car shouldn't nose-dive on a downhill grade the way this made it look
        // ("angling downwards instead of staying flat" reported live). Flattening
        // tangent.z to 0 fixed that, but a subsequent live test reported the level car
        // visually clipping into the sloped tunnel floor on grade changes, so that was
        // changed to a clamped partial pitch instead of a full flatten. Confirmed via a
        // later live comparison against the real game client that this was the wrong
        // trade-off: in the real client, tram cars stay level (parallel to the ground)
        // through elevation changes - any clamped tilt is visibly wrong regardless of
        // whether it also reduces clipping. Reverted to a full flatten to match the
        // real client's confirmed behavior; other transports keep full tangent-based
        // orientation.
        const bool isDeeprunTram =
            transport.displayId == 3831u ||
            (transport.entry >= 176080u && transport.entry <= 176085u) ||
            (transport.pathId >= 176080u && transport.pathId <= 176085u);
        if (isDeeprunTram) {
            tangent.z = 0.0f;
        }
        transport.rotation = math::CatmullRomSpline::orientationFromTangent(tangent);
    }
}

float TransportAnimator::clampZOffset(float z, bool worldCoords, bool clientAnim,
                                       int serverUpdateCount, bool hasServerClock)
{
    // Skip Z clamping for world-coordinate paths (TaxiPathNode) where values are absolute positions.
    if (worldCoords) return z;

    constexpr float kMinFallbackZOffset = -2.0f;
    constexpr float kMaxFallbackZOffset =  8.0f;

    // Clamp fallback Z offsets for non-world-coordinate paths to prevent transport
    // models from sinking below sea level on paths derived only from spawn-time data
    // (notably icebreaker routes where the DBC path has steep vertical curves).
    if (clientAnim && serverUpdateCount <= 1) {
        z = std::max(z, kMinFallbackZOffset);
    }
    if (!clientAnim && !hasServerClock) {
        z = std::clamp(z, kMinFallbackZOffset, kMaxFallbackZOffset);
    }
    return z;
}

} // namespace wowee::game
