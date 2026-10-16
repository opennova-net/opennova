#include "lwf/lwf_data.h"

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <runtime/audio/sound_selector.h>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace godot {

namespace {

// The class-constant mirrors (lwf_data.h) are pinned to their engine
// homes here so they can never drift: selection modes to
// opennova::audio::SelectionMode, flags to opennova::lwf::PlaylistFlags.
static_assert(LwfData::SELECTION_RANDOM == opennova::audio::kRandom);
static_assert(LwfData::SELECTION_SEQUENTIAL == opennova::audio::kSequential);
static_assert(LwfData::SELECTION_RANDOM_SEQ == opennova::audio::kRandomSeq);
static_assert(LwfData::FLAG_HEADING == opennova::lwf::kFlagHeading);
static_assert(LwfData::FLAG_INTERNAL == opennova::lwf::kFlagInternal);
static_assert(LwfData::FLAG_EXTERNAL == opennova::lwf::kFlagExternal);
static_assert(LwfData::FLAG_RANDOM == opennova::lwf::kFlagRandom);
static_assert(LwfData::FLAG_SEQUENTIAL == opennova::lwf::kFlagSequential);
static_assert(LwfData::FLAG_STOPPABLE == opennova::lwf::kFlagStoppable);
static_assert(LwfData::FLAG_PRELOAD == opennova::lwf::kFlagPreload);
static_assert(LwfData::FLAG_RANDOM_SEQUENTIAL == opennova::lwf::kFlagRandomSequential);
static_assert(LwfData::FLAG_DIRECTIONAL == opennova::lwf::kFlagDirectional);
static_assert(LwfData::FLAG_LOOPING == opennova::lwf::kFlagLooping);
static_assert(LwfData::FLAG_REVERB == opennova::lwf::kFlagReverb);
static_assert(LwfData::FLAG_RAPID == opennova::lwf::kFlagRapid);

// Engine playback precedence lives in opennova::audio::selection_mode_for_flags
// (audio/sound_selector.h carries the witness); the mirror pins above make
// the cast exact.
int selection_mode_from_flags(uint32_t flags) {
	return static_cast<int>(opennova::audio::selection_mode_for_flags(flags));
}

// The flag bits a selection mode is written as: the explicit marker the authoring tools
// write for the engine's default mode (RANDOM), the two the engine tests (lwf.h), none for a
// value that names no mode.
uint32_t selection_flags(int mode) {
	switch (mode) {
		case LwfData::SELECTION_RANDOM: return opennova::lwf::kFlagRandom;
		case LwfData::SELECTION_SEQUENTIAL: return opennova::lwf::kFlagSequential;
		case LwfData::SELECTION_RANDOM_SEQ: return opennova::lwf::kFlagRandomSequential;
		default: return 0;
	}
}

// A layer view's boolean keys and the flag bit each is.
struct LayerFlagKey {
	const char *key;
	uint32_t bit;
};
const LayerFlagKey kLayerFlagKeys[] = {
	{ "looping", opennova::lwf::kFlagLooping },
	{ "directional", opennova::lwf::kFlagDirectional },
	{ "heading", opennova::lwf::kFlagHeading },
	{ "preload", opennova::lwf::kFlagPreload },
	{ "stoppable", opennova::lwf::kFlagStoppable },
	{ "internal", opennova::lwf::kFlagInternal },
	{ "external", opennova::lwf::kFlagExternal },
	{ "reverb", opennova::lwf::kFlagReverb },
	{ "rapid", opennova::lwf::kFlagRapid },
};

Dictionary member_view(const opennova::lwf::File &f, uint32_t sndparm_index) {
	Dictionary member;
	const opennova::lwf::Sndparm &sp = f.sndparms[sndparm_index];
	String name;
	String path;
	int value_hi = 0;
	if (sp.single_index < f.singles.size()) {
		const opennova::lwf::Single &sg = f.singles[sp.single_index];
		name = opennova::to_gd(sg.name);
		path = opennova::to_gd(sg.path);
		value_hi = static_cast<int>(sg.volume) << 8; // the u16 at +32, its high byte the volume
	}
	member["name"] = name;
	member["wav_path"] = path;
	member["value_hi"] = value_hi;
	// Q16 (0x10000 = 1.0), the one engine pitch scale (lwf.h).
	member["base_pitch"] = opennova::lwf::pitch_from_q16(sp.pitch_scaled);
	member["rand_pitch"] = opennova::lwf::pitch_from_q16(sp.random_pitch_scaled);
	member["volume"] = static_cast<int>(sp.volume);
	member["clamp_volume"] = static_cast<int>(sp.clamp_volume);
	return member;
}

Dictionary layer_view(const opennova::lwf::File &f, uint32_t playlist_index) {
	Dictionary layer;
	const opennova::lwf::Playlist &pl = f.playlists[playlist_index];
	layer["selection_mode"] = selection_mode_from_flags(pl.flags);
	layer["falloff_radius"] = static_cast<int>(pl.falloff_radius);
	layer["min_distance"] = static_cast<int>(pl.min_distance);
	for (const LayerFlagKey &flag : kLayerFlagKeys) {
		layer[flag.key] = (pl.flags & flag.bit) != 0;
	}
	Array members;
	for (uint32_t sidx : pl.sndparm_indices) {
		if (sidx < f.sndparms.size()) {
			members.push_back(member_view(f, sidx));
		}
	}
	layer["members"] = members;
	return layer;
}

Dictionary set_view(const opennova::lwf::File &f, const opennova::lwf::Multi &multi) {
	Dictionary set;
	set["name"] = opennova::to_gd(multi.name);
	set["target_id"] = static_cast<int64_t>(multi.target_id);
	set["pitch_base"] = static_cast<int64_t>(multi.pitch_base);
	set["pitch_random_range"] = static_cast<int64_t>(multi.pitch_random_range);
	set["set_flags"] = static_cast<int64_t>(multi.set_flags);
	Array layers;
	for (uint32_t pidx : multi.playlist_indices) {
		if (pidx < f.playlists.size()) {
			layers.push_back(layer_view(f, pidx));
		}
	}
	set["layers"] = layers;
	return set;
}

// The views list a set's layers and a layer's members skipping an index past its table,
// as the model's slots stand; the edits find the slot the view's index names.
int view_slot(const std::vector<uint32_t> &indices, size_t table_size, int p_view_index) {
	if (p_view_index < 0) {
		return -1;
	}
	int seen = 0;
	for (size_t slot = 0; slot < indices.size(); ++slot) {
		if (indices[slot] >= table_size) {
			continue;
		}
		if (seen++ == p_view_index) {
			return static_cast<int>(slot);
		}
	}
	return -1;
}

} // namespace

void LwfData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &LwfData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name"), &LwfData::open_from_resource_root);
	ClassDB::bind_method(D_METHOD("load_bytes", "bytes"), &LwfData::load_bytes);
	ClassDB::bind_method(D_METHOD("create_empty"), &LwfData::create_empty);
	ClassDB::bind_method(D_METHOD("save_file", "path"), &LwfData::save_file);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &LwfData::save_as);
	ClassDB::bind_method(D_METHOD("to_bytes"), &LwfData::to_bytes);

	ClassDB::bind_method(D_METHOD("is_loaded"), &LwfData::is_loaded);
	ClassDB::bind_method(D_METHOD("is_modified"), &LwfData::is_modified);
	ClassDB::bind_method(D_METHOD("get_source_path"), &LwfData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &LwfData::get_last_error);

	ClassDB::bind_method(D_METHOD("get_set_count"), &LwfData::get_set_count);
	ClassDB::bind_method(D_METHOD("get_set", "set_index"), &LwfData::get_set);

	ClassDB::bind_method(D_METHOD("set_set_field", "set_index", "key", "value"), &LwfData::set_set_field);
	ClassDB::bind_method(D_METHOD("set_layer_field", "set_index", "layer_index", "key", "value"), &LwfData::set_layer_field);
	ClassDB::bind_method(D_METHOD("set_member_field", "set_index", "layer_index", "member_index", "key", "value"), &LwfData::set_member_field);

	ClassDB::bind_method(D_METHOD("add_set"), &LwfData::add_set);
	ClassDB::bind_method(D_METHOD("add_layer", "set_index"), &LwfData::add_layer);
	ClassDB::bind_method(D_METHOD("add_member", "set_index", "layer_index"), &LwfData::add_member);

	BIND_CONSTANT(SELECTION_RANDOM);
	BIND_CONSTANT(SELECTION_SEQUENTIAL);
	BIND_CONSTANT(SELECTION_RANDOM_SEQ);
}

