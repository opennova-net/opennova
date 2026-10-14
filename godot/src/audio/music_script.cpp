// MUS interactive-music script wrapper. The underlying parser is engine/formats/mus
// (mus_open_memory). Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.

#include "audio/music_script.h"

#include "audio/sbf_bank.h"
#include "util/data_format.h"

#include <formats/mus/mus.h>

#include <godot_cpp/classes/file_access.hpp>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace godot;
using namespace opennova::mus;

MusicScript::MusicScript() {
	std::memset(&_mf, 0, sizeof(_mf));
}

MusicScript::~MusicScript() {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
}

void MusicScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_script_count"), &MusicScript::get_script_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &MusicScript::get_source_path);
	ClassDB::bind_method(D_METHOD("get_default_script_name"), &MusicScript::get_default_script_name);
	ClassDB::bind_method(D_METHOD("get_section_names", "script_name"), &MusicScript::get_section_names);
	ClassDB::bind_method(D_METHOD("get_decompiled_text", "script_name"), &MusicScript::get_decompiled_text);
	ClassDB::bind_method(D_METHOD("get_decompiled_text_with_bank", "script_name", "bank"), &MusicScript::get_decompiled_text_with_bank);
	ClassDB::bind_method(D_METHOD("load_from_decrypted_bytes", "bytes", "source"),
			&MusicScript::load_from_decrypted_bytes);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &MusicScript::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MusicScript::save_to_path);
	ClassDB::bind_method(D_METHOD("get_raw_file_bytes"), &MusicScript::get_raw_file_bytes);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path",
								   PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT),
			"", "get_source_path");
}

void MusicScript::load_from_decrypted_bytes(const PackedByteArray &bytes, const String &p_source) {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
	source_path = p_source;
	_file_bytes = PackedByteArray();

	if (bytes.size() <= 0) {
		UtilityFunctions::push_warning("MusicScript: empty bytes for ", p_source);
		return;
	}
	if (mus_open_memory(&_mf, bytes.ptr(), (size_t)bytes.size()) != 0) {
		UtilityFunctions::push_warning("MusicScript: mus_open_memory failed for ", p_source);
		return;
	}
	_opened = true;
	_file_bytes = bytes;
}

int MusicScript::get_script_count() const {
	return _opened ? (int)_mf.header.chunk_count : 0;
}

String MusicScript::get_default_script_name() const {
	if (!_opened || _mf.header.chunk_count == 0 || _mf.scripts == nullptr) {
		return String();
	}
	// MusScript.name is null-padded to MUS_NAME_SIZE.
	char buf[MUS_NAME_SIZE + 1] = { 0 };
	std::memcpy(buf, _mf.scripts[0].name, MUS_NAME_SIZE);
	return String(buf);
}

