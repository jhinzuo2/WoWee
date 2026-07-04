#include "auth/auth_packets.hpp"
#include "auth/crypto.hpp"
#include "auth/integrity.hpp"
#include "auth/srp.hpp"
#include "network/tcp_socket.hpp"
#include "network/world_socket.hpp"
#include "game/opcode_table.hpp"
#include "game/packet_parsers.hpp"
#include "game/world_packets.hpp"
#include "game/character.hpp"
#include "game/warden_constants.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_module.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace wowee;

namespace {

enum class IntegrityMode { Static, File, Zero };

struct Options {
    std::string authHost;
    int authPort = 3724;
    std::string account;
    int major = 1;
    int minor = 18;
    int patch = 1;
    int build = 7272;
    int worldBuild = 5875;
    int logonProto = 8;
    int proto = 3;
    std::string locale = "zhCN";
    std::string password;
    std::vector<uint8_t> authHash;
    bool havePassword = false;
    bool haveHash = false;
    bool readPasswordStdin = false;
    IntegrityMode integrityMode = IntegrityMode::File;
    std::string integrityDir;
    std::string integrityExe = "WoW.exe";
    std::string realmName;
    std::string opcodeJson = "Data/expansions/turtle/opcodes.json";
    int timeoutMs = 12000;
    int wardenWaitMs = 0;
};

struct AuthResultData {
    bool ok = false;
    std::vector<uint8_t> sessionKey;
    auth::RealmListResponse realmList;
    std::unique_ptr<network::TCPSocket> authSocket;
};

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    std::string h;
    h.reserve(hex.size());
    for (char c : hex) {
        if (!std::isspace(static_cast<unsigned char>(c))) h.push_back(c);
    }
    if (h.size() % 2 != 0) throw std::runtime_error("hex length must be even");
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

void usage() {
    std::cerr
        << "Usage:\n"
        << "  world_char_probe <auth_host> <auth_port> <account> [options]\n"
        << "\n"
        << "Auth options:\n"
        << "  --password <pass> | --password-stdin | --hash <hexsha1>\n"
        << "  --version <major.minor.patch> --build <build> --world-build <build>\n"
        << "  --logon-proto <auth challenge protocol> --proto <realm/proof protocol> --locale <locale>\n"
        << "  --integrity file|static|zero --misc-dir <client_dir> --integrity-exe <exe>\n"
        << "\n"
        << "World options:\n"
        << "  --realm <name> --opcodes <json> --timeout-ms <ms> --warden-wait-ms <ms>\n";
}

bool parseVersion(const std::string& raw, int& major, int& minor, int& patch) {
    char dot1 = 0;
    char dot2 = 0;
    std::istringstream ss(raw);
    ss >> major >> dot1 >> minor >> dot2 >> patch;
    return ss && dot1 == '.' && dot2 == '.';
}

bool parseArgs(int argc, char** argv, Options& opt) {
    if (argc < 4) {
        usage();
        return false;
    }

    opt.authHost = argv[1];
    opt.authPort = std::atoi(argv[2]);
    opt.account = argv[3];

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](const char* name) -> bool {
            if (i + 1 < argc) return true;
            std::cerr << name << " requires a value\n";
            return false;
        };

