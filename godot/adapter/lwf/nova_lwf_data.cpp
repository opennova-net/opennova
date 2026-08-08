#include "lwf/nova_lwf_data.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace godot {

namespace {

constexpr double kPitchScale = 65535.0;

// Engine playback precedence [orig: SoundBank_PlayTriggerEntries @ 0x75cd5c..0x75cdfc]:
// sequential (0x10) wins, then random-sequential (0x80); anything else -- including layers
// carrying only kFlagRandom, or no selection flag at all -- plays RANDOM (the engine never
// tests 0x08; random is its default).
int selection_mode_from_flags(uint32_t flags) {
	if (flags & opennova::lwf::kFlagSequential) {
		return NovaLwfData::SELECTION_SEQUENTIAL;
	}
	if (flags & opennova::lwf::kFlagRandomSequential) {
		return NovaLwfData::SELECTION_RANDOM_SEQ;
	}
	return NovaLwfData::SELECTION_RANDOM;
}

uint32_t flags_from_layer(const Dictionary &layer) {
	uint32_t flags = 0;
	switch (static_cast<int>(layer.get("selection_mode", NovaLwfData::SELECTION_RANDOM))) {
		case NovaLwfData::SELECTION_RANDOM:
			// The explicit marker the authoring tools write for the engine's default mode.
			flags |= opennova::lwf::kFlagRandom;
			break;
		case NovaLwfData::SELECTION_SEQUENTIAL:
			flags |= opennova::lwf::kFlagSequential;
			break;
		case NovaLwfData::SELECTION_RANDOM_SEQ:
			flags |= opennova::lwf::kFlagRandomSequential;
			break;
		default:
			break;
	}
	if (static_cast<bool>(layer.get("looping", false))) flags |= opennova::lwf::kFlagLooping;
	if (static_cast<bool>(layer.get("directional", false))) flags |= opennova::lwf::kFlagDirectional;
	if (static_cast<bool>(layer.get("heading", false))) flags |= opennova::lwf::kFlagHeading;
	if (static_cast<bool>(layer.get("preload", false))) flags |= opennova::lwf::kFlagPreload;
	if (static_cast<bool>(layer.get("stoppable", false))) flags |= opennova::lwf::kFlagStoppable;
	if (static_cast<bool>(layer.get("internal", false))) flags |= opennova::lwf::kFlagInternal;
	if (static_cast<bool>(layer.get("external", false))) flags |= opennova::lwf::kFlagExternal;
	if (static_cast<bool>(layer.get("reverb", false))) flags |= opennova::lwf::kFlagReverb;
	if (static_cast<bool>(layer.get("rapid", false))) flags |= opennova::lwf::kFlagRapid;
	return flags;
}

Dictionary make_member() {
	Dictionary m;
	m["name"] = String();
	m["wav_path"] = String();
	m["value_hi"] = 0;
	m["base_pitch"] = 1.0;
	m["rand_pitch"] = 0.0;
	m["volume"] = 255;
	m["clamp_volume"] = 255;
	return m;
}

Dictionary make_layer() {
	Dictionary l;
	l["selection_mode"] = NovaLwfData::SELECTION_RANDOM;
	l["falloff_radius"] = 0;
	l["min_distance"] = 0;
	l["looping"] = false;
	l["directional"] = false;
	l["heading"] = false;
	l["preload"] = false;
	l["stoppable"] = false;
	l["internal"] = false;
	l["external"] = false;
	l["reverb"] = false;
	l["rapid"] = false;
	l["members"] = Array();
	return l;
}

Dictionary make_set() {
	Dictionary s;
	s["name"] = String("NewSoundSet");
	s["target_id"] = 0;
	s["pitch_base"] = 0xFFFF; // Q16 unity [orig: SoundBank_SelectTriggerEntryFromBank @ 0x75c0be]
	s["pitch_random_range"] = 0;
	s["set_flags"] = 0;
	s["layers"] = Array();
	return s;
}

// Reorder `arr` in place: move the element at p_from to p_to (both clamped).
void array_move(Array arr, int p_from, int p_to) {
	const int n = arr.size();
	if (p_from < 0 || p_from >= n) {
		return;
	}
	p_to = std::clamp(p_to, 0, n - 1);
	if (p_from == p_to) {
		return;
	}
	Variant v = arr[p_from];
	arr.remove_at(p_from);
	arr.insert(p_to, v);
}

} // namespace

