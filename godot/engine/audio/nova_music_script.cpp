// MUS interactive-music script wrapper. The underlying parser is libs/mus
// (mus_open_memory). Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.

#include "nova_music_script.h"

#include "nova_sbf_bank.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace godot;

NovaMusicScript::NovaMusicScript() {
	std::memset(&_mf, 0, sizeof(_mf));
}

NovaMusicScript::~NovaMusicScript() {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
}

void NovaMusicScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_script_count"), &NovaMusicScript::get_script_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaMusicScript::get_source_path);
	ClassDB::bind_method(D_METHOD("get_default_script_name"), &NovaMusicScript::get_default_script_name);
	ClassDB::bind_method(D_METHOD("get_scripts"), &NovaMusicScript::get_scripts);
	ClassDB::bind_method(D_METHOD("has_script", "name"), &NovaMusicScript::has_script);
	ClassDB::bind_method(D_METHOD("get_script_names"), &NovaMusicScript::get_script_names);
	ClassDB::bind_method(D_METHOD("get_section_names", "script_name"), &NovaMusicScript::get_section_names);
	ClassDB::bind_method(D_METHOD("get_intrinsic_names"), &NovaMusicScript::get_intrinsic_names);
	ClassDB::bind_method(D_METHOD("get_decompiled_text", "script_name"), &NovaMusicScript::get_decompiled_text);
	ClassDB::bind_method(D_METHOD("get_decompiled_text_with_bank", "script_name", "bank"), &NovaMusicScript::get_decompiled_text_with_bank);
	ClassDB::bind_method(D_METHOD("get_section_model", "script_name"), &NovaMusicScript::get_section_model);
	ClassDB::bind_method(D_METHOD("compile_text", "text"), &NovaMusicScript::compile_text);
	ClassDB::bind_method(D_METHOD("set_compiled_bytecode", "bytecode"), &NovaMusicScript::set_compiled_bytecode);
	ClassDB::bind_method(D_METHOD("set_compiled_file_bytes", "file_bytes"), &NovaMusicScript::set_compiled_file_bytes);
	ClassDB::bind_method(D_METHOD("load_from_decrypted_bytes", "bytes", "source"),
			&NovaMusicScript::load_from_decrypted_bytes);
	ClassDB::bind_method(D_METHOD("get_raw_file_bytes"), &NovaMusicScript::get_raw_file_bytes);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path",
								   PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT),
			"", "get_source_path");
}

void NovaMusicScript::load_from_decrypted_bytes(const PackedByteArray &bytes, const String &p_source) {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
	source_path = p_source;
	_file_bytes = PackedByteArray();

	if (bytes.size() <= 0) {
		UtilityFunctions::printerr("NovaMusicScript: empty bytes for ", p_source);
		return;
	}
	if (mus_open_memory(&_mf, bytes.ptr(), (size_t)bytes.size()) != 0) {
		UtilityFunctions::printerr("NovaMusicScript: mus_open_memory failed for ", p_source);
		return;
	}
	_opened = true;
	_file_bytes = bytes;
}

int NovaMusicScript::get_script_count() const {
	return _opened ? (int)_mf.header.chunk_count : 0;
}

String NovaMusicScript::get_default_script_name() const {
	if (!_opened || _mf.header.chunk_count == 0 || _mf.scripts == nullptr) {
		return String();
	}
	// MusScript.name is null-padded to MUS_NAME_SIZE.
	char buf[MUS_NAME_SIZE + 1] = { 0 };
	std::memcpy(buf, _mf.scripts[0].name, MUS_NAME_SIZE);
	return String(buf);
}

Array NovaMusicScript::get_scripts() const {
	Array out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		const MusScript &s = _mf.scripts[i];
		char name_buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(name_buf, s.name, MUS_NAME_SIZE);

		Array section_names;
		for (uint32_t j = 0; j < s.section_count; ++j) {
			char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			std::memcpy(sec_buf, s.sections[j].name, MUS_SECTION_NAME_SIZE);
			section_names.append(String(sec_buf));
		}

		Dictionary d;
		d["name"] = String(name_buf);
		d["code_size"] = (int64_t)s.code_size;
		d["section_count"] = (int64_t)s.section_count;
		d["sections"] = section_names;
		out.append(d);
	}
	return out;
}

bool NovaMusicScript::has_script(const StringName &p_name) const {
	if (!_opened) {
		return false;
	}
	String want = String(p_name);
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (want == String(buf)) {
			return true;
		}
	}
	return false;
}

