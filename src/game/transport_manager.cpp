#include "game/transport_manager.hpp"
#include "game/transport_clock_sync.hpp"
#include "game/transport_animator.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <chrono>
#include <cmath>
#include <limits>

namespace wowee::game {

namespace {

// Absolute wall-clock ms, used (only) to seed a freshly-registered client-animated
// transport's starting phase. elapsedTime_ is time-since-this-process-started, so
// seeding from it makes localClockMs depend on when each client happened to log in
// or first see a given transport GUID - unrelated clients (or the same client
// restarted) compute unrelated phases for the same entry, and there's nothing tying
// two GUIDs that are meant to move in lockstep (e.g. Deeprun Tram's two cars, which
// should always be roughly opposite each other) into any particular relationship.
// Once seeded, localClockMs only ever advances by real per-frame deltaTime (see
// TransportClockSync::computePathTime), so anchoring the seed to absolute time is
// enough to make it deterministic and consistent across clients/restarts - matching
// how tools/bot_fleet_manager/deeprun_tram.py already predicts these positions using
// time.time()*1000 rather than time-since-launch.
uint64_t nowEpochMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool isDeeprunTramTransport(const ActiveTransport& transport) {
    return transport.displayId == 3831u ||
           (transport.entry >= 176080u && transport.entry <= 176085u) ||
           (transport.pathId >= 176080u && transport.pathId <= 176085u);
}

bool seedDeeprunTramStationPhase(ActiveTransport& transport,
                                 const PathEntry& pathEntry,
                                 const glm::vec3& serverPosition,
                                 float orientation) {
    const auto& spline = pathEntry.spline;
    const auto& keys = spline.keys();
    if (!pathEntry.fromDBC || spline.durationMs() == 0 || keys.empty()) {
        return false;
    }

    size_t maxOffsetIdx = 0;
    float maxAbsX = 0.0f;
    for (size_t i = 0; i < keys.size(); ++i) {
        const float absX = std::abs(keys[i].position.x);
        if (absX > maxAbsX) {
            maxAbsX = absX;
            maxOffsetIdx = i;
        }
    }

    if (maxAbsX < 100.0f) {
        return false;
    }

    const bool pathMaxIsPositive = keys[maxOffsetIdx].position.x > 0.0f;
    const bool serverAtPositiveEnd = serverPosition.x > 1000.0f;
    const bool useMaxOffsetStation = (pathMaxIsPositive == serverAtPositiveEnd);

    size_t stationIdx = maxOffsetIdx;
    if (!useMaxOffsetStation) {
        stationIdx = 0;
        for (size_t step = 1; step < keys.size(); ++step) {
            const size_t idx = (maxOffsetIdx + step) % keys.size();
            if (std::abs(keys[idx].position.x) < 1.0f) {
                stationIdx = idx;
                break;
            }
        }
    }

    // CMaNGOS's "presence echo" for these objects is always the same static DB spawn
    // row, not a live position - it tells us this spawn coordinate corresponds to
    // local path point stationIdx (a geometric fact about how this entry's path is
    // anchored in world space), not that the tram is *currently* there. Correct
    // basePosition so the path curve sits in the right place; leave localClockMs
    // alone so it keeps reflecting the absolute-time-derived phase set at
    // registration (see nowEpochMs comment above) instead of snapping back to
    // "docked at this waypoint" every time an echo arrives.
    const glm::vec3 stationOffset = keys[stationIdx].position;
    transport.basePosition = serverPosition - stationOffset;
    transport.position = transport.basePosition + spline.evaluatePosition(transport.localClockMs % spline.durationMs());
    transport.hasServerYaw = false;

    LOG_WARNING("Deeprun tram station anchor corrected: guid=0x", std::hex, transport.guid, std::dec,
                " entry=", transport.entry,
                " pathId=", transport.pathId,
                " localClockMs=", transport.localClockMs,
                " stationOffset=(", stationOffset.x, ",", stationOffset.y, ",", stationOffset.z, ")",
                " base=(", transport.basePosition.x, ",", transport.basePosition.y, ",", transport.basePosition.z, ")",
                " serverPos=(", serverPosition.x, ",", serverPosition.y, ",", serverPosition.z, ")");
    return true;
}

} // namespace

TransportManager::TransportManager() = default;
TransportManager::~TransportManager() = default;

void TransportManager::update(float deltaTime) {
    elapsedTime_ += deltaTime;

    for (auto& [guid, transport] : transports_) {
        // Once we have server clock offset, we can predict server time indefinitely
        // No need for watchdog - keep using the offset even if server updates stop
        updateTransportMovement(transport, deltaTime);
    }
}

void TransportManager::registerTransport(uint64_t guid,
                                         uint32_t wmoInstanceId,
                                         uint32_t pathId,
                                         const glm::vec3& spawnWorldPos,
                                         uint32_t entry,
                                         uint32_t displayId,
                                         bool isM2) {
    auto* pathEntry = pathRepo_.findPath(pathId);
    if (!pathEntry) {
        LOG_ERROR("TransportManager: Path ", pathId, " not found for transport ", guid);
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        LOG_ERROR("TransportManager: Path ", pathId, " has no waypoints");
        return;
    }

    ActiveTransport transport;
    transport.guid = guid;
    transport.wmoInstanceId = wmoInstanceId;
    transport.pathId = pathId;
    transport.entry = entry;
    transport.displayId = displayId;
    transport.isM2 = isM2;
    transport.allowBootstrapVelocity = false;

    // CRITICAL: Set basePosition from spawn position and t=0 offset
    // For stationary paths (1 waypoint), just use spawn position directly
    if (spline.durationMs() == 0 || spline.keyCount() <= 1) {
        // Stationary transport - no path animation
        transport.basePosition = spawnWorldPos;
        transport.position = spawnWorldPos;
    } else if (pathEntry->worldCoords) {
        // World-coordinate path (TaxiPathNode) - points are absolute world positions
        transport.basePosition = glm::vec3(0.0f);
        transport.position = spline.evaluatePosition(0);
    } else {
        // Moving transport - infer base from first path offset
        glm::vec3 offset0 = spline.evaluatePosition(0);
        transport.basePosition = spawnWorldPos - offset0;  // Infer base from spawn
        transport.position = spawnWorldPos;  // Start at spawn position (base + offset0)

        // TransportAnimation paths are local offsets; first waypoint is expected near origin.
        // Warn only if the local path itself looks suspicious.
        glm::vec3 firstWaypoint = spline.keys()[0].position;
        if (glm::dot(firstWaypoint, firstWaypoint) > 100.0f) {
            LOG_WARNING("Transport 0x", std::hex, guid, std::dec, " path ", pathId,
                        ": first local waypoint far from origin: (",
                        firstWaypoint.x, ",", firstWaypoint.y, ",", firstWaypoint.z, ")");
        }
    }

    transport.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // Identity quaternion
    transport.playerOnBoard = false;
    transport.playerLocalOffset = glm::vec3(0.0f);
    transport.hasDeckBounds = false;
    transport.localClockMs = 0;
    transport.hasServerClock = false;
    transport.serverClockOffsetMs = 0;
    // Start with client-side animation for all DBC paths with real movement.
    // If the server sends actual position updates, updateServerTransport() will switch
    // to server-driven mode. This ensures transports like trams (which the server doesn't
    // stream updates for) still animate, while ships/zeppelins switch to server authority.
    transport.useClientAnimation = (pathEntry->fromDBC && spline.durationMs() > 0);
    transport.clientAnimationReverse = false;
    transport.serverYaw = 0.0f;
    transport.hasServerYaw = false;
    transport.serverYawFlipped180 = false;
    transport.serverYawAlignmentScore = 0;
    transport.lastServerUpdate = 0.0f;
    transport.serverUpdateCount = 0;
    transport.serverLinearVelocity = glm::vec3(0.0f);
    transport.serverAngularVelocity = 0.0f;
    transport.hasServerVelocity = false;

    if (transport.useClientAnimation && spline.durationMs() > 0) {
        // Seed to a stable phase derived from absolute time (see nowEpochMs comment)
        // so elevators don't all start at t=0, and so paired/opposing transports like
        // the Deeprun Tram's two cars land at the same relative phase for every client.
        transport.localClockMs = static_cast<uint32_t>(nowEpochMs() % spline.durationMs());
        LOG_INFO("TransportManager: Enabled client animation for transport 0x",
                 std::hex, guid, std::dec, " path=", pathId,
                 " durationMs=", spline.durationMs(), " seedMs=", transport.localClockMs,
                 (pathEntry->worldCoords ? " [worldCoords]" : (pathEntry->zOnly ? " [z-only]" : "")));
    }

    updateTransformMatrices(transport);

    // CRITICAL: Update WMO renderer with initial transform
    pushTransform(transport);

    transports_[guid] = transport;

    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);
    LOG_INFO("TransportManager: Registered transport 0x", std::hex, guid, std::dec,
             " at path ", pathId, " with ", (pathEntry ? pathEntry->spline.keyCount() : 0u), " waypoints",
             " wmoInstanceId=", wmoInstanceId,
             " entry=", entry,
             " displayId=", displayId,
             " isM2=", isM2,
             " spawnPos=(", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z, ")",
             " basePos=(", transport.basePosition.x, ", ", transport.basePosition.y, ", ", transport.basePosition.z, ")",
             " initialRenderPos=(", renderPos.x, ", ", renderPos.y, ", ", renderPos.z, ")");