// ---------------------------------------------------------------- decode/encode

bool LwfData::decode(const PackedByteArray &bytes) {
	opennova::lwf::File f;
	std::string err;
	if (!opennova::lwf::parse_lwf_buffer(bytes.ptr(), static_cast<size_t>(bytes.size()), f, err)) {
		last_error_ = String(err.c_str());
		return false;
	}
	file_ = std::move(f);
	loaded_ = true;
	modified_ = false;
	return true;
}

PackedByteArray LwfData::encode(String &r_error) const {
	// The bank from its model's fields alone, edited or not (ADR 0003).
	std::vector<uint8_t> out;
	std::string err;
	if (!opennova::lwf::encode_lwf(file_, out, err)) {
		r_error = String(err.c_str());
		return PackedByteArray();
	}
	PackedByteArray result;
	result.resize(static_cast<int64_t>(out.size()));
	if (!out.empty()) {
		std::memcpy(result.ptrw(), out.data(), out.size());
	}
	return result;
}

// ----------------------------------------------------------------------- open/save

Error LwfData::open_file(const String &p_path) {
	last_error_ = String();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		last_error_ = "Cannot open file: " + p_path;
		return ERR_CANT_OPEN;
	}
	PackedByteArray bytes = file->get_buffer(file->get_length());
	file->close();
	if (!decode(bytes)) {
		return ERR_FILE_CORRUPT;
	}
	source_path_ = p_path;
	return OK;
}

