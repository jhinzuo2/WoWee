#include "auth/integrity.hpp"
#include "auth/crypto.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

namespace wowee {
namespace auth {

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "missing: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) size = 0;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) {
            err = "read failed: " + path;
            return false;
        }
    }
    return true;
}

bool computeIntegrityHashWin32WithExe(const std::array<uint8_t, 16>& checksumSalt,
                                      const std::vector<uint8_t>& clientPublicKeyA,
                                      const std::string& miscDir,
                                      const std::string& exeName,
                                      std::array<uint8_t, 20>& outHash,
                                      std::string& outError) {
    // Files expected by 1.12.x Windows clients for the integrity check.
    // If this needs to vary by build, make it data-driven in expansion.json later.
    //
    // Turtle WoW ships a custom loader DLL. Some Turtle auth servers appear to validate integrity against
    // that distribution rather than a stock 1.12.1 client, so when using Turtle's executable we include
    // Turtle-specific DLLs as well.
    const bool isTurtleExe = (exeName == "TurtleWoW.exe");
    // Some macOS client layouts use FMOD dylib naming instead of fmod.dll.
    // We accept the first matching filename in each alias group.
    std::vector<std::vector<std::string>> fileGroups = {
        { exeName },
        { "fmod.dll", "fmod.dylib", "libfmod.dylib", "fmodex.dll", "fmodex.dylib", "libfmod.so" },
        { "ijl15.dll" },
        { "dbghelp.dll" },
        { "unicows.dll" },
    };
    if (isTurtleExe) {
        fileGroups.push_back({ "twloader.dll" });
        fileGroups.push_back({ "twdiscord.dll" });
    }

    std::vector<uint8_t> allFiles;
    for (const auto& group : fileGroups) {
        bool foundInGroup = false;
        std::string groupErr;

        for (const auto& nameStr : group) {
            std::vector<uint8_t> bytes;
            std::string path = miscDir;
            if (!path.empty() && path.back() != '/') path += '/';
            path += nameStr;

            std::string err;
            if (!readWholeFile(path, bytes, err)) {
                if (groupErr.empty()) groupErr = err;
                continue;
            }

            allFiles.insert(allFiles.end(), bytes.begin(), bytes.end());
            foundInGroup = true;
            break;
        }

        if (!foundInGroup) {
            outError = groupErr.empty() ? "missing required integrity file group" : groupErr;
            return false;
        }
    }

    // HMAC_SHA1(checksumSalt, allFiles)
    std::vector<uint8_t> key(checksumSalt.begin(), checksumSalt.end());
    const std::vector<uint8_t> checksum = Crypto::hmacSHA1(key, allFiles); // 20 bytes

    // SHA1(A || checksum)
    std::vector<uint8_t> shaIn;
    shaIn.reserve(clientPublicKeyA.size() + checksum.size());
    shaIn.insert(shaIn.end(), clientPublicKeyA.begin(), clientPublicKeyA.end());
    shaIn.insert(shaIn.end(), checksum.begin(), checksum.end());
    const std::vector<uint8_t> finalHash = Crypto::sha1(shaIn);

    if (finalHash.size() != outHash.size()) {
        outError = "unexpected sha1 size";
        return false;
    }
    std::copy(finalHash.begin(), finalHash.end(), outHash.begin());
    return true;
}

bool computeIntegrityHashWin32(const std::array<uint8_t, 16>& checksumSalt,
                               const std::vector<uint8_t>& clientPublicKeyA,
                               const std::string& miscDir,
                               std::array<uint8_t, 20>& outHash,
                               std::string& outError) {
    return computeIntegrityHashWin32WithExe(checksumSalt, clientPublicKeyA, miscDir, "WoW.exe", outHash, outError);
}

bool computeIntegrityHashFromVersionHash(const std::vector<uint8_t>& clientPublicKeyA,
                                         const std::array<uint8_t, 20>& versionHash,
                                         std::array<uint8_t, 20>& outHash,
                                         std::string& outError) {
    std::vector<uint8_t> shaIn;
    shaIn.reserve(clientPublicKeyA.size() + versionHash.size());
    shaIn.insert(shaIn.end(), clientPublicKeyA.begin(), clientPublicKeyA.end());
    shaIn.insert(shaIn.end(), versionHash.begin(), versionHash.end());

    const std::vector<uint8_t> finalHash = Crypto::sha1(shaIn);
    if (finalHash.size() != outHash.size()) {
        outError = "unexpected sha1 size";
        return false;
    }

    std::copy(finalHash.begin(), finalHash.end(), outHash.begin());
    return true;
}

bool getKnownClientVersionHash(uint16_t build,
                               const std::string& os,
                               std::array<uint8_t, 20>& outHash) {
    auto isMac = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value == "osx" || value == "mac";
    };

    // Values from CMaNGOS classic RealmList.cpp ExpectedRealmdClientBuilds.
    // Build 5875 is stock vanilla 1.12.1.
    if (build == 5875) {
        if (isMac(os)) {
            outHash = {{ 0x8D, 0x17, 0x3C, 0xC3, 0x81, 0x96, 0x1E, 0xEB, 0xAB, 0xF3,
                         0x36, 0xF5, 0xE6, 0x67, 0x5B, 0x10, 0x1B, 0xB5, 0x13, 0xE5 }};
        } else {
            outHash = {{ 0x95, 0xED, 0xB2, 0x7C, 0x78, 0x23, 0xB3, 0x63, 0xCB, 0xDD,
                         0xAB, 0x56, 0xA3, 0x92, 0xE7, 0xCB, 0x73, 0xFC, 0xCA, 0x20 }};
        }
        return true;
    }

    return false;
}

} // namespace auth
} // namespace wowee
