#include "audio/sound_bank.h"

#include "lwf/lwf_data.h"
#include "lwf/wav_loader.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/audio/ambient_mixer.h>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

const opennova::lwf::File &empty_bank() {
	static const opennova::lwf::File empty;
	return empty;
}

} // namespace

void SoundBank::_bind_methods() {
	ClassDB::bind_static_method("SoundBank", D_METHOD("create", "resource_root"), &SoundBank::create);
	ClassDB::bind_method(D_METHOD("set_occlusion_provider", "provider"), &SoundBank::set_occlusion_provider);
	ClassDB::bind_method(D_METHOD("set_occlusion_override", "override"), &SoundBank::set_occlusion_override);
	ClassDB::bind_method(D_METHOD("add_bank", "lwf"), &SoundBank::add_bank);
	ClassDB::bind_method(D_METHOD("has_set", "name"), &SoundBank::has_set);
	ClassDB::bind_method(D_METHOD("get_set_names"), &SoundBank::get_set_names);
	ClassDB::bind_method(D_METHOD("reset_oneshots", "parent"), &SoundBank::reset_oneshots);
	ClassDB::bind_method(D_METHOD("describe_ambient", "name"), &SoundBank::describe_ambient);
	ClassDB::bind_method(D_METHOD("resolve_ambient_stream", "layer"), &SoundBank::resolve_ambient_stream);
	ClassDB::bind_method(D_METHOD("spawn_ambient", "parent", "world_pos", "name", "bus"),
			&SoundBank::spawn_ambient);
	ClassDB::bind_method(
			D_METHOD("play_oneshot_3d", "parent", "world_pos", "name", "bus", "listener_pos",
					"source_bms_id", "sound_id"),
			&SoundBank::play_oneshot_3d, DEFVAL(Vector3(INFINITY, INFINITY, INFINITY)), DEFVAL(0),
			DEFVAL(0));
	ClassDB::bind_method(D_METHOD("spawn_oneshot_2d", "parent", "name", "bus"),
			&SoundBank::spawn_oneshot_2d);
	ClassDB::bind_static_method("SoundBank", D_METHOD("volume_db_from_255", "vol255"),
			&SoundBank::volume_db_from_255);
	ClassDB::bind_static_method("SoundBank",
			D_METHOD("calc_distance_volume", "dist_q16", "radius_q16", "vol255", "clamp_vol"),
			&SoundBank::calc_distance_volume);
	ClassDB::bind_static_method("SoundBank",
			D_METHOD("emitter_layer_volume", "dist_q16", "falloff_u", "min_u", "vol_byte",
					"member_vol", "clamp_vol"),
			&SoundBank::emitter_layer_volume);
	ClassDB::bind_method(D_METHOD("oneshot_distance_volume", "dist_q16", "layer"),
			&SoundBank::oneshot_distance_volume);
	BIND_CONSTANT(VOLUME_BYTE_MAX);
}

Ref<SoundBank> SoundBank::create(const Ref<ResourceRoot> &p_resource_root) {
	Ref<SoundBank> bank;
	bank.instantiate();
	bank->resource_root_ = p_resource_root;
	return bank;
}

void SoundBank::set_occlusion_provider(const Ref<Simulation> &p_provider) {
	occlusion_provider_id_ = p_provider.is_valid() ? ObjectID(p_provider->get_instance_id()) : ObjectID();
}

void SoundBank::set_occlusion_override(const Callable &p_override) {
	occlusion_override_ = p_override;
}

void SoundBank::add_bank(const Ref<LwfData> &p_lwf) {
	if (p_lwf.is_null() || !p_lwf->is_loaded()) {
		return;
	}
	const int32_t bank_index = static_cast<int32_t>(banks_.size());
	banks_.push_back(p_lwf->engine_file());
	index_.add_bank(bank_index, banks_.back());
}

bool SoundBank::has_set(const String &p_name) const {
	return _find_set(p_name).valid();
}

PackedStringArray SoundBank::get_set_names() const {
	PackedStringArray out;
	for (const std::string &name : index_.names()) {
		out.push_back(opennova::to_gd(name));
	}
	return out;
}