    if (isDeeprunTramTransport(transport)) {
        LOG_WARNING("Deeprun tram registered: guid=0x", std::hex, guid, std::dec,
                    " entry=", entry,
                    " displayId=", displayId,
                    " pathId=", pathId,
                    " instanceId=", wmoInstanceId,
                    " isM2=", isM2,
                    " mode=", (transport.useClientAnimation ? "client" : "server"),
                    " spawn=(", spawnWorldPos.x, ",", spawnWorldPos.y, ",", spawnWorldPos.z, ")",
                    " base=(", transport.basePosition.x, ",", transport.basePosition.y, ",", transport.basePosition.z, ")");
    }
}

void TransportManager::unregisterTransport(uint64_t guid) {
    transports_.erase(guid);
    LOG_INFO("TransportManager: Unregistered transport ", guid);
}

void TransportManager::resolveAndRegisterSpawn(uint64_t guid,
                                               uint32_t entry,
                                               uint32_t displayId,
                                               const glm::vec3& canonicalSpawnPos,
                                               uint32_t wmoInstanceId,
                                               bool isM2,
                                               bool preferServerData) {
    // TransportAnimation.dbc is indexed by GameObject entry.
    uint32_t pathId = entry;

    // Check if we have a real usable path, otherwise remap/infer/fall back to stationary.
    const bool shipOrZeppelinDisplay =
        (displayId == 3015 || displayId == 3031 || displayId == 7546 ||
         displayId == 7446 || displayId == 1587 || displayId == 2454 ||
         displayId == 807 || displayId == 808);
    bool hasUsablePath = hasPathForEntry(entry);
    if (shipOrZeppelinDisplay) {
        hasUsablePath = hasUsableMovingPathForEntry(entry, 25.0f);
    }

    if (preferServerData) {
        // Strict server-authoritative mode: no inferred/remapped fallback routes.
        if (!hasUsablePath) {
            std::vector<glm::vec3> path = { canonicalSpawnPos };
            loadPathFromNodes(pathId, path, false, 0.0f);
            LOG_INFO("Auto-spawned transport in strict server-first mode (stationary fallback): entry=", entry,
                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
        } else {
            LOG_INFO("Auto-spawned transport in server-first mode with entry DBC path: entry=", entry,
                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
        }
    } else if (!hasUsablePath) {
        bool allowZOnly = (displayId == 455 || displayId == 462);
        uint32_t inferredPath = inferDbcPathForSpawn(canonicalSpawnPos, 1200.0f, allowZOnly);
        if (inferredPath != 0) {
            pathId = inferredPath;
            LOG_INFO("Auto-spawned transport with inferred path: entry=", entry,
                     " inferredPath=", pathId, " displayId=", displayId,
                     " wmoInstance=", wmoInstanceId);
        } else {
            uint32_t remappedPath = pickFallbackMovingPath(entry, displayId);
            if (remappedPath != 0) {
                pathId = remappedPath;
                LOG_INFO("Auto-spawned transport with remapped fallback path: entry=", entry,
                         " remappedPath=", pathId, " displayId=", displayId,
                         " wmoInstance=", wmoInstanceId);
            } else {
                std::vector<glm::vec3> path = { canonicalSpawnPos };
                loadPathFromNodes(pathId, path, false, 0.0f);
                LOG_INFO("Auto-spawned transport with stationary path: entry=", entry,
                         " displayId=", displayId, " wmoInstance=", wmoInstanceId);
            }
        }
    } else {
        LOG_INFO("Auto-spawned transport with real path: entry=", entry,
                 " displayId=", displayId, " wmoInstance=", wmoInstanceId);
    }

    registerTransport(guid, wmoInstanceId, pathId, canonicalSpawnPos, entry, displayId, isM2);

    if (displayId == 3831u) {
        if (auto* tr = getTransport(guid)) {
            LOG_WARNING("Auto-spawned Deeprun tram transport: guid=0x",
                        std::hex, guid, std::dec,
                        " entry=", entry,
                        " pathId=", tr->pathId,
                        " isM2=", tr->isM2,
                        " mode=", (tr->useClientAnimation ? "client" : "server"));
        }
    }
}

ActiveTransport* TransportManager::getTransport(uint64_t guid) {
    auto it = transports_.find(guid);
    if (it != transports_.end()) {
        return &it->second;
    }
    return nullptr;
}

glm::vec3 TransportManager::getPlayerWorldPosition(uint64_t transportGuid, const glm::vec3& localOffset) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        LOG_WARNING("getPlayerWorldPosition: transport 0x", std::hex, transportGuid, std::dec,
                    " not found — returning localOffset as-is (callers should guard)");
        return localOffset;
    }

    if (transport->isM2) {
        // M2 transports (trams): localOffset is a canonical world-space delta
        // from the transport's canonical position. Just add directly.
        return transport->position + localOffset;
    }

    // WMO transports (ships): localOffset is in transport-local space,
    // use the render-space transform matrix.
    glm::vec4 localPos(localOffset, 1.0f);
    glm::vec4 worldPos = transport->transform * localPos;
    return glm::vec3(worldPos);
}

