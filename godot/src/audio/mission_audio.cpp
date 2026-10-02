#include "audio/mission_audio.h"

#include "dbf/dbf_data.h"
#include "lwf/lwf_data.h"
#include "lwf/wav_loader.h"
#include "mission/mission_data.h"
#include "mission/mission_info.h"
#include "object/item_database.h"
#include "resource_index/resource_root.h"
#include "simulation/present_event_records.h"
#include "util/axes.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_reverb.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/environment/environment_state.h>
#include <runtime/mission/placement_traits.h>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace {

constexpr const char *kAmbientBus = "Ambient";
constexpr const char *kSfxBus = "SFX";
constexpr const char *kVoiceBus = "Voice";
// Marker -> sound set resolution is the marker item's items.def soundloop_1..7
// set names (e.g. id 106178 "snd: Lp Flourescent Light" -> soundloop_1
// LPNV_LIGHT) [orig: ItemDef_ParseProperty @ 0x49fec4]; the engine is
// name-keyed (docs/audio/lwf-dbf-sound-re.md) and resolves it natively
// (audio/envs_markers.h resolve_envs_markers).

// The thunder bearing is an 8-bit binary angle (one byte = a full turn;
// world/weather_state.h WeatherSound.bearing).
constexpr double kBearingBam8Turn = 256.0;
constexpr double kMinutesPerHour = 60.0;
// Hard-silent floor for out-of-mix voices (the engine's volume law).
constexpr double kSilentDb = opennova::audio::kVolumeSilentDb;
// Time-of-day region cuts (4/10/17/21 h) and the ~5-game-minute crossfade margin
// live with the eval in engine/runtime/audio (ambient_mixer.cpp time_of_day_region)
// [orig: Entity_CalcTimeOfDayRegion @ 0x408110; margin @ 0x408203].

String dynamic_emitter_key(int64_t p_source_spawn_id, int p_lane) {
	return vformat("%d:%d", p_source_spawn_id, p_lane);
}

// One stride-5 mixer layer row: [candidate_id, falloff_u, min_u, member_vol, clamp_vol].
void append_layer_row(PackedInt32Array &r_rows, int p_candidate_id, const Ref<AmbientLayer> &p_layer) {
	r_rows.push_back(p_candidate_id);
	r_rows.push_back(p_layer->get_falloff_radius());
	r_rows.push_back(p_layer->get_min_distance());
	r_rows.push_back(p_layer->get_volume());
	r_rows.push_back(p_layer->get_clamp_volume());
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
	ClassDB::bind_method(D_METHOD("set_markers", "markers", "container"), &MissionAudio::set_markers,
			DEFVAL(nullptr));
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
	ClassDB::bind_method(D_METHOD("resolve_dialog_set", "wav_id"), &MissionAudio::resolve_dialog_set);
	ClassDB::bind_method(D_METHOD("play_wac_wave", "filename"), &MissionAudio::play_wac_wave);
	ClassDB::bind_method(D_METHOD("sync_script_voice"), &MissionAudio::sync_script_voice);
	ClassDB::bind_method(D_METHOD("_on_script_voice_finished", "serial", "player_id"),
			&MissionAudio::_on_script_voice_finished);
	ClassDB::bind_method(D_METHOD("set_time_of_day_hhmm", "hhmm"), &MissionAudio::set_time_of_day_hhmm);
	ClassDB::bind_method(D_METHOD("advance_ticks", "logic_tick"), &MissionAudio::advance_ticks);
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &MissionAudio::set_simulation);
	ClassDB::bind_method(D_METHOD("set_occlusion_override", "override"), &MissionAudio::set_occlusion_override);
	ClassDB::bind_method(D_METHOD("tick", "camera_pos", "delta"), &MissionAudio::tick, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("teardown"), &MissionAudio::teardown);
	ClassDB::bind_method(D_METHOD("recent_fired_soundsets"), &MissionAudio::recent_fired_soundsets);
	ClassDB::bind_method(D_METHOD("_on_dialog_finished"), &MissionAudio::_on_dialog_finished);
	BIND_CONSTANT(MIX_CHANNELS);
	BIND_CONSTANT(RECENT_FIRES);
}

MissionAudio::MissionAudio() {
	set_name("MissionAudio");
	last_camera_pos_ = Vector3(INFINITY, INFINITY, INFINITY);
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
	Ref<FiredSoundset> fired;
	fired.instantiate();
	fired->set_set_name(p_set_name);
	fired->set_position(p_world_pos);
	fired->set_source_bms_id(p_source_bms_id);
	fired->set_slot(p_slot);
	fired->set_played(p_played);
	recent_fires_.push_back(fired);
	while (recent_fires_.size() > RECENT_FIRES) {
		recent_fires_.remove_at(0);
	}
	return p_played;
}