TypedArray<AmbientLayer> SoundBank::describe_ambient(const String &p_name) {
	TypedArray<AmbientLayer> out;
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid()) {
		return out;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const opennova::lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
	for (uint32_t playlist_index : opennova::audio::set_layers(bank, set)) {
		const opennova::lwf::Playlist &layer = bank.playlists[playlist_index];
		const std::vector<uint32_t> members = opennova::audio::layer_members(bank, layer);
		if (members.empty()) {
			continue;
		}
		const opennova::lwf::Sndparm &member = bank.sndparms[members[0]];
		const String wav_path = _member_wav_path(bank, member);
		if (wav_path.is_empty()) {
			continue;
		}
		// Match _resolve_stream's basename lookup without paying its read/decode
		// cost during mission setup. Corrupt data is rejected lazily and cached
		// if the candidate first reaches the physical channel budget.
		if (resource_root_.is_null() || !resource_root_->has_file(wav_path.get_file())) {
			continue;
		}
		Ref<AmbientLayer> row;
		row.instantiate();
		row->set_wav_path(wav_path);
		row->set_falloff_radius(static_cast<int>(layer.falloff_radius));
		row->set_min_distance(static_cast<int>(layer.min_distance));
		row->set_volume(static_cast<int>(member.volume));
		row->set_clamp_volume(static_cast<int>(member.clamp_volume));
		row->set_base_pitch(_member_base_pitch(member));
		out.push_back(row);
	}
	return out;
}

Ref<AudioStreamWAV> SoundBank::resolve_ambient_stream(const Ref<AmbientLayer> &p_layer) {
	if (p_layer.is_null()) {
		return Ref<AudioStreamWAV>();
	}
	return _resolve_stream(p_layer->get_wav_path());
}

void SoundBank::configure_ambient_player(AudioStreamPlayer3D *p_player,
		const Ref<AudioStreamWAV> &p_stream, const Ref<AmbientLayer> &p_layer,
		const StringName &p_bus) {
	if (p_player == nullptr || p_stream.is_null()) {
		return;
	}
	Ref<AudioStreamWAV> loop_stream = p_stream->duplicate();
	loop_stream->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
	loop_stream->set_loop_begin(0);
	loop_stream->set_loop_end(_stream_frames(loop_stream));
	p_player->set_stream(loop_stream);
	p_player->set_attenuation_model(AudioStreamPlayer3D::ATTENUATION_DISABLED);
	if (p_bus != StringName() && AudioServer::get_singleton()->get_bus_index(p_bus) >= 0) {
		p_player->set_bus(p_bus);
	}
	p_player->set_pitch_scale(effective_base_pitch(p_layer.is_valid() ? p_layer->get_base_pitch() : 1.0));
}

Node3D *SoundBank::spawn_ambient(Node3D *p_parent, const Vector3 &p_world_pos, const String &p_name,
		const StringName &p_bus) {
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid() || p_parent == nullptr) {
		return nullptr;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const opennova::lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
	Node3D *holder = nullptr;
	for (uint32_t playlist_index : opennova::audio::set_layers(bank, set)) {
		const opennova::lwf::Playlist &layer = bank.playlists[playlist_index];
		const std::vector<uint32_t> members = opennova::audio::layer_members(bank, layer);
		if (members.empty()) {
			continue;
		}
		const opennova::lwf::Sndparm &member = bank.sndparms[members[0]];
		const Ref<AudioStreamWAV> stream = _resolve_stream(_member_wav_path(bank, member));
		if (stream.is_null()) {
			continue;
		}
		if (holder == nullptr) {
			holder = memnew(Node3D);
			holder->set_name(vformat("Ambient_%s", p_name));
			holder->set_position(p_world_pos);
			p_parent->add_child(holder);
		}
		AudioStreamPlayer3D *player = _make_player(stream, _member_base_pitch(member), p_bus, true, 0);
		holder->add_child(player);
		player->play();
		// Ambient candidates are data until the top-eight mixer selects them.
		// play() clears stream_paused, so pause only after starting the looping
		// playback, then remove the silent player from SceneTree processing. An
		// inherited AudioStreamPlayer3D keeps an internal physics callback alive
		// even at -80 dB; dense missions otherwise revisit hundreds of silent
		// candidates on every physics catch-up step.
		player->set_stream_paused(true);
		player->set_process_mode(Node::PROCESS_MODE_DISABLED);
	}
	return holder;
}

