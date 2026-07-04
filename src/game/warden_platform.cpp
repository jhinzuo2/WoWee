#include "game/warden_platform.hpp"
#include "core/logger.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

namespace wowee {
namespace game {

namespace {

std::filesystem::path envPath(const char* name) {
    if (const char* value = std::getenv(name)) {
        if (*value) return std::filesystem::path(value);
    }
    return {};
}

uint32_t readPEImageSize(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return 0;
    f.seekg(0, std::ios::end);
    auto fileSize = f.tellg();
    if (fileSize < 256) return 0;

    f.seekg(0x3C);
    uint32_t peOfs = 0;
    f.read(reinterpret_cast<char*>(&peOfs), sizeof(peOfs));
    if (!f || peOfs + 4 + 20 + 60 > static_cast<uint32_t>(fileSize)) return 0;

    f.seekg(peOfs + 4 + 20 + 56);
    uint32_t imageSize = 0;
    f.read(reinterpret_cast<char*>(&imageSize), sizeof(imageSize));
    return f ? imageSize : 0;
}

uint32_t expectedImageSizeForBuild(uint16_t build, bool isTurtle) {
    switch (build) {
        case 5875:
            return isTurtle ? 0x00906000u : 0x009FD000u;
        default:
            return 0;
    }
}

} // namespace

std::filesystem::path WardenPlatformServices::wardenCacheDir() const {
    if (auto overrideDir = envPath("WOWEE_WARDEN_CACHE_DIR"); !overrideDir.empty()) {
        return overrideDir;
    }

#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        if (*appdata) return std::filesystem::path(appdata) / "wowee" / "warden_cache";
    }
    return std::filesystem::path(".") / "warden_cache";
#elif defined(__ANDROID__)
    return std::filesystem::path(".") / "warden_cache";
#else
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::filesystem::path(home) / ".local" / "share" / "wowee" / "warden_cache";
    }
    return std::filesystem::path(".") / "warden_cache";
#endif
}

std::filesystem::path WardenPlatformServices::integrityImagePath(uint16_t build, bool isTurtle) const {
    std::vector<std::filesystem::path> candidateDirs;

    if (auto dir = envPath("WOWEE_INTEGRITY_DIR"); !dir.empty()) {
        candidateDirs.push_back(dir);
    }

#if !defined(__ANDROID__)
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            std::filesystem::path homePath(home);
            candidateDirs.push_back(homePath / "Downloads");
            candidateDirs.push_back(homePath / "Downloads" / "twmoa_1180");
            candidateDirs.push_back(homePath / "twmoa_1180");
        }
    }
#endif

    candidateDirs.emplace_back("Data/misc");
    candidateDirs.emplace_back("Data/expansions/turtle/overlay/misc");

    std::vector<std::string> candidateExes;
    if (const char* exe = std::getenv("WOWEE_INTEGRITY_EXE")) {
        if (*exe) candidateExes.emplace_back(exe);
    }
    candidateExes.emplace_back("WoW.exe");
    candidateExes.emplace_back("TurtleWoW.exe");
    candidateExes.emplace_back("Wow.exe");

    std::vector<std::filesystem::path> allPaths;
    for (const auto& dir : candidateDirs) {
        for (const auto& exe : candidateExes) {
            std::filesystem::path path = dir / exe;
            std::error_code ec;
            if (std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec)) {
                allPaths.push_back(path);
            }
        }
    }

    const uint32_t expectedSize = expectedImageSizeForBuild(build, isTurtle);
    if (expectedSize != 0 && allPaths.size() > 1) {
        for (const auto& path : allPaths) {
            uint32_t imageSize = readPEImageSize(path);
            if (imageSize == expectedSize) {
                LOG_INFO("WardenPlatform: Matched build ", build, " to ", path.string(),
                         " (imageSize=0x", std::hex, imageSize, std::dec, ")");
                return path;
            }
        }
    }

    std::filesystem::path bestPath;
    uintmax_t bestSize = 0;
    for (const auto& path : allPaths) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > bestSize) {
            bestSize = size;
            bestPath = path;
        }
    }

    if (!bestPath.empty()) return bestPath;
    return allPaths.empty() ? std::filesystem::path{} : allPaths.front();
}

bool WardenPlatformServices::readFile(const std::filesystem::path& path, std::vector<uint8_t>& out) const {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    auto fileSize = f.tellg();
    if (fileSize < 0) return false;
    f.seekg(0, std::ios::beg);

    out.resize(static_cast<size_t>(fileSize));
    if (!out.empty()) {
        f.read(reinterpret_cast<char*>(out.data()), fileSize);
        if (!f) {
            out.clear();
            return false;
        }
    }
    return true;
}

bool WardenPlatformServices::writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(f);
}

std::shared_ptr<WardenPlatformServices> WardenPlatformServices::defaultServices() {
    static std::shared_ptr<WardenPlatformServices> services = std::make_shared<WardenPlatformServices>();
    return services;
}

} // namespace game
} // namespace wowee
