#include "audio/mission_audio.h"

#include "audio/audio_out.h"
#include "dbf/dbf_data.h"
#include "lwf/lwf_data.h"
#include "mission/mission_data.h"
#include "mission/mission_info.h"
#include "object/item_database.h"
#include "resource_index/resource_root.h"
#include "simulation/present_event_records.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/gameprofile/resource_missing.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <formats/lwf/lwf.h>
#include <formats/lwf/wav_pcm.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/sound_options.h>
#include <runtime/mission/mission_sidecars.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace godot;

namespace {

// The ambient mix's occlusion seam (audio::OcclusionFn): the frame's Simulation
// (the witnessed two-ray LOS), else the test override, else the raw distance.
struct AmbientOcclusion {
	Simulation *sim = nullptr;
	const Callable *override_fn = nullptr;
};

int64_t ambient_occlusion(void *p_ctx, const float p_listener[3], const float p_source[3],
		int64_t p_dist_q16, int64_t p_source_id) {
	const AmbientOcclusion *host = static_cast<const AmbientOcclusion *>(p_ctx);
	const Vector3 listener(p_listener[0], p_listener[1], p_listener[2]);
	const Vector3 source(p_source[0], p_source[1], p_source[2]);
	if (host->sim != nullptr) {
		return host->sim->sound_occlusion_distance_q16(listener, source, p_dist_q16,
				static_cast<int>(p_source_id));
	}
	if (host->override_fn != nullptr && host->override_fn->is_valid()) {
		return static_cast<int64_t>(host->override_fn->call(listener, source, p_dist_q16,
				static_cast<int>(p_source_id)));
	}
	return p_dist_q16;
}

} // namespace

void MissionAudio::_bind_methods() {
	ClassDB::bind_static_method("MissionAudio", D_METHOD("create", "resource_root", "item_db"),
			&MissionAudio::create);
	ClassDB::bind_method(D_METHOD("setup", "mission", "mission_name", "container"), &MissionAudio::setup);
	ClassDB::bind_method(D_METHOD("get_stats"), &MissionAudio::get_stats);
	ClassDB::bind_method(D_METHOD("active_ambient_candidate_ids"), &MissionAudio::active_ambient_candidate_ids);
	ClassDB::bind_method(D_METHOD("ambient_channel_for_candidate", "candidate_id"),
			&MissionAudio::ambient_channel_for_candidate);
	ClassDB::bind_method(D_METHOD("ambient_candidate_wave", "candidate_id"), &MissionAudio::ambient_candidate_wave);
	ClassDB::bind_method(D_METHOD("set_ambient_markers_enabled", "enabled"),
			&MissionAudio::set_ambient_markers_enabled);
	ClassDB::bind_method(D_METHOD("is_dialog_voice_playing"), &MissionAudio::is_dialog_voice_playing);
	ClassDB::bind_method(D_METHOD("dialog_voice_handle"), &MissionAudio::dialog_voice_handle);
	ClassDB::bind_method(D_METHOD("script_voice_handle"), &MissionAudio::script_voice_handle);
	ClassDB::bind_method(D_METHOD("stop_dialog_line"), &MissionAudio::stop_dialog_line);
	ClassDB::bind_method(D_METHOD("channel_volume", "channel"), &MissionAudio::channel_volume);
	ClassDB::bind_method(D_METHOD("channel_pan", "channel"), &MissionAudio::channel_pan);
	ClassDB::bind_method(D_METHOD("channel_play", "channel"), &MissionAudio::channel_play);
	ClassDB::bind_method(D_METHOD("channel_sounding", "channel"), &MissionAudio::channel_sounding);
	ClassDB::bind_method(D_METHOD("get_perf_counters"), &MissionAudio::get_perf_counters);
	ClassDB::bind_method(D_METHOD("get_bank"), &MissionAudio::get_bank);
	ClassDB::bind_method(D_METHOD("apply_sound_emitters", "events"), &MissionAudio::apply_sound_emitters);
	ClassDB::bind_method(D_METHOD("fire_soundset", "name", "world_pos", "source_bms_id", "sound_id"),
			&MissionAudio::fire_soundset, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("ui_soundset", "name"), &MissionAudio::ui_soundset);
	ClassDB::bind_method(D_METHOD("slot_soundset", "name", "world_pos", "source_bms_id", "sound_id"),
			&MissionAudio::slot_soundset, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("play_dialog", "wav_id"), &MissionAudio::play_dialog);
	ClassDB::bind_method(D_METHOD("advance_dialog_tick"), &MissionAudio::advance_dialog_tick);
	ClassDB::bind_method(D_METHOD("resolve_dialog_wave", "wav_id"), &MissionAudio::resolve_dialog_wave);
	ClassDB::bind_method(D_METHOD("play_wac_wave", "filename"), &MissionAudio::play_wac_wave);
	ClassDB::bind_method(D_METHOD("follow_script_voice"), &MissionAudio::follow_script_voice);
	ClassDB::bind_method(D_METHOD("update_script_voice"), &MissionAudio::update_script_voice);
	ClassDB::bind_method(D_METHOD("set_time_of_day_hhmm", "hhmm"), &MissionAudio::set_time_of_day_hhmm);
	ClassDB::bind_method(D_METHOD("advance_ticks", "logic_tick"), &MissionAudio::advance_ticks);
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &MissionAudio::set_simulation);
	ClassDB::bind_method(D_METHOD("set_occlusion_override", "override"), &MissionAudio::set_occlusion_override);
	ClassDB::bind_method(D_METHOD("tick", "camera_pos", "delta"), &MissionAudio::tick, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("teardown"), &MissionAudio::teardown);
	ClassDB::bind_method(D_METHOD("set_world_sound_parked", "parked"),
			&MissionAudio::set_world_sound_parked);
	ClassDB::bind_method(D_METHOD("is_world_sound_parked"), &MissionAudio::is_world_sound_parked);
	ClassDB::bind_method(D_METHOD("recent_fired_soundsets"), &MissionAudio::recent_fired_soundsets);
	BIND_CONSTANT(MIX_CHANNELS);
	BIND_CONSTANT(RECENT_FIRES);
}

