#include <catch_amalgamated.hpp>

#include "game/warden_crypto.hpp"
#include "game/warden_emulator.hpp"
#include "game/warden_memory.hpp"
#include "game/warden_module.hpp"
#include "game/warden_platform.hpp"
#include "auth/crypto.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <vector>

using wowee::game::WardenCrypto;
using wowee::game::WardenEmulator;
using wowee::game::WardenMemory;
using wowee::game::WardenModule;
using wowee::game::WardenPlatformServices;

namespace {

std::vector<uint8_t> testSessionKey() {
    std::vector<uint8_t> key(40);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return key;
}

std::filesystem::path findCachedWardenModule() {
    auto cacheDir = WardenPlatformServices::defaultServices()->wardenCacheDir();
    std::error_code ec;
    if (!std::filesystem::exists(cacheDir, ec)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
        if (ec) break;
        if (entry.path().extension() == ".wdn") return entry.path();
    }
    return {};
}

std::vector<uint8_t> hashFromCacheFilename(const std::filesystem::path& path) {
    std::string hex = path.stem().string();
    if (hex.size() != 32) return std::vector<uint8_t>(16, 0);

    std::vector<uint8_t> out;
    out.reserve(16);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        unsigned long value = std::strtoul(hex.substr(i, 2).c_str(), &end, 16);
        if (!end || *end != '\0') return std::vector<uint8_t>(16, 0);
        out.push_back(static_cast<uint8_t>(value));
    }
    return out;
}

} // namespace

TEST_CASE("WardenCrypto initializes deterministic client streams", "[warden][crypto]") {
    auto sessionKey = testSessionKey();

    WardenCrypto clientA;
    WardenCrypto clientB;
    REQUIRE(clientA.initFromSessionKey(sessionKey));
    REQUIRE(clientB.initFromSessionKey(sessionKey));
    REQUIRE(clientA.isInitialized());
    REQUIRE(clientB.isInitialized());

    std::vector<uint8_t> plain = {0x00, 0x01, 0x02, 0x05, 0xAA, 0x55, 0x10};
    auto encryptedA = clientA.encrypt(plain);
    auto encryptedB = clientB.encrypt(plain);
    REQUIRE(encryptedA != plain);
    REQUIRE(encryptedA == encryptedB);
}

TEST_CASE("WardenCrypto rekeyed streams remain compatible", "[warden][crypto]") {
    auto sessionKey = testSessionKey();
    uint8_t encryptKey[16] = {};
    uint8_t decryptKey[16] = {};
    WardenCrypto::sha1RandxGenerate(sessionKey, encryptKey, decryptKey);

    WardenCrypto client;
    WardenCrypto serverMirror;
    REQUIRE(client.initFromSessionKey(sessionKey));
    REQUIRE(serverMirror.initFromSessionKey(sessionKey));

    client.replaceKeys(
        std::vector<uint8_t>(encryptKey, encryptKey + 16),
        std::vector<uint8_t>(decryptKey, decryptKey + 16));
    serverMirror.replaceKeys(
        std::vector<uint8_t>(decryptKey, decryptKey + 16),
        std::vector<uint8_t>(encryptKey, encryptKey + 16));

    std::vector<uint8_t> plain = {0x05, 0x10, 0x20, 0x30};
    REQUIRE(serverMirror.decrypt(client.encrypt(plain)) == plain);
}

TEST_CASE("WardenEmulator initializes Unicorn x86 backend when compiled in", "[warden][emulator]") {
#ifdef HAVE_UNICORN
    WardenEmulator emu;
    std::array<uint8_t, 16> module{};
    module[0] = 0xC3;

    REQUIRE(emu.initialize(module.data(), module.size(), 0x400000));
    emu.setupCommonAPIHooks();
    REQUIRE(emu.getAPIAddress("kernel32.dll", "VirtualAlloc") != 0);
    REQUIRE(emu.getAPIAddress("kernel32.dll", "GetTickCount") != 0);

    uint32_t addr = emu.allocateMemory(16, 0x04);
    REQUIRE(addr != 0);
    uint32_t value = 0x12345678u;
    uint32_t readBack = 0;
    REQUIRE(emu.writeMemory(addr, &value, sizeof(value)));
    REQUIRE(emu.readMemory(addr, &readBack, sizeof(readBack)));
    REQUIRE(readBack == value);
#else
    SKIP("Warden emulator was built without HAVE_UNICORN");
#endif
}

TEST_CASE("WardenMemory loads configured PE integrity image when available", "[warden][memory]") {
    auto path = WardenPlatformServices::defaultServices()->integrityImagePath(5875, false);
    if (path.empty()) {
        SKIP("No Warden PE integrity image configured");
    }

    WardenMemory memory;
    REQUIRE(memory.loadFromFile(path.string()));

    std::array<uint8_t, 16> text{};
    REQUIRE(memory.readMemory(0x00401000, static_cast<uint8_t>(text.size()), text.data()));

    std::array<uint8_t, 12> kuser{};
    REQUIRE(memory.readMemory(0x7FFE026C, static_cast<uint8_t>(kuser.size()), kuser.data()));
}

TEST_CASE("WardenModule loader keeps compatibility pipeline loaded on local cache input", "[warden][module]") {
    auto modulePath = findCachedWardenModule();
    if (modulePath.empty()) {
        SKIP("No cached .wdn module available for diagnostic loader test");
    }

    std::vector<uint8_t> moduleData;
    REQUIRE(WardenPlatformServices::defaultServices()->readFile(modulePath, moduleData));
    REQUIRE_FALSE(moduleData.empty());

    WardenModule module;
    std::vector<uint8_t> md5 = hashFromCacheFilename(modulePath);
    std::vector<uint8_t> zeroRc4Key(16, 0);

    REQUIRE(module.load(moduleData, md5, zeroRc4Key));
    REQUIRE(module.isLoaded());
}
