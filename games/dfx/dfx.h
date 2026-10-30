#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::world {
struct TitleRules; // runtime/world/title_rules.h (opennova_games_rules)
}

namespace opennova::games {

// Delta Force: Xtreme (DFX.EXE, the project's IDB's), unwitnessed: the title (dfx_title.cpp),
// its stub protocol, of which no identity is witnessed (only its engine build was read), so its
// session and web flow are refused by name (dfx_protocol.cpp), and the rules it runs, Joint
// Operations' (dfx_rules.cpp).
const Title &dfx_title();
const Protocol &dfx_protocol();
const world::TitleRules &dfx_rules();

} // namespace opennova::games
