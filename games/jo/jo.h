#pragma once

#include <base/title/title.h>
#include <net/protocol/protocol.h>

namespace opennova::games {

// Joint Operations: Typhoon Rising, the JO:CA Jointops.exe (docs/engine-primer.md's IDB): the
// title (jo_title.cpp) and the protocol it speaks (jo_protocol.cpp).
const Title &jo_title();
const Protocol &jo_protocol();

} // namespace opennova::games