PackedStringArray NovaMusicScript::get_script_names() const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		out.push_back(String(buf));
	}
	return out;
}

PackedStringArray NovaMusicScript::get_section_names(const StringName &p_script_name) const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	String want = String(p_script_name);
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char nbuf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(nbuf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (want != String(nbuf)) {
			continue;
		}
		const MusScript &s = _mf.scripts[i];
		for (uint32_t j = 0; j < s.section_count; ++j) {
			char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			std::memcpy(sec_buf, s.sections[j].name, MUS_SECTION_NAME_SIZE);
			out.push_back(String(sec_buf));
		}
		break;
	}
	return out;
}

PackedStringArray NovaMusicScript::get_intrinsic_names() const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.intrinsic_count && i < (uint32_t)MUS_INTRINSIC_NAMES; ++i) {
		char buf[MUS_INTRINSIC_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.intrinsic_names[i], MUS_INTRINSIC_NAME_SIZE);
		out.push_back(String(buf));
	}
	return out;
}

const MusScript *NovaMusicScript::raw_script(const String &p_name) const {
	if (!_opened) {
		return nullptr;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (p_name == String(buf)) {
			return &_mf.scripts[i];
		}
	}
	return nullptr;
}

Dictionary NovaMusicScript::compile_text(const String &p_text) {
	Dictionary out;
	out["rc"] = -1;
	out["bytecode"] = PackedByteArray();
	out["file_bytes"] = PackedByteArray();
	out["err_line"] = 0;
	out["err_col"] = 0;
	out["err_msg"] = String();

	MusScript script = {};
	int err_line = 0;
	int err_col = 0;
	const char *err_msg = nullptr;
	CharString utf8 = p_text.utf8();
	int rc = mus_compile(utf8.get_data(), &script, &err_line, &err_col, &err_msg);
	if (rc != 0) {
		out["rc"] = rc;
		out["err_line"] = err_line;
		out["err_col"] = err_col;
		out["err_msg"] = String(err_msg ? err_msg : "compile error");
		return out;
	}

	// Keep the legacy raw-code field for narrow tests, but also encode the full
	// SCR0/MU01 file so editor runs can replace script name, section table,
	// debug names, locals frame offset, and bytecode together.
	PackedByteArray bytecode;
	if (script.code != nullptr && script.code_size > 0) {
		bytecode.resize((int)script.code_size);
		std::memcpy(bytecode.ptrw(), script.code, script.code_size);
	}

	PackedByteArray file_bytes;
	const MusScript *scripts[1] = { &script };
	uint8_t *encoded = nullptr;
	size_t encoded_size = 0;
	rc = mus_encode_file(scripts, 1, &encoded, &encoded_size);
	if (rc != 0 || encoded == nullptr) {
		mus_script_free(&script);
		out["rc"] = rc != 0 ? rc : -1;
		out["err_msg"] = String("encode failed");
		return out;
	}
	file_bytes.resize((int)encoded_size);
	std::memcpy(file_bytes.ptrw(), encoded, encoded_size);
	mus_free(encoded);

	mus_script_free(&script);

	out["rc"] = 0;
	out["bytecode"] = bytecode;
	out["file_bytes"] = file_bytes;
	return out;
}

void NovaMusicScript::set_compiled_bytecode(const PackedByteArray &p_bytecode) {
	// Replaces the default script's chunk bytecode in place, then rewrites
	// _file_bytes so the saver's raw-passthrough path picks up the new bytes
	// without needing a separate re-encode hook.
	if (!_opened || _mf.scripts == nullptr || _mf.header.chunk_count == 0) {
		return;
	}
	MusScript &s = _mf.scripts[0];
	if (s.code != nullptr) {
		std::free(s.code);
		s.code = nullptr;
		s.code_size = 0;
	}
	int n = p_bytecode.size();
	if (n > 0) {
		s.code = (uint8_t *)std::malloc((size_t)n);
		if (s.code == nullptr) {
			return;
		}
		std::memcpy(s.code, p_bytecode.ptr(), (size_t)n);
		s.code_size = (uint32_t)n;
	}

	// Re-emit the SCR0/MU01 wrapper from the live MusFile so the saver's raw
	// passthrough writes the rewritten bytes. We collect chunk_count script
	// pointers (typically 1 for jo_gamemus / bhd_menumus), feed mus_encode_file
	// with the canonical 11 intrinsic-method names baked in by the encoder.
	uint32_t cc = _mf.header.chunk_count;
	std::vector<const MusScript *> ptrs(cc, nullptr);
	for (uint32_t i = 0; i < cc; ++i) {
		ptrs[i] = &_mf.scripts[i];
	}
	uint8_t *out_buf = nullptr;
	size_t out_size = 0;
	if (mus_encode_file(ptrs.data(), cc, &out_buf, &out_size) != 0 || out_buf == nullptr) {
		UtilityFunctions::printerr("NovaMusicScript: mus_encode_file failed in set_compiled_bytecode");
		return;
	}
	PackedByteArray fresh;
	fresh.resize((int)out_size);
	std::memcpy(fresh.ptrw(), out_buf, out_size);
	mus_free(out_buf);
	_file_bytes = fresh;
}

