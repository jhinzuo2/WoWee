#pragma once

// Pure spell-classification rules, kept free of GameHandler so they can be tested
// directly. Every decision here is driven by Spell.dbc / SpellRange.dbc data rather
// than by hardcoded spell ids: classifying by school instead once cost us Battle Shout
// (a physical-school self-buff read as melee) and every Hunter shot past 8 yards.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace wowee {
namespace game {
namespace spellclass {

/// SpellRange calls melee "Combat Range", and every melee ability resolves to it.
inline constexpr float kCombatRangeYards = 5.0f;

/// Returned when SpellRange.dbc was unavailable — nothing may be inferred from it.
inline constexpr float kUnknownRange = -1.0f;

/// "Self Only" range: the spell cannot reach another unit, so it lands on the caster
/// no matter what is targeted (shouts, self-buffs, hearthstone).
inline bool isSelfCastRange(float maxRange) {
    return maxRange == 0.0f;
}

/// A melee ability, i.e. one cast at Combat Range. Ranged physical abilities such as
/// Steady Shot or Taunt carry 30-35 yards and are deliberately excluded. An unknown
/// range is not melee: the server should arbitrate rather than the client blocking a
/// legitimate cast.
inline bool isMeleeRange(float maxRange) {
    return maxRange > 0.0f && maxRange <= kCombatRangeYards;
}

/// Legacy client IDs for the three repeating ranged weapon attacks.
///
/// Their Spell.dbc rows carry a dummy one-point rage cost even for classes that do
/// not use rage. The server drives their real weapon/ammunition readiness, so UI
/// resource checks must not treat that placeholder as a cast cost.
inline bool isRangedWeaponAutoAttack(uint32_t spellId) {
    return spellId == 75 ||    // Auto Shot
           spellId == 5019 ||  // Shoot (wand)
           spellId == 2764;    // Throw
}

/// Spell.dbc stores the rank as a display string ("Rank 3"). Rankless spells sort as 0.
inline int rankValue(const std::string& rank) {
    int value = 0;
    bool sawDigit = false;
    for (char c : rank) {
        if (c >= '0' && c <= '9') {
            sawDigit = true;
            value = value * 10 + (c - '0');
        } else if (sawDigit) {
            break;  // stop at the first non-digit after the number
        }
    }
    return value;
}

/// Name and rank as they appear in Spell.dbc.
struct SpellRankInfo {
    std::string name;
    std::string rank;
};

/// Maps a superseded spell rank onto the highest rank the player actually knows.
///
/// Action bars restored from the server can still hold a rank that a higher rank has
/// since superseded. The server drops casts of superseded ranks without sending any
/// error at all, so the client has to correct them itself.
///
/// Returns spellId unchanged when it is already known, or when no known spell shares
/// its name (item, gather and vehicle spells are absent from the known list too, and
/// must be left alone).
///
/// `lookup` resolves a spell id to its Spell.dbc name/rank, or null if unknown.
inline uint32_t resolveHighestKnownRank(
        uint32_t spellId,
        const std::unordered_set<uint32_t>& knownSpells,
        const std::function<const SpellRankInfo*(uint32_t)>& lookup) {
    if (spellId == 0 || knownSpells.count(spellId) > 0) return spellId;

    const SpellRankInfo* info = lookup(spellId);
    if (!info || info->name.empty()) return spellId;

    uint32_t best = 0;
    int bestRank = -1;
    for (uint32_t known : knownSpells) {
        const SpellRankInfo* candidate = lookup(known);
        if (!candidate || candidate->name != info->name) continue;
        const int rank = rankValue(candidate->rank);
        if (rank > bestRank) {
            bestRank = rank;
            best = known;
        }
    }
    return best != 0 ? best : spellId;
}

} // namespace spellclass
} // namespace game
} // namespace wowee
