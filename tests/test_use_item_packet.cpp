// CMSG_USE_ITEM SpellCastTargets encoding across expansions.
//
// Items that enchant another item (sharpening stones, weightstones, weapon oils)
// must send TARGET_FLAG_ITEM plus the target item's packed GUID; the server drops
// the cast otherwise.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/packet_parsers.hpp"
#include "core/application.hpp"

// The packet builders live in translation units that inline isActiveExpansion(),
// which reaches through the Application singleton. The builders under test never
// call it, so a null instance is enough to satisfy the linker.
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

// Packet::getData() is the body only — the opcode is held separately — so these
// offsets are into the CMSG_USE_ITEM payload.
using Bytes = std::vector<uint8_t>;

TEST_CASE("CMSG_USE_ITEM sends TARGET_FLAG_ITEM for item-targeted spells", "[use_item]") {
    constexpr uint64_t kItemGuid   = 0x0000000000001234ull;  // the stone
    constexpr uint64_t kTargetGuid = 0x0000000000005678ull;  // the weapon it sharpens
    constexpr uint32_t kSpellId    = 2828;                   // Sharpen Blade

    SECTION("WotLK: uint32 mask 0x10 + packed target GUID") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) castCount(1) spellId(4) itemGuid(8) glyph(4) castFlags(1) = 20
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x10);
        // Packed GUID: byte mask then the non-zero bytes, little end first.
        REQUIRE(targets[4] == 0x03);   // bytes 0 and 1 present (0x78, 0x56)
        REQUIRE(targets[5] == 0x78);
        REQUIRE(targets[6] == 0x56);
    }

    SECTION("Classic: uint16 mask 0x10 + packed target GUID") {
        ClassicPacketParsers parsers;
        auto packet = parsers.buildUseItem(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) spellIndex(1) = 3
        REQUIRE(bytes.size() > 3);
        const uint8_t* targets = bytes.data() + 3;
        uint16_t mask = static_cast<uint16_t>(targets[0] | (targets[1] << 8));
        REQUIRE(mask == 0x10);
        REQUIRE(targets[2] == 0x03);
        REQUIRE(targets[3] == 0x78);
        REQUIRE(targets[4] == 0x56);
    }

    SECTION("TBC: uint32 mask 0x10 + packed target GUID") {
        TbcPacketParsers parsers;
        auto packet = parsers.buildUseItem(0xFF, 23, kItemGuid, kSpellId, 0, kTargetGuid);
        const Bytes& bytes = packet.getData();

        // bag(1) slot(1) spellIndex(1) castCount(1) itemGuid(8) = 12
        REQUIRE(bytes.size() > 12);
        const uint8_t* targets = bytes.data() + 12;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x10);
        REQUIRE(targets[4] == 0x03);
        REQUIRE(targets[5] == 0x78);
        REQUIRE(targets[6] == 0x56);
    }
}

TEST_CASE("CMSG_USE_ITEM keeps unit/self targeting when no item target", "[use_item]") {
    constexpr uint64_t kItemGuid   = 0x0000000000001234ull;
    constexpr uint64_t kPlayerGuid = 0x0000000000000042ull;

    SECTION("unit target still uses TARGET_FLAG_UNIT") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, 1234, kPlayerGuid, 0);
        const Bytes& bytes = packet.getData();
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x02);
    }

    SECTION("no target still uses TARGET_FLAG_SELF") {
        auto packet = UseItemPacket::build(0xFF, 23, kItemGuid, 1234, 0, 0);
        const Bytes& bytes = packet.getData();
        REQUIRE(bytes.size() > 20);
        const uint8_t* targets = bytes.data() + 20;
        uint32_t mask = targets[0] | (targets[1] << 8) | (targets[2] << 16) | (targets[3] << 24);
        REQUIRE(mask == 0x00);
    }
}
