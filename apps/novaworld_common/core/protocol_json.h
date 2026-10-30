#pragma once

// A protocol as the service's routes and the downloads manifest answer one (CONTEXT.md
// "Protocol"): {code ("jo"), name ("JO"), version (its protocol version, PV2: "16"; null where
// unwitnessed)}, which the site and the editor show as "JO protocol 16"
// (base/version/version_line.h protocol_words); null for none.

#include <base/io/json.h>
#include <net/protocol/protocol.h>

namespace opennova::novaworld_server {

inline io::JsonValue protocol_json(const Protocol *protocol) {
	if (protocol == nullptr || protocol->code == nullptr)
		return io::JsonValue::make_null();
	io::JsonValue out = io::JsonValue::make_object();
	out.set("code", io::json_string(protocol->code));
	out.set("name", io::json_string(protocol->name != nullptr ? protocol->name : protocol->code));
	const char *version = protocol_version(*protocol);
	out.set("version", version != nullptr ? io::json_string(version) : io::JsonValue::make_null());
	return out;
}

} // namespace opennova::novaworld_server