PackedStringArray MusicScript::get_section_names(const StringName &p_script_name) const {
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

const MusScript *MusicScript::raw_script(const String &p_name) const {
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

String MusicScript::get_decompiled_text(const StringName &p_script_name) {
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
		UtilityFunctions::push_warning("MusicScript: mus_decompile size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile(s, buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::push_warning("MusicScript: mus_decompile write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

String MusicScript::get_decompiled_text_with_bank(const StringName &p_script_name,
		const Ref<SbfBank> &p_bank) {
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
	// so engine/formats/mus can read the names without owning the storage. The
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
		UtilityFunctions::push_warning(
				"MusicScript: mus_decompile_with_names size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile_with_names(s, name_ptrs.data(),
			(uint32_t)name_ptrs.size(), buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::push_warning(
				"MusicScript: mus_decompile_with_names write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

Error MusicScript::load_from_path(const String &p_path) {
	// The .bin clash hazard (cc.bin and other config blobs share the
	// extension): after the generic payload decode, only an SCR0 magic that
	// mus_validate accepts is a music script.
	PackedByteArray bytes;
	if (!read_nova_payload_file(p_path, bytes)) {
		UtilityFunctions::push_warning("MusicScript: cannot open ", p_path);
		return ERR_FILE_CANT_OPEN;
	}
	if (bytes.size() < 4 || bytes[0] != 'S' || bytes[1] != 'C' ||
			bytes[2] != 'R' || bytes[3] != '0' ||
			mus_validate(bytes.ptr(), static_cast<size_t>(bytes.size())) != 0) {
		UtilityFunctions::push_warning("MusicScript: ", p_path,
				" is not a valid SCR0 MUS file after generic payload decode");
		return ERR_FILE_UNRECOGNIZED;
	}
	load_from_decrypted_bytes(bytes, p_path);
	return OK;
}

namespace {

// What the game reads of a script, the same in two parsed files: the method-name table it
// resolves the `method` calls through, and each program's name, code, sections, entry, the
// globals' and locals' sizes, the `enter` frame base and the MessageHandler entry (mus.h
// carries each field's witness).
bool same_as_read(const MusFile &a, const MusFile &b) {
	if (a.intrinsic_count != b.intrinsic_count || a.header.chunk_count != b.header.chunk_count) {
		return false;
	}
	for (uint32_t i = 0; i < a.intrinsic_count; ++i) {
		if (std::strncmp(a.intrinsic_names[i], b.intrinsic_names[i], MUS_METHOD_NAME_SIZE) != 0) {
			return false;
		}
	}
	for (uint32_t i = 0; i < a.header.chunk_count; ++i) {
		const MusScript &x = a.scripts[i];
		const MusScript &y = b.scripts[i];
		if (std::strncmp(x.name, y.name, MUS_NAME_SIZE) != 0 || x.code_size != y.code_size ||
				(x.code_size && std::memcmp(x.code, y.code, x.code_size) != 0) ||
				x.section_count != y.section_count || x.entry_section_index != y.entry_section_index ||
				x.globals_size != y.globals_size || x.locals_size != y.locals_size ||
				x.locals_frame_offset != y.locals_frame_offset ||
				x.has_message_handler != y.has_message_handler ||
				(x.has_message_handler && x.message_handler_offset != y.message_handler_offset)) {
			return false;
		}
		for (uint32_t k = 0; k < x.section_count; ++k) {
			if (x.sections[k].code_offset != y.sections[k].code_offset) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

Error MusicScript::save_to_path(const String &p_path) const {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	// The script from its model alone (ADR 0003), each program as the game reads it: its editor debug
	// section empty (the source path, the sections' names, the globals', parameters' and functions'
	// names, the line table), which the runtime relocates and never reads (mus.h, MusChunkHeader's
	// debug_info_offset and string_section_offset). Shallow copies: the code stays the model's.
	std::vector<MusScript> programs((size_t)_mf.header.chunk_count);
	std::vector<std::vector<MusSection>> nameless((size_t)_mf.header.chunk_count);
	std::vector<const MusScript *> scripts;
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		MusScript &program = programs[i];
		program = _mf.scripts[i];
		std::memset(program.source_path, 0, sizeof(program.source_path));
		nameless[i].resize(program.section_count);
		for (uint32_t k = 0; k < program.section_count; ++k) {
			std::memset(&nameless[i][k], 0, sizeof(MusSection));
			nameless[i][k].code_offset = _mf.scripts[i].sections[k].code_offset;
		}
		program.sections = nameless[i].empty() ? nullptr : nameless[i].data();
		program.variables = nullptr;
		program.variable_count = 0;
		program.locals = nullptr;
		program.local_count = 0;
		program.functions = nullptr;
		program.function_count = 0;
		program.lines = nullptr;
		program.line_count = 0;
		scripts.push_back(&program);
	}
	uint8_t *buf = nullptr;
	size_t size = 0;
	if (mus_encode_file(scripts.data(), (uint32_t)scripts.size(), &buf, &size) != 0 || buf == nullptr) {
		mus_free(buf);
		UtilityFunctions::push_warning("MusicScript: ", source_path, " cannot be written");
		return ERR_INVALID_DATA;
	}
	const PackedByteArray bytes = to_packed_bytes(buf, size);
	mus_free(buf);
	// The written bytes read back as the model in what the game reads, or the save is refused (the writer
	// writes the model's fields and its method table as they are).
	MusFile again;
	std::memset(&again, 0, sizeof(again));
	bool same = mus_open_memory(&again, bytes.ptr(), (size_t)bytes.size()) == 0;
	if (same) {
		same = same_as_read(_mf, again);
		mus_close(&again);
	}
	if (!same) {
		UtilityFunctions::push_warning("MusicScript: ", source_path,
				" cannot be written as the game reads it");
		return ERR_INVALID_DATA;
	}
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) {
		return ERR_CANT_OPEN;
	}
	fa->store_buffer(bytes);
	return OK;
}