int64_t SoundBank::occlusion_trampoline(void *p_ctx, const float p_listener[3],
		const float p_source[3], int64_t p_dist_q16, int64_t p_source_id) {
	FireContext *ctx = static_cast<FireContext *>(p_ctx);
	const Vector3 listener(p_listener[0], p_listener[1], p_listener[2]);
	const Vector3 source(p_source[0], p_source[1], p_source[2]);
	if (ctx->sim != nullptr) {
		return ctx->sim->sound_occlusion_distance_q16(listener, source, p_dist_q16,
				static_cast<int>(p_source_id));
	}
	if (ctx->override != nullptr && ctx->override->is_valid()) {
		return static_cast<int64_t>(ctx->override->call(listener, source, p_dist_q16,
				static_cast<int>(p_source_id)));
	}
	return p_dist_q16;
}

uint8_t SoundBank::listener_view_flags() const {
    const auto *sim = Object::cast_to<Simulation>(ObjectDB::get_instance(occlusion_provider_id_));
    return sim != nullptr ? sim->sound_listener_view_flags() : 6;
}

bool SoundBank::play_oneshot_3d(Node3D *p_parent, const Vector3 &p_world_pos, const String &p_name,
		const StringName &p_bus, const Vector3 &p_listener_pos, int p_source_bms_id, int p_sound_id) {
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid() || p_parent == nullptr) {
		return false;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const bool has_listener = p_listener_pos.is_finite();
	const float world[3] = { static_cast<float>(p_world_pos.x), static_cast<float>(p_world_pos.y),
		static_cast<float>(p_world_pos.z) };
	const float listener[3] = { static_cast<float>(p_listener_pos.x),
		static_cast<float>(p_listener_pos.y), static_cast<float>(p_listener_pos.z) };
	// Resolve the provider fresh each fire: a freed provider silently degrades
	// to the override, then to the unoccluded fire, instead of dangling.
	FireContext ctx;
	ctx.sim = Object::cast_to<Simulation>(ObjectDB::get_instance(occlusion_provider_id_));
	ctx.override = &occlusion_override_;
	const bool has_provider = ctx.sim != nullptr || occlusion_override_.is_valid();
	const opennova::audio::OneshotPlan plan = opennova::audio::plan_oneshot_3d(bank, loc, world,
			listener, has_listener, p_source_bms_id, static_cast<uint32_t>(p_sound_id),
			has_provider ? &SoundBank::occlusion_trampoline : nullptr, &ctx, selector_, listener_view_flags());
    return _play_oneshot_plan(p_parent, p_world_pos, bank, plan, p_bus);
}

bool SoundBank::play_oneshot_at_distance(Node3D *p_parent, const Vector3 &p_pan_position,
        const String &p_name, const StringName &p_bus, int64_t p_dist_q16) {
    const opennova::audio::SetLocation loc = _find_set(p_name);
    if (!loc.valid() || p_parent == nullptr) {
        return false;
    }
    const opennova::lwf::File &bank = _bank_at(loc);
    const auto plan = opennova::audio::plan_oneshot_at_distance(bank, loc, p_dist_q16, selector_, listener_view_flags());
    return _play_oneshot_plan(p_parent, p_pan_position, bank, plan, p_bus);
}

namespace {
bool oneshot_is_playing(Object *object) {
	if (auto *voice = Object::cast_to<AudioStreamPlayer3D>(object)) return voice->is_playing();
	if (auto *voice = Object::cast_to<AudioStreamPlayer>(object)) return voice->is_playing();
	return false;
}
void stop_oneshot(Node *node) {
	if (auto *voice = Object::cast_to<AudioStreamPlayer3D>(node)) voice->stop();
	if (auto *voice = Object::cast_to<AudioStreamPlayer>(node)) voice->stop();
	if (node) node->queue_free();
}
} // namespace

