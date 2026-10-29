#include "acidgambit.h"

#include <games/jo/jo.h>

namespace opennova::games {

// OP: Acid Gambit speaks Joint Operations' protocol: the same object, so a JOINTOPERATIONS peer
// resolves to the one protocol and a retail JO host or joiner meets it as JO. A customization
// gives the game a Protocol of its own here, with its own code, name and protocol version.
const Protocol &acidgambit_protocol() {
	return jo_protocol();
}

} // namespace opennova::games