glm::mat4 TransportManager::getTransportInvTransform(uint64_t transportGuid) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        return glm::mat4(1.0f);  // Identity fallback
    }
    return transport->invTransform;
}

void TransportManager::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    pathRepo_.loadPathFromNodes(pathId, waypoints, looping, speed);
}

void TransportManager::setDeckBounds(uint64_t guid, const glm::vec3& min, const glm::vec3& max) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_ERROR("TransportManager: Cannot set deck bounds for unknown transport ", guid);
        return;
    }

    transport->deckMin = min;
    transport->deckMax = max;
    transport->hasDeckBounds = true;
}

void TransportManager::updateTransportMovement(ActiveTransport& transport, float deltaTime) {
    auto* pathEntry = pathRepo_.findPath(transport.pathId);
    if (!pathEntry) {
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        return;
    }

    // Stationary transport (durationMs = 0)
    if (spline.durationMs() == 0) {
        // Just update transform (position already set)
        updateTransformMatrices(transport);
        pushTransform(transport);
        return;
    }

    // Compute path time via ClockSync
    uint32_t pathTimeMs = 0;
    if (!clockSync_.computePathTime(transport, spline, elapsedTime_, deltaTime, pathTimeMs)) {
        // Strict server-authoritative mode: do not guess movement between server snapshots.
        updateTransformMatrices(transport);
        pushTransform(transport);
        return;
    }

    // Evaluate position + rotation via Animator
    animator_.evaluateAndApply(transport, *pathEntry, pathTimeMs);

    // Update transform matrices
    updateTransformMatrices(transport);
    pushTransform(transport);

    // Debug logging every 600 frames (~10 seconds at 60fps)
    static int debugFrameCount = 0;
    if (debugFrameCount++ % 600 == 0) {
        LOG_DEBUG("Transport 0x", std::hex, transport.guid, std::dec,
                 " pathTime=", pathTimeMs, "ms / ", spline.durationMs(), "ms",
                 " pos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")",
                 " mode=", (transport.useClientAnimation ? "client" : "server"),
                 " isM2=", transport.isM2);
    }
}

