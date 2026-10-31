#include "audio/mission_audio.h"

#include "dbf/dbf_data.h"
#include "lwf/lwf_data.h"
#include "lwf/wav_loader.h"
#include "mission/mission_data.h"
#include "mission/mission_info.h"
#include "object/item_database.h"
#include "resource_index/resource_root.h"
#include "simulation/present_event_records.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_reverb.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/gameprofile/resource_missing.h>
#include <formats/lwf/lwf.h>
#include <formats/lwf/wav_pcm.h>
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/volume_law.h>
#include <runtime/mission/mission_sidecars.h>
#include <base/io/strutil.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace godot;

namespace {

constexpr const char *kAmbientBus = "Ambient";
constexpr const char *kSfxBus = "SFX";
constexpr const char *kVoiceBus = "Voice";
// The buses the sound-effects option drives, by the engine park's outputs
// (audio::EffectsVolumePark::kEffectsOutput / kAmbientOutput).
constexpr const char *kEffectsOptionBuses[opennova::audio::EffectsVolumePark::kOutputCount] = {
	kSfxBus, kAmbientBus
};

// The thunder bearing is an 8-bit binary angle (one byte = a full turn;
// world/weather_state.h WeatherSound.bearing).
constexpr double kBearingBam8Turn = 256.0;
// Hard-silent floor for out-of-mix voices (the engine's volume law).
constexpr double kSilentDb = opennova::audio::kVolumeSilentDb;

// The bus a candidate's table plays on: a placed marker's layers the Ambient
// bus, an entity-attached lane's the SFX bus.
StringName ambient_bus(opennova::audio::AmbientSource p_source) {
	return StringName(p_source == opennova::audio::AmbientSource::Marker ? kAmbientBus : kSfxBus);
}

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
	ClassDB::bind_method(D_METHOD("ambient_player_for_candidate", "candidate_id"),
			&MissionAudio::ambient_player_for_candidate);
	ClassDB::bind_method(D_METHOD("ambient_candidate_wave", "candidate_id"), &MissionAudio::ambient_candidate_wave);
	ClassDB::bind_method(D_METHOD("set_ambient_markers_enabled", "enabled"),
			&MissionAudio::set_ambient_markers_enabled);
	ClassDB::bind_method(D_METHOD("dialog_voice"), &MissionAudio::dialog_voice);
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
	ClassDB::bind_method(D_METHOD("_on_script_voice_finished", "serial", "player_id"),
			&MissionAudio::_on_script_voice_finished);
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

// Drop every voice child (the physical channels, the one-shot voices, the
// dialog and WAC voices): the former audio root's teardown. Deferred like
// that root's queue_free was -- the children stay in the tree until the idle
// free, so a same-frame reader (and GUT's orphan monitor) sees no strays.
void MissionAudio::_free_voice_nodes() {
	const TypedArray<Node> children = get_children();
	for (int64_t i = 0; i < children.size(); ++i) {
		Node *child = Object::cast_to<Node>(children[i]);
		if (child != nullptr) {
			child->queue_free();
		}
	}
}

Ref<MissionAudioStats> MissionAudio::setup(const Ref<MissionData> &p_mission, const String &p_mission_name,
		Node3D *p_container) {
	stats_.instantiate();
	if (p_mission.is_null() || p_container == nullptr || resource_root_.is_null()) {
		return stats_;
	}
	const Ref<MissionInfo> mission_info = p_mission->get_info();
	// A repeated setup is not the normal owner lifecycle, but it must not orphan
	// an earlier physical pool or carry dialog state into the next mission.
	_stop_all_ambient_channels();
	_reset_mission_playback_state();
	_free_voice_nodes();
	channel_players_.clear();
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
	_apply_reverb(mission_info.is_valid() ? mission_info->get_reverb() : 0);
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

AudioStreamPlayer3D *MissionAudio::_channel_player(int p_channel) const {
	if (p_channel < 0 || static_cast<size_t>(p_channel) >= channel_players_.size()) {
		return nullptr;
	}
	return Object::cast_to<AudioStreamPlayer3D>(
			ObjectDB::get_instance(channel_players_[static_cast<size_t>(p_channel)]));
}

AudioStreamPlayer3D *MissionAudio::ambient_player_for_candidate(int p_candidate_id) const {
	return p_candidate_id < 0 ? nullptr : _channel_player(ambient_.channel_of(p_candidate_id));
}

String MissionAudio::ambient_candidate_wave(int p_candidate_id) const {
	const opennova::audio::AmbientCandidateRef *candidate = ambient_.candidate(p_candidate_id);
	return candidate != nullptr ? opennova::to_gd(candidate->wave) : String();
}

void MissionAudio::set_ambient_markers_enabled(bool p_enabled) {
	ambient_markers_enabled_ = p_enabled;
}

AudioStreamPlayer *MissionAudio::_dialog_voice_node() const {
	return Object::cast_to<AudioStreamPlayer>(ObjectDB::get_instance(dialog_voice_id_));
}

AudioStreamPlayer *MissionAudio::_wac_voice_node() const {
	return Object::cast_to<AudioStreamPlayer>(ObjectDB::get_instance(wac_voice_id_));
}

AudioStreamPlayer *MissionAudio::dialog_voice() const {
	return _dialog_voice_node();
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
	perf->set_physical_channels(static_cast<int64_t>(channel_players_.size()));
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
		const std::vector<opennova::world::WeatherSoundEvent> &p_events,
		const Transform3D &p_camera_xform) {
    for (const auto &event : p_events) {
        _play_listener_relative(opennova::world::kThunderSoundSet, event.distance_q16, event.bearing, p_camera_xform);
    }
}

void MissionAudio::play_script_sounds(
        const std::vector<opennova::world::ScriptSoundEvent> &p_events,
        const Transform3D &p_camera_xform) {
    for (const auto &event : p_events) {
        if (event.kind == opennova::world::ScriptSoundEvent::Kind::Interface)
            ui_soundset(opennova::to_gd(event.name));
        else
            _play_listener_relative(opennova::to_gd(event.name), event.distance_q16,
                    event.bearing, p_camera_xform);
    }
}

void MissionAudio::_play_listener_relative(const String &p_name, int32_t p_distance_q16,
        int32_t p_bearing, const Transform3D &p_camera_xform) {
    if (bank_.is_null() || !root_attached_) {
        return;
    }
    // Retail passes the raw bearing and distance straight to the trigger player.
    // Godot supplies the device panner (D-SND-8); a unit offset preserves the
    // bearing even for zero distance. The native plan owns attenuation.
    const Vector3 forward = -p_camera_xform.basis.get_column(2);
    const auto bearing = static_cast<uint8_t>(p_bearing);
    const Vector3 direction = forward.rotated(Vector3(0, 1, 0),
            static_cast<real_t>(static_cast<double>(bearing) * Math_TAU / kBearingBam8Turn));
    const Vector3 position = p_camera_xform.origin + direction;
    _record_fire(p_name, position, 0, false,
            bank_->play_oneshot_at_distance(this, position, p_name, StringName(kSfxBus),
                    p_distance_q16));
}

void MissionAudio::shutdown_audio_channels() {
	if (bank_.is_valid()) {
		bank_->reset_oneshots(this);
	}
	_stop_all_ambient_channels();
	const Ref<Simulation> sim = _simulation();
	if (opennova::audio::DialogQueue *queue = sim.is_valid() ? sim->dialog_queue() : nullptr) {
		for (const uint64_t voice : queue->voices()) {
			if (AudioStreamPlayer *dialog = Object::cast_to<AudioStreamPlayer>(
						ObjectDB::get_instance(ObjectID(voice)))) {
				dialog->stop();
			}
		}
	}
	if (AudioStreamPlayer *dialog = _dialog_voice_node()) {
		dialog->stop();
	}
	if (AudioStreamPlayer *wac = _wac_voice_node()) {
		wac->stop();
	}
}

void MissionAudio::set_world_sound_parked(bool p_parked) {
	if (p_parked == effects_park_.parked()) return;
	AudioServer *server = AudioServer::get_singleton();
	int buses[opennova::audio::EffectsVolumePark::kOutputCount] = { -1, -1 };
	for (int i = 0; i < opennova::audio::EffectsVolumePark::kOutputCount; ++i) {
		if (server != nullptr) buses[i] = server->get_bus_index(StringName(kEffectsOptionBuses[i]));
	}
	opennova::audio::EffectsVolumePark::Levels levels{};
	if (p_parked) {
		for (int i = 0; i < opennova::audio::EffectsVolumePark::kOutputCount; ++i) {
			if (buses[i] >= 0) levels[static_cast<size_t>(i)] = server->get_bus_volume_db(buses[i]);
		}
		if (!effects_park_.park(levels)) return;
		for (const int bus : buses) {
			if (bus >= 0) {
				server->set_bus_volume_db(bus,
						static_cast<float>(opennova::audio::EffectsVolumePark::parked_db()));
			}
		}
		return;
	}
	if (!effects_park_.restore(levels)) return;
	for (int i = 0; i < opennova::audio::EffectsVolumePark::kOutputCount; ++i) {
		if (buses[i] >= 0) {
			server->set_bus_volume_db(buses[i], static_cast<float>(levels[static_cast<size_t>(i)]));
		}
	}
}

void MissionAudio::stop_dialog_line() {
	if (AudioStreamPlayer *dialog = _dialog_voice_node()) {
		dialog->stop();
	}
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

bool MissionAudio::fire_soundset(const String &p_name, const Vector3 &p_world_pos, int p_source_bms_id,
		int p_sound_id) {
	if (bank_.is_null() || !root_attached_) {
		return _record_fire(p_name, p_world_pos, p_source_bms_id, false, false);
	}
	return _record_fire(p_name, p_world_pos, p_source_bms_id, false,
			bank_->play_oneshot_3d(this, p_world_pos, p_name, StringName(kSfxBus), last_camera_pos_,
					p_source_bms_id, _oneshot_sound_id(p_source_bms_id, p_sound_id)));
}

bool MissionAudio::ui_soundset(const String &p_name) {
	if (bank_.is_null() || !root_attached_) {
		return false;
	}
	return bank_->play_interface_oneshot(this, p_name, StringName(kSfxBus));
}

bool MissionAudio::slot_soundset(const String &p_name, const Vector3 &p_world_pos,
        int p_source_bms_id, int p_sound_id) {
    if (bank_.is_null() || !root_attached_) {
        return _record_fire(p_name, p_world_pos, p_source_bms_id, true, false);
    }
    return _record_fire(p_name, p_world_pos, p_source_bms_id, true,
            bank_->play_oneshot_3d(this, p_world_pos, p_name, StringName(kSfxBus), last_camera_pos_,
                    p_source_bms_id, _oneshot_sound_id(p_source_bms_id, p_sound_id)));
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
	return _spawn_dialog_voice(_resolve_wav(opennova::to_gd(line.file).get_file()), line.volume) != nullptr;
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

AudioStreamPlayer *MissionAudio::_spawn_dialog_voice(const Ref<AudioStreamWAV> &p_stream, int p_volume) {
	if (p_stream.is_null()) {
		return nullptr;
	}
	AudioStreamPlayer *voice = memnew(AudioStreamPlayer);
	voice->set_name("DialogVoice");
	if (AudioServer::get_singleton()->get_bus_index(StringName(kVoiceBus)) >= 0) {
		voice->set_bus(StringName(kVoiceBus));
	}
	voice->set_stream(p_stream);
	// The line plays at the play factor 0x10000, centred: the dialog's play hook writes both into the
	// line's record before the open [orig: Dialog_ApplyDialogueVolumeAndPlayFactor @ 0x52757c, called through dword_A8A23C
	// @ 0x44de50]. A decoded wave's player takes its scale through WavLoader::pitch_scale_for (the
	// mixer's step of the wave, a boxed rate's too).
	voice->set_pitch_scale(static_cast<float>(WavLoader::pitch_scale_for(p_stream, 1.0)));
	voice->set_volume_db(opennova::audio::volume_db_from_byte(p_volume));
	add_child(voice);
	voice->play();
	voice->connect("finished", Callable(voice, "queue_free"));
	return voice;
}

// A line the dialog queue loads: its wave read and decoded through the VFS (a
// missing .wav extension tolerated), what the game's wave loader records of it
// for the line's hold (runtime/audio/dialog_queue dialog_clip_hold) and, when the
// engine's gate lets it play, a voice of the dialog channel at the wave's volume
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
			if (AudioStreamPlayer *voice = _spawn_dialog_voice(WavLoader::from_pcm(decoded), p_line.volume)) {
				dialog_voice_id_ = ObjectID(voice->get_instance_id());
				clip.voice = static_cast<uint64_t>(voice->get_instance_id());
			}
		}
	}
	return clip;
}

bool MissionAudio::_dialog_voice_playing(uint64_t p_voice) const {
	const AudioStreamPlayer *voice =
			Object::cast_to<AudioStreamPlayer>(ObjectDB::get_instance(ObjectID(p_voice)));
	return voice != nullptr && voice->is_playing();
}

bool MissionAudio::play_wac_wave(const String &p_filename) {
	const Ref<Simulation> sim = _simulation();
	if (sim.is_valid()) {
		const bool loaded = sim->play_script_wave(p_filename);
		follow_script_voice();
		return loaded;
	}
	// Standalone tooling shares the same interrupt-before-load rule.
	if (AudioStreamPlayer *previous = _wac_voice_node()) previous->stop();
	if (!root_attached_ || resource_root_.is_null() || p_filename.is_empty()) {
		return false;
	}
	const Ref<AudioStreamWAV> stream = _resolve_wav(p_filename);
	if (stream.is_null()) {
		UtilityFunctions::push_warning(vformat("MissionAudio: WAC wave '%s' did not resolve", p_filename));
		return false;
	}
	// The engine's channel rule: one dedicated voice, reset before each play.
	AudioStreamPlayer *voice = _wac_voice_node();
	if (voice == nullptr) {
		voice = memnew(AudioStreamPlayer);
		voice->set_bus(_script_voice_bus());
		add_child(voice);
		wac_voice_id_ = ObjectID(voice->get_instance_id());
	}
	voice->set_stream(stream);
	voice->set_pitch_scale(static_cast<float>(WavLoader::pitch_scale_for(stream, 1.0)));
	voice->play(); // play() on an active player restarts it -> interrupts the previous wave
	return true;
}

// Resolve + cache a .wav by filename through the VFS (tolerates a missing .wav
// extension). Returns the decoded AudioStreamWAV, or null.
Ref<AudioStreamWAV> MissionAudio::_resolve_wav(const String &p_filename) {
	const String key = p_filename.to_lower();
	if (const Ref<AudioStreamWAV> *cached = wac_wav_cache_.getptr(key)) {
		return *cached;
	}
	PackedByteArray bytes = resource_root_->read_file(p_filename);
	if (bytes.is_empty() && !key.ends_with(".wav")) {
		bytes = resource_root_->read_file(p_filename + String(".wav"));
	}
	Ref<AudioStreamWAV> stream;
	if (!bytes.is_empty()) {
		stream = WavLoader::from_bytes(bytes);
	}
	wac_wav_cache_[key] = stream;
	return stream;
}

void MissionAudio::set_time_of_day_hhmm(double p_hhmm) {
	ambient_.set_time_of_day_hhmm(p_hhmm);
}

void MissionAudio::advance_ticks(int64_t p_logic_tick) {
	follow_script_voice();
	ambient_.advance_to_logic_tick(p_logic_tick);
}

void MissionAudio::set_simulation(const Ref<Simulation> &p_sim) {
	const Ref<Simulation> previous = _simulation();
	if (previous.is_valid() && previous != p_sim) previous->set_script_voice_resolver({});
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

void MissionAudio::tick(const Vector3 &p_camera_pos, double p_delta) {
	const uint64_t start = Time::get_singleton()->get_ticks_usec();
	last_camera_pos_ = p_camera_pos;
	// The frame this pass mixes renders: the next dialog tick starts a fresh
	// line's hold (runtime/audio/dialog_queue DialogQueue::tick).
	dialog_frame_rendered_ = true;
	// The scripted voice's device follows its channel here; its update is the scene frame's
	// (update_script_voice).
	follow_script_voice();
	int writes = 0;
	if (!ambient_.live()) {
		perf_markers_ = 0;
		perf_voice_writes_ = 0;
		perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
		return;
	}
	const Ref<Simulation> sim = _simulation();
	// A header-only join's envs emitters, once the host's world stream has
	// landed (pool 3, the markers, streams last, before the match opens).
	if (world_envs_pending_ && sim.is_valid() && sim->is_joined_in_match() && item_db_.is_valid()) {
		world_envs_pending_ = false;
		ambient_.add_envs_markers(sim->envs_markers_from_world(item_db_->native_items()));
		ambient_.restart();
	}
	// The engine plans the frame; the stream resolve is the one device step it
	// asks for, once per entrant, answered here and kept for the bind.
	struct Entrant {
		Ref<AudioStreamWAV> stream;
		StringName bus;
	};
	std::unordered_map<int32_t, Entrant> entrants;
	const auto resolve = [&](int32_t p_candidate_id,
								 const opennova::audio::AmbientCandidateRef &p_candidate) {
		const Ref<AudioStreamWAV> stream = _resolve_ambient_stream(p_candidate);
		if (stream.is_null()) return false;
		entrants[p_candidate_id] = Entrant{ stream, ambient_bus(p_candidate.source) };
		return true;
	};
	AmbientOcclusion occlusion;
	occlusion.sim = sim.ptr();
	occlusion.override_fn = &occlusion_override_;
	const bool occluded = occlusion.sim != nullptr || occlusion_override_.is_valid();
	const float listener[3] = { static_cast<float>(p_camera_pos.x),
		static_cast<float>(p_camera_pos.y), static_cast<float>(p_camera_pos.z) };
	const opennova::audio::AmbientChannelPlan &plan = ambient_.frame(listener,
			static_cast<float>(p_delta), root_attached_, resolve,
			occluded ? &ambient_occlusion : nullptr, &occlusion);
	while (static_cast<int>(channel_players_.size()) < plan.channel_count) _new_channel();

	for (const int channel_index : plan.released) {
		if (AudioStreamPlayer3D *player = _channel_player(channel_index)) {
			player->stop();
			player->set_stream(Ref<AudioStream>());
			player->set_volume_db(static_cast<float>(kSilentDb));
			player->set_process_mode(Node::PROCESS_MODE_DISABLED);
		}
		writes += 1;
	}
	for (const opennova::audio::AmbientChannelPlan::Bind &bind : plan.binds) {
		AudioStreamPlayer3D *player = _channel_player(bind.channel);
		const auto entrant = entrants.find(bind.row.candidate_id);
		if (player == nullptr || entrant == entrants.end()) continue;
		const Ref<AudioStreamWAV> &stream = entrant->second.stream;
		SoundBank::configure_ambient_player(player, stream, entrant->second.bus);
		player->set_position(Vector3(bind.row.pos[0], bind.row.pos[1], bind.row.pos[2]));
		player->set_volume_db(static_cast<float>(SoundBank::volume_db_from_255(bind.row.vol)));
		player->set_pitch_scale(static_cast<float>(WavLoader::pitch_scale_for(stream,
				_pitch_scale(bind.row.pitch_q16))));
		player->set_process_mode(Node::PROCESS_MODE_INHERIT);
		player->play();
		writes += 1;
	}
	for (const opennova::audio::AmbientChannelPlan::Bind &update : plan.updates) {
		AudioStreamPlayer3D *incumbent = _channel_player(update.channel);
		if (incumbent == nullptr) continue;
		bool changed = false;
		const Vector3 pos(update.row.pos[0], update.row.pos[1], update.row.pos[2]);
		if (incumbent->get_position() != pos) {
			incumbent->set_position(pos);
			changed = true;
		}
		const double db = SoundBank::volume_db_from_255(update.row.vol);
		if (!Math::is_equal_approx(static_cast<double>(incumbent->get_volume_db()), db)) {
			incumbent->set_volume_db(static_cast<float>(db));
			changed = true;
		}
		const double pitch_scale = WavLoader::pitch_scale_for(incumbent->get_stream(),
				_pitch_scale(update.row.pitch_q16));
		if (!Math::is_equal_approx(static_cast<double>(incumbent->get_pitch_scale()), pitch_scale)) {
			incumbent->set_pitch_scale(static_cast<float>(pitch_scale));
			changed = true;
		}
		if (changed) writes += 1;
	}
	perf_markers_ = ambient_.marker_count();
	perf_voice_writes_ = writes;
	perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
	_publish_ambient_stats();
}

Ref<AudioStreamWAV> MissionAudio::_resolve_ambient_stream(
		const opennova::audio::AmbientCandidateRef &p_candidate) {
	const String wave = opennova::to_gd(p_candidate.wave);
	const Ref<AudioStreamWAV> stream =
			bank_.is_valid() ? bank_->resolve_ambient_wave(wave) : Ref<AudioStreamWAV>();
	if (stream.is_null() && !warned_ambient_decode_failure_) {
		warned_ambient_decode_failure_ = true;
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: ambient WAV '%s' failed to decode; excluding failed candidates from the eight-channel mix",
				wave));
	}
	return stream;
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
	stats_->set_physical_channels(static_cast<int>(channel_players_.size()));
}