MissionAudio::MissionAudio() {
	set_name("MissionAudio");
	last_camera_pos_ = Vector3(INFINITY, INFINITY, INFINITY);
	// The engine's emitters describe their sets through the loaded bank chain.
	ambient_.set_sets(opennova::audio::AmbientSetSource{
			[this](const std::string &p_set) { return bank_.is_valid() && bank_->set_index().has(p_set); },
			[this](const std::string &p_set) {
				return bank_.is_valid() ? bank_->ambient_layer_rows(p_set)
										: std::vector<opennova::audio::AmbientLayerRow>();
			} });
}

MissionAudio *MissionAudio::create(const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db) {
	MissionAudio *audio = memnew(MissionAudio);
	audio->resource_root_ = p_resource_root;
	audio->item_db_ = p_item_db;
	return audio;
}

Ref<Simulation> MissionAudio::_simulation() const {
	return Ref<Simulation>(Object::cast_to<Simulation>(ObjectDB::get_instance(simulation_id_)));
}

bool MissionAudio::_record_fire(const String &p_set_name, const Vector3 &p_world_pos, int p_source_bms_id,
		bool p_slot, bool p_played) {
	// A fixed ring of plain records: the oldest overwritten, nothing made per sound.
	RecentFire &fire = recent_fires_[(recent_fire_head_ + recent_fire_count_) % RECENT_FIRES];
	fire = { p_set_name, p_world_pos, p_source_bms_id, p_slot, p_played };
	if (recent_fire_count_ < RECENT_FIRES) {
		++recent_fire_count_;
	} else {
		recent_fire_head_ = (recent_fire_head_ + 1) % RECENT_FIRES;
	}
	return p_played;
}

TypedArray<FiredSoundset> MissionAudio::recent_fired_soundsets() const {
	TypedArray<FiredSoundset> out;
	for (int i = 0; i < recent_fire_count_; ++i) {
		const RecentFire &fire = recent_fires_[(recent_fire_head_ + i) % RECENT_FIRES];
		Ref<FiredSoundset> fired;
		fired.instantiate();
		fired->set_set_name(fire.set_name);
		fired->set_position(fire.position);
		fired->set_source_bms_id(fire.source_bms_id);
		fired->set_slot(fire.slot);
		fired->set_played(fire.played);
		out.push_back(fired);
	}
	return out;
}

void MissionAudio::_attach_under(Node3D *p_container) {
	if (p_container == nullptr) {
		return;
	}
	Node *parent = get_parent();
	if (parent != p_container) {
		if (parent != nullptr) {
			parent->remove_child(this);
		}
		p_container->add_child(this);
	}
	root_attached_ = true;
}

