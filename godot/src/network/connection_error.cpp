#include "network/connection_error.h"

#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

namespace godot {

Ref<ConnectionError> ConnectionError::make(const opennova::inmatch::ConnectionErrorRecord &p_record) {
	Ref<ConnectionError> out;
	out.instantiate();
	out->record_ = p_record;
	return out;
}

Ref<ConnectionError> ConnectionError::make_absent() {
	Ref<ConnectionError> out;
	out.instantiate();
	out->absent_ = true;
	return out;
}

Ref<ConnectionError> ConnectionError::from_fields(int p_connect_error, int p_connect_param,
		const String &p_connect_text, int p_disconnect_code, int p_disconnect_param,
		const String &p_disconnect_text) {
	opennova::inmatch::ConnectionErrorRecord record;
	record.connect_error = static_cast<uint32_t>(p_connect_error);
	record.connect_param = static_cast<uint32_t>(p_connect_param);
	record.connect_text = opennova::to_std(p_connect_text);
	record.disconnect_code = static_cast<uint32_t>(p_disconnect_code);
	record.disconnect_param = static_cast<uint32_t>(p_disconnect_param);
	record.disconnect_text = opennova::to_std(p_disconnect_text);
	return make(record);
}

bool ConnectionError::is_set() const {
	return absent_ || record_.connect_error != 0 || record_.disconnect_code != 0;
}

String ConnectionError::reason_key() const {
	if (!is_set()) return String();
	return String(
			opennova::inmatch::disconnect_reason_key(absent_ ? nullptr : &record_).key.c_str());
}

String ConnectionError::reason_text(const Ref<RtxtStringFile> &p_gameerr) const {
	const std::shared_ptr<const opennova::rtxt::File> override_table = opennova::rtxt::text_override();
	return opennova::cp1252_to_gd(opennova::inmatch::disconnect_reason_string(
			absent_ ? nullptr : &record_, override_table.get(),
			p_gameerr.is_valid() ? &p_gameerr->get_native() : nullptr));
}

void ConnectionError::_bind_methods() {
	ClassDB::bind_static_method("ConnectionError",
			D_METHOD("from_fields", "connect_error", "connect_param", "connect_text",
					"disconnect_code", "disconnect_param", "disconnect_text"),
			&ConnectionError::from_fields);
	ClassDB::bind_method(D_METHOD("is_set"), &ConnectionError::is_set);
	ClassDB::bind_method(D_METHOD("get_connect_error"), &ConnectionError::get_connect_error);
	ClassDB::bind_method(D_METHOD("get_connect_param"), &ConnectionError::get_connect_param);
	ClassDB::bind_method(D_METHOD("get_disconnect_code"), &ConnectionError::get_disconnect_code);
	ClassDB::bind_method(D_METHOD("get_disconnect_param"), &ConnectionError::get_disconnect_param);
	ClassDB::bind_method(D_METHOD("reason_key"), &ConnectionError::reason_key);
	ClassDB::bind_method(D_METHOD("reason_text", "gameerr"),
			&ConnectionError::reason_text);
}

} // namespace godot