TypedArray<FiredSoundset> MissionAudio::recent_fired_soundsets() const {
	TypedArray<FiredSoundset> out;
	for (const Ref<FiredSoundset> &fired : recent_fires_) {
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
	markers_.clear();
	channels_.clear();
	channel_pool_.reset();
	validated_candidate_ids_.clear();
	warned_ambient_decode_failure_ = false;
	next_candidate_id_ = 1;
	free_candidate_ids_.clear();
	retired_candidate_ids_.clear();
	queued_sound_emitters_.clear();
	dynamic_emitter_states_.clear();

	bank_ = SoundBank::create(resource_root_);
	_install_voice_resolver();
	bank_->set_occlusion_provider(_simulation());
	bank_->set_occlusion_override(occlusion_override_);
	const String mission_base = p_mission_name.get_file().get_basename();
	_load_bank(mission_base + String(".LWF"));
	// The global slots in the engine's order -- expansion pair (when one is
	// mounted) ahead of the statics [orig: slot table @ 0x82A5B0, walk
	// @ 0x525443; expansion fill @ 0x4a4989 / @ 0x4a495e]. The witnessed table
	// lives native (audio/bank_chain.h); missing files skip like retail's
	// SoundBank_LoadIfExists (D-SND-2 closed).
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

	// The mission's co-named .DBF maps a PlayWavList dialog id (dlg001) to the LWF
	// set name(s) it plays; loaded only if present.
	const String dbf_name = mission_base + String(".DBF");
	if (resource_root_->has_file(dbf_name)) {
		Ref<DbfData> dbf;
		dbf.instantiate();
		if (dbf->open_from_resource_root(resource_root_, dbf_name) == OK) {
			dbf_ = dbf;
			stats_->set_dialogs(dbf->get_dialog_count());
		}
	}

	_attach_under(p_container);

	std::vector<opennova::audio::EnvsMarker> marker_rows;
	// S13 (ADR 0028): the faithful envs dispatch + the four soundloop slot
	// names resolve natively over the retained items.def and the mission's
	// bms document (audio/envs_markers.h). The bank-presence filter below
	// stays a shell stream-resolution concern (the original has no such
	// gate -- a missing set is simply silent).
	if (ambient_markers_enabled_ && item_db_.is_valid()) {
		marker_rows = opennova::audio::resolve_envs_markers(
				p_mission->native_file(), item_db_->native_items());
	}
	for (const opennova::audio::EnvsMarker &row : marker_rows) {
		stats_->set_markers_total(stats_->get_markers_total() + 1);
		// Authored slot names -> playable slots: only sets the loaded bank chain
		// actually carries participate; an empty slot stays SILENT in its region.
		const auto &authored = row.slot_sets;
		PackedStringArray slot_sets;
		slot_sets.resize(4);
		for (int i = 0; i < 4; ++i) {
			slot_sets[i] = String();
			if (i < authored.size()) {
				const String n(authored[i].c_str());
				if (!n.is_empty() && bank_->has_set(n)) {
					slot_sets[i] = n;
				}
			}
		}
		PackedStringArray distinct;
		for (int i = 0; i < slot_sets.size(); ++i) {
			const String s = slot_sets[i];
			if (!s.is_empty() && !distinct.has(s)) {
				distinct.push_back(s);
			}
		}
		if (distinct.is_empty()) {
			continue;
		}
		const opennova::mission::PlacementVec3 placed = opennova::mission::bms_to_presentation_position(
				opennova::mission::PlacementVec3{ row.x, row.y, row.z });
		const Vector3 pos(placed.x, placed.y, placed.z);
		// Keep layer candidates as data. The original registers only the current
		// region's set and has eight physical channels; it does not materialize a
		// player for every marker/time-of-day layer [orig: @ 0x4a81da].
		Ref<MissionAudioMarker> marker;
		marker.instantiate();
		int candidate_count = 0;
		for (int i = 0; i < distinct.size(); ++i) {
			const String set_name = distinct[i];
			const TypedArray<AmbientLayer> described = bank_->describe_ambient(set_name);
			if (described.is_empty()) {
				continue;
			}
			for (int64_t li = 0; li < described.size(); ++li) {
				const Ref<AmbientLayer> layer = described[li];
				layer->set_candidate_id(next_candidate_id_);
				next_candidate_id_ += 1;
			}
			marker->set_layers(set_name, described);
			candidate_count += static_cast<int>(described.size());
		}
		if (marker->set_count() == 0) {
			continue;
		}
		marker->set_pos(pos);
		marker->set_source_bms_id(row.bms_id);
		marker->set_slot_sets(slot_sets);
		marker->set_stagger_slot(static_cast<int>(markers_.size() & 0xF));
		markers_.push_back(marker);
		stats_->set_markers_resolved(stats_->get_markers_resolved() + 1);
		stats_->set_ambient_candidates(stats_->get_ambient_candidates() + candidate_count);
	}

	// Silence here has historically gone unnoticed (a bare stats print) -- warn on
	// the two states that mean "no ambience will play" so they surface in logs.
	if (stats_->get_banks_loaded() == 0) {
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: no sound banks loaded (probed %s.LWF, %s) — mission ambience will be silent",
				mission_base, global_chain_text));
	} else if (stats_->get_markers_total() > 0 && stats_->get_markers_resolved() == 0) {
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: 0/%d sound markers resolved (item db %s) — mission ambience will be silent",
				stats_->get_markers_total(), item_db_.is_null() ? String("missing") : String("loaded")));
	}

	_feed_mixer();
	_apply_reverb(mission_info.is_valid() ? mission_info->get_reverb() : 0);
	_apply_music(mission_info.is_valid() ? mission_info->get_music() : 0);
	return stats_;
}