Ref<MissionAudioStats> MissionAudio::setup(const Ref<MissionData> &p_mission, const String &p_mission_name,
		Node3D *p_container) {
	stats_.instantiate();
	if (p_mission.is_null() || p_container == nullptr || resource_root_.is_null()) {
		return stats_;
	}
	const Ref<MissionInfo> mission_info = p_mission->get_info();
	// A repeated setup is not the normal owner lifecycle, but it must not leave
	// an earlier mission's channels sounding or carry dialog state into the next.
	_reset_ambient_channels();
	_reset_mission_playback_state();
	ambient_.clear();
	warned_ambient_decode_failure_ = false;

	bank_ = SoundBank::create(resource_root_);
	_install_voice_resolver();
	bank_->set_occlusion_provider(_simulation());
	bank_->set_occlusion_override(occlusion_override_);
	const String mission_base = p_mission_name.get_file().get_basename();
	// The global slots in the engine's order -- expansion pair (when one is
	// mounted) ahead of the statics (engine: base/gameprofile/required_resources.cpp). The witnessed table
	// lives native (audio/bank_chain.h); missing files skip like retail's
	// SoundBank_LoadIfExists (D-SND-2 closed). A set is searched for in these
	// slots alone: the mission's own <mission>.lwf is its dialog bank's sounds,
	// loaded below (docs/audio/lwf-dbf-sound-re.md, D-SND-1 fixed).
	const std::vector<std::string> global_chain = opennova::audio::global_bank_chain(
			opennova::to_std(resource_root_->get_expansion()));
	String global_chain_text;
	for (const std::string &global_name : global_chain) {
		if (!global_chain_text.is_empty()) {
			global_chain_text += ", ";
		}
		global_chain_text += opennova::to_gd(global_name);
		_load_bank(opennova::to_gd(global_name));
	}

	// The mission's dialog bank (runtime/mission/mission_sidecars dialog_bank_name: its
	// own <base>.dbf, or the one its header names) maps a PlayWavList dialog id
	// (dlg001) to its lines, each naming a wave of the bank's sounds (<bank>.lwf,
	// else .pwf), both loaded only if the bank is present.
	const opennova::bms::File &mission_file = p_mission->native_file();
	const std::string bank_name = opennova::mission::dialog_bank_name(opennova::to_std(mission_base),
			opennova::strutil::fixed_string(mission_file.header.terrain + 16, 16));
	const String dbf_name = opennova::to_gd(bank_name);
	if (resource_root_->has_file(dbf_name)) {
		Ref<DbfData> dbf;
		dbf.instantiate();
		if (dbf->open_from_resource_root(resource_root_, dbf_name) == OK) {
			dbf_ = dbf;
			stats_->set_dialogs(dbf->get_dialog_count());
			for (const bool alternate : { false, true }) {
				const String sounds = opennova::to_gd(opennova::mission::dialog_sounds_name(bank_name, alternate));
				if (!resource_root_->has_file(sounds)) continue;
				const PackedByteArray bytes = resource_root_->read_file(sounds);
				std::string error;
				dialog_sounds_loaded_ = !bytes.is_empty() &&
						opennova::lwf::parse_lwf_buffer(bytes.ptr(), static_cast<size_t>(bytes.size()), dialog_sounds_, error);
				break;
			}
		}
	}

	_attach_under(p_container);
	AudioOut::attach(this);

	std::vector<opennova::audio::EnvsMarker> marker_rows;
	// S13 (ADR 0028): the faithful envs dispatch + the four soundloop slot
	// names resolve natively over the retained items.def and the mission's
	// bms document (audio/envs_markers.h); the engine's emitters keep the slots
	// whose sets the loaded chain holds (AmbientEmitters::add_envs_markers).
	if (ambient_markers_enabled_ && item_db_.is_valid()) {
		marker_rows = opennova::audio::resolve_envs_markers(
				p_mission->native_file(), item_db_->native_items());
	}
	// A header-only join's document has no entities: its emitters arrive with
	// the host's world stream, built once the joiner is in the match (tick).
	world_envs_pending_ = ambient_markers_enabled_ && item_db_.is_valid() &&
			p_mission->is_wire_header_only();
	ambient_.add_envs_markers(marker_rows);
	_publish_ambient_stats();

	// Silence here has historically gone unnoticed (a bare stats print) -- warn on
	// the two states that mean "no ambience will play" so they surface in logs.
	if (stats_->get_banks_loaded() == 0) {
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: no sound banks loaded (probed %s): mission ambience will be silent",
				global_chain_text));
		// The log line the editor's Play reads back into a Problems row (ADR 0046 DI-27): game.lwf, the
		// global chain's bank the game's own sets live in, which a project makes or imports to be heard.
		ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kFile, "game.lwf", String(),
				vformat("no sound bank loaded (probed %s), so the mission is silent", global_chain_text));
	} else if (stats_->get_markers_total() > 0 && stats_->get_markers_resolved() == 0) {
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: 0/%d sound markers resolved (item db %s): mission ambience will be silent",
				stats_->get_markers_total(), item_db_.is_null() ? String("missing") : String("loaded")));
	}

	ambient_.restart();
	_apply_music(mission_info.is_valid() ? mission_info->get_music() : 0);
	return stats_;
}

TypedArray<int64_t> MissionAudio::active_ambient_candidate_ids() const {
	TypedArray<int64_t> out;
	for (int channel = 0; channel < ambient_.channel_count(); ++channel) {
		const int32_t candidate_id = ambient_.candidate_of(channel);
		if (candidate_id >= 0) {
			out.push_back(static_cast<int64_t>(candidate_id));
		}
	}
	out.sort();
	return out;
}

int MissionAudio::ambient_channel_for_candidate(int p_candidate_id) const {
	const int channel = p_candidate_id < 0 ? -1 : ambient_.channel_of(p_candidate_id);
	return channel < 0 ? -1 : opennova::audio::kFirstAmbientChannel + channel;
}

namespace {

const opennova::audio::ChannelParams *mixer_channel(int p_channel) {
	if (p_channel < 0 || size_t(p_channel) >= opennova::audio::kAudioChannelCount) return nullptr;
	return &AudioOut::mixer().channel(p_channel);
}

} // namespace

int MissionAudio::channel_volume(int p_channel) const {
	const opennova::audio::ChannelParams *params = mixer_channel(p_channel);
	return params ? int(params->volume) : 0;
}

