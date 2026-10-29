#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::games {

// OP: Acid Gambit, OpenNova's own game: the title (acidgambit_title.cpp) and the protocol it
// speaks (acidgambit_protocol.cpp), Joint Operations' until the game customizes it.
const Title &acidgambit_title();
const Protocol &acidgambit_protocol();

} // namespace opennova::games