        if (a == "--password" && needValue("--password")) {
            opt.password = argv[++i];
            opt.havePassword = true;
        } else if (a == "--password-stdin") {
            opt.readPasswordStdin = true;
        } else if (a == "--hash" && needValue("--hash")) {
            opt.authHash = hexToBytes(argv[++i]);
            opt.haveHash = true;
        } else if (a == "--version" && needValue("--version")) {
            if (!parseVersion(argv[++i], opt.major, opt.minor, opt.patch)) {
                std::cerr << "Invalid --version; expected major.minor.patch\n";
                return false;
            }
        } else if (a == "--build" && needValue("--build")) {
            opt.build = std::atoi(argv[++i]);
        } else if (a == "--world-build" && needValue("--world-build")) {
            opt.worldBuild = std::atoi(argv[++i]);
        } else if (a == "--logon-proto" && needValue("--logon-proto")) {
            opt.logonProto = std::atoi(argv[++i]);
        } else if (a == "--proto" && needValue("--proto")) {
            opt.proto = std::atoi(argv[++i]);
        } else if (a == "--locale" && needValue("--locale")) {
            opt.locale = argv[++i];
        } else if (a == "--integrity" && needValue("--integrity")) {
            std::string v = argv[++i];
            if (v == "file" || v == "hmac") opt.integrityMode = IntegrityMode::File;
            else if (v == "static") opt.integrityMode = IntegrityMode::Static;
            else if (v == "zero") opt.integrityMode = IntegrityMode::Zero;
            else {
                std::cerr << "Unknown --integrity value: " << v << "\n";
                return false;
            }
        } else if (a == "--misc-dir" && needValue("--misc-dir")) {
            opt.integrityDir = argv[++i];
        } else if (a == "--integrity-exe" && needValue("--integrity-exe")) {
            opt.integrityExe = argv[++i];
        } else if (a == "--realm" && needValue("--realm")) {
            opt.realmName = argv[++i];
        } else if (a == "--opcodes" && needValue("--opcodes")) {
            opt.opcodeJson = argv[++i];
        } else if (a == "--timeout-ms" && needValue("--timeout-ms")) {
            opt.timeoutMs = std::atoi(argv[++i]);
        } else if (a == "--warden-wait-ms" && needValue("--warden-wait-ms")) {
            opt.wardenWaitMs = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            return false;
        }
    }

    if (opt.readPasswordStdin) {
        std::getline(std::cin, opt.password);
        if (!opt.password.empty() && opt.password.back() == '\r') opt.password.pop_back();
        opt.havePassword = true;
    }

    if (!opt.havePassword && !opt.haveHash) {
        std::cerr << "Must supply --password, --password-stdin, or --hash\n";
        return false;
    }

    if (opt.integrityDir.empty()) {
        const char* home = std::getenv("HOME");
        opt.integrityDir = home ? std::string(home) + "/Downloads/TurtleWoW" : "Data/misc";
    }

    return true;
}

bool parseHostPort(const std::string& address, std::string& host, uint16_t& port) {
    host = address;
    port = 8085;
    const size_t colon = address.rfind(':');
    if (colon == std::string::npos) return true;
    host = address.substr(0, colon);
    try {
        int parsed = std::stoi(address.substr(colon + 1));
        if (parsed <= 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
    } catch (...) {
        return false;
    }
    return true;
}

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void appendReversedFourCC(std::vector<uint8_t>& out, const std::string& value) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    const size_t n = std::min<size_t>(4, value.size());
    for (size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<uint8_t>(value[n - 1 - i]);
    }
    out.insert(out.end(), std::begin(bytes), std::end(bytes));
}

std::vector<uint8_t> buildWowSvcsLoginRequest(const Options& opt) {
    std::string account = upperAscii(opt.account);
    if (account.size() > 255) account.resize(255);

    std::vector<uint8_t> out;
    out.reserve(4 + 30 + account.size());
    out.push_back(0x00);
    out.push_back(0x03);

    const size_t lengthOffset = out.size();
    appendU16(out, 0);

    appendReversedFourCC(out, "WoW");
    out.push_back(static_cast<uint8_t>(opt.major));
    out.push_back(static_cast<uint8_t>(opt.minor));
    out.push_back(static_cast<uint8_t>(opt.patch));
    appendU16(out, static_cast<uint16_t>(opt.build));
    appendReversedFourCC(out, "x86");
    appendReversedFourCC(out, "Win");
    appendReversedFourCC(out, opt.locale);

    const char* timezoneEnv = std::getenv("WOWEE_WOWSVCS_TIMEZONE");
    uint32_t timezone = timezoneEnv && *timezoneEnv ? static_cast<uint32_t>(std::strtoul(timezoneEnv, nullptr, 10)) : 480u;
    appendU32(out, timezone);

    appendU32(out, 0); // Client IP/connection field; official code reads it from the login socket object.
    out.push_back(static_cast<uint8_t>(account.size()));
    out.insert(out.end(), account.begin(), account.end());

    const uint16_t blockLen = static_cast<uint16_t>(out.size() - lengthOffset - 2);
    out[lengthOffset] = static_cast<uint8_t>(blockLen & 0xff);
    out[lengthOffset + 1] = static_cast<uint8_t>((blockLen >> 8) & 0xff);
    return out;
}

