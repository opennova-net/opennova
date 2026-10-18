#include "saved_machine.h"

#include "hosted/agent_protocol.h"

#include <base/io/json.h>

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

namespace {

bool is_secret_text(const std::string &text) {
	if (text.size() < 32 || text.size() > 128)
		return false;
	for (const char c : text)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	return true;
}

} // namespace

std::string encode_saved_machine(const SavedMachine &saved) {
	io::JsonValue v = io::JsonValue::make_object();
	v.set("machine", io::json_number(static_cast<double>(saved.machine_id)));
	v.set("agent_secret", io::json_string(saved.agent_secret));
	v.set("serve_image", io::json_string(saved.serve_image));
	v.set("slots", io::json_number(saved.slots));
	return io::json_write(v);
}

bool decode_saved_machine(std::string_view text, SavedMachine &out, std::string &error) {
	io::JsonValue v;
	std::string why;
	if (!io::json_parse(text, v, why) || !v.is_object()) {
		error = "not a JSON object";
		return false;
	}
	if (!io::json_members_known(
				v, { "machine", "agent_secret", "serve_image", "slots" }, "saved machine", error))
		return false;
	const io::JsonValue *machine = v.get("machine");
	int64_t id = 0;
	if (!machine || !io::json_exact_whole_in(*machine, 1, io::kJsonSafeWholeMax, id)) {
		error = "no machine id";
		return false;
	}
	const io::JsonValue *secret = v.get("agent_secret");
	if (!secret || !secret->is_string() || !is_secret_text(secret->string)) {
		error = "agent_secret is not 32..128 letters and digits";
		return false;
	}
	const io::JsonValue *image = v.get("serve_image");
	if (!image || !image->is_string() || !hosted::is_image_reference(image->string)) {
		error = "serve_image is no image reference";
		return false;
	}
	const io::JsonValue *slots = v.get("slots");
	int64_t count = 0;
	if (!slots || !io::json_exact_whole_in(*slots, 1, hosted::kMaxSlots, count)) {
		error = "slots is not 1.." + std::to_string(hosted::kMaxSlots);
		return false;
	}
	out.machine_id = id;
	out.agent_secret = secret->string;
	out.serve_image = image->string;
	out.slots = static_cast<int>(count);
	return true;
}

} // namespace opennova::hostd