void SoundBank::reset_oneshots(Node3D *p_parent) {
    for (size_t slot = 0; slot < oneshots_.size(); ++slot) {
        auto *player = Object::cast_to<Node>(ObjectDB::get_instance(oneshots_[slot]));
        if (player && !player->is_queued_for_deletion() &&
                (!p_parent || player->get_parent() != p_parent)) continue;
        if (player && !player->is_queued_for_deletion()) stop_oneshot(player);
        oneshots_[slot] = ObjectID();
        oneshot_pool_.release(slot);
    }
    // The per-layer selection state (playlist record cursor +2, anchor +12,
    // cycle bit 0x100) survives a round restart: Game_RestartRoundSP @0x5263A0
    // re-enters Game_StartMission, whose bank loop (Game_StartMission @0x52544A
    // -> SoundBank_LoadIfExists @0x527530) returns early on an already loaded slot
    // [orig: SoundBank_OpenFile @0x75CAA5]; only Game_TeardownMission @0x522600
    // -> sub_527890 frees the banks. The selector therefore keeps its cursors.
}

bool SoundBank::_play_oneshot_plan(Node *p_parent, const Vector3 &p_world_pos,
        const opennova::lwf::File &p_bank, const opennova::audio::OneshotPlan &p_plan,
        const StringName &p_bus, bool p_interface) {
    if (!p_plan.in_range) {
        return false;
    }
    for (size_t slot = 0; slot < oneshots_.size(); ++slot) {
        auto *player = Object::cast_to<Node>(ObjectDB::get_instance(oneshots_[slot]));
        if (!player || player->is_queued_for_deletion() || !oneshot_is_playing(player)) {
            oneshot_pool_.release(slot);
            oneshots_[slot] = ObjectID();
        }
    }
    bool played = false;
    for (const opennova::audio::OneshotVoice &voice : p_plan.voices) {
        const opennova::lwf::Sndparm &member = p_bank.sndparms[voice.sndparm];
		const Ref<AudioStreamWAV> stream = _resolve_stream(_member_wav_path(p_bank, member));
		if (stream.is_null()) {
			continue;
		}
		// The plan's own-channel key: the same wave for the same source retakes
		// its live channel (stopped below) instead of stealing another.
		const int slot = oneshot_pool_.acquire(stream->get_instance_id(),
				static_cast<uint8_t>(voice.vol255), p_plan.sound_id);
		if (slot < 0) continue;
		stop_oneshot(Object::cast_to<Node>(ObjectDB::get_instance(oneshots_[slot])));
		const double pitch = opennova::lwf::pitch_from_q16(voice.pitch_q16);
		if (p_interface) {
			auto *player = memnew(AudioStreamPlayer);
			player->set_stream(stream);
			player->set_pitch_scale(effective_base_pitch(pitch));
			player->set_volume_db(volume_db_from_255(voice.vol255));
			if (p_bus != StringName() && AudioServer::get_singleton()->get_bus_index(p_bus) >= 0)
				player->set_bus(p_bus);
			oneshots_[slot] = ObjectID(player->get_instance_id());
			p_parent->add_child(player);
			player->connect("finished", Callable(player, "queue_free"));
			player->play();
		} else {
			AudioStreamPlayer3D *player = _make_player(stream, pitch, p_bus, false, voice.vol255);
			player->set_position(p_world_pos);
			oneshots_[slot] = ObjectID(player->get_instance_id());
			p_parent->add_child(player);
			player->connect("finished", Callable(player, "queue_free"));
			player->play();
		}
		played = true;
	}
	return played;
}

bool SoundBank::play_interface_oneshot(Node *parent, const String &name, const StringName &bus) {
	const auto loc = _find_set(name);
	if (!loc.valid() || !parent) return false;
	const auto &bank = _bank_at(loc);
	const auto plan = opennova::audio::plan_oneshot_at_distance(bank, loc, 0, selector_, listener_view_flags());
	return _play_oneshot_plan(parent, {}, bank, plan, bus, true);
}