int MissionAudio::channel_pan(int p_channel) const {
	const opennova::audio::ChannelParams *params = mixer_channel(p_channel);
	return params ? int(params->pan) : 0;
}

int64_t MissionAudio::channel_play(int p_channel) const {
	const opennova::audio::ChannelParams *params = mixer_channel(p_channel);
	return params ? int64_t(params->play_q16) : 0;
}

bool MissionAudio::channel_sounding(int p_channel) const {
	const opennova::audio::ChannelParams *params = mixer_channel(p_channel);
	return params && params->wave && params->wave != opennova::audio::silent_mix_wave();
}

String MissionAudio::ambient_candidate_wave(int p_candidate_id) const {
	const opennova::audio::AmbientCandidateRef *candidate = ambient_.candidate(p_candidate_id);
	return candidate != nullptr ? opennova::to_gd(candidate->wave) : String();
}

void MissionAudio::set_ambient_markers_enabled(bool p_enabled) {
	ambient_markers_enabled_ = p_enabled;
}

bool MissionAudio::is_dialog_voice_playing() const {
	return dialog_handle_ != 0 && AudioOut::mixer().validate(dialog_handle_) != 0;
}

Ref<MissionAudioPerf> MissionAudio::get_perf_counters() const {
	int active_channels = 0;
	for (int channel = 0; channel < ambient_.channel_count(); ++channel) {
		if (ambient_.candidate_of(channel) >= 0) {
			active_channels += 1;
		}
	}
	Ref<MissionAudioPerf> perf;
	perf.instantiate();
	perf->set_tick_us(perf_tick_us_);
	perf->set_markers(perf_markers_);
	perf->set_voice_writes(perf_voice_writes_);
	perf->set_physical_channels(static_cast<int64_t>(ambient_.channel_count()));
	perf->set_active_channels(active_channels);
	perf->set_ambient_decode_failures(static_cast<int64_t>(ambient_.failed_count()));
	return perf;
}

void MissionAudio::apply_sound_emitters(const Array &p_events) {
	std::vector<opennova::world::SoundEmitterEvent> events;
	events.reserve(static_cast<size_t>(p_events.size()));
	for (int64_t i = 0; i < p_events.size(); ++i) {
		const Ref<SoundEmitterRow> event = p_events[i];
		if (event.is_valid()) {
			events.push_back(event->value());
		}
	}
	apply_sound_emitter_events(events);
}

void MissionAudio::apply_sound_emitter_events(
		const std::vector<opennova::world::SoundEmitterEvent> &p_events) {
	if (bank_.is_null()) {
		return;
	}
	ambient_.queue_emitters(p_events);
}

void MissionAudio::play_weather_sounds(
		const std::vector<opennova::world::WeatherSoundEvent> &p_events, const Transform3D &) {
	for (const auto &event : p_events) {
		_play_listener_relative(opennova::world::kThunderSoundSet, event.distance_q16, event.bearing);
	}
}

void MissionAudio::play_script_sounds(
		const std::vector<opennova::world::ScriptSoundEvent> &p_events, const Transform3D &) {
	for (const auto &event : p_events) {
		if (event.kind == opennova::world::ScriptSoundEvent::Kind::Interface)
			ui_soundset(opennova::to_gd(event.name));
		else
			_play_listener_relative(opennova::to_gd(event.name), event.distance_q16, event.bearing);
	}
}

void MissionAudio::_play_listener_relative(const String &p_name, int32_t p_distance_q16, int32_t p_bearing) {
	if (bank_.is_null() || !root_attached_) {
		return;
	}
	// The raw bearing and distance straight to the trigger player (SoundBank::play_oneshot_at_distance);
	// the native plan owns attenuation.
	_record_fire(p_name, last_camera_pos_, 0, false,
			bank_->play_oneshot_at_distance(p_name, p_distance_q16, static_cast<uint8_t>(p_bearing)));
}

void MissionAudio::shutdown_audio_channels() {
	// Every channel opened on the silent buffer (engine audio::Mixer::init_all) and the emitter
	// handles dropped; the dialog queue and the scripted voice see their handles stop validating.
	AudioOut::mixer().init_all();
	ambient_.release_channels();
}

void MissionAudio::set_world_sound_parked(bool p_parked) {
	if (p_parked)
		AudioOut::options().park();
	else
		AudioOut::options().restore();
}

bool MissionAudio::is_world_sound_parked() const {
	return AudioOut::options().parked();
}

void MissionAudio::stop_dialog_line() {
	AudioOut::mixer().reset(dialog_handle_);
}

// The local player's own action-slot presentation arrives as source -1 with no
// key (player_weapon_effects); it keys on that marker so its re-fires restart
// their own voice like every other entity's. Everything else keeps the key the
// sim stamped (0 = no identity: the delayed slots, destruction, script plays).
int MissionAudio::_oneshot_sound_id(int p_source_bms_id, int p_sound_id) {
	if (p_sound_id != 0 || p_source_bms_id != -1) {
		return p_sound_id;
	}
	return static_cast<int>(opennova::audio::oneshot_sound_id(
			opennova::audio::kNoSourceHandle, p_source_bms_id));
}