void NovaLwfData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &NovaLwfData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name"), &NovaLwfData::open_from_resource_root);
	ClassDB::bind_method(D_METHOD("load_bytes", "bytes"), &NovaLwfData::load_bytes);
	ClassDB::bind_method(D_METHOD("create_empty"), &NovaLwfData::create_empty);
	ClassDB::bind_method(D_METHOD("save_file", "path"), &NovaLwfData::save_file);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &NovaLwfData::save_as);
	ClassDB::bind_method(D_METHOD("to_bytes"), &NovaLwfData::to_bytes);

	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaLwfData::is_loaded);
	ClassDB::bind_method(D_METHOD("is_modified"), &NovaLwfData::is_modified);
	ClassDB::bind_method(D_METHOD("mark_clean"), &NovaLwfData::mark_clean);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaLwfData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaLwfData::get_last_error);

	ClassDB::bind_method(D_METHOD("get_set_count"), &NovaLwfData::get_set_count);
	ClassDB::bind_method(D_METHOD("get_sets"), &NovaLwfData::get_sets);
	ClassDB::bind_method(D_METHOD("get_set", "set_index"), &NovaLwfData::get_set);
	ClassDB::bind_method(D_METHOD("get_layer_count", "set_index"), &NovaLwfData::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "set_index", "layer_index"), &NovaLwfData::get_layer);
	ClassDB::bind_method(D_METHOD("get_member_count", "set_index", "layer_index"), &NovaLwfData::get_member_count);
	ClassDB::bind_method(D_METHOD("get_member", "set_index", "layer_index", "member_index"), &NovaLwfData::get_member);

	ClassDB::bind_method(D_METHOD("set_set_field", "set_index", "key", "value"), &NovaLwfData::set_set_field);
	ClassDB::bind_method(D_METHOD("set_layer_field", "set_index", "layer_index", "key", "value"), &NovaLwfData::set_layer_field);
	ClassDB::bind_method(D_METHOD("set_member_field", "set_index", "layer_index", "member_index", "key", "value"), &NovaLwfData::set_member_field);

	ClassDB::bind_method(D_METHOD("add_set"), &NovaLwfData::add_set);
	ClassDB::bind_method(D_METHOD("remove_set", "set_index"), &NovaLwfData::remove_set);
	ClassDB::bind_method(D_METHOD("move_set", "from", "to"), &NovaLwfData::move_set);
	ClassDB::bind_method(D_METHOD("add_layer", "set_index"), &NovaLwfData::add_layer);
	ClassDB::bind_method(D_METHOD("remove_layer", "set_index", "layer_index"), &NovaLwfData::remove_layer);
	ClassDB::bind_method(D_METHOD("move_layer", "set_index", "from", "to"), &NovaLwfData::move_layer);
	ClassDB::bind_method(D_METHOD("add_member", "set_index", "layer_index"), &NovaLwfData::add_member);
	ClassDB::bind_method(D_METHOD("remove_member", "set_index", "layer_index", "member_index"), &NovaLwfData::remove_member);
	ClassDB::bind_method(D_METHOD("move_member", "set_index", "layer_index", "from", "to"), &NovaLwfData::move_member);

	BIND_CONSTANT(SELECTION_FIRST);
	BIND_CONSTANT(SELECTION_RANDOM);
	BIND_CONSTANT(SELECTION_SEQUENTIAL);
	BIND_CONSTANT(SELECTION_RANDOM_SEQ);
	BIND_CONSTANT(FLAG_HEADING);
	BIND_CONSTANT(FLAG_INTERNAL);
	BIND_CONSTANT(FLAG_EXTERNAL);
	BIND_CONSTANT(FLAG_RANDOM);
	BIND_CONSTANT(FLAG_SEQUENTIAL);
	BIND_CONSTANT(FLAG_STOPPABLE);
	BIND_CONSTANT(FLAG_PRELOAD);
	BIND_CONSTANT(FLAG_RANDOM_SEQUENTIAL);
	BIND_CONSTANT(FLAG_DIRECTIONAL);
	BIND_CONSTANT(FLAG_LOOPING);
	BIND_CONSTANT(FLAG_REVERB);
	BIND_CONSTANT(FLAG_RAPID);
}

// ---------------------------------------------------------------- decode/encode

