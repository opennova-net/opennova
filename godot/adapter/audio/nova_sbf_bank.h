#ifndef NOVA_SBF_BANK_H
#define NOVA_SBF_BANK_H

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cstdint>

#include "sbf/sbf.h"

namespace godot {

class NovaSbfAudioStream;

class NovaSbfBank : public Resource {
	GDCLASS(NovaSbfBank, Resource)

public:
	NovaSbfBank();
	~NovaSbfBank();

	// Populated by SbfResourceFormatLoader; safe to call directly from script
	// for tests / fixtures that bypass the loader.
	void load_from_path(const String &p_path);

	// Factory: a fresh, empty, editable bank (zero entries) ready for add_entry()
	// and save. Mirrors the in-memory state a loaded archive has but with no
	// entries, so the music editor can author a sound bank from scratch. The
	// default constructor leaves the bank unconfigured (_opened == false), which
	// makes every mutation return ERR_UNCONFIGURED; this is the only producer of
	// an editable bank that does not read a file from disk.
	static Ref<NovaSbfBank> create_empty();

	int get_entry_count() const;
	String get_source_path() const { return source_path; }
	Array get_entries() const;
	bool has_entry(const StringName &p_name) const;
	String get_entry_name(int p_index) const;
	Ref<NovaSbfAudioStream> get_stream(const StringName &p_name);
	Ref<NovaSbfAudioStream> get_stream_at(int p_index);

	// Used by AudioStreamPlayback in Phase E to re-open chunk reads against
	// the bank's source path (PFF / VFS aware).
	String get_path_for_playback() const { return source_path; }
	const SbfRawEntry *raw_entry_at(int p_index) const;

	// Raw source bytes held in memory from load_from_path (kept for
	// sbf_open_memory lifetime). Used by the saver for raw passthrough.
	PackedByteArray get_raw_file_bytes() const { return _file_bytes; }
	bool read_file_block(uint64_t p_offset, uint32_t p_size, PackedByteArray &r_block) const;

	// Editor mutation API (Phase D / SBF F2). Replaces the audio for an entry
	// with int16-equivalent floats in [-1, 1]. Marks bank dirty so the saver
	// switches to the re-encode path.
	Error set_entry_pcm(int p_index, const PackedFloat32Array &p_samples);
	bool is_dirty() const { return _dirty; }
	void clear_dirty() { _dirty = false; }

	// Phase E (bank-edit ops). All mutate _arc.entries + override map and set
	// _dirty so the saver re-encodes.
	Error reorder_entry(int p_from, int p_to);
	Error rename_entry(int p_index, const String &p_name);
	Error add_entry(const String &p_name, const PackedFloat32Array &p_samples);
	Error delete_entry(int p_index);

	// Build a fresh SBF byte stream from the current entry table + override
	// PCM map. Entries without an override are decoded from the original
	// chunks (lossy round-trip; precision-of-int8). Used by the saver when
	// is_dirty() is true.
	Error build_encoded_bytes(PackedByteArray &out) const;

protected:
	static void _bind_methods();

private:
	String source_path;
	// _file_bytes outlives _arc; sbf_open_memory keeps the entry index
	// pointing into this buffer until sbf_close.
	PackedByteArray _file_bytes;
	SbfArchive _arc;
	bool _opened = false;
	bool _dirty = false;
	// Per-entry int16 PCM overrides keyed by entry index. Populated by
	// set_entry_pcm; consumed by build_encoded_bytes during re-encode save.
	HashMap<int, Vector<int16_t>> _entry_pcm_overrides;
};

} // namespace godot

#endif // NOVA_SBF_BANK_H