Error LwfData::open_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	last_error_ = String();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error_ = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		last_error_ = "Sound profile filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		last_error_ = "Sound profile not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}
	if (!decode(bytes)) {
		return ERR_CANT_OPEN;
	}
	source_path_ = file;
	return OK;
}

bool LwfData::load_bytes(const PackedByteArray &p_bytes) {
	last_error_ = String();
	return decode(p_bytes);
}

void LwfData::create_empty() {
	file_ = opennova::lwf::File();
	source_path_ = String();
	last_error_ = String();
	loaded_ = true;
	modified_ = true;
}

Error LwfData::save_as(const String &p_path) {
	String err;
	PackedByteArray bytes = encode(err);
	if (!err.is_empty()) {
		last_error_ = err;
		return ERR_CANT_CREATE;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		last_error_ = "Cannot open file for writing: " + p_path;
		return ERR_CANT_OPEN;
	}
	file->store_buffer(bytes);
	file->close();
	source_path_ = p_path;
	modified_ = false;
	return OK;
}

Error LwfData::save_file(const String &p_path) {
	return save_as(p_path);
}

PackedByteArray LwfData::to_bytes() const {
	String err;
	return encode(err);
}

// --------------------------------------------------------------- the edited records

int LwfData::own_playlist(int p_si, int p_li) {
	if (p_si < 0 || p_si >= static_cast<int>(file_.multis.size())) {
		return -1;
	}
	opennova::lwf::Multi &multi = file_.multis[static_cast<size_t>(p_si)];
	const int slot = view_slot(multi.playlist_indices, file_.playlists.size(), p_li);
	if (slot < 0) {
		return -1;
	}
	// Edited in place: formats/lwf's reader sizes the layer table by the sets' layer counts
	// (parse_lwf_buffer, as the builders lay a bank out), so a copy would misplace the tables
	// after it.
	return static_cast<int>(multi.playlist_indices[static_cast<size_t>(slot)]);
}

int LwfData::own_sndparm(int p_si, int p_li, int p_mi) {
	const int playlist = own_playlist(p_si, p_li);
	if (playlist < 0) {
		return -1;
	}
	opennova::lwf::Playlist &pl = file_.playlists[static_cast<size_t>(playlist)];
	const int slot = view_slot(pl.sndparm_indices, file_.sndparms.size(), p_mi);
	if (slot < 0) {
		return -1;
	}
	const uint32_t index = pl.sndparm_indices[static_cast<size_t>(slot)];
	size_t names = 0;
	for (const opennova::lwf::Playlist &p : file_.playlists) {
		names += static_cast<size_t>(std::count(p.sndparm_indices.begin(), p.sndparm_indices.end(), index));
	}
	if (names > 1) {
		file_.sndparms.push_back(file_.sndparms[index]);
		pl.sndparm_indices[static_cast<size_t>(slot)] = static_cast<uint32_t>(file_.sndparms.size() - 1);
	}
	return static_cast<int>(pl.sndparm_indices[static_cast<size_t>(slot)]);
}