AuthResultData authenticate(const Options& opt) {
    AuthResultData result;

    auth::ClientInfo info;
    info.majorVersion = static_cast<uint8_t>(opt.major);
    info.minorVersion = static_cast<uint8_t>(opt.minor);
    info.patchVersion = static_cast<uint8_t>(opt.patch);
    info.build = static_cast<uint16_t>(opt.build);
    info.logonProtocolVersion = static_cast<uint8_t>(opt.logonProto);
    info.protocolVersion = static_cast<uint8_t>(opt.proto);
    info.locale = opt.locale;
    info.platform = "x86";
    info.os = "Win";

    auto sock = std::make_unique<network::TCPSocket>();
    std::unique_ptr<auth::SRP> srp;
    std::array<uint8_t, 16> checksumSalt{};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> gotRealmList{false};
    std::atomic<bool> sentRealmList{false};
    std::atomic<bool> sentWowSvcsProbe{false};
    auto proofAt = std::chrono::steady_clock::time_point{};
    const bool wowSvcsProbe = []() {
        const char* raw = std::getenv("WOWEE_WOWSVCS_PROBE");
        return raw && *raw && std::string(raw) != "0";
    }();

    auto sendProof = [&]() {
        auto A = srp->getA();
        auto M1 = srp->getM1();

        std::array<uint8_t, 20> crcHash{};
        const std::array<uint8_t, 20>* crcHashPtr = nullptr;
        if (opt.integrityMode == IntegrityMode::Zero) {
            crcHash.fill(0);
            crcHashPtr = &crcHash;
        } else if (opt.integrityMode == IntegrityMode::Static) {
            std::array<uint8_t, 20> versionHash{};
            std::string err;
            if (auth::getKnownClientVersionHash(static_cast<uint16_t>(opt.build), info.os, versionHash) &&
                auth::computeIntegrityHashFromVersionHash(A, versionHash, crcHash, err)) {
                crcHashPtr = &crcHash;
            } else {
                std::cerr << "Static integrity hash not computed"
                          << (err.empty() ? "" : (": " + err)) << "\n";
            }
        } else {
            std::string err;
            if (auth::computeIntegrityHashWin32WithExe(checksumSalt, A, opt.integrityDir,
                                                       opt.integrityExe, crcHash, err)) {
                crcHashPtr = &crcHash;
                std::cerr << "Auth: computed integrity hash from " << opt.integrityDir
                          << " (" << opt.integrityExe << ")\n";
            } else {
                std::cerr << "Auth: integrity hash not computed: " << err << "\n";
            }
        }

        sock->send(auth::LogonProofPacket::buildLegacy(A, M1, crcHashPtr));
        std::cerr << "Auth: sent LOGON_PROOF\n";
    };

    sock->setPacketCallback([&](const network::Packet& p) {
        network::Packet pkt = p;
        if (pkt.getSize() < 1) return;
        uint8_t opcode = pkt.readUInt8();

        if (sentWowSvcsProbe && opcode != static_cast<uint8_t>(auth::AuthOpcode::REALM_LIST)) {
            std::cerr << "WowSvcs: legacy parser saw opcode=0x" << std::hex
                      << static_cast<int>(opcode) << std::dec
                      << " while probing; raw dump above is authoritative\n";
            return;
        }

        if (opcode == static_cast<uint8_t>(auth::AuthOpcode::LOGON_CHALLENGE)) {
            auth::LogonChallengeResponse resp{};
            if (!auth::LogonChallengeResponseParser::parse(pkt, resp) || !resp.isSuccess()) {
                std::cerr << "Auth: LOGON_CHALLENGE failed\n";
                failed = true;
                done = true;
                return;
            }
            checksumSalt = resp.checksumSalt;

            srp = std::make_unique<auth::SRP>();
            if (opt.haveHash) srp->initializeWithHash(opt.account, opt.authHash);
            else srp->initialize(opt.account, opt.password);
            srp->feed(resp.B, resp.g, resp.N, resp.salt);
            sendProof();
            return;
        }

        if (opcode == static_cast<uint8_t>(auth::AuthOpcode::LOGON_PROOF)) {
            auth::LogonProofResponse resp{};
            if (!auth::LogonProofResponseParser::parse(pkt, resp) || !resp.isSuccess()) {
                std::cerr << "Auth: LOGON_PROOF failed\n";
                failed = true;
                done = true;
                return;
            }
            if (!srp->verifyServerProof(resp.M2)) {
                std::cerr << "Auth: server proof verification failed\n";
                failed = true;
                done = true;
                return;
            }
            result.sessionKey = srp->getSessionKey();
            std::cerr << "Auth: proof success, sessionKey=" << result.sessionKey.size() << " bytes\n";
            if (wowSvcsProbe) {
                sock->sendRaw(buildWowSvcsLoginRequest(opt));
                proofAt = std::chrono::steady_clock::now();
                sentWowSvcsProbe = true;
            } else {
                sock->send(auth::RealmListPacket::build());
                sentRealmList = true;
            }
            return;
        }

        if (opcode == static_cast<uint8_t>(auth::AuthOpcode::REALM_LIST)) {
            if (!auth::RealmListResponseParser::parse(pkt, result.realmList, info.protocolVersion)) {
                std::cerr << "Auth: REALM_LIST parse failed\n";
                failed = true;
            } else {
                gotRealmList = true;
                std::cerr << "Auth: realm list has " << result.realmList.realms.size() << " realms\n";
            }
            done = true;
            return;
        }
    });

    if (!sock->connect(opt.authHost, static_cast<uint16_t>(opt.authPort))) {
        std::cerr << "Auth: connect failed\n";
        return result;
    }

    sock->send(auth::LogonChallengePacket::build(opt.account, info));

    const auto start = std::chrono::steady_clock::now();
    while (!done) {
        sock->update();
        if (wowSvcsProbe && sentWowSvcsProbe && !sentRealmList &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - proofAt).count() > 1500) {
            std::cerr << "WowSvcs: requesting legacy realm list after raw probe window\n";
            sock->send(auth::RealmListPacket::build());
            sentRealmList = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!sock->isConnected()) {
            std::cerr << "Auth: disconnected\n";
            failed = true;
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > opt.timeoutMs) {
            std::cerr << "Auth: timeout\n";
            failed = true;
            break;
        }
    }

    result.ok = !failed && gotRealmList && result.sessionKey.size() == 40;
    if (result.ok) {
        sock->setPacketCallback({});
        result.authSocket = std::move(sock);
    } else {
        sock->disconnect();
    }
    return result;
}

