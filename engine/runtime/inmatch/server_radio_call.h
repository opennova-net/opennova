#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/emote_wire.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_connection.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

struct NapiNPServerCtx;

// One row of the radio-call rule table: the RAD_ key it matches (the key
// builder's flags-6 form, no body prefix), the recipient rule type, the range
// in whole units (0 = unlimited), and the designation it leaves (seconds and
// mode; mode 0 = none).
// [orig: g_RadioCallRules @0x840C20 (ex off_840C20), 20-byte rows {char *key,
//  int type, int range, int seconds, u8 mode}]
struct RadioCallRule {
	const char *key = "";
	int32_t type = 0;
	int32_t range_units = 0;
	int32_t designation_seconds = 0;
	uint8_t designation_mode = 0;
};

// The rule whose key matches `key` (ASCII case-insensitive), or null. The
// walk ends at the first row whose type is 0 (RAD_1), so the table's MP_*,
// class and remaining vehicle rows after it are never compared.
// [orig: RadioCall_FindRuleByKey @0x5BFE50 (ex Terrain_FindSectorEntryByName)
//  — the key sub_5BFB00(buf, call, 6, entity) @0x5bfe7e, the row-0 type gate
//  @0x5bfe86, the stricmp walk @0x5bfe98..0x5bfeb9]
const RadioCallRule *radio_call_find_rule(const std::string &key);

// The host side of a Radio-menu pick (C2S 0x13 -> S2C 0x6D). The sender must
// hold a live, non-spectator slot whose player is alive and whose radio
// cooldown has run out. The call's RAD_ key selects a rule; the sender's
// radio latch clears (call 6 sets it for 30 seconds); the body is {call, the
// sender's pool-0 index, the nearest 2044 location or -1}. With no rule the
// body goes to the sender's team (mask 0x180); with one it goes, unicast, to
// every active same-team slot in NetPlayer state 10 that the rule type admits
// (the sender always), and a mode-3 rule registers a designation at the
// sender's aim point. The cooldown re-arms to 4 seconds. The sender's own
// copy is returned as the reply (the dispatcher frames it with the request);
// every other recipient is staged on its transport, unreliable (msgClass 0).
// Null `ctx` (the World-less unit path) consumes the message without acting.
// [orig: NapiNPServerMsg_HandleRadioCall @0x514330 (ex HandleSectorAction)]
std::vector<ProtocolMessage> Server_HandleRadioCall(NapiNPServerCtx *ctx,
		NapiNPConnection &sender, const RadioCallRequest &request,
		std::vector<NapiNPConnection> &roster, world::World &world);

} // namespace opennova::inmatch