// Push transform to the appropriate renderer (WMO or M2).
void TransportManager::pushTransform(ActiveTransport& transport) {
    if (transport.isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    } else {
        if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    }
}

void TransportManager::updateTransformMatrices(ActiveTransport& transport) {
    // Convert position from canonical to render coordinates for WMO rendering
    // Canonical: +X=North, +Y=West, +Z=Up
    // Render: renderX=wowY (west), renderY=wowX (north), renderZ=wowZ (up)
    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);

    // Convert rotation from canonical to render space using proper basis change
    // Canonical → Render is a 90° CCW rotation around Z (swaps X and Y)
    // Proper formula: q_render = q_basis * q_canonical * q_basis^-1
    glm::quat basisRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat basisInverse = glm::conjugate(basisRotation);
    glm::quat renderRot = basisRotation * transport.rotation * basisInverse;

    // Build transform matrix: translate * rotate * scale
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), renderPos);
    glm::mat4 rotation = glm::mat4_cast(renderRot);
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));  // No scaling for transports

    transport.transform = translation * rotation * scale;
    transport.invTransform = glm::inverse(transport.transform);
}

void TransportManager::updateServerTransport(uint64_t guid, const glm::vec3& position, float orientation) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_WARNING("TransportManager::updateServerTransport: Transport not found: 0x", std::hex, guid, std::dec);
        return;
    }

    auto* pathEntry = pathRepo_.findPath(transport->pathId);

    if (!pathEntry || pathEntry->spline.durationMs() == 0) {
        // No path or stationary — handle directly before delegating to ClockSync.
        // Still track update count so future path assignments work.
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->basePosition = position;
        transport->position = position;
        transport->rotation = glm::angleAxis(orientation, glm::vec3(0.0f, 0.0f, 1.0f));
        updateTransformMatrices(*transport);
        pushTransform(*transport);
        return;
    }

    if (transport->isM2 && isDeeprunTramTransport(*transport) && pathEntry->fromDBC) {
        // CMangos sends occasional position echoes for Deeprun subway cars, but the client
        // owns the TransportAnimation.dbc path phase. Treat those samples as presence/yaw
        // hints rather than switching the M2 tram into stationary server-driven mode.
        const bool firstUpdate = transport->serverUpdateCount == 0;
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->useClientAnimation = true;
        transport->clientAnimationReverse = false;
        transport->hasServerClock = false;
        const glm::vec3 baseDelta = position - transport->basePosition;
        const bool stationEcho = glm::dot(baseDelta, baseDelta) < 4.0f;
        if (firstUpdate || stationEcho) {
            if (!seedDeeprunTramStationPhase(*transport, *pathEntry, position, orientation)) {
                transport->basePosition = position;
                transport->position = position;
                transport->localClockMs = 0;
                transport->hasServerYaw = false;
                transport->rotation = glm::angleAxis(orientation, glm::vec3(0.0f, 0.0f, 1.0f));
            }
        }
        if (transport->serverUpdateCount <= 3) {
            LOG_WARNING("Deeprun tram server update kept client-driven: guid=0x", std::hex, guid, std::dec,
                        " entry=", transport->entry,
                        " displayId=", transport->displayId,
                        " pathId=", transport->pathId,
                        " pos=(", position.x, ",", position.y, ",", position.z, ")",
                        " orientation=", orientation);
        }
        updateTransformMatrices(*transport);
        pushTransform(*transport);
        return;
    }

    // Delegate clock sync, yaw correction, and velocity bootstrap to ClockSync.
    clockSync_.processServerUpdate(*transport, pathEntry, position, orientation, elapsedTime_);

    updateTransformMatrices(*transport);
    pushTransform(*transport);
}