TypedArray<int64_t> MissionAudio::active_ambient_candidate_ids() const {
	TypedArray<int64_t> out;
	for (const Ref<MissionAudioChannel> &channel : channels_) {
		if (channel->get_candidate_id() >= 0) {
			out.push_back(static_cast<int64_t>(channel->get_candidate_id()));
		}
	}
	out.sort();
	return out;
}

AudioStreamPlayer3D *MissionAudio::ambient_player_for_candidate(int p_candidate_id) const {
	if (p_candidate_id < 0) {
		return nullptr;
	}
	for (const Ref<MissionAudioChannel> &channel : channels_) {
		if (channel->get_candidate_id() == p_candidate_id) {
			AudioStreamPlayer3D *player = channel->get_player();
			if (player != nullptr) {
				return player;
			}
		}
	}
	return nullptr;
}

void MissionAudio::set_markers(const Array &p_markers, Node3D *p_container) {
	_stop_all_ambient_channels();
	markers_.clear();
	for (int64_t i = 0; i < p_markers.size(); ++i) {
		const Ref<MissionAudioMarker> marker = p_markers[i];
		if (marker.is_valid()) {
			markers_.push_back(marker);
		}
	}
	channel_pool_.forget_failures();
	validated_candidate_ids_.clear();
	warned_ambient_decode_failure_ = false;
	next_candidate_id_ = 1;
	free_candidate_ids_.clear();
	retired_candidate_ids_.clear();
	queued_sound_emitters_.clear();
	dynamic_emitter_states_.clear();
	if (p_container != nullptr && !root_attached_) {
		_attach_under(p_container);
	}
	for (const Ref<MissionAudioMarker> &marker : markers_) {
		for (int si = 0; si < marker->set_count(); ++si) {
			const TypedArray<AmbientLayer> &layers = marker->layers_at(si);
			for (int64_t li = 0; li < layers.size(); ++li) {
				const Ref<AmbientLayer> layer = layers[li];
				if (layer.is_null()) {
					continue;
				}
				if (layer->get_candidate_id() <= 0) {
					layer->set_candidate_id(next_candidate_id_);
				}
				next_candidate_id_ = MAX(next_candidate_id_, layer->get_candidate_id() + 1);
			}
		}
	}
	_feed_mixer();
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
	for (const Ref<MissionAudioChannel> &channel : channels_) {
		if (channel->get_candidate_id() >= 0) {
			active_channels += 1;
		}
	}
	Ref<MissionAudioPerf> perf;
	perf.instantiate();
	perf->set_tick_us(perf_tick_us_);
	perf->set_markers(perf_markers_);
	perf->set_voice_writes(perf_voice_writes_);
	perf->set_physical_channels(channels_.size());
	perf->set_active_channels(active_channels);
	perf->set_ambient_decode_failures(static_cast<int64_t>(channel_pool_.failed_count()));
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
	if (mixer_.is_null() || bank_.is_null()) {
		return;
	}
	queued_sound_emitters_.insert(queued_sound_emitters_.end(), p_events.begin(), p_events.end());
}

