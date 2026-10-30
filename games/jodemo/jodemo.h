#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::world {
struct TitleRules; // runtime/world/title_rules.h (opennova_games_rules)
}

namespace opennova::games {

// The Joint Operations demo (jodemo.exe, the demo install's), unwitnessed: the title
// (jodemo_title.cpp), its stub protocol, whose gate tag alone is witnessed, so its session and
// web flow are refused by name (jodemo_protocol.cpp), and the rules it runs, Joint Operations'
// (jodemo_rules.cpp).
const Title &jodemo_title();
const Protocol &jodemo_protocol();
const world::TitleRules &jodemo_rules();

} // namespace opennova::games