SoundBank::Hearing MissionAudio::_hearing(int p_sound_id) const {
	SoundBank::Hearing hearing;
	hearing.listener = last_camera_pos_.is_finite() ? &listener_ : nullptr;
	const Ref<Simulation> sim = _simulation();
	hearing.ridden = sim.is_valid() && sim->sound_source_ridden(static_cast<uint32_t>(p_sound_id));
	return hearing;
}

bool MissionAudio::fire_soundset(const String &p_name, const Vector3 &p_world_pos, int p_source_bms_id,
		int p_sound_id) {
	if (bank_.is_null() || !root_attached_) {
		return _record_fire(p_name, p_world_pos, p_source_bms_id, false, false);
	}
	const int sound_id = _oneshot_sound_id(p_source_bms_id, p_sound_id);
	return _record_fire(p_name, p_world_pos, p_source_bms_id, false,
			bank_->play_oneshot_3d(p_world_pos, p_name, last_camera_pos_, _hearing(sound_id),
					p_source_bms_id, sound_id));
}

bool MissionAudio::ui_soundset(const String &p_name) {
	if (bank_.is_null() || !root_attached_) {
		return false;
	}
	return bank_->play_interface_oneshot(p_name);
}

bool MissionAudio::slot_soundset(const String &p_name, const Vector3 &p_world_pos,
		int p_source_bms_id, int p_sound_id) {
	if (bank_.is_null() || !root_attached_) {
		return _record_fire(p_name, p_world_pos, p_source_bms_id, true, false);
	}
	const int sound_id = _oneshot_sound_id(p_source_bms_id, p_sound_id);
	return _record_fire(p_name, p_world_pos, p_source_bms_id, true,
			bank_->play_oneshot_3d(p_world_pos, p_name, last_camera_pos_, _hearing(sound_id),
					p_source_bms_id, sound_id));
}

bool MissionAudio::play_dialog(int p_wav_id) {
	const Ref<Simulation> sim = _simulation();
	if (sim.is_null() || !sim->play_dialog(p_wav_id)) {
		UtilityFunctions::push_warning(vformat("MissionAudio: dialog id %d did not play", p_wav_id));
		return false;
	}
	return true;
}

void MissionAudio::advance_dialog_tick() {
	const bool rendered = dialog_frame_rendered_;
	dialog_frame_rendered_ = false;
	const Ref<Simulation> sim = _simulation();
	if (sim.is_null()) {
		return;
	}
	sim->tick_dialog_queue(rendered,
			[this](const opennova::audio::DialogLineRef &p_line, bool p_play) {
				return _load_dialog_line(p_line, p_play);
			},
			[this](uint64_t p_voice) { return _dialog_voice_playing(p_voice); });
}

bool MissionAudio::play_dialog_line(const String &p_dialog_name, int p_line,
		int p_player_class) {
	if (bank_.is_null() || !root_attached_) {
		return false;
	}
	const opennova::dbf::File *dialog_bank =
			(dbf_.is_valid() && dbf_->is_loaded()) ? &dbf_->engine_file() : nullptr;
	const opennova::audio::DialogLinePlayback line = opennova::audio::resolve_dialog_line(
			dialog_bank, dialog_sounds_loaded_ ? &dialog_sounds_ : nullptr, nullptr, nullptr,
			opennova::to_std(p_dialog_name), p_line, p_player_class);
	if (line.file.empty()) {
		return false;
	}
	const uint32_t handle = _play_dialog_wave(_resolve_wav(opennova::to_gd(line.file).get_file()), line.volume);
	if (handle != 0) dialog_handle_ = handle;
	return handle != 0;
}

String MissionAudio::resolve_dialog_wave(int p_wav_id) {
	for (const opennova::audio::DialogLineRef &line : _resolve_dialog_lines(p_wav_id)) {
		if (!line.file.empty()) {
			return opennova::to_gd(line.file);
		}
	}
	return String();
}

std::vector<opennova::audio::DialogLineRef> MissionAudio::_resolve_dialog_lines(
		int p_wav_id) const {
	const opennova::dbf::File *dialog_bank =
			(dbf_.is_valid() && dbf_->is_loaded()) ? &dbf_->engine_file() : nullptr;
	return opennova::audio::resolve_dialog_lines(dialog_bank,
			dialog_sounds_loaded_ ? &dialog_sounds_ : nullptr, p_wav_id);
}

uint32_t MissionAudio::_play_dialog_wave(const opennova::audio::MixWaveRef &p_wave, int p_volume) {
	if (!p_wave) return 0;
	return AudioOut::mixer().open(opennova::audio::kDialogChannel,
			opennova::audio::dialog_line_params(p_wave, p_volume, AudioOut::options().dialogue()));
}

