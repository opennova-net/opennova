#pragma once

#include <net/protocol/protocol.h>

namespace opennova::games {

// NovaLogic's 2009 generation, which Jointops.exe and dfx2.exe both build from: one NAPI port,
// one protocol revision (PV2 "16") and one set of 0x42 build values (generation.cpp cites each
// in both images).
const Generation &novalogic_2009();

} // namespace opennova::games