int LwfData::own_single(int p_si, int p_li, int p_mi) {
	const int sndparm = own_sndparm(p_si, p_li, p_mi);
	if (sndparm < 0) {
		return -1;
	}
	opennova::lwf::Sndparm &sp = file_.sndparms[static_cast<size_t>(sndparm)];
	if (sp.single_index >= file_.singles.size()) {
		// A member naming no single (the engine reads single 0 for it: lwf.h's
		// Sndparm::single_index carries the witness): its edit makes it one of its own.
		file_.singles.push_back(opennova::lwf::Single());
		sp.single_index = static_cast<uint32_t>(file_.singles.size() - 1);
		return static_cast<int>(sp.single_index);
	}
	size_t names = 0;
	for (const opennova::lwf::Sndparm &p : file_.sndparms) {
		names += p.single_index == sp.single_index ? 1 : 0;
	}
	if (names > 1) {
		file_.singles.push_back(file_.singles[sp.single_index]);
		sp.single_index = static_cast<uint32_t>(file_.singles.size() - 1);
	}
	return static_cast<int>(sp.single_index);
}

// --------------------------------------------------------------------------- read

int LwfData::get_set_count() const {
	return static_cast<int>(file_.multis.size());
}

Dictionary LwfData::get_set(int p_si) const {
	if (p_si < 0 || p_si >= static_cast<int>(file_.multis.size())) {
		return Dictionary();
	}
	return set_view(file_, file_.multis[static_cast<size_t>(p_si)]);
}

// ------------------------------------------------------------------- scalar edits

void LwfData::set_set_field(int p_si, const String &p_key, const Variant &p_value) {
	if (p_si < 0 || p_si >= static_cast<int>(file_.multis.size())) {
		return;
	}
	opennova::lwf::Multi &multi = file_.multis[static_cast<size_t>(p_si)];
	if (p_key == "name") {
		multi.name = opennova::to_std(String(p_value));
	} else if (p_key == "target_id") {
		multi.target_id = static_cast<uint32_t>(static_cast<int64_t>(p_value));
	} else if (p_key == "pitch_base") {
		multi.pitch_base = static_cast<uint32_t>(static_cast<int64_t>(p_value));
	} else if (p_key == "pitch_random_range") {
		multi.pitch_random_range = static_cast<uint32_t>(static_cast<int64_t>(p_value));
	} else if (p_key == "set_flags") {
		multi.set_flags = static_cast<uint32_t>(static_cast<int64_t>(p_value));
	} else {
		UtilityFunctions::push_warning("LwfData: a set has no field '", p_key, "'");
		return;
	}
	modified_ = true;
}

void LwfData::set_layer_field(int p_si, int p_li, const String &p_key, const Variant &p_value) {
	uint32_t bit = 0;
	for (const LayerFlagKey &flag : kLayerFlagKeys) {
		if (p_key == flag.key) {
			bit = flag.bit;
		}
	}
	if (bit == 0 && p_key != "selection_mode" && p_key != "falloff_radius" && p_key != "min_distance") {
		UtilityFunctions::push_warning("LwfData: a layer has no field '", p_key, "'");
		return;
	}
	const int playlist = own_playlist(p_si, p_li);
	if (playlist < 0) {
		return;
	}
	opennova::lwf::Playlist &pl = file_.playlists[static_cast<size_t>(playlist)];
	if (bit != 0) {
		pl.flags = static_cast<bool>(p_value) ? (pl.flags | bit) : (pl.flags & ~bit);
	} else if (p_key == "selection_mode") {
		const uint32_t selection = opennova::lwf::kFlagRandom | opennova::lwf::kFlagSequential |
				opennova::lwf::kFlagRandomSequential;
		pl.flags = (pl.flags & ~selection) | selection_flags(static_cast<int>(p_value));
	} else if (p_key == "falloff_radius") {
		pl.falloff_radius = static_cast<uint16_t>(static_cast<int>(p_value));
	} else {
		pl.min_distance = static_cast<uint16_t>(static_cast<int>(p_value));
	}
	modified_ = true;
}