// A line the dialog queue loads: its wave read and decoded through the VFS (a
// missing .wav extension tolerated), what the game's wave loader records of it
// for the line's hold (runtime/audio/dialog_queue dialog_clip_hold) and, when the
// engine's gate lets it play, its wave on the dialog channel at the wave's volume
// byte. The authority's leg (its chat lines and the co-op broadcast) runs after it
// in the engine (Simulation::tick_dialog_queue; docs/audio/lwf-dbf-sound-re.md D-SND-42).
opennova::audio::DialogClip MissionAudio::_load_dialog_line(
		const opennova::audio::DialogLineRef &p_line, bool p_play) {
	opennova::audio::DialogClip clip;
	if (!p_line.file.empty() && root_attached_ && resource_root_.is_valid()) {
		const String name = opennova::to_gd(p_line.file).get_file();
		PackedByteArray bytes = resource_root_->read_file(name);
		if (bytes.is_empty() && !name.to_lower().ends_with(".wav")) {
			bytes = resource_root_->read_file(name + String(".wav"));
		}
		opennova::lwf::WavPcm decoded;
		std::string error;
		if (!bytes.is_empty() && opennova::lwf::wav_decode_pcm16(bytes.ptr(),
					static_cast<size_t>(bytes.size()), decoded, error)) {
			clip.loaded = true;
			clip.samples = decoded.loader_samples;
			clip.pitch_q16 = decoded.loader_pitch_q16;
			if (!p_play) return clip;
			const uint32_t handle = _play_dialog_wave(opennova::audio::mix_wave_from_pcm(decoded), p_line.volume);
			if (handle != 0) {
				dialog_handle_ = handle;
				clip.voice = handle;
			}
		}
	}
	return clip;
}

bool MissionAudio::_dialog_voice_playing(uint64_t p_voice) const {
	// The queue's channel gate (engine audio::DialogQueue::tick).
	return p_voice != 0 && AudioOut::mixer().validate(static_cast<uint32_t>(p_voice)) != 0;
}

bool MissionAudio::play_wac_wave(const String &p_filename) {
	const Ref<Simulation> sim = _simulation();
	if (sim.is_valid()) {
		const bool loaded = sim->play_script_wave(p_filename);
		follow_script_voice();
		return loaded;
	}
	// Standalone tooling shares the scripted voice's interrupt-before-load rule on channel zero
	// (engine world::ScriptVoiceChannel::start).
	AudioOut::mixer().reset(tool_voice_handle_);
	tool_voice_handle_ = 0;
	if (!root_attached_ || resource_root_.is_null() || p_filename.is_empty()) {
		return false;
	}
	const opennova::audio::MixWaveRef wave = _resolve_wav(p_filename);
	if (!wave) {
		UtilityFunctions::push_warning(vformat("MissionAudio: WAC wave '%s' did not resolve", p_filename));
		return false;
	}
	tool_voice_handle_ = AudioOut::mixer().open(opennova::audio::kVoiceChannel,
			opennova::audio::voice_params(wave, 0, opennova::audio::kVolumeByteMax, 0x10000));
	return tool_voice_handle_ != 0;
}

// Resolve + cache a .wav by filename through the VFS (tolerates a missing .wav
// extension). Returns the decoded buffer, or null.
opennova::audio::MixWaveRef MissionAudio::_resolve_wav(const String &p_filename) {
	const std::string key = opennova::to_std(p_filename.to_lower());
	if (const auto cached = wac_waves_.find(key); cached != wac_waves_.end()) {
		return cached->second;
	}
	PackedByteArray bytes = resource_root_->read_file(p_filename);
	if (bytes.is_empty() && !p_filename.to_lower().ends_with(".wav")) {
		bytes = resource_root_->read_file(p_filename + String(".wav"));
	}
	opennova::audio::MixWaveRef wave;
	opennova::lwf::WavPcm pcm;
	std::string error;
	if (!bytes.is_empty() &&
			opennova::lwf::wav_decode_pcm16(bytes.ptr(), static_cast<size_t>(bytes.size()), pcm, error)) {
		wave = opennova::audio::mix_wave_from_pcm(pcm);
	}
	wac_waves_[key] = wave;
	return wave;
}

void MissionAudio::set_time_of_day_hhmm(double p_hhmm) {
	ambient_.set_time_of_day_hhmm(p_hhmm);
}

void MissionAudio::advance_ticks(int64_t p_logic_tick) {
	logic_tick_ = p_logic_tick;
	follow_script_voice();
	ambient_.advance_to_logic_tick(p_logic_tick);
}

void MissionAudio::set_simulation(const Ref<Simulation> &p_sim) {
	const Ref<Simulation> previous = _simulation();
	if (previous.is_valid() && previous != p_sim) previous->set_script_voice_resolver({}, {});
	simulation_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
	_install_voice_resolver();
	if (bank_.is_valid()) {
		bank_->set_occlusion_provider(p_sim);
	}
}

void MissionAudio::set_occlusion_override(const Callable &p_override) {
	occlusion_override_ = p_override;
	if (bank_.is_valid()) {
		bank_->set_occlusion_override(p_override);
	}
}

