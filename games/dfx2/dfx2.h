#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>
#include <runtime/world/title_rules.h>

namespace opennova::games {

// Delta Force: Xtreme 2 (dfx2.exe, the DFX2 install's): JO's code over its own data
// (docs/titles/dfx2-vs-jo-re.md §6, DFX2-3). The title (dfx2_title.cpp), the protocol it
// speaks (dfx2_protocol.cpp) and the rules it runs (dfx2_rules.cpp).
const Title &dfx2_title();
const Protocol &dfx2_protocol();
const world::TitleRules &dfx2_rules();

} // namespace opennova::games