void TransportManager::rebindTransportInstance(uint64_t guid, uint32_t instanceId, bool isM2, uint32_t displayId) {
    auto* transport = getTransport(guid);
    if (!transport) return;

    const bool changed = transport->wmoInstanceId != instanceId ||
                         transport->isM2 != isM2 ||
                         (displayId != 0 && transport->displayId != displayId);
    transport->wmoInstanceId = instanceId;
    transport->isM2 = isM2;
    if (displayId != 0) {
        transport->displayId = displayId;
    }

    updateTransformMatrices(*transport);
    pushTransform(*transport);

    if (changed && isDeeprunTramTransport(*transport)) {
        LOG_WARNING("Deeprun tram rebound to render instance: guid=0x", std::hex, guid, std::dec,
                    " instanceId=", instanceId,
                    " isM2=", isM2,
                    " displayId=", transport->displayId,
                    " pathId=", transport->pathId);
    }
}

bool TransportManager::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTransportAnimationDBC(assetMgr);
}

bool TransportManager::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTaxiPathNodeDBC(assetMgr);
}

bool TransportManager::hasTaxiPath(uint32_t taxiPathId) const {
    return pathRepo_.hasTaxiPath(taxiPathId);
}

bool TransportManager::assignTaxiPathToTransport(uint32_t entry, uint32_t taxiPathId) {
    auto* taxiEntry = pathRepo_.findTaxiPath(taxiPathId);
    if (!taxiEntry) {
        LOG_WARNING("No TaxiPathNode path for taxiPathId=", taxiPathId);
        return false;
    }

    // Find transport(s) with matching entry that are at (0,0,0)
    for (auto& [guid, transport] : transports_) {
        if (transport.entry != entry) continue;
        if (glm::dot(transport.position, transport.position) > 1.0f) continue;  // Already has real position

        // Copy the taxi path into the main paths (indexed by GO entry for this transport)
        PathEntry copied(taxiEntry->spline, entry, taxiEntry->zOnly, taxiEntry->fromDBC, taxiEntry->worldCoords);
        pathRepo_.storePath(entry, std::move(copied));

        auto* storedEntry = pathRepo_.findPath(entry);

        // Update transport to use the new path
        transport.pathId = entry;
        transport.basePosition = glm::vec3(0.0f);  // World-coordinate path, no base offset
        if (storedEntry && storedEntry->spline.keyCount() > 0) {
            transport.position = storedEntry->spline.evaluatePosition(0);
        }
        transport.useClientAnimation = true;  // Server won't send position updates

        // Seed local clock to a deterministic phase (see nowEpochMs comment above)
        if (storedEntry && storedEntry->spline.durationMs() > 0) {
            transport.localClockMs = static_cast<uint32_t>(nowEpochMs() % storedEntry->spline.durationMs());
        }

        updateTransformMatrices(transport);
        pushTransform(transport);

        LOG_INFO("Assigned TaxiPathNode path to transport 0x", std::hex, guid, std::dec,
                 " entry=", entry, " taxiPathId=", taxiPathId,
                 " waypoints=", storedEntry ? storedEntry->spline.keyCount() : 0u,
                 " duration=", storedEntry ? storedEntry->spline.durationMs() : 0u, "ms",
                 " startPos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")");
        return true;
    }

    LOG_DEBUG("No transport at (0,0,0) found for entry=", entry, " taxiPathId=", taxiPathId);
    return false;
}

bool TransportManager::hasPathForEntry(uint32_t entry) const {
    return pathRepo_.hasPathForEntry(entry);
}

bool TransportManager::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    return pathRepo_.hasUsableMovingPathForEntry(entry, minXYRange);
}

uint32_t TransportManager::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                               float maxDistance,
                                               bool allowZOnly) const {
    return pathRepo_.inferDbcPathForSpawn(spawnWorldPos, maxDistance, allowZOnly);
}

uint32_t TransportManager::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return pathRepo_.inferMovingPathForSpawn(spawnWorldPos, maxDistance);
}

uint32_t TransportManager::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    return pathRepo_.pickFallbackMovingPath(entry, displayId);
}

} // namespace wowee::game