void MissionAudio::set_listener_view(const Transform3D &p_camera) {
	last_camera_pos_ = p_camera.origin;
	// The view in the mission frame's 16.16 units (presentation (x, y, z) is mission (x, -z, y))
	// and its yaw from its facing, -Z.
	const Vector3 origin = p_camera.origin;
	const int32_t at[3] = { int32_t(origin.x * opennova::io::kFp16One),
		int32_t(-origin.z * opennova::io::kFp16One), int32_t(origin.y * opennova::io::kFp16One) };
	const Vector3 facing = -p_camera.basis.get_column(2);
	const int32_t yaw = opennova::audio::listener_yaw(double(facing.x), double(-facing.z));
	// The listener moves over the frame's logic ticks; with no simulation (a tool) each view
	// places it anew.
	if (_simulation().is_null() || listener_tick_ < 0) {
		listener_.place(at, yaw);
		listener_tick_ = logic_tick_;
		return;
	}
	const int64_t ticks = logic_tick_ - listener_tick_;
	listener_tick_ = logic_tick_;
	listener_.update(at, yaw, ticks > 0 ? static_cast<uint32_t>(ticks) : 0u);
}

void MissionAudio::tick(const Vector3 &p_camera_pos, double p_delta) {
	tick_view(Transform3D(Basis(), p_camera_pos), p_delta);
}

void MissionAudio::tick_view(const Transform3D &p_camera, double p_delta) {
	const uint64_t start = Time::get_singleton()->get_ticks_usec();
	AudioOut::attach(this);
	set_listener_view(p_camera);
	// The frame this pass mixes renders: the next dialog tick starts a fresh
	// line's hold (runtime/audio/dialog_queue DialogQueue::tick).
	dialog_frame_rendered_ = true;
	// The scripted voice's device follows its channel here; its update is the scene frame's
	// (update_script_voice).
	follow_script_voice();
	const Ref<Simulation> sim = _simulation();
	opennova::audio::Mixer &mixer = AudioOut::mixer();
	// Every channel playing for an entity follows it (engine audio::repan_entity_channels).
	if (sim.is_valid()) {
		const Simulation *source = sim.ptr();
		opennova::audio::repan_entity_channels(mixer, listener_,
				[source](uint32_t p_id, int32_t r_position[3]) { return source->sound_source_place(p_id, r_position); });
	}
	if (!ambient_.live()) {
		perf_markers_ = 0;
		perf_voice_writes_ = 0;
		perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
		return;
	}
	// A header-only join's envs emitters, once the host's world stream has
	// landed (pool 3, the markers, streams last, before the match opens).
	if (world_envs_pending_ && sim.is_valid() && sim->is_joined_in_match() && item_db_.is_valid()) {
		world_envs_pending_ = false;
		ambient_.add_envs_markers(sim->envs_markers_from_world(item_db_->native_items()));
		ambient_.restart();
	}
	AmbientOcclusion occlusion;
	occlusion.sim = sim.ptr();
	occlusion.override_fn = &occlusion_override_;
	const bool occluded = occlusion.sim != nullptr || occlusion_override_.is_valid();
	const Vector3 camera = p_camera.origin;
	const float listener[3] = { static_cast<float>(camera.x), static_cast<float>(camera.y),
		static_cast<float>(camera.z) };
	const opennova::audio::AmbientChannelPlan &plan = ambient_.frame(listener,
			static_cast<float>(p_delta), root_attached_,
			[this](int32_t, const opennova::audio::AmbientCandidateRef &p_candidate) {
				return _resolve_ambient_wave(p_candidate) != nullptr;
			},
			occluded ? &ambient_occlusion : nullptr, &occlusion);
	const Simulation *source = sim.ptr();
	ambient_.play(mixer, listener_, AudioOut::options().sound_fx(),
			[this](int32_t, const opennova::audio::AmbientCandidateRef &p_candidate) {
				return _resolve_ambient_wave(p_candidate);
			},
			[source](uint64_t p_spawn_id) { return source != nullptr && source->sound_spawn_ridden(p_spawn_id); });
	perf_markers_ = ambient_.marker_count();
	// The channels opened or reset this frame (an incumbent's re-pan and re-level each frame is
	// no write: retail sets every one of the eight each frame).
	perf_voice_writes_ = static_cast<int>(plan.released.size() + plan.binds.size());
	perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
	_publish_ambient_stats();
}

opennova::audio::MixWaveRef MissionAudio::_resolve_ambient_wave(
		const opennova::audio::AmbientCandidateRef &p_candidate) {
	const String wave = opennova::to_gd(p_candidate.wave);
	const opennova::audio::MixWaveRef buffer = bank_.is_valid() ? bank_->resolve_wave(wave) : nullptr;
	if (!buffer && !warned_ambient_decode_failure_) {
		warned_ambient_decode_failure_ = true;
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: ambient WAV '%s' failed to decode; excluding failed candidates from the eight-channel mix",
				wave));
	}
	return buffer;
}

void MissionAudio::_publish_ambient_stats() {
	if (stats_.is_null()) {
		return;
	}
	const opennova::audio::AmbientEmitterStats &ambient = ambient_.stats();
	stats_->set_markers_total(ambient.markers_total);
	stats_->set_markers_resolved(ambient.markers_resolved);
	stats_->set_ambient_candidates(ambient.candidates);
	stats_->set_ambient_candidates_validated(ambient.candidates_validated);
	stats_->set_ambient_decode_failures(ambient.decode_failures);
	stats_->set_physical_channels(ambient_.channel_count());
}