bool NovaLwfData::decode_into_tree(const PackedByteArray &bytes) {
	opennova::lwf::File f;
	std::string err;
	if (!opennova::lwf::parse_lwf_buffer(bytes.ptr(), static_cast<size_t>(bytes.size()), f, err)) {
		last_error_ = String(err.c_str());
		return false;
	}

	Array sets;
	for (const auto &multi : f.multis) {
		Dictionary set;
		set["name"] = String::utf8(multi.name.c_str());
		set["target_id"] = static_cast<int64_t>(multi.target_id);
		set["pitch_base"] = static_cast<int64_t>(multi.pitch_base);
		set["pitch_random_range"] = static_cast<int64_t>(multi.pitch_random_range);
		set["set_flags"] = static_cast<int64_t>(multi.set_flags);

		Array layers;
		for (uint32_t pidx : multi.playlist_indices) {
			if (pidx >= f.playlists.size()) {
				continue;
			}
			const auto &pl = f.playlists[pidx];

			Dictionary layer;
			layer["selection_mode"] = selection_mode_from_flags(pl.flags);
			layer["falloff_radius"] = static_cast<int>(pl.falloff_radius);
			layer["min_distance"] = static_cast<int>(pl.min_distance);
			layer["looping"] = (pl.flags & opennova::lwf::kFlagLooping) != 0;
			layer["directional"] = (pl.flags & opennova::lwf::kFlagDirectional) != 0;
			layer["heading"] = (pl.flags & opennova::lwf::kFlagHeading) != 0;
			layer["preload"] = (pl.flags & opennova::lwf::kFlagPreload) != 0;
			layer["stoppable"] = (pl.flags & opennova::lwf::kFlagStoppable) != 0;
			layer["internal"] = (pl.flags & opennova::lwf::kFlagInternal) != 0;
			layer["external"] = (pl.flags & opennova::lwf::kFlagExternal) != 0;
			layer["reverb"] = (pl.flags & opennova::lwf::kFlagReverb) != 0;
			layer["rapid"] = (pl.flags & opennova::lwf::kFlagRapid) != 0;

			Array members;
			for (uint32_t sidx : pl.sndparm_indices) {
				if (sidx >= f.sndparms.size()) {
					continue;
				}
				const auto &sp = f.sndparms[sidx];

				Dictionary member;
				String name;
				String path;
				int value_hi = 0;
				if (sp.single_index < f.singles.size()) {
					const auto &sg = f.singles[sp.single_index];
					name = String::utf8(sg.name.c_str());
					path = String::utf8(sg.path.c_str());
					value_hi = static_cast<int>(sg.value_hi);
				}
				member["name"] = name;
				member["wav_path"] = path;
				member["value_hi"] = value_hi;
				member["base_pitch"] = static_cast<double>(sp.pitch_scaled) / kPitchScale;
				member["rand_pitch"] = static_cast<double>(sp.random_pitch_scaled) / kPitchScale;
				member["volume"] = static_cast<int>(sp.volume);
				member["clamp_volume"] = static_cast<int>(sp.clamp_volume);
				members.push_back(member);
			}
			layer["members"] = members;
			layers.push_back(layer);
		}
		set["layers"] = layers;
		sets.push_back(set);
	}

	sets_ = sets;
	original_bytes_ = bytes;
	loaded_ = true;
	modified_ = false;
	return true;
}

