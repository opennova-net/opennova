#include "bhd.h"

#include <games/jo/jo.h>
#include <runtime/world/title_rules.h>

namespace opennova::games {

// Unwitnessed: no binary of the title was read for its in-match rules (the DFX2 catalog's sites,
// runtime/world/title_rules.h), so it runs Joint Operations' rules, the same object, as its
// session is refused by name before any rule runs.
const world::TitleRules &bhd_rules() {
	return jo_rules();
}

} // namespace opennova::games
