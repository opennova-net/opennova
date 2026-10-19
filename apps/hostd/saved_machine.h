#pragma once

// What opennova-hostd keeps of its bootstrap across its restarts: the
// machine's id, the agent secret the bootstrap handed it, the serve image and
// the machine's slots, saved owner-only at <state dir>/machine-<id>.json
// (agent.h). The file is the agent's own format, never the bootstrap reply's:
// it names no protocol, so an agent of a later protocol reads what an earlier
// one saved, and a protocol bump never strands a machine whose one-time token
// was spent (the service holds an agent of another protocol, agent_protocol.h,
// and the restarted agent still syncs with its secret). Service policy, not a
// port.
//
//   {"machine": <id>, "agent_secret": "<32..128 letters and digits>",
//    "serve_image": "<image reference>", "slots": <1..kMaxSlots>}

#include <cstdint>
#include <string>
#include <string_view>

namespace opennova::hostd {

struct SavedMachine {
	int64_t machine_id = 0;
	std::string agent_secret;
	std::string serve_image;
	int slots = 0;
};

std::string encode_saved_machine(const SavedMachine &saved);
// A strict read: false (with `error`, never quoting the secret) for anything
// but the members above, each in its shape.
bool decode_saved_machine(std::string_view text, SavedMachine &out, std::string &error);

} // namespace opennova::hostd