PackedByteArray NovaLwfData::encode_current(String &r_error) const {
	if (!modified_ && !original_bytes_.is_empty()) {
		return original_bytes_;
	}

	// Re-normalize the editable tree into a canonical opennova::lwf::File:
	// dedup the shared singles table, then rebuild multis/playlists/sndparms.
	opennova::lwf::File f;

	struct SingleKey {
		std::string name;
		std::string path;
		bool operator<(const SingleKey &o) const {
			if (name != o.name) {
				return name < o.name;
			}
			return path < o.path;
		}
	};
	std::map<SingleKey, uint32_t> key_to_single;

	auto intern_single = [&](const Dictionary &member) -> uint32_t {
		const std::string name = String(member.get("name", String())).utf8().get_data();
		const std::string path = String(member.get("wav_path", String())).utf8().get_data();
		SingleKey key{ name, path };
		auto it = key_to_single.find(key);
		if (it != key_to_single.end()) {
			return it->second;
		}
		const uint32_t idx = static_cast<uint32_t>(f.singles.size());
		key_to_single[key] = idx;
		opennova::lwf::Single single;
		single.name = name;
		single.path = path;
		single.value_hi = static_cast<uint16_t>(static_cast<int>(member.get("value_hi", 0)));
		single.path_offset = 0; // encode_lwf recomputes.
		f.singles.push_back(std::move(single));
		return idx;
	};

	// First pass: intern all singles so indices are stable.
	for (int si = 0; si < sets_.size(); ++si) {
		Dictionary set = sets_[si];
		Array layers = set.get("layers", Array());
		for (int li = 0; li < layers.size(); ++li) {
			Dictionary layer = layers[li];
			Array members = layer.get("members", Array());
			for (int mi = 0; mi < members.size(); ++mi) {
				Dictionary member = members[mi];
				intern_single(member);
			}
		}
	}

	// Second pass: build the multi/playlist/sndparm tables.
	for (int si = 0; si < sets_.size(); ++si) {
		Dictionary set = sets_[si];
		opennova::lwf::Multi multi;
		multi.name = String(set.get("name", String())).utf8().get_data();
		multi.pitch_base = static_cast<uint32_t>(static_cast<int64_t>(set.get("pitch_base", 0xFFFF)));
		multi.pitch_random_range = static_cast<uint32_t>(static_cast<int64_t>(set.get("pitch_random_range", 0)));
		multi.target_id = static_cast<uint32_t>(static_cast<int64_t>(set.get("target_id", 0)));
		multi.set_flags = static_cast<uint32_t>(static_cast<int64_t>(set.get("set_flags", 0)));

		Array layers = set.get("layers", Array());
		for (int li = 0; li < layers.size(); ++li) {
			Dictionary layer = layers[li];
			opennova::lwf::Playlist playlist;
			playlist.falloff_radius = static_cast<uint16_t>(static_cast<int>(layer.get("falloff_radius", 0)));
			playlist.min_distance = static_cast<uint16_t>(static_cast<int>(layer.get("min_distance", 0)));
			playlist.flags = flags_from_layer(layer);

			Array members = layer.get("members", Array());
			for (int mi = 0; mi < members.size(); ++mi) {
				Dictionary member = members[mi];
				opennova::lwf::Sndparm sndparm;
				sndparm.single_index = intern_single(member);
				const double base_pitch = static_cast<double>(member.get("base_pitch", 1.0));
				const double rand_pitch = static_cast<double>(member.get("rand_pitch", 0.0));
				sndparm.pitch_scaled = static_cast<uint32_t>(std::max(0.0, base_pitch * kPitchScale));
				sndparm.random_pitch_scaled = static_cast<uint32_t>(std::max(0.0, rand_pitch * kPitchScale));
				sndparm.volume = static_cast<uint32_t>(static_cast<int>(member.get("volume", 255)));
				sndparm.clamp_volume = static_cast<uint32_t>(static_cast<int>(member.get("clamp_volume", 255)));
				playlist.sndparm_indices.push_back(static_cast<uint32_t>(f.sndparms.size()));
				f.sndparms.push_back(std::move(sndparm));
			}

			multi.playlist_indices.push_back(static_cast<uint32_t>(f.playlists.size()));
			f.playlists.push_back(std::move(playlist));
		}
		f.multis.push_back(std::move(multi));
	}

	std::vector<uint8_t> out;
	std::string err;
	if (!opennova::lwf::encode_lwf(f, out, err)) {
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

Error NovaLwfData::open_file(const String &p_path) {
	last_error_ = String();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		last_error_ = "Cannot open file: " + p_path;
		return ERR_CANT_OPEN;
	}
	PackedByteArray bytes = file->get_buffer(file->get_length());
	file->close();
	if (!decode_into_tree(bytes)) {
		return ERR_FILE_CORRUPT;
	}
	source_path_ = p_path;
	return OK;
}

Error NovaLwfData::open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
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
	if (!decode_into_tree(bytes)) {
		return ERR_CANT_OPEN;
	}
	source_path_ = file;
	return OK;
}

bool NovaLwfData::load_bytes(const PackedByteArray &p_bytes) {
	last_error_ = String();
	return decode_into_tree(p_bytes);
}

void NovaLwfData::create_empty() {
	sets_ = Array();
	original_bytes_ = PackedByteArray();
	source_path_ = String();
	last_error_ = String();
	loaded_ = true;
	modified_ = true;
}

Error NovaLwfData::save_as(const String &p_path) {
	String err;
	PackedByteArray bytes = encode_current(err);
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
	original_bytes_ = bytes;
	modified_ = false;
	return OK;
}

Error NovaLwfData::save_file(const String &p_path) {
	return save_as(p_path);
}

PackedByteArray NovaLwfData::to_bytes() const {
	String err;
	return encode_current(err);
}

// ----------------------------------------------------------------------- live refs

Dictionary NovaLwfData::set_ref(int p_si) const {
	if (p_si < 0 || p_si >= sets_.size()) {
		return Dictionary();
	}
	return sets_[p_si];
}

Dictionary NovaLwfData::layer_ref(int p_si, int p_li) const {
	Dictionary set = set_ref(p_si);
	Array layers = set.get("layers", Array());
	if (p_li < 0 || p_li >= layers.size()) {
		return Dictionary();
	}
	return layers[p_li];
}

