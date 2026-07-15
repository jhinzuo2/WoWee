#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

// WorldMapArea.dbc bounds converted to canonical ADT tile indices.
constexpr bool isDuskwoodAdtTile(int tileX, int tileY) {
    return tileX >= 50 && tileX <= 53 && tileY >= 30 && tileY <= 35;
}

struct ZoneInfo {
    uint32_t id;
    std::string name;
    std::vector<std::string> musicPaths;  // MPQ paths to music files
};

class ZoneManager {
public:
    void initialize();

    // Supplement zone music paths using AreaTable → ZoneMusic → SoundEntries DBC chain.
    // Safe to call after initialize(); idempotent and additive (does not remove existing paths).
    void enrichFromDBC(pipeline::AssetManager* assets);

    uint32_t getZoneId(int tileX, int tileY) const;
    const ZoneInfo* getZoneInfo(uint32_t zoneId) const;
    std::string getRandomMusic(uint32_t zoneId);
    std::vector<std::string> getAllMusicPaths() const;

    // When false, file: (original soundtrack) tracks are excluded from the pool
    void setUseOriginalSoundtrack(bool use) { useOriginalSoundtrack_ = use; }
    bool getUseOriginalSoundtrack() const { return useOriginalSoundtrack_; }

private:
    // tile key = tileX * 100 + tileY
    std::unordered_map<int, uint32_t> tileToZone;
    std::unordered_map<uint32_t, ZoneInfo> zones;
    std::string lastPlayedMusic_;
    bool useOriginalSoundtrack_ = true;
};

} // namespace game
} // namespace wowee