int probeWorld(const Options& opt, const auth::Realm& realm, const std::vector<uint8_t>& sessionKey) {
    std::string host;
    uint16_t port = 0;
    if (!parseHostPort(realm.address, host, port)) {
        std::cerr << "World: invalid realm address: " << realm.address << "\n";
        return 10;
    }

    game::OpcodeTable opcodeTable;
    if (!opcodeTable.loadFromJson(opt.opcodeJson)) {
        std::cerr << "World: failed to load opcodes: " << opt.opcodeJson << "\n";
        return 11;
    }
    game::setActiveOpcodeTable(&opcodeTable);
    game::PacketParsers parsers;
    game::ClassicPacketParsers classicParsers;

    network::WorldSocket sock;
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> gotChallenge{false};
    std::atomic<bool> gotAuthResponse{false};
    std::atomic<bool> gotChars{false};
    auto gotCharsAt = std::chrono::steady_clock::time_point{};
    game::CharEnumResponse charResponse;
    std::unique_ptr<game::WardenCrypto> wardenCrypto;
    std::shared_ptr<game::WardenModule> wardenModule;
    std::vector<uint8_t> wardenModuleHash;
    std::vector<uint8_t> wardenModuleKey;
    std::vector<uint8_t> wardenModuleData;
    uint32_t wardenModuleSize = 0;

    const uint32_t clientSeed = 0xA1B2C3D4u;

    auto sendWardenPlain = [&](const std::vector<uint8_t>& plain) {
        if (!wardenCrypto) return;
        auto encrypted = wardenCrypto->encrypt(plain);
        network::Packet response(opcodeTable.toWire(game::LogicalOpcode::CMSG_WARDEN_DATA));
        for (uint8_t b : encrypted) response.writeUInt8(b);
        sock.send(response);
        std::cerr << "Warden: TX plain=" << plain.size() << "\n";
    };

    auto handleWarden = [&](network::Packet& pkt) {
        if (!wardenCrypto) {
            wardenCrypto = std::make_unique<game::WardenCrypto>();
            if (!wardenCrypto->initFromSessionKey(sessionKey)) {
                std::cerr << "Warden: crypto init failed\n";
                failed = true;
                done = true;
                return;
            }
        }

        std::vector<uint8_t> dec = wardenCrypto->decrypt(pkt.getData());
        if (dec.empty()) return;
        uint8_t op = dec[0];
        std::cerr << "Warden: RX op=0x" << std::hex << static_cast<int>(op)
                  << std::dec << " size=" << dec.size() << "\n";

        if (op == game::WARDEN_SMSG_MODULE_USE) {
            if (dec.size() < 37) {
                std::cerr << "Warden: MODULE_USE too short\n";
                return;
            }
            wardenModuleHash.assign(dec.begin() + 1, dec.begin() + 17);
            wardenModuleKey.assign(dec.begin() + 17, dec.begin() + 33);
            wardenModuleSize = static_cast<uint32_t>(dec[33])
                             | (static_cast<uint32_t>(dec[34]) << 8)
                             | (static_cast<uint32_t>(dec[35]) << 16)
                             | (static_cast<uint32_t>(dec[36]) << 24);
            wardenModuleData.clear();
            std::cerr << "Warden: MODULE_USE size=" << wardenModuleSize << "\n";
            sendWardenPlain({game::WARDEN_CMSG_MODULE_MISSING});
            return;
        }

        if (op == game::WARDEN_SMSG_MODULE_CACHE) {
            if (dec.size() < 3) return;
            uint16_t chunkSize = static_cast<uint16_t>(dec[1]) | (static_cast<uint16_t>(dec[2]) << 8);
            if (dec.size() < 3u + chunkSize) return;
            wardenModuleData.insert(wardenModuleData.end(), dec.begin() + 3, dec.begin() + 3 + chunkSize);
            if (wardenModuleSize != 0 && wardenModuleData.size() >= wardenModuleSize) {
                std::cerr << "Warden: module complete " << wardenModuleData.size() << "\n";
                wardenModule = std::make_shared<game::WardenModule>();
                wardenModule->setCallbackDependencies(
                    wardenCrypto.get(),
                    [&](const uint8_t* data, size_t len) {
                        std::vector<uint8_t> plain(data, data + len);
                        sendWardenPlain(plain);
                    });
                wardenModule->setSessionKey(sessionKey);
                wardenModule->load(wardenModuleData, wardenModuleHash, wardenModuleKey);
                sendWardenPlain({game::WARDEN_CMSG_MODULE_OK});
            }
            return;
        }

        if (op == game::WARDEN_SMSG_HASH_REQUEST) {
            bool handled = wardenModule && wardenModule->processPacket(dec);
            std::cerr << "Warden: HASH_REQUEST handled=" << handled << "\n";
            return;
        }

        if (wardenModule) {
            bool handled = wardenModule->processPacket(dec);
            std::cerr << "Warden: module packet handled=" << handled << "\n";
        }
    };

    sock.setPacketCallback([&](const network::Packet& p) {
        network::Packet pkt = p;
        const uint16_t wireOpcode = pkt.getOpcode();
        auto logical = opcodeTable.fromWire(wireOpcode);
        std::cerr << "World: RX opcode=0x" << std::hex << wireOpcode << std::dec
                  << " logical=" << (logical ? game::OpcodeTable::logicalToName(*logical) : "UNKNOWN")
                  << " size=" << pkt.getSize() << "\n";

        if (!logical) return;

        if (*logical == game::LogicalOpcode::SMSG_WARDEN_DATA) {
            handleWarden(pkt);
            return;
        }

        if (*logical == game::LogicalOpcode::SMSG_AUTH_CHALLENGE) {
            gotChallenge = true;
            game::AuthChallengeData challenge{};
            if (!game::AuthChallengeParser::parse(pkt, challenge)) {
                std::cerr << "World: SMSG_AUTH_CHALLENGE parse failed\n";
                failed = true;
                done = true;
                return;
            }

            auto authSession = game::AuthSessionPacket::build(
                static_cast<uint32_t>(opt.worldBuild),
                upperAscii(opt.account),
                clientSeed,
                sessionKey,
                challenge.serverSeed,
                realm.id);
            sock.send(authSession);
            sock.initEncryption(sessionKey, static_cast<uint32_t>(opt.worldBuild));
            std::cerr << "World: sent CMSG_AUTH_SESSION build=" << opt.worldBuild
                      << " realmId=" << static_cast<int>(realm.id) << "\n";
            return;
        }

        if (*logical == game::LogicalOpcode::SMSG_AUTH_RESPONSE) {
            gotAuthResponse = true;
            game::AuthResponseData response{};
            if (!game::AuthResponseParser::parse(pkt, response) || !response.isSuccess()) {
                std::cerr << "World: auth response failed: "
                          << game::getAuthResultString(response.result) << "\n";
                failed = true;
                done = true;
                return;
            }

            std::cerr << "World: auth response OK, requesting character list\n";
            sock.send(game::CharEnumPacket::build());
            return;
        }

        if (*logical == game::LogicalOpcode::SMSG_CHAR_ENUM) {
            if (!classicParsers.parseCharEnum(pkt, charResponse)) {
                std::cerr << "World: SMSG_CHAR_ENUM parse failed\n";
                failed = true;
            } else {
                gotChars = true;
                gotCharsAt = std::chrono::steady_clock::now();
                std::cerr << "World: character list received: "
                          << charResponse.characters.size() << " characters\n";
                for (size_t i = 0; i < charResponse.characters.size(); ++i) {
                    const auto& c = charResponse.characters[i];
                    std::cout << "CHAR[" << i << "]"
                              << " name=" << c.name
                              << " guid=0x" << std::hex << c.guid << std::dec
                              << " level=" << static_cast<int>(c.level)
                              << " race=" << game::getRaceName(c.race)
                              << " class=" << game::getClassName(c.characterClass)
                              << " map=" << c.mapId
                              << " zone=" << c.zoneId
                              << " pos=(" << c.x << "," << c.y << "," << c.z << ")"
                              << " guild=" << c.guildId
                              << " flags=0x" << std::hex << c.flags << std::dec
                              << "\n";
                }
            }
            if (opt.wardenWaitMs <= 0) done = true;
            return;
        }
    });

    std::cerr << "World: connecting to " << realm.name << " @ " << host << ":" << port << "\n";
    if (!sock.connect(host, port)) {
        std::cerr << "World: connect failed\n";
        return 12;
    }
    sock.tracePacketsFor(std::chrono::seconds(10), "world_char_probe");

    const auto start = std::chrono::steady_clock::now();
    while (!done) {
        sock.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!sock.isConnected()) {
            std::cerr << "World: disconnected"
                      << " gotChallenge=" << gotChallenge
                      << " gotAuthResponse=" << gotAuthResponse
                      << " gotChars=" << gotChars << "\n";
            failed = true;
            break;
        }
        if (gotChars && opt.wardenWaitMs > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - gotCharsAt).count() > opt.wardenWaitMs) {
            std::cerr << "World: warden wait complete, still connected\n";
            done = true;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > opt.timeoutMs) {
            std::cerr << "World: timeout"
                      << " gotChallenge=" << gotChallenge
                      << " gotAuthResponse=" << gotAuthResponse
                      << " gotChars=" << gotChars << "\n";
            failed = true;
            break;
        }
    }

    sock.disconnect();
    return (!failed && gotChars) ? 0 : 13;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    AuthResultData authResult = authenticate(opt);
    if (!authResult.ok) return 3;

    const auth::Realm* selected = nullptr;
    for (const auto& realm : authResult.realmList.realms) {
        std::cerr << "Realm: " << realm.name << " @ " << realm.address
                  << " id=" << static_cast<int>(realm.id)
                  << " build=" << realm.build << "\n";
        if (!selected && (opt.realmName.empty() || realm.name == opt.realmName)) {
            selected = &realm;
        }
    }

    if (!selected) {
        std::cerr << "No matching realm found: " << opt.realmName << "\n";
        return 4;
    }

    int rc = probeWorld(opt, *selected, authResult.sessionKey);
    if (authResult.authSocket) {
        authResult.authSocket->disconnect();
    }
    return rc;
}