// One more physical channel: the engine pool decided the growth under its
// budget, on an attached root.
void MissionAudio::_new_channel() {
	AudioStreamPlayer3D *player = memnew(AudioStreamPlayer3D);
	player->set_name(vformat("AmbientChannel%d", static_cast<int64_t>(channel_players_.size())));
	player->set_volume_db(static_cast<float>(kSilentDb));
	player->set_process_mode(Node::PROCESS_MODE_DISABLED);
	add_child(player);
	channel_players_.push_back(ObjectID(player->get_instance_id()));
}

void MissionAudio::_stop_all_ambient_channels() {
	ambient_.release_channels();
	for (size_t channel = 0; channel < channel_players_.size(); ++channel) {
		if (AudioStreamPlayer3D *player = _channel_player(static_cast<int>(channel))) {
			player->stop();
			player->set_stream(Ref<AudioStream>());
			player->set_volume_db(static_cast<float>(kSilentDb));
			player->set_process_mode(Node::PROCESS_MODE_DISABLED);
		}
	}
}

void MissionAudio::_reset_mission_playback_state() {
	_stop_script_voice(true);
	// Dialog and WAC voices are mission-owned even though they use separate
	// physical players from ambience. Stop them before replacing their audio
	// root so no playback crosses missions; the dialog table is the world's,
	// which the next mission's boot starts empty.
	const Ref<Simulation> sim = _simulation();
	if (opennova::audio::DialogQueue *queue = sim.is_valid() ? sim->dialog_queue() : nullptr) {
		for (const uint64_t voice : queue->voices()) {
			if (AudioStreamPlayer *dialog = Object::cast_to<AudioStreamPlayer>(
						ObjectDB::get_instance(ObjectID(voice)))) {
				dialog->stop();
			}
		}
	}
	if (AudioStreamPlayer *dialog = _dialog_voice_node()) {
		dialog->stop();
	}
	if (AudioStreamPlayer *wac = _wac_voice_node()) {
		wac->stop();
	}
	dialog_voice_id_ = ObjectID();
	dialog_frame_rendered_ = false;
	wac_voice_id_ = ObjectID();
	wac_wav_cache_.clear();
	dbf_.unref();
	dialog_sounds_ = opennova::lwf::File();
	dialog_sounds_loaded_ = false;
}