void LwfData::set_member_field(int p_si, int p_li, int p_mi, const String &p_key, const Variant &p_value) {
	if (p_key == "name" || p_key == "wav_path" || p_key == "value_hi") {
		const int single = own_single(p_si, p_li, p_mi);
		if (single < 0) {
			return;
		}
		opennova::lwf::Single &sg = file_.singles[static_cast<size_t>(single)];
		if (p_key == "name") {
			sg.name = opennova::to_std(String(p_value));
		} else if (p_key == "wav_path") {
			sg.path = opennova::to_std(String(p_value));
		} else {
			sg.volume = static_cast<uint8_t>((static_cast<int>(p_value) >> 8) & 0xFF);
		}
	} else if (p_key == "base_pitch" || p_key == "rand_pitch" || p_key == "volume" || p_key == "clamp_volume") {
		const int sndparm = own_sndparm(p_si, p_li, p_mi);
		if (sndparm < 0) {
			return;
		}
		opennova::lwf::Sndparm &sp = file_.sndparms[static_cast<size_t>(sndparm)];
		if (p_key == "base_pitch") {
			sp.pitch_scaled = opennova::lwf::pitch_to_q16(static_cast<double>(p_value));
		} else if (p_key == "rand_pitch") {
			sp.random_pitch_scaled = opennova::lwf::pitch_to_q16(static_cast<double>(p_value));
		} else if (p_key == "volume") {
			sp.volume = static_cast<uint32_t>(static_cast<int>(p_value));
		} else {
			sp.clamp_volume = static_cast<uint32_t>(static_cast<int>(p_value));
		}
	} else {
		UtilityFunctions::push_warning("LwfData: a member has no field '", p_key, "'");
		return;
	}
	modified_ = true;
}

// --------------------------------------------------------------- structural edits

int LwfData::add_set() {
	opennova::lwf::Multi multi;
	multi.name = "NewSoundSet";
	// The authoring tools' stock ~unity set pitch (engine lwf.h carries the
	// Q16 convention and its witness).
	multi.pitch_base = opennova::lwf::kAuthoredSetPitchBase;
	file_.multis.push_back(std::move(multi));
	modified_ = true;
	return static_cast<int>(file_.multis.size()) - 1;
}

int LwfData::add_layer(int p_si) {
	if (p_si < 0 || p_si >= static_cast<int>(file_.multis.size())) {
		return -1;
	}
	opennova::lwf::Multi &multi = file_.multis[static_cast<size_t>(p_si)];
	if (multi.playlist_indices.size() >= opennova::lwf::kMaxPlaylistsPerMulti) {
		last_error_ = "A set names at most eight layers";
		return -1;
	}
	// A new layer selects at random, the engine's default (selection_flags).
	opennova::lwf::Playlist playlist;
	playlist.flags = selection_flags(SELECTION_RANDOM);
	file_.playlists.push_back(std::move(playlist));
	multi.playlist_indices.push_back(static_cast<uint32_t>(file_.playlists.size() - 1));
	modified_ = true;
	int layers = 0;
	for (uint32_t pidx : multi.playlist_indices) {
		layers += pidx < file_.playlists.size() ? 1 : 0;
	}
	return layers - 1;
}

int LwfData::add_member(int p_si, int p_li) {
	const int playlist = own_playlist(p_si, p_li);
	if (playlist < 0) {
		return -1;
	}
	if (file_.playlists[static_cast<size_t>(playlist)].sndparm_indices.size() >= opennova::lwf::kMaxSndparmsPerPlaylist) {
		last_error_ = "A layer names at most eight members";
		return -1;
	}
	// A new member: a single of its own (no name, no file, volume byte 0) at unity pitch,
	// full volume and ceiling.
	file_.singles.push_back(opennova::lwf::Single());
	opennova::lwf::Sndparm sndparm;
	sndparm.single_index = static_cast<uint32_t>(file_.singles.size() - 1);
	sndparm.pitch_scaled = opennova::lwf::pitch_to_q16(1.0);
	sndparm.random_pitch_scaled = 0;
	sndparm.volume = 255;
	sndparm.clamp_volume = 255;
	file_.sndparms.push_back(sndparm);
	opennova::lwf::Playlist &pl = file_.playlists[static_cast<size_t>(playlist)];
	pl.sndparm_indices.push_back(static_cast<uint32_t>(file_.sndparms.size() - 1));
	modified_ = true;
	int members = 0;
	for (uint32_t sidx : pl.sndparm_indices) {
		members += sidx < file_.sndparms.size() ? 1 : 0;
	}
	return members - 1;
}

} // namespace godot
