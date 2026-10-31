#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::world {
struct TitleRules; // runtime/world/title_rules.h (opennova_games_rules)
}

namespace opennova::games {

// Delta Force: Black Hawk Down, unwitnessed: the title (bhd_title.cpp), its stub protocol, neither
// read from a binary (no program, no identity), so its session and web flow are refused by name
// (bhd_protocol.cpp), and the rules it runs, Joint Operations' (bhd_rules.cpp).
const Title &bhd_title();
const Protocol &bhd_protocol();
const world::TitleRules &bhd_rules();

} // namespace opennova::games