Dictionary NovaLwfData::member_ref(int p_si, int p_li, int p_mi) const {
	Dictionary layer = layer_ref(p_si, p_li);
	Array members = layer.get("members", Array());
	if (p_mi < 0 || p_mi >= members.size()) {
		return Dictionary();
	}
	return members[p_mi];
}

// --------------------------------------------------------------------------- read

int NovaLwfData::get_set_count() const {
	return sets_.size();
}

Array NovaLwfData::get_sets() const {
	return sets_.duplicate(true);
}

Dictionary NovaLwfData::get_set(int p_si) const {
	return set_ref(p_si).duplicate(true);
}

int NovaLwfData::get_layer_count(int p_si) const {
	Dictionary set = set_ref(p_si);
	Array layers = set.get("layers", Array());
	return layers.size();
}

Dictionary NovaLwfData::get_layer(int p_si, int p_li) const {
	return layer_ref(p_si, p_li).duplicate(true);
}

int NovaLwfData::get_member_count(int p_si, int p_li) const {
	Dictionary layer = layer_ref(p_si, p_li);
	Array members = layer.get("members", Array());
	return members.size();
}

Dictionary NovaLwfData::get_member(int p_si, int p_li, int p_mi) const {
	return member_ref(p_si, p_li, p_mi).duplicate(true);
}

// ------------------------------------------------------------------- scalar edits

void NovaLwfData::set_set_field(int p_si, const String &p_key, const Variant &p_value) {
	Dictionary set = set_ref(p_si);
	if (set.is_empty() || p_key == "layers") {
		return;
	}
	set[p_key] = p_value;
	modified_ = true;
}

void NovaLwfData::set_layer_field(int p_si, int p_li, const String &p_key, const Variant &p_value) {
	Dictionary layer = layer_ref(p_si, p_li);
	if (layer.is_empty() || p_key == "members") {
		return;
	}
	layer[p_key] = p_value;
	modified_ = true;
}

void NovaLwfData::set_member_field(int p_si, int p_li, int p_mi, const String &p_key, const Variant &p_value) {
	Dictionary member = member_ref(p_si, p_li, p_mi);
	if (member.is_empty()) {
		return;
	}
	member[p_key] = p_value;
	modified_ = true;
}

// --------------------------------------------------------------- structural edits

int NovaLwfData::add_set() {
	sets_.push_back(make_set());
	modified_ = true;
	return sets_.size() - 1;
}

void NovaLwfData::remove_set(int p_si) {
	if (p_si < 0 || p_si >= sets_.size()) {
		return;
	}
	sets_.remove_at(p_si);
	modified_ = true;
}

void NovaLwfData::move_set(int p_from, int p_to) {
	array_move(sets_, p_from, p_to);
	modified_ = true;
}

int NovaLwfData::add_layer(int p_si) {
	Dictionary set = set_ref(p_si);
	if (set.is_empty()) {
		return -1;
	}
	Array layers = set.get("layers", Array());
	layers.push_back(make_layer());
	modified_ = true;
	return layers.size() - 1;
}

void NovaLwfData::remove_layer(int p_si, int p_li) {
	Dictionary set = set_ref(p_si);
	Array layers = set.get("layers", Array());
	if (p_li < 0 || p_li >= layers.size()) {
		return;
	}
	layers.remove_at(p_li);
	modified_ = true;
}

void NovaLwfData::move_layer(int p_si, int p_from, int p_to) {
	Dictionary set = set_ref(p_si);
	Array layers = set.get("layers", Array());
	array_move(layers, p_from, p_to);
	modified_ = true;
}

int NovaLwfData::add_member(int p_si, int p_li) {
	Dictionary layer = layer_ref(p_si, p_li);
	if (layer.is_empty()) {
		return -1;
	}
	Array members = layer.get("members", Array());
	members.push_back(make_member());
	modified_ = true;
	return members.size() - 1;
}

void NovaLwfData::remove_member(int p_si, int p_li, int p_mi) {
	Dictionary layer = layer_ref(p_si, p_li);
	Array members = layer.get("members", Array());
	if (p_mi < 0 || p_mi >= members.size()) {
		return;
	}
	members.remove_at(p_mi);
	modified_ = true;
}

void NovaLwfData::move_member(int p_si, int p_li, int p_from, int p_to) {
	Dictionary layer = layer_ref(p_si, p_li);
	Array members = layer.get("members", Array());
	array_move(members, p_from, p_to);
	modified_ = true;
}

} // namespace godot
