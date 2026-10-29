#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>
#include <runtime/world/title_rules.h>

namespace opennova::games {

// OP: Acid Gambit, OpenNova's own game: the title (acidgambit_title.cpp), the protocol it
// speaks (acidgambit_protocol.cpp) and the rules it runs (acidgambit_rules.cpp), Joint
// Operations' until the game customizes them.
const Title &acidgambit_title();
const Protocol &acidgambit_protocol();
const world::TitleRules &acidgambit_rules();

} // namespace opennova::games