void MissionAudio::_reset_ambient_channels() {
	opennova::audio::Mixer &mixer = AudioOut::mixer();
	for (int channel = 0; channel < opennova::audio::kAmbientMixChannels; ++channel)
		mixer.reset(ambient_.channel_handle(channel));
	ambient_.release_channels();
}

void MissionAudio::_reset_mission_playback_state() {
	_stop_script_voice(true);
	// The dialog and scripted-voice channels are mission-owned: silenced before the next
	// mission's, the dialog table being the world's, which the next mission's boot starts empty.
	opennova::audio::Mixer &mixer = AudioOut::mixer();
	mixer.reset(dialog_handle_);
	mixer.reset(tool_voice_handle_);
	dialog_handle_ = 0;
	tool_voice_handle_ = 0;
	dialog_frame_rendered_ = false;
	wac_waves_.clear();
	dbf_.unref();
	dialog_sounds_ = opennova::lwf::File();
	dialog_sounds_loaded_ = false;
}

void MissionAudio::teardown() {
	// A parked world-sound option is released first, as the teardown's cine release puts the
	// saved option back (engine EpilogCine::world_sound_parked carries the witness).
	set_world_sound_parked(false);
	_reset_ambient_channels();
	_reset_mission_playback_state();
	root_attached_ = false;
	ambient_.clear();
	warned_ambient_decode_failure_ = false;
	listener_tick_ = -1;
	// The last mission's fires are no reading of the next.
	recent_fire_head_ = 0;
	recent_fire_count_ = 0;
	bank_.unref();
	_install_voice_resolver();
}

// --- Internals ---

void MissionAudio::_load_bank(const String &p_lwf_name) {
	if (!resource_root_->has_file(p_lwf_name)) {
		return;
	}
	Ref<LwfData> lwf;
	lwf.instantiate();
	if (lwf->open_from_resource_root(resource_root_, p_lwf_name) == OK) {
		bank_->add_bank(lwf);
		stats_->set_banks_loaded(stats_->get_banks_loaded() + 1);
	}
}

// Witnessed no-op: the .bms header `music` field has NO live consumer in retail
// JO -- the only per-entry music starter (Sbf_StartEntry @ 0x4ed910, the WAC
// `music` handler) targets a stream whose opener (Sbf_OpenFile_Gamemus
// @ 0x4ed6c0) is unreferenced, so nothing ever plays it. The field is vestigial
// data the mission editor round-trips (docs/audio/mus-sbf-re.md §Game music
// driving). Kept as the seam in case a sibling title turns out to consume it.
void MissionAudio::_apply_music(int /*p_music_id*/) {
}

void MissionAudio::_install_voice_resolver() {
    const Ref<Simulation> sim = _simulation();
    if (sim.is_null()) return;
    const ObjectID bank_id = bank_.is_valid() ? ObjectID(bank_->get_instance_id()) : ObjectID();
    sim->set_script_voice_resolver([bank_id](const std::string &name, uint8_t listener_view_flags,
            bool bank_member) -> std::optional<opennova::world::ScriptVoiceChannel::SetSelection> {
        auto *bank = Object::cast_to<SoundBank>(ObjectDB::get_instance(bank_id));
        return bank ? bank->select_radio_set(name, listener_view_flags, bank_member) : std::nullopt;
    }, [] { return AudioOut::options().dialogue(); });
}

void MissionAudio::meter_out() {
    const Ref<Simulation> sim = _simulation();
    if (sim.is_valid()) sim->set_audio_out_meter(AudioOut::mixer().meter());
}

void MissionAudio::_stop_script_voice(bool p_report_finished) {
    const opennova::world::ScriptVoiceChannel::Frame &frame = script_voice_.frame();
    if (p_report_finished && frame.state.clip) {
        const Ref<Simulation> sim = _simulation();
        if (sim.is_valid()) sim->finish_script_voice(frame.serial, frame.state.clip.get());
    }
    script_voice_.stop(AudioOut::mixer());
}

void MissionAudio::follow_script_voice() {
    const Ref<Simulation> sim = _simulation();
    if (sim.is_null() || !root_attached_) return;
    script_voice_.follow(AudioOut::mixer(), listener_,
            sim->script_voice_state(last_camera_pos_.is_finite() ? last_camera_pos_ : Vector3()));
}

void MissionAudio::update_script_voice() {
    const Ref<Simulation> sim = _simulation();
    if (sim.is_null() || !root_attached_) return;
    // The scene frame's voice update (engine audio::ScriptVoicePlayer): the validation, whose
    // lapse the world finishes, then the anchor reset and the distance gain
    // (world::ScriptVoiceChannel::frame), the device following the channel and placing it.
    opennova::audio::Mixer &mixer = AudioOut::mixer();
    if (script_voice_.lapsed(mixer)) _stop_script_voice(true);
    const auto frame = sim->script_voice_frame(last_camera_pos_.is_finite() ? last_camera_pos_ : Vector3());
    script_voice_.follow(mixer, listener_, frame);
    script_voice_.place(mixer, listener_, frame);
}