void MissionAudio::teardown() {
	// Freeing the voice children drops the dialog + wac voice nodes too; the
	// queue and its voices are cleared so nothing crosses missions. A parked
	// world-sound volume is released first, as the teardown's cine release puts the
	// saved option back (engine EpilogCine::world_sound_parked carries the witness).
	set_world_sound_parked(false);
	_apply_reverb(0);
	_stop_all_ambient_channels();
	_reset_mission_playback_state();
	_free_voice_nodes();
	root_attached_ = false;
	channel_players_.clear();
	ambient_.clear();
	warned_ambient_decode_failure_ = false;
	// The last mission's fires are no reading of the next.
	recent_fire_head_ = 0;
	recent_fire_count_ = 0;
	bank_.unref();
	_install_voice_resolver();
}

// --- Internals ---

// The player's pitch: the channel's play word (the engine's
// audio::emitter_play_word over the native mix row's pitch_q16, which carries the
// witness), as a scale; a word whose step is 0 plays at the mixer's least step
// through the player's scale (WavLoader::pitch_scale_for).
double MissionAudio::_pitch_scale(int p_pitch_q16) {
	return opennova::lwf::pitch_from_q16(opennova::audio::emitter_play_word(p_pitch_q16));
}

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

// The original mixer copies its selected preset, but the live sample path has
// no consumer for those values. Region selection lives in World::reverb;
// see the original-output witness in docs/audio/lwf-dbf-sound-re.md.
void MissionAudio::_apply_reverb(int /*p_reverb_id*/) {
    AudioServer *audio_server = AudioServer::get_singleton();
    const int bus_idx = audio_server->get_bus_index(StringName(kAmbientBus));
    if (bus_idx < 0) return;
    for (int i = audio_server->get_bus_effect_count(bus_idx) - 1; i >= 0; --i) {
        const Ref<AudioEffect> effect = audio_server->get_bus_effect(bus_idx, i);
        if (Object::cast_to<AudioEffectReverb>(effect.ptr())) audio_server->remove_bus_effect(bus_idx, i);
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
