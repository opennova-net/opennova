#include "acidgambit.h"

#include <games/jo/jo.h>
#include <runtime/world/title_rules.h>

namespace opennova::games {

// OP: Acid Gambit runs Joint Operations' rules: the same object, as it speaks Joint Operations'
// protocol (acidgambit_protocol.cpp). A customization gives the game a TitleRules of its own here;
// one that changes a joiner-side knob (runtime/world/title_rules.h) also gives it a Protocol of its
// own with its own protocol version (games/CLAUDE.md).
const world::TitleRules &acidgambit_rules() {
	return jo_rules();
}

} // namespace opennova::games
