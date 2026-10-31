#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cstdint>
#include <string>

#include <formats/sbf/sbf.h>

namespace godot {


class SbfBank : public Resource {
	GDCLASS(SbfBank, Resource)

public:
	SbfBank();
	~SbfBank();

	// Populated by SbfResourceFormatLoader; safe to call directly from script
	// for tests / fixtures that bypass the loader.
	void load_from_path(const String &p_path);

	// Factory: a fresh, empty, editable bank (zero entries) ready for add_entry()
	// and save. Mirrors the in-memory state a loaded archive has but with no
	// entries, so the music editor can author a sound bank from scratch. The
	// default constructor leaves the bank unconfigured (_opened == false), which
	// makes every mutation return ERR_UNCONFIGURED; this is the only producer of
	// an editable bank that does not read a file from disk.
	static Ref<SbfBank> create_empty();

	int get_entry_count() const;
	String get_source_path() const { return source_path; }
	Array get_entries() const;
	bool has_entry(const StringName &p_name) const;
	String get_entry_name(int p_index) const;

	const opennova::sbf::SbfRawEntry *raw_entry_at(int p_index) const;
	// The streaming reader's index and the bytes it points into (the music stream's,
	// audio::MusicStream), null when load_from_path opened none.
	const opennova::sbf::SbfArchive *archive() const { return _opened ? &_arc : nullptr; }
	const PackedByteArray &file_bytes() const { return _file_bytes; }
	// The opened bank's header, null when load_from_path opened none (the music
	// context open's bank).
	const opennova::sbf::SbfHeader *opened_header() const {
		return _opened ? &_arc.header : nullptr;
	}
	// The header's flags word: 1 the byte-paired form, any other the IMA ADPCM form
	// (sbf::sbf_decode_entry).
	uint32_t flags() const { return _arc.header.flags; }

	// The bytes load_from_path read, the streaming reader's (sbf_open_memory keeps
	// its index pointing into them; playback reads its chunks from them). The
	// saver never writes them: it writes the bank's model.
	PackedByteArray get_raw_file_bytes() const { return _file_bytes; }
	bool read_file_block(uint64_t p_offset, uint32_t p_size, PackedByteArray &r_block) const;

	// Editor mutation API. Replaces an entry's stream in the bank's model with
	// int16-equivalent floats in [-1, 1], encoded chunk by chunk
	// (sbf_encode_stream). Marks the bank dirty.
	Error set_entry_pcm(int p_index, const PackedFloat32Array &p_samples);
	bool is_dirty() const { return _dirty; }
	void clear_dirty() { _dirty = false; }

	// Appends an entry: its name (at most 15 bytes) and its stream encoded from the
	// samples, to the entry table and the bank's model. Marks the bank dirty.
	Error add_entry(const String &p_name, const PackedFloat32Array &p_samples);

	// The .sbf bytes from the bank's model alone (sbf_write_bank, ADR 0003): every
	// stream's chunks as read or as an edit encoded them, never the bytes the bank
	// was read from.
	Error build_encoded_bytes(PackedByteArray &out) const;
	// The .sbf write (ADR 0032 direct document I/O): build_encoded_bytes, then the
	// dirty bit cleared.
	Error save_to_path(const String &p_path);

protected:
	static void _bind_methods();

private:
	String source_path;
	// _file_bytes outlives _arc; sbf_open_memory keeps the entry index
	// pointing into this buffer until sbf_close.
	PackedByteArray _file_bytes;
	opennova::sbf::SbfArchive _arc;
	bool _opened = false;
	bool _dirty = false;
	// The bank as its fields (formats/sbf: SbfFile, sbf_read_bank), what every edit
	// changes and the saver writes. Read from the loaded bytes on the first edit or
	// save (`_model_read`; playback never needs it); `_model_error` says why a bank
	// the streaming reader opened has none (`_modeled` false), which refuses both.
	mutable opennova::sbf::SbfFile _model;
	mutable bool _model_read = false;
	mutable bool _modeled = false;
	mutable std::string _model_error;
	bool _ensure_model() const;
	// Whether the model's streams are the byte-paired form, the one form the PCM edits encode
	// (sbf_encode_stream); a bank of any other flags word streams IMA ADPCM.
	bool _model_byte_paired() const;
};

} // namespace godot

