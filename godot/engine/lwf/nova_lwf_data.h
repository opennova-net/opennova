#ifndef NOVA_LWF_DATA_H
#define NOVA_LWF_DATA_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

#include <lwf/lwf.h>

namespace godot {

class NovaResourceRoot;

// GDExtension wrapper over opennova::lwf (libs/lwf). Parses a NovaLogic .lwf
// sound-profile and exposes its Sound Set -> Layer -> Member hierarchy as an
// editable Godot Array-of-Dictionary tree for the ONED sound workspace.
//
// Save fidelity: the original bytes are retained; an open->save with NO edits
// writes them verbatim (byte-exact). Once edited, save re-normalizes the tree
// (dedup the shared singles table, rebuild playlists/sndparms + string pool)
// and encodes via opennova::lwf::encode_lwf — a valid, canonical .lwf.
//
// Dictionary shapes (all keys present on read; field semantics grilled vs
// Jointops.exe -- see libs/lwf/include/lwf/lwf.h for the [orig:] anchors):
//   set:    { name:String, target_id:int, pitch_base:int, pitch_random_range:int,
//             set_flags:int, layer_count:int, layers:Array[layer] }
//   layer:  { selection_mode:int, falloff_radius:int, min_distance:int,
//             looping/directional/heading/preload/stoppable/internal/external/
//             reverb/rapid:bool, member_count:int, members:Array[member] }
//   member: { name:String, wav_path:String, value_hi:int,
//             base_pitch:float, rand_pitch:float, volume:int, clamp_volume:int }
class NovaLwfData : public RefCounted {
	GDCLASS(NovaLwfData, RefCounted)

private:
	Array sets_;                    // editable tree (live; getters deep-copy)
	PackedByteArray original_bytes_; // for byte-exact save when unmodified
	String source_path_;
	String last_error_;
	bool loaded_ = false;
	bool modified_ = false;

	bool decode_into_tree(const PackedByteArray &bytes);
	PackedByteArray encode_current(String &r_error) const;

	// Live (shared-ref) accessors into sets_; empty container on bad index.
	Dictionary set_ref(int p_si) const;
	Dictionary layer_ref(int p_si, int p_li) const;
	Dictionary member_ref(int p_si, int p_li, int p_mi) const;

protected:
	static void _bind_methods();

public:
	// Mirrors opennova::lwf selection modes + PlaylistFlags. Bound as constants.
	// SELECTION_FIRST is an authoring/preview extension; the engine's default for
	// unflagged layers is RANDOM [orig: SoundBank_PlayTriggerEntries @ 0x75cdfc].
	enum : uint32_t {
		SELECTION_FIRST = 0,
		SELECTION_RANDOM = 1,
		SELECTION_SEQUENTIAL = 2,
		SELECTION_RANDOM_SEQ = 3,

		FLAG_HEADING = 0x0001,
		FLAG_INTERNAL = 0x0002,
		FLAG_EXTERNAL = 0x0004,
		FLAG_RANDOM = 0x0008,
		FLAG_SEQUENTIAL = 0x0010,
		FLAG_STOPPABLE = 0x0020,
		FLAG_PRELOAD = 0x0040,
		FLAG_RANDOM_SEQUENTIAL = 0x0080,
		FLAG_DIRECTIONAL = 0x0200,
		FLAG_LOOPING = 0x0400,
		FLAG_REVERB = 0x0800,
		FLAG_RAPID = 0x1000,
	};

	// --- open / save ---
	Error open_file(const String &p_path);
	Error open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool load_bytes(const PackedByteArray &p_bytes);
	void create_empty();
	Error save_file(const String &p_path);
	Error save_as(const String &p_path);
	PackedByteArray to_bytes() const;

	// --- state ---
	bool is_loaded() const { return loaded_; }
	bool is_modified() const { return modified_; }
	String get_source_path() const { return source_path_; }
	String get_last_error() const { return last_error_; }
	void mark_clean() { modified_ = false; }

	// --- read (deep copies) ---
	int get_set_count() const;
	Array get_sets() const;
	Dictionary get_set(int p_si) const;
	int get_layer_count(int p_si) const;
	Dictionary get_layer(int p_si, int p_li) const;
	int get_member_count(int p_si, int p_li) const;
	Dictionary get_member(int p_si, int p_li, int p_mi) const;

	// --- scalar edits ---
	void set_set_field(int p_si, const String &p_key, const Variant &p_value);
	void set_layer_field(int p_si, int p_li, const String &p_key, const Variant &p_value);
	void set_member_field(int p_si, int p_li, int p_mi, const String &p_key, const Variant &p_value);

	// --- structural edits (return new index where applicable, -1 on error) ---
	int add_set();
	void remove_set(int p_si);
	void move_set(int p_from, int p_to);
	int add_layer(int p_si);
	void remove_layer(int p_si, int p_li);
	void move_layer(int p_si, int p_from, int p_to);
	int add_member(int p_si, int p_li);
	void remove_member(int p_si, int p_li, int p_mi);
	void move_member(int p_si, int p_li, int p_from, int p_to);

	NovaLwfData() = default;
};

} // namespace godot

#endif // NOVA_LWF_DATA_H