AudioStreamPlayer *SoundBank::spawn_oneshot_2d(Node *p_parent, const String &p_name,
		const StringName &p_bus) {
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid() || p_parent == nullptr) {
		return nullptr;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const opennova::lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
	const std::vector<uint32_t> layers = opennova::audio::set_layers(bank, set);
	for (size_t li = 0; li < layers.size(); ++li) {
		// The pick advances the selector per layer tried, up to the first
		// layer whose wave resolves (the layers after it are never picked).
		const int32_t member_ordinal = opennova::audio::pick_layer_member(bank, loc,
				static_cast<int32_t>(li), layers[li], selector_);
		if (member_ordinal < 0) {
			continue;
		}
		const std::vector<uint32_t> members = opennova::audio::layer_members(bank,
				bank.playlists[layers[li]]);
		const opennova::lwf::Sndparm &member = bank.sndparms[members[static_cast<size_t>(member_ordinal)]];
		const Ref<AudioStreamWAV> stream = _resolve_stream(_member_wav_path(bank, member));
		if (stream.is_null()) {
			continue;
		}
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		if (p_bus != StringName() && AudioServer::get_singleton()->get_bus_index(p_bus) >= 0) {
			player->set_bus(p_bus);
		}
		// Dialog never enters the trigger-set player: the engine resolves the line
		// to one wave entry by name and plays it at the dialog module's fixed
		// frequency, with no set/member pitch composition and none of its two
		// ROL3 draws; the per-layer member pick above still draws for random
		// layers (docs/audio/lwf-dbf-sound-re.md, Dialog_LoadAudioClip).
		player->set_pitch_scale(effective_base_pitch(_member_base_pitch(member)));
		player->set_volume_db(volume_db_from_255(static_cast<int>(member.volume)));
		player->set_stream(stream);
		p_parent->add_child(player);
		player->play();
		return player;
	}
	return nullptr;
}

std::optional<opennova::world::ScriptVoiceChannel::SetSelection>
SoundBank::select_radio_set(const std::string &name, uint8_t listener_view_flags,
		bool p_bank_member) {
	const auto loc = index_.find(name);
	if (!loc.valid()) return std::nullopt;
	const auto &bank = _bank_at(loc);
	return p_bank_member
			? opennova::audio::select_entity_voice(bank, loc, selector_, listener_view_flags)
			: opennova::audio::select_radio_voice(bank, loc, selector_, listener_view_flags);
}

// --- Internals ---

opennova::audio::SetLocation SoundBank::_find_set(const String &p_name) const {
	return index_.find(opennova::to_std(p_name));
}

const opennova::lwf::File &SoundBank::_bank_at(const opennova::audio::SetLocation &p_loc) const {
	if (!p_loc.valid() || static_cast<size_t>(p_loc.bank) >= banks_.size()) {
		return empty_bank();
	}
	return banks_[static_cast<size_t>(p_loc.bank)];
}

String SoundBank::_member_wav_path(const opennova::lwf::File &p_bank,
		const opennova::lwf::Sndparm &p_member) {
	if (p_member.single_index >= p_bank.singles.size()) {
		return String();
	}
	return opennova::to_gd(p_bank.singles[p_member.single_index].path);
}

double SoundBank::_member_base_pitch(const opennova::lwf::Sndparm &p_member) {
	// Q16 (0x10000 = 1.0), the one engine pitch scale (lwf.h).
	return opennova::lwf::pitch_from_q16(p_member.pitch_scaled);
}

