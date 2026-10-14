#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

#include <formats/lwf/lwf.h>

namespace godot {

class ResourceRoot;

// GDExtension wrapper over opennova::lwf (engine/formats/lwf). Parses a NovaLogic .lwf
// sound-profile into its model (opennova::lwf::File) and exposes its Sound Set ->
// Layer -> Member hierarchy as Godot Dictionary views of that model for format tooling.
//
// The model is the one thing an edit changes and a save writes: every save is
// opennova::lwf::encode_lwf over the model's fields alone (ADR 0003), edited or not,
// never the bytes the bank was read from. An edit sets the model's fields; a member's
// record or single that other members share is copied for the one edited first, so no
// other member changes. A layer is edited in place: formats/lwf's reader sizes the layer
// table by the sets' layer counts, so one set names each layer.
//
// Dictionary shapes (views; field semantics grilled vs Jointops.exe -- see
// engine/formats/lwf/lwf.h for the [orig:, see docs/audio/lwf-dbf-sound-re.md] anchors):
//   set:    { name:String, target_id:int, pitch_base:int, pitch_random_range:int,
//             set_flags:int, layers:Array[layer] }
//   layer:  { selection_mode:int, falloff_radius:int, min_distance:int,
//             looping/directional/heading/preload/stoppable/internal/external/
//             reverb/rapid:bool, members:Array[member] }
//   member: { name:String, wav_path:String, value_hi:int,
//             base_pitch:float, rand_pitch:float, volume:int, clamp_volume:int }
// (value_hi is the single's volume byte at +33 shifted up a byte, as the old u16 view held it.)
class LwfData : public RefCounted {
	GDCLASS(LwfData, RefCounted)

private:
	// The bank's model: what every view shows, every edit changes and every save writes.
	opennova::lwf::File file_;
	String source_path_;
	String last_error_;
	bool loaded_ = false;
	bool modified_ = false;

	bool decode(const PackedByteArray &bytes);
	PackedByteArray encode(String &r_error) const;

	// The model's index of a set's layer, a layer's member and a member's single, the
	// member's and the single's made the edited member's own first (a copy of a record
	// another member names); -1 on a bad index.
	int own_playlist(int p_si, int p_li);
	int own_sndparm(int p_si, int p_li, int p_mi);
	int own_single(int p_si, int p_li, int p_mi);

protected:
	static void _bind_methods();

public:
	// Mirrors opennova::audio::SelectionMode + opennova::lwf::PlaylistFlags,
	// bound as constants; the .cpp static_asserts pin every mirror to its
	// engine home. SELECTION_FIRST is an authoring/preview extension; the
	// engine's default for unflagged layers is RANDOM (see
	// audio/sound_selector.h selection_mode_for_flags).
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
	Error open_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
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

	// --- read (deep copies) ---
	int get_set_count() const;
	Dictionary get_set(int p_si) const;
	// C++-side seam (not bound): the sound profile as the engine's formats/lwf
	// structs, the model itself. The runtime sound bank (audio/sound_bank) reads
	// sets/layers/members through this rather than the Dictionary views.
	const opennova::lwf::File &engine_file() const { return file_; }

	// --- scalar edits ---
	void set_set_field(int p_si, const String &p_key, const Variant &p_value);
	void set_layer_field(int p_si, int p_li, const String &p_key, const Variant &p_value);
	void set_member_field(int p_si, int p_li, int p_mi, const String &p_key, const Variant &p_value);

	// --- structural edits (return new index where applicable, -1 on error) ---
	int add_set();
	int add_layer(int p_si);
	int add_member(int p_si, int p_li);

	LwfData() = default;
};

} // namespace godot