void MissionAudio::play_weather_sounds(
		const std::vector<opennova::world::WeatherSoundEvent> &p_events,
		const Transform3D &p_camera_xform) {
    for (const auto &event : p_events) {
        _play_listener_relative("THUNDER", event.distance_q16, event.bearing, p_camera_xform);
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

void MissionAudio::reset_oneshot_playback() {
    if (bank_.is_valid()) {
        bank_->reset_oneshots(this);
    }
    recent_fires_.clear();
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
	if (bank_.is_null() || !root_attached_) {
		return false;
	}
	const std::vector<std::string> sets = _resolve_dialog_sets(p_wav_id);
	if (sets.empty()) {
		UtilityFunctions::push_warning(vformat("MissionAudio: unresolved dialog id %d", p_wav_id));
		return false;
	}
	dialog_queue_.enqueue(sets);
	_pump_dialog_queue();
	return true;
}

bool MissionAudio::play_dialog_line(const String &p_dialog_name, int p_line,
		int p_player_class) {
	if (bank_.is_null() || !root_attached_) {
		return false;
	}
	const opennova::dbf::File *dialog_bank =
			(dbf_.is_valid() && dbf_->is_loaded()) ? &dbf_->engine_file() : nullptr;
	const opennova::audio::DialogLinePlayback line = opennova::audio::resolve_dialog_line(
			dialog_bank, bank_->set_index(), nullptr, opennova::to_std(p_dialog_name), p_line,
			p_player_class);
	if (line.set_name.empty()) {
		return false;
	}
	AudioStreamPlayer *voice = bank_->spawn_oneshot_2d(this, opennova::to_gd(line.set_name),
			StringName(kVoiceBus));
	if (voice == nullptr) {
		return false;
	}
	voice->connect("finished", Callable(voice, "queue_free"));
	return true;
}

String MissionAudio::resolve_dialog_set(int p_wav_id) {
	const std::vector<std::string> sets = _resolve_dialog_sets(p_wav_id);
	return sets.empty() ? String() : opennova::to_gd(sets.front());
}

std::vector<std::string> MissionAudio::_resolve_dialog_sets(int p_wav_id) const {
	if (bank_.is_null()) {
		return {};
	}
	const opennova::dbf::File *dialog_bank =
			(dbf_.is_valid() && dbf_->is_loaded()) ? &dbf_->engine_file() : nullptr;
	return opennova::audio::resolve_dialog_sets(dialog_bank, bank_->set_index(), p_wav_id);
}

// Start the next queued dialog line if nothing is currently playing. A line that
// fails to actually spawn is skipped so the queue never stalls.
void MissionAudio::_pump_dialog_queue() {
	if (_dialog_voice_node() != nullptr) {
		return; // a line is still playing; _on_dialog_finished pumps the next
	}
	dialog_voice_id_ = ObjectID();
	dialog_queue_.line_finished();
	if (bank_.is_null()) {
		return;
	}
	std::string set_name;
	while (dialog_queue_.take_next(set_name)) {
		AudioStreamPlayer *voice = bank_->spawn_oneshot_2d(this, opennova::to_gd(set_name),
				StringName(kVoiceBus));
		if (voice != nullptr) {
			dialog_voice_id_ = ObjectID(voice->get_instance_id());
			dialog_queue_.line_started();
			voice->connect("finished", Callable(this, "_on_dialog_finished"));
			return;
		}
	}
}

void MissionAudio::_on_dialog_finished() {
	AudioStreamPlayer *voice = _dialog_voice_node();
	if (voice != nullptr) {
		voice->queue_free();
	}
	dialog_voice_id_ = ObjectID();
	dialog_queue_.line_finished();
	_pump_dialog_queue();
}

bool MissionAudio::play_wac_wave(const String &p_filename) {
	const Ref<Simulation> sim = _simulation();
	if (sim.is_valid()) {
		const bool loaded = sim->play_script_wave(p_filename);
		sync_script_voice();
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
		if (AudioServer::get_singleton()->get_bus_index(StringName(kVoiceBus)) >= 0) {
			voice->set_bus(StringName(kVoiceBus));
		}
		add_child(voice);
		wac_voice_id_ = ObjectID(voice->get_instance_id());
	}
	voice->set_stream(stream);
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
	time_of_day_hhmm_ = p_hhmm;
	if (mixer_.is_valid()) {
		mixer_->set_time_of_day_hours(static_cast<float>(_hhmm_to_hours(p_hhmm)));
	}
}

void MissionAudio::advance_ticks(int64_t p_logic_tick) {
	sync_script_voice();
	if (mixer_.is_null()) {
		return;
	}
	if (!world_driven_ticks_) {
		world_driven_ticks_ = true;
		world_driven_tick_offset_ = MAX(static_cast<int64_t>(0), mixer_->clock_tick() - p_logic_tick);
	}
	const int64_t target_tick = p_logic_tick + world_driven_tick_offset_;
	// Apply each intent at its producer tick before advancing to the end of a
	// catch-up batch. A lane last refreshed early in the batch therefore spends
	// the elapsed ticks from its real 30-tick lifetime.
	_flush_sound_emitters(target_tick);
	mixer_->advance_to_tick(target_tick);
}

void MissionAudio::set_simulation(const Ref<Simulation> &p_sim) {
	const Ref<Simulation> previous = _simulation();
	if (previous.is_valid() && previous != p_sim) previous->set_script_voice_resolver({});
	simulation_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
	_install_voice_resolver();
	if (bank_.is_valid()) {
		bank_->set_occlusion_provider(p_sim);
	}
	if (mixer_.is_valid()) {
		mixer_->set_occlusion_provider(p_sim);
	}
}

void MissionAudio::set_occlusion_override(const Callable &p_override) {
	occlusion_override_ = p_override;
	if (bank_.is_valid()) {
		bank_->set_occlusion_override(p_override);
	}
	if (mixer_.is_valid()) {
		mixer_->set_occlusion_override(p_override);
	}
}

void MissionAudio::tick(const Vector3 &p_camera_pos, double p_delta) {
	const uint64_t start = Time::get_singleton()->get_ticks_usec();
	last_camera_pos_ = p_camera_pos;
	sync_script_voice();
	int writes = 0;
	if (mixer_.is_null()) {
		perf_markers_ = 0;
		perf_voice_writes_ = 0;
		perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
		return;
	}
	// Autonomous owners register at the current clock before consuming this
	// render frame's elapsed time. World-driven callers normally flush chronologically
	// from advance_ticks above; the fallback handles a late same-tick delivery.
	_flush_sound_emitters(mixer_->clock_tick());
	if (!world_driven_ticks_ && p_delta > 0.0) {
		mixer_->advance_seconds(static_cast<float>(p_delta));
	}
	const std::vector<opennova::audio::AmbientCandidate> &rows = mixer_->mix_rows(p_camera_pos);
	// Ranked loudest-first (candidate-id tie-break) by the native mixer. The
	// rows this node can describe go to the engine's channel pool, which decides
	// which candidate rides which physical channel (incumbents keep theirs,
	// dropouts release, entrants restart); the stream resolve is the one device
	// step it asks for, once per entrant, answered here and kept for the bind.
	std::vector<opennova::audio::AmbientCandidate> describable;
	describable.reserve(rows.size());
	for (const opennova::audio::AmbientCandidate &row : rows) {
		const Ref<MissionAudioCandidateBinding> *binding = candidate_lookup_.getptr(row.candidate_id);
		if (binding != nullptr && binding->is_valid()) describable.push_back(row);
	}
	HashMap<int, Ref<AudioStreamWAV>> resolved;
	opennova::audio::AmbientChannelPlan plan;
	channel_pool_.plan(describable, [&](int32_t candidate_id) {
		const Ref<MissionAudioCandidateBinding> *binding = candidate_lookup_.getptr(candidate_id);
		const Ref<AudioStreamWAV> stream =
				_validate_candidate_stream(candidate_id, (*binding)->get_descriptor());
		if (stream.is_null()) return false;
		resolved[candidate_id] = stream;
		return true;
	}, root_attached_, plan);
	while (channels_.size() < plan.channel_count) _new_channel();

	for (const int channel_index : plan.released) {
		const Ref<MissionAudioChannel> &channel = channels_[channel_index];
		AudioStreamPlayer3D *player = channel->get_player();
		if (player != nullptr) {
			player->stop();
			player->set_stream(Ref<AudioStream>());
			player->set_volume_db(static_cast<float>(kSilentDb));
			player->set_process_mode(Node::PROCESS_MODE_DISABLED);
		}
		channel->set_candidate_id(-1);
		writes += 1;
	}
	for (const opennova::audio::AmbientChannelPlan::Bind &bind : plan.binds) {
		const Ref<MissionAudioChannel> &channel = channels_[bind.channel];
		AudioStreamPlayer3D *player = channel->get_player();
		const Ref<MissionAudioCandidateBinding> *binding = candidate_lookup_.getptr(bind.row.candidate_id);
		const Ref<AudioStreamWAV> *stream = resolved.getptr(bind.row.candidate_id);
		if (player == nullptr || binding == nullptr || stream == nullptr) continue;
		SoundBank::configure_ambient_player(player, *stream, (*binding)->get_descriptor(),
				(*binding)->get_bus());
		player->set_position(Vector3(bind.row.pos[0], bind.row.pos[1], bind.row.pos[2]));
		player->set_volume_db(static_cast<float>(SoundBank::volume_db_from_255(bind.row.vol)));
		player->set_pitch_scale(static_cast<float>(
				_pitch_scale((*binding)->get_descriptor(), bind.row.pitch_q16)));
		player->set_process_mode(Node::PROCESS_MODE_INHERIT);
		channel->set_candidate_id(bind.row.candidate_id);
		player->play();
		writes += 1;
	}
	for (const opennova::audio::AmbientChannelPlan::Bind &update : plan.updates) {
		AudioStreamPlayer3D *incumbent = channels_[update.channel]->get_player();
		const Ref<MissionAudioCandidateBinding> *binding =
				candidate_lookup_.getptr(update.row.candidate_id);
		if (incumbent == nullptr || binding == nullptr) continue;
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
		const double pitch_scale = _pitch_scale((*binding)->get_descriptor(), update.row.pitch_q16);
		if (!Math::is_equal_approx(static_cast<double>(incumbent->get_pitch_scale()), pitch_scale)) {
			incumbent->set_pitch_scale(static_cast<float>(pitch_scale));
			changed = true;
		}
		if (changed) writes += 1;
	}
	_prune_dynamic_emitter_states();
	_release_retired_candidate_ids();
	perf_markers_ = static_cast<int>(markers_.size());
	perf_voice_writes_ = writes;
	perf_tick_us_ = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() - start);
	if (stats_.is_valid()) {
		stats_->set_physical_channels(static_cast<int>(channels_.size()));
	}
}

Ref<AudioStreamWAV> MissionAudio::_resolve_candidate_stream(const Ref<AmbientLayer> &p_descriptor) {
	if (p_descriptor.is_null()) {
		return Ref<AudioStreamWAV>();
	}
	const Ref<AudioStreamWAV> injected = p_descriptor->get_stream();
	if (injected.is_valid()) {
		return injected;
	}
	if (bank_.is_null()) {
		return Ref<AudioStreamWAV>();
	}
	return bank_->resolve_ambient_stream(p_descriptor);
}

Ref<AudioStreamWAV> MissionAudio::_validate_candidate_stream(int p_candidate_id,
		const Ref<AmbientLayer> &p_descriptor) {
	const bool first_validation = !validated_candidate_ids_.has(p_candidate_id);
	const Ref<AudioStreamWAV> stream = _resolve_candidate_stream(p_descriptor);
	if (first_validation) {
		validated_candidate_ids_.insert(p_candidate_id);
		if (stats_.is_valid()) {
			stats_->set_ambient_candidates_validated(static_cast<int>(validated_candidate_ids_.size()));
		}
	}
	if (stream.is_valid()) {
		return stream;
	}
	if (stats_.is_valid()) {
		stats_->set_ambient_decode_failures(channel_pool_.failed_count() + 1);
	}
	if (!warned_ambient_decode_failure_) {
		warned_ambient_decode_failure_ = true;
		const String wav_path = p_descriptor.is_valid() ? p_descriptor->get_wav_path() : String();
		UtilityFunctions::push_warning(vformat(
				"MissionAudio: ambient WAV '%s' failed to decode; excluding failed candidates from the eight-channel mix",
				wav_path.is_empty() ? String("<injected>") : wav_path));
	}
	return Ref<AudioStreamWAV>();
}

// One more physical channel: the engine pool decided the growth under its
// budget, on an attached root.
Ref<MissionAudioChannel> MissionAudio::_new_channel() {
	AudioStreamPlayer3D *player = memnew(AudioStreamPlayer3D);
	player->set_name(vformat("AmbientChannel%d", channels_.size()));
	player->set_volume_db(static_cast<float>(kSilentDb));
	player->set_process_mode(Node::PROCESS_MODE_DISABLED);
	add_child(player);
	Ref<MissionAudioChannel> channel;
	channel.instantiate();
	channel->set_player(player);
	channels_.push_back(channel);
	return channel;
}

void MissionAudio::_stop_all_ambient_channels() {
	channel_pool_.release_all();
	for (const Ref<MissionAudioChannel> &channel : channels_) {
		AudioStreamPlayer3D *player = channel->get_player();
		if (player != nullptr) {
			player->stop();
			player->set_stream(Ref<AudioStream>());
			player->set_volume_db(static_cast<float>(kSilentDb));
			player->set_process_mode(Node::PROCESS_MODE_DISABLED);
		}
		channel->set_candidate_id(-1);
	}
}

void MissionAudio::_reset_mission_playback_state() {
	_stop_script_voice(true);
	// Dialog and WAC voices are mission-owned even though they use separate
	// physical players from ambience. Stop them before replacing/queuing their
	// audio root so neither playback nor a queued dialog can cross missions.
	if (AudioStreamPlayer *dialog = _dialog_voice_node()) {
		dialog->stop();
	}
	if (AudioStreamPlayer *wac = _wac_voice_node()) {
		wac->stop();
	}
	dialog_queue_.clear();
	dialog_voice_id_ = ObjectID();
	wac_voice_id_ = ObjectID();
	wac_wav_cache_.clear();
	dbf_.unref();
}

void MissionAudio::teardown() {
	// Freeing the voice children drops the dialog + wac voice nodes too; the
	// ids are cleared so a late `finished` after teardown can't pump a freed
	// queue.
	_apply_reverb(0);
	_stop_all_ambient_channels();
	_reset_mission_playback_state();
	_free_voice_nodes();
	root_attached_ = false;
	markers_.clear();
	channels_.clear();
	channel_pool_.reset();
	validated_candidate_ids_.clear();
	warned_ambient_decode_failure_ = false;
	mixer_.unref();
	candidate_lookup_.clear();
	free_candidate_ids_.clear();
	retired_candidate_ids_.clear();
	queued_sound_emitters_.clear();
	dynamic_emitter_states_.clear();
	world_driven_ticks_ = false;
	world_driven_tick_offset_ = 0;
	bank_.unref();
	_install_voice_resolver();
}

// --- Internals ---

// Push the resolved marker/layer data into a fresh native mixer. The mixer owns
// the witnessed cadence (staggered eval, tick-unit slot lifetimes) and the ranked
// mix; this node keeps each candidate's descriptor for stream resolution by id.
void MissionAudio::_feed_mixer() {
	mixer_.instantiate();
	mixer_->set_occlusion_provider(_simulation());
	mixer_->set_occlusion_override(occlusion_override_);
	mixer_->set_time_of_day_hours(static_cast<float>(_hhmm_to_hours(time_of_day_hhmm_)));
	candidate_lookup_.clear();
	free_candidate_ids_.clear();
	retired_candidate_ids_.clear();
	queued_sound_emitters_.clear();
	dynamic_emitter_states_.clear();
	world_driven_ticks_ = false;
	world_driven_tick_offset_ = 0;
	for (const Ref<MissionAudioMarker> &marker : markers_) {
		Vector<String> set_names;
		Array sets;
		for (int si = 0; si < marker->set_count(); ++si) {
			PackedInt32Array packed;
			const TypedArray<AmbientLayer> &layers = marker->layers_at(si);
			for (int64_t li = 0; li < layers.size(); ++li) {
				const Ref<AmbientLayer> layer = layers[li];
				if (layer.is_null()) {
					continue;
				}
				const int candidate_id = layer->get_candidate_id();
				Ref<MissionAudioCandidateBinding> binding;
				binding.instantiate();
				binding->set_descriptor(layer);
				binding->set_bus(StringName(kAmbientBus));
				candidate_lookup_[candidate_id] = binding;
				append_layer_row(packed, candidate_id, layer);
			}
			if (packed.is_empty()) {
				continue;
			}
			set_names.push_back(marker->set_name_at(si));
			sets.push_back(packed);
		}
		PackedInt32Array slot_keys;
		slot_keys.resize(4);
		const PackedStringArray slot_sets = marker->get_slot_sets();
		for (int r = 0; r < 4; ++r) {
			slot_keys[r] = -1;
			if (r < slot_sets.size()) {
				slot_keys[r] = set_names.find(slot_sets[r]);
			}
		}
		mixer_->add_marker(marker->get_pos(), marker->get_source_bms_id(), marker->get_stagger_slot(), 0,
				slot_keys, sets);
	}
}

// Resolve queued name-keyed registrations into LWF layer descriptors at their
// producer ticks. Each layer keeps a stable candidate ID while its keyed lane is
// alive; retired IDs are recycled only after no physical channel still carries
// them, keeping the native row identity exact for long missions.
void MissionAudio::_flush_sound_emitters(int64_t p_final_tick) {
	if (mixer_.is_null() || bank_.is_null() || queued_sound_emitters_.empty()) {
		return;
	}
	std::vector<opennova::world::SoundEmitterEvent> pending;
	pending.swap(queued_sound_emitters_);
	std::stable_sort(pending.begin(), pending.end(),
			[](const opennova::world::SoundEmitterEvent &a,
					const opennova::world::SoundEmitterEvent &b) {
				return a.emitted_tick < b.emitted_tick;
			});
	for (const opennova::world::SoundEmitterEvent &event : pending) {
		int64_t event_tick = static_cast<int64_t>(event.emitted_tick);
		if (world_driven_ticks_) {
			event_tick += world_driven_tick_offset_;
		}
		event_tick = MIN(event_tick, p_final_tick);
		if (event_tick > mixer_->clock_tick()) {
			mixer_->advance_to_tick(event_tick);
		}
		const int64_t source_spawn_id = static_cast<int64_t>(event.source_spawn_id);
		const int lane = static_cast<int>(event.lane);
		const String key = dynamic_emitter_key(source_spawn_id, lane);
		// Mission coordinates -> Godot (x, z, -y), matching every other
		// positional presentation drain.
		const Vector3 pos = mission_to_godot(event.pos);
		const int source_bms_id = static_cast<int>(event.source_bms_id);
		// The retail 16-bit slot word the mixer keeps (a sign-extended kick byte
		// is a long loop, not a dead one); the expiry bookkeeping below counts
		// the same word.
		const int lifetime = opennova::audio::emitter_lifetime_word(event.lifetime_ticks);
		const int pitch_q16 = static_cast<int>(event.pitch_q16);
		const int volume_q8_8 = static_cast<int>(event.volume_q8_8);
		if (event.source_only) {
			mixer_->update_emitter_source(source_spawn_id, pos, source_bms_id);
			continue;
		}
		if (pitch_q16 == 0 || volume_q8_8 == 0) {
			mixer_->register_emitter(source_spawn_id, lane, pos, source_bms_id, lifetime, pitch_q16,
					volume_q8_8, PackedInt32Array());
			_forget_dynamic_emitter(key);
			continue;
		}

		const String set_name = opennova::to_gd(event.set_name);
		if (set_name.is_empty()) {
			continue;
		}
		const TypedArray<AmbientLayer> described = bank_->describe_ambient(set_name);
		if (described.is_empty()) {
			continue;
		}
		Ref<MissionAudioDynamicEmitter> emitter;
		if (const Ref<MissionAudioDynamicEmitter> *known = dynamic_emitter_states_.getptr(key)) {
			emitter = *known;
		}
		if (emitter.is_null() || emitter->get_set_name() != set_name ||
				emitter->get_candidate_ids().size() != described.size()) {
			if (emitter.is_valid()) {
				mixer_->register_emitter(source_spawn_id, lane, pos, source_bms_id, lifetime, 0, 0,
						PackedInt32Array());
				_forget_dynamic_emitter(key);
			}
			PackedInt32Array candidate_ids;
			candidate_ids.resize(described.size());
			for (int64_t i = 0; i < described.size(); ++i) {
				candidate_ids[i] = _allocate_dynamic_candidate_id();
			}
			emitter.instantiate();
			emitter->set_set_name(set_name);
			emitter->set_candidate_ids(candidate_ids);
			dynamic_emitter_states_[key] = emitter;
		}
		emitter->set_expires_tick(mixer_->clock_tick() + lifetime);

		PackedInt32Array layers;
		const PackedInt32Array candidate_ids = emitter->get_candidate_ids();
		for (int64_t i = 0; i < described.size(); ++i) {
			const Ref<AmbientLayer> descriptor = described[i];
			const int candidate_id = candidate_ids[i];
			descriptor->set_candidate_id(candidate_id);
			Ref<MissionAudioCandidateBinding> binding;
			binding.instantiate();
			binding->set_descriptor(descriptor);
			binding->set_bus(StringName(kSfxBus));
			candidate_lookup_[candidate_id] = binding;
			append_layer_row(layers, candidate_id, descriptor);
		}
		mixer_->register_emitter(source_spawn_id, lane, pos, source_bms_id, lifetime, pitch_q16,
				volume_q8_8, layers);
	}
}

void MissionAudio::_forget_dynamic_emitter(const String &p_key) {
	const Ref<MissionAudioDynamicEmitter> *known = dynamic_emitter_states_.getptr(p_key);
	if (known == nullptr || known->is_null()) {
		return;
	}
	const PackedInt32Array candidate_ids = (*known)->get_candidate_ids();
	for (int64_t i = 0; i < candidate_ids.size(); ++i) {
		const int id = candidate_ids[i];
		candidate_lookup_.erase(id);
		channel_pool_.forget_candidate(id);
		validated_candidate_ids_.erase(id);
		retired_candidate_ids_.push_back(id);
	}
	dynamic_emitter_states_.erase(p_key);
}

int MissionAudio::_allocate_dynamic_candidate_id() {
	if (!free_candidate_ids_.is_empty()) {
		const int id = free_candidate_ids_[free_candidate_ids_.size() - 1];
		free_candidate_ids_.resize(free_candidate_ids_.size() - 1);
		return id;
	}
	const int candidate_id = next_candidate_id_;
	next_candidate_id_ += 1;
	return candidate_id;
}

void MissionAudio::_prune_dynamic_emitter_states() {
	if (mixer_.is_null()) {
		return;
	}
	const int64_t now_tick = mixer_->clock_tick();
	Vector<String> expired_keys;
	for (const KeyValue<String, Ref<MissionAudioDynamicEmitter>> &entry : dynamic_emitter_states_) {
		if (entry.value.is_valid() && now_tick > entry.value->get_expires_tick()) {
			expired_keys.push_back(entry.key);
		}
	}
	for (const String &key : expired_keys) {
		_forget_dynamic_emitter(key);
	}
}

void MissionAudio::_release_retired_candidate_ids() {
	if (retired_candidate_ids_.is_empty()) {
		return;
	}
	HashSet<int> bound_ids;
	for (const Ref<MissionAudioChannel> &channel : channels_) {
		if (channel->get_candidate_id() >= 0) {
			bound_ids.insert(channel->get_candidate_id());
		}
	}
	Vector<int> still_retired;
	for (const int candidate_id : retired_candidate_ids_) {
		if (bound_ids.has(candidate_id)) {
			still_retired.push_back(candidate_id);
		} else {
			free_candidate_ids_.push_back(candidate_id);
		}
	}
	retired_candidate_ids_ = still_retired;
}

// The player's pitch: the layer's authored base pitch times the emitter's
// 16.16 pitch word (the native mix row's pitch_q16).
double MissionAudio::_pitch_scale(const Ref<AmbientLayer> &p_descriptor, int p_pitch_q16) {
	const double base_pitch = p_descriptor.is_valid() ? p_descriptor->get_base_pitch() : 1.0;
	return SoundBank::effective_base_pitch(base_pitch) *
			MAX(static_cast<double>(AmbientMixer::q16_to_float(p_pitch_q16)), 0.0001);
}

// HHMM (MissionEnvironment.time_of_day) -> hours, through the engine's
// conversion (environment_state.h hhmm_to_minute_of_day).
double MissionAudio::_hhmm_to_hours(double p_hhmm) {
	return opennova::env::EnvironmentState::hhmm_to_minute_of_day(p_hhmm) / kMinutesPerHour;
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
