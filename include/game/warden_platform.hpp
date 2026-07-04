#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace wowee {
namespace game {

/**
 * Platform services used by Warden code for filesystem-backed artifacts.
 *
 * The protocol and module loader stay platform-neutral; desktop and Android
 * provide cache/integrity locations through this boundary.
 */
class WardenPlatformServices {
public:
    virtual ~WardenPlatformServices() = default;

    virtual std::filesystem::path wardenCacheDir() const;
    virtual std::filesystem::path integrityImagePath(uint16_t build, bool isTurtle) const;

    virtual bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& out) const;
    virtual bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) const;

    static std::shared_ptr<WardenPlatformServices> defaultServices();
};

} // namespace game
} // namespace wowee