AudioStreamPlayer3D *SoundBank::_make_player(const Ref<AudioStreamWAV> &p_stream, double p_base_pitch,
		const StringName &p_bus, bool p_loop, int p_vol255) {
	AudioStreamPlayer3D *player = memnew(AudioStreamPlayer3D);
	// Loop the stream copy (not the cached one's loop flag for one-shots): duplicate
	// so the looping ambient flag never leaks into a shared cached one-shot.
	Ref<AudioStreamWAV> stream = p_stream;
	if (p_loop) {
		stream = p_stream->duplicate();
		stream->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
		stream->set_loop_begin(0);
		// loop_end is an absolute frame index and playback wraps the moment it is
		// reached -- 0 does NOT mean "whole stream", it pins the voice at sample 0
		// forever (constant DC = silence). Loop the full decoded buffer.
		stream->set_loop_end(_stream_frames(stream));
	}
	player->set_stream(stream);
	// The witnessed distance model owns volume (the engine computes a 0..255
	// channel volume from the two layer radii; Godot must not attenuate on top
	// of it -- its inverse-distance curve AMPLIFIES inside unit_size, which is
	// how a marker bed drowned the mission dialog). Positional panning stays
	// (the shell's approximation of the bearing-byte pan [orig: @ 0x5289c2]).
	player->set_attenuation_model(AudioStreamPlayer3D::ATTENUATION_DISABLED);
	// Only route to a bus that actually exists; otherwise keep the default (Master)
	// so a missing/renamed bus can never silence the voice.
	if (p_bus != StringName() && AudioServer::get_singleton()->get_bus_index(p_bus) >= 0) {
		player->set_bus(p_bus);
	}
	player->set_pitch_scale(effective_base_pitch(p_base_pitch));
	player->set_volume_db(volume_db_from_255(p_vol255));
	return player;
}

double SoundBank::effective_base_pitch(double p_base_pitch) {
	return p_base_pitch > 0.01 ? p_base_pitch : 1.0;
}

double SoundBank::volume_db_from_255(int p_vol255) {
	return opennova::audio::volume_db_from_byte(p_vol255);
}

int SoundBank::calc_distance_volume(int64_t p_dist_q16, int64_t p_radius_q16, int p_vol255,
		int p_clamp_vol) {
	return opennova::audio::calc_distance_volume(p_dist_q16, p_radius_q16, p_vol255, p_clamp_vol);
}

int SoundBank::emitter_layer_volume(int64_t p_dist_q16, int p_falloff_u, int p_min_u,
		int p_vol_byte, int p_member_vol, int p_clamp_vol) {
	return opennova::audio::emitter_layer_volume(p_dist_q16, p_falloff_u, p_min_u, p_vol_byte,
			p_member_vol, p_clamp_vol);
}

int SoundBank::oneshot_distance_volume(int64_t p_dist_q16, const Ref<AmbientLayer> &p_layer) const {
	if (p_layer.is_null()) {
		return 0;
	}
	return opennova::audio::oneshot_layer_volume(p_dist_q16,
			static_cast<int64_t>(p_layer->get_min_distance()) << 16,
			static_cast<int64_t>(p_layer->get_falloff_radius()) << 16,
			p_layer->get_volume(), p_layer->get_clamp_volume());
}

// Frame count of a decoded stream, exact from the byte size (get_length() *
// mix_rate re-derives it through a float). WavLoader always emits 16-bit
// PCM; the 8-bit branch is for completeness -- IMA-ADPCM never reaches here
// (the loader decodes it to 16-bit).
int64_t SoundBank::_stream_frames(const Ref<AudioStreamWAV> &p_stream) {
	const int bytes_per_sample = p_stream->get_format() == AudioStreamWAV::FORMAT_16_BITS ? 2 : 1;
	const int bytes_per_frame = bytes_per_sample * (p_stream->is_stereo() ? 2 : 1);
	return p_stream->get_data().size() / bytes_per_frame;
}

Ref<AudioStreamWAV> SoundBank::_resolve_stream(const String &p_wav_path) {
	if (p_wav_path.is_empty()) {
		return Ref<AudioStreamWAV>();
	}
	const String name = p_wav_path.get_file().to_lower();
	if (const Ref<AudioStreamWAV> *cached = wav_cache_.getptr(name)) {
		return *cached;
	}
	Ref<AudioStreamWAV> stream;
	if (resource_root_.is_valid()) {
		const PackedByteArray bytes = resource_root_->read_file(p_wav_path.get_file());
		if (!bytes.is_empty()) {
			stream = WavLoader::from_bytes(bytes);
		}
	}
	wav_cache_[name] = stream;
	return stream;
}
