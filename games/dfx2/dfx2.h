#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::world {
struct TitleRules; // runtime/world/title_rules.h (opennova_games_rules)
}

namespace opennova::games {

// Delta Force: Xtreme 2 (dfx2.exe, the DFX2 install's): JO's code over its own data
// (docs/titles/dfx2-vs-jo-re.md §6, DFX2-3). The title (dfx2_title.cpp), the protocol it
// speaks (dfx2_protocol.cpp) and the rules it runs (dfx2_rules.cpp).
const Title &dfx2_title();
const Protocol &dfx2_protocol();
const world::TitleRules &dfx2_rules();

// The single-player kit page dfx2.exe compiles in (0x832368), null-terminated: one table two sites
// copy, a fresh profile's page (Title::profile_single_player_kit, DFX2-29) and the profile-less
// spawn's (TitleRules::spawn_kit, DFX2-33). Each axis's row points at it, neither at the other's
// row (dfx2_title.cpp holds it).
const char *const *dfx2_single_player_kit();

} // namespace opennova::games
