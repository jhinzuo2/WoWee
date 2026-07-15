#include <catch_amalgamated.hpp>

#include "rendering/lighting_manager.hpp"
#include "game/zone_manager.hpp"

using wowee::rendering::LightingParams;
using wowee::rendering::applyZoneAmbienceOverride;
using wowee::rendering::resolveZoneVisualTimeHours;

TEST_CASE("Duskwood ADT coverage follows client map bounds", "[zone_ambience]") {
    using wowee::game::isDuskwoodAdtTile;

    REQUIRE(isDuskwoodAdtTile(50, 30));
    REQUIRE(isDuskwoodAdtTile(51, 34)); // Darkshire
    REQUIRE(isDuskwoodAdtTile(53, 35));
    REQUIRE_FALSE(isDuskwoodAdtTile(49, 32)); // Elwynn/Redridge side
    REQUIRE_FALSE(isDuskwoodAdtTile(54, 34)); // Stranglethorn side
    REQUIRE_FALSE(isDuskwoodAdtTile(52, 29));
}

TEST_CASE("Duskwood uses a permanent late-night sky clock", "[zone_ambience]") {
    REQUIRE(resolveZoneVisualTimeHours(10, false, 12.0f) == Catch::Approx(22.0f));
    REQUIRE(resolveZoneVisualTimeHours(10, false, 6.0f) == Catch::Approx(22.0f));
    REQUIRE(resolveZoneVisualTimeHours(10, true, 12.0f) == Catch::Approx(12.0f));
    REQUIRE(resolveZoneVisualTimeHours(12, false, 12.0f) == Catch::Approx(12.0f));
}

TEST_CASE("Duskwood ambience stays dark and foggy", "[zone_ambience]") {
    LightingParams params;
    params.ambientColor = glm::vec3(0.8f);
    params.diffuseColor = glm::vec3(1.0f);
    params.fogStart = 300.0f;
    params.fogEnd = 1500.0f;
    params.fogDensity = 0.001f;
    params.cloudDensity = 0.1f;
    params.horizonGlow = 0.5f;

    applyZoneAmbienceOverride(10, params);

    REQUIRE(params.ambientColor.r <= 0.20f);
    REQUIRE(params.diffuseColor.r <= 0.26f);
    REQUIRE(params.fogStart == Catch::Approx(35.0f));
    REQUIRE(params.fogEnd == Catch::Approx(525.0f));
    REQUIRE(params.fogDensity >= 0.006f);
    REQUIRE(params.cloudDensity >= 0.88f);
    REQUIRE(params.horizonGlow <= 0.08f);
}

TEST_CASE("Zone ambience does not alter other zones", "[zone_ambience]") {
    LightingParams params;
    const LightingParams original = params;

    applyZoneAmbienceOverride(12, params);

    REQUIRE(params.ambientColor == original.ambientColor);
    REQUIRE(params.diffuseColor == original.diffuseColor);
    REQUIRE(params.fogColor == original.fogColor);
    REQUIRE(params.fogStart == original.fogStart);
    REQUIRE(params.fogEnd == original.fogEnd);
    REQUIRE(params.cloudDensity == original.cloudDensity);
}