void NovaMusicScript::set_compiled_file_bytes(const PackedByteArray &p_file_bytes) {
	if (p_file_bytes.size() <= 0) {
		return;
	}
	String keep_source = source_path;
	load_from_decrypted_bytes(p_file_bytes, keep_source);
}

String NovaMusicScript::get_decompiled_text(const StringName &p_script_name) {
	if (!_opened) {
		return String();
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return String();
	}
	// Two-pass decompile: query required size, then write into a sized buffer.
	int needed = mus_decompile(s, nullptr, 0);
	if (needed < 0) {
		UtilityFunctions::printerr("NovaMusicScript: mus_decompile size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile(s, buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::printerr("NovaMusicScript: mus_decompile write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

String NovaMusicScript::get_decompiled_text_with_bank(const StringName &p_script_name,
		const Ref<NovaSbfBank> &p_bank) {
	if (!_opened) {
		return String();
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return String();
	}
	// When the bank is missing, route through the names-less path so callers
	// don't need a separate code branch.
	if (p_bank.is_null()) {
		return get_decompiled_text(p_script_name);
	}
	// Walk the bank's entries into a parallel CharString + const char* array
	// so libs/mus can read the names without owning the storage. The
	// CharStrings keep the utf8 bytes alive for the duration of this call.
	int entry_count = p_bank->get_entry_count();
	std::vector<CharString> name_storage;
	name_storage.reserve((size_t)entry_count);
	std::vector<const char *> name_ptrs;
	name_ptrs.reserve((size_t)entry_count);
	for (int i = 0; i < entry_count; ++i) {
		String n = p_bank->get_entry_name(i);
		name_storage.push_back(n.utf8());
		name_ptrs.push_back(name_storage.back().get_data());
	}

	int needed = mus_decompile_with_names(s, name_ptrs.data(),
			(uint32_t)name_ptrs.size(), nullptr, 0);
	if (needed < 0) {
		UtilityFunctions::printerr(
				"NovaMusicScript: mus_decompile_with_names size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile_with_names(s, name_ptrs.data(),
			(uint32_t)name_ptrs.size(), buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::printerr(
				"NovaMusicScript: mus_decompile_with_names write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

Array NovaMusicScript::get_section_model(const StringName &p_script_name) const {
	Array out;
	if (!_opened) {
		return out;
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return out;
	}
	MusModel model;
	if (mus_build_section_model(s, &model) != 0) {
		return out;
	}
	for (uint32_t i = 0; i < model.section_count; ++i) {
		const MusSectionInfo &si = model.sections[i];
		Dictionary d;
		char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
		if (si.section_index < s->section_count) {
			std::memcpy(sec_buf, s->sections[si.section_index].name, MUS_SECTION_NAME_SIZE);
		}
		d["name"] = String(sec_buf);
		d["index"] = (int64_t)si.section_index;
		d["is_entry"] = (bool)si.is_entry;
		d["is_idle_loop"] = (bool)si.is_idle_loop;

		Array edges;
		for (uint32_t e = 0; e < si.edge_count; ++e) {
			Dictionary ed;
			uint32_t to = si.edges[e].to_section_index;
			ed["to"] = (int64_t)to;
			char to_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			if (to < s->section_count) {
				std::memcpy(to_buf, s->sections[to].name, MUS_SECTION_NAME_SIZE);
			}
			ed["to_name"] = String(to_buf);
			ed["kind"] = (int64_t)si.edges[e].kind;
			edges.append(ed);
		}
		d["edges"] = edges;

		Array plays;
		for (uint32_t p = 0; p < si.play_count; ++p) {
			Dictionary pd;
			pd["track"] = (int64_t)si.plays[p].track_index;
			pd["wait"] = (bool)si.plays[p].wait;
			plays.append(pd);
		}
		d["plays"] = plays;

		out.append(d);
	}
	mus_model_free(&model);
	return out;
}
