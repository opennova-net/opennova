#include "mission/mission_root.h"

#include <runtime/mission/mission_sidecars.h>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "audio/mission_audio.h"
#include "env/mission_environment.h"
#include "lights/effect_light_director.h"
#include "network/net_protocol.h"
#include "particle/effect_world.h"
#include "resource_index/launch_flags.h"
#include "simulation/present_event_records.h"
#include "util/string_convert.h"
#include "world/item_effect_director.h"

namespace godot {

namespace {

constexpr const char *kEntitiesName = "Entities";
constexpr const char *kSignalEffectsDrained = "effects_drained";
constexpr const char *kSignalFixedTickCompleted = "fixed_tick_completed";
constexpr const char *kSignalSimulationRestarted = "simulation_restarted";
constexpr const char *kSignalCaptureChanged = "capture_changed";
constexpr const char *kResetWireRuntimeState = "reset_wire_runtime_state";
constexpr const char *kOnCaptureChanged = "_on_frame_stats_capture_changed";

int64_t now_usec() {
	return Time::get_singleton()->get_ticks_usec();
}

} // namespace

MissionRoot::MissionRoot() = default;

// A root that never entered the tree (an isolated construction) still owns
// its sim: the Ref member drops with the destructor. In the tree the
// explicit _exit_tree order below runs first and this releases nothing new.
MissionRoot::~MissionRoot() = default;

void MissionRoot::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "mission", "container", "options"),
			&MissionRoot::setup, DEFVAL(Ref<MissionSetupOptions>()));
	ClassDB::bind_method(D_METHOD("has_current_present_effect_snapshot"),
			&MissionRoot::has_current_present_effect_snapshot);
	ClassDB::bind_method(D_METHOD("presented_entity_effect_transform", "entity_ref"),
			&MissionRoot::presented_entity_effect_transform);
	ClassDB::bind_method(D_METHOD("get_mission_file"), &MissionRoot::get_mission_file);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &MissionRoot::get_mission_name);
	ClassDB::bind_method(D_METHOD("has_player"), &MissionRoot::has_player);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "board"), &MissionRoot::set_frame_stats);
	ClassDB::bind_method(D_METHOD("set_runtime_profiling_enabled", "enabled"),
			&MissionRoot::set_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD(kOnCaptureChanged, "active"),
			&MissionRoot::_on_frame_stats_capture_changed);
	ClassDB::bind_method(D_METHOD("get_mission_present_stats"),
			&MissionRoot::get_mission_present_stats);
	ClassDB::bind_method(D_METHOD("setup_passes", "audio", "fx", "lights", "environment",
			"anchors"), &MissionRoot::setup_passes);
	ClassDB::bind_method(D_METHOD("get_fire_present_stats"),
			&MissionRoot::get_fire_present_stats);
	ClassDB::bind_method(D_METHOD("get_destruction_present_stats"),
			&MissionRoot::get_destruction_present_stats);
	ClassDB::bind_method(D_METHOD("get_wire_present_stats"),
			&MissionRoot::get_wire_present_stats);
	ClassDB::bind_method(D_METHOD("get_throwable_present_stats"),
			&MissionRoot::get_throwable_present_stats);
	ClassDB::bind_method(D_METHOD("get_scar_present_stats"),
			&MissionRoot::get_scar_present_stats);
	ClassDB::bind_method(D_METHOD("get_sim"), &MissionRoot::get_sim);
	ClassDB::bind_method(D_METHOD("set_presentation_time_ms", "value_ms"),
			&MissionRoot::set_presentation_time_ms);
	ClassDB::bind_method(D_METHOD("get_entity_index"), &MissionRoot::get_entity_index);
	ClassDB::bind_method(D_METHOD("get_item_db"), &MissionRoot::get_item_db);
	ClassDB::bind_method(D_METHOD("get_entity_presenter"),
			&MissionRoot::get_entity_presenter);
	ClassDB::bind_method(D_METHOD("entity_count"), &MissionRoot::entity_count);
	ClassDB::bind_method(D_METHOD("is_playing"), &MissionRoot::is_playing);
	ClassDB::bind_method(D_METHOD("tick"), &MissionRoot::tick);
	ClassDB::bind_method(D_METHOD("advance_session_frame", "input"),
			&MissionRoot::advance_session_frame);
	ClassDB::bind_method(D_METHOD("get_perf_counters"), &MissionRoot::get_perf_counters);
	ClassDB::bind_method(D_METHOD("is_transport_locked"), &MissionRoot::is_transport_locked);
	ClassDB::bind_method(D_METHOD("play"), &MissionRoot::play);
	ClassDB::bind_method(D_METHOD("pause"), &MissionRoot::pause);
	ClassDB::bind_method(D_METHOD("step_once"), &MissionRoot::step_once);
	ClassDB::bind_method(D_METHOD("stop"), &MissionRoot::stop);

	// Emitted per authoritative 62.5 Hz logic step with that step's drained
	// side effects (MissionEffect rows), synchronously inside the catch-up
	// batch; empty drains emit nothing.
	ADD_SIGNAL(MethodInfo(kSignalEffectsDrained, PropertyInfo(Variant::ARRAY, "effects")));
	// Emitted once after every authoritative 62.5 Hz logic step, after that
	// step's side effects have been delivered. Presentation-only fixed-step
	// systems (particles) subscribe here instead of integrating render delta.
	ADD_SIGNAL(MethodInfo(kSignalFixedTickCompleted, PropertyInfo(Variant::INT, "logic_tick")));
	// Emitted after Stop rewinds simulation and authored transforms.
	// GameWorld-owned presentation systems use this boundary to discard
	// transient runtime state.
	ADD_SIGNAL(MethodInfo(kSignalSimulationRestarted));

	BIND_ENUM_CONSTANT(STATS_ROLE_SINGLE_PLAYER);
	BIND_ENUM_CONSTANT(STATS_ROLE_HOST);
	BIND_ENUM_CONSTANT(STATS_ROLE_JOINER);
}

EntityPresenter *MissionRoot::entities() const {
	return entities_id_.is_valid()
			? Object::cast_to<EntityPresenter>(ObjectDB::get_instance(entities_id_))
			: nullptr;
}

Node3D *MissionRoot::container() const {
	return container_id_.is_valid()
			? Object::cast_to<Node3D>(ObjectDB::get_instance(container_id_))
			: nullptr;
}

int MissionRoot::setup(const Ref<MissionData> &p_mission, Node *p_container,
		const Ref<MissionSetupOptions> &p_options) {
	Ref<MissionSetupOptions> options = p_options;
	if (options.is_null()) {
		options.instantiate();
	}
	clear_present_effect_poses();
	setup_error_ = OK;
	mission_ = p_mission;
	// A remote join may already own the live socket + NP session while it waits
	// for S2C 0x7B to identify the mission. Keep that exact connection across the
	// wire-header world construction instead of reconnecting after discovery. Standalone host/SP and
	// isolated test/tooling callers do not provide a simulation and retain the fresh-instance path.
	sim_ = options->get_simulation();
	if (sim_.is_null()) {
		sim_.instantiate();
		// `--capture-pcap <path>` (LaunchFlags) records this process's datagrams.
		sim_->set_capture_pcap_path(LaunchFlags::capture_pcap());
	}
	sim_->set_music_director(options->get_music_director());
	sim_->set_frame_stats(frame_stats_);
	has_trace_stats_sampling_ = sim_.is_valid();
	sync_runtime_profiling();
	mission_file_ = options->get_mission_file().replace("\\", "/").get_file();
	mission_name_ = options->get_mission_name();
	if (mission_name_.is_empty() && p_mission.is_valid()) {
		mission_name_ = p_mission->get_mission_name().strip_edges();
	}
	if (mission_name_.is_empty() && !mission_file_.is_empty()) {
		mission_name_ = mission_file_.get_file().get_basename();
	}

	// The listen host stamps its own type-2 connection during role bring-up, so
	// its two per-side character selections must already be present here. The
	// same profile is what a joiner uploads through ClientAuth.
	// [orig: Game_ApplySessionSettingsToGlobals @0x551500;
	//  Server_PlayerAdd @0x51CBC0 -> packed id @0x51D0B1]
	if (options->get_local_character_profile().is_valid()) {
		sim_->set_local_character_profile(options->get_local_character_profile());
	}
	// The player profile's current records ahead of the boot: single player's
	// session words are the record's (the session config the boot builds).
	if (options->get_player_profiles().is_valid()) {
		sim_->use_player_profile(options->get_player_profiles());
	}
	// P7 / ADR 0011: every authoritative live mission is an in-process listen server, stood up BEFORE
	// load; the host player auto-spawns at bring-up (faithful §5.0 mode-3). MainGame/GameWorld is the
	// sole live runtime owner (ADR 0025). Isolated tests may instantiate this same
	// seam. A co-op LAN host additionally binds a real UDP
	// socket; a joiner is the non-authority client.
	const bool playable = options->get_playable();
	const Ref<JoinTarget> join_target = options->get_join_target();
	const Ref<HostSessionOptions> host_session = options->get_host_session();
	const bool is_joiner = join_target.is_valid() || sim_->is_joiner();
	if (is_joiner) {
		// Co-op LAN JOINER (a non-authority client): dial the host and run the witnessed
		// in-match JOIN. The local player L is spawned on the name-match (inside the sim's
		// joiner poll), NOT here. The player_name rides game ClientAuth.NA. [net-re §5.38b]
		// A preconnected simulation already completed this socket leg while the loading
		// screen was up; do not replace its connection, restart its handshake, or
		// reload charattr after ordered S2C 0x41 mutations have already landed.
		const bool needs_join_connection = !sim_->is_joiner();
		if (needs_join_connection && options->get_resource_root().is_valid()) {
			sim_->load_charattr(options->get_resource_root());
		}
		if (needs_join_connection && options->get_join_character_profile().is_valid()) {
			sim_->set_join_character_profile(options->get_join_character_profile());
		}
		if (needs_join_connection && join_target.is_valid() &&
				!join_target->get_integrity_profile().is_empty() &&
				!sim_->set_join_integrity_profile(join_target->get_integrity_profile())) {
			UtilityFunctions::push_warning(vformat(
					"MissionRoot: unknown join integrity profile '%s'.",
					join_target->get_integrity_profile()));
			setup_error_ = ERR_INVALID_PARAMETER;
			sim_.unref();
			has_trace_stats_sampling_ = false;
			return 0;
		}
		if (needs_join_connection && join_target.is_valid() && !sim_->enable_join(
				join_target->get_host_ip(), join_target->get_port(),
				join_target->get_player_name(), join_target->get_join_role(),
				join_target->get_spectator_password(), join_target->get_server_password())) {
			UtilityFunctions::push_warning(vformat(
					"MissionRoot: could not dial co-op host %s:%d — joiner disabled.",
					join_target->get_host_ip(), join_target->get_port()));
		}
		// The proxy-assisted NovaWorld join rides the same target (the preloaded
		// drive path installs it at its own dial).
		if (needs_join_connection && join_target.is_valid() && join_target->has_join_proxy() &&
				sim_->is_joiner()) {
			sim_->set_join_proxy(join_target->get_proxy_node(), join_target->get_proxy_relay(),
					join_target->get_proxy_cookie());
		}
		// The JOIN VERSIONCRCSTRING checksum reads the loose
		// expansion/<name>/version.txt under the install root (D-NET-166).
		if (needs_join_connection && options->get_resource_root().is_valid()) {
			sim_->set_join_expansion_version_root(options->get_resource_root()->get_root_dir());
		}
	} else if (host_session.is_valid()) {
		// Co-op LAN HOST: the typed session request already projected to the sim's
		// record (HostSessionConfig.to_session_options, at staging); stamp the
		// mission-derived identity the session advertises on top. bind_port
		// defaults to the witnessed retail LAN host port (HostSessionConfig.DEFAULT_LAN_PORT);
		// serve-and-play vs DEDICATED and the lobby player cap ride the same record
		// (net-re §5.2b, host_session_pump step 5).
		const Ref<HostSessionOptions> session_options = host_session;
		if (host_session->get_game_type_auto()) {
			int mission_mode = 0;
			if (p_mission.is_valid()) {
				mission_mode = static_cast<int>(p_mission->get_game_mode());
			}
			session_options->set_game_type(NetProtocol::game_type_for_mission_mode(mission_mode));
		}
		String mission_name = options->get_mission_name().strip_edges();
		if (mission_name.is_empty() && p_mission.is_valid()) {
			mission_name = p_mission->get_mission_name().strip_edges();
		}
		if (mission_name.is_empty() && !options->get_mission_file().is_empty()) {
			mission_name = options->get_mission_file().get_basename();
		}
		if (!options->get_mission_file().is_empty()) {
			session_options->set_mission_file(options->get_mission_file());
		}
		if (!options->get_spawn_names().is_empty()) {
			session_options->set_spawn_names(options->get_spawn_names());
		}
		if (!mission_name.is_empty()) {
			session_options->set_mission_name(mission_name);
			if (session_options->get_spawn_names().is_empty()) {
				PackedStringArray spawn_names;
				spawn_names.push_back(mission_name);
				session_options->set_spawn_names(spawn_names);
			}
		}
		// The host's expansion version checksum (retail's g_ExpansionChecksum) is
		// CRC'd from the loose expansion/<name>/version.txt under the install root;
		// the join gate compares it against each joiner's VERSIONCRCSTRING while an
		// expansion is active (D-NET-166).
		if (options->get_resource_root().is_valid()) {
			session_options->set_game_root(options->get_resource_root()->get_root_dir());
		}
		// The loose score.ini overlays the session's score table inside the
		// boot, ahead of the bring-up (inmatch/host_boot.h).
		sim_->configure_host_session(session_options);
		// A NovaWorld host's match rides its NovaWorld session's socket (D-NET-346).
		const Ref<UdpPump> host_pump = options->get_host_pump();
		const bool listening = host_pump.is_valid() ? sim_->enable_host_listen_on(host_pump)
		                                            : sim_->enable_host_listen(host_session->get_bind_port());
		if (!listening) {
			// A requested LAN host that cannot own its UDP endpoint is not a host.
			// Never degrade into the visually-identical socketless SP/listen path:
			// the caller must surface the bind failure and keep the menu active.
			setup_error_ = ERR_CANT_CREATE;
			sim_.unref();
			has_trace_stats_sampling_ = false;
			return 0;
		}
	} else if (!sim_->is_host_listening()) {
		// Standalone SP (or an isolated tooling/test preview): the in-process listen server. Live
		// play reaches this branch only through GameWorld. The host player auto-spawns at bring-up.
		// A host's map change brings its live session (its role, its bound socket and its
		// connections) in the simulation the drive kept, and boots the next map inside it.
		sim_->enable_listen_server(true);
	}
	// S9 (ADR 0028): the ordered mission boot. The sequence, its gates, and the
	// file-resolution policy (mission-text fallback, .aip profile speeds, the
	// adm default) live in engine/runtime/mission runtime_boot; boot_mission
	// supplies the step bodies over the sim's feeds — including the S16 native
	// seat-spec extraction (the GDScript extractor and its Array pass-through
	// are gone). Role bring-up ran above; presentation composition follows.
	// The infantry .adm name is the boot's own default (no composition ever
	// named one; the record's dead field went with it).
	const int64_t boot_err = sim_->boot_mission(
			p_mission,
			options->get_resource_root(),
			options->get_item_db(),
			options->get_terrain(),
			options->get_terrain_til(),
			options->get_wac_basename(),
			String(),
			// The by-name readers' base: the name cut at its first '.'.
			opennova::to_gd(opennova::mission::mission_base_name(opennova::to_std(mission_file_))),
			playable);
	if (boot_err != OK) {
		setup_error_ = ERR_CANT_OPEN;
		sim_.unref(); // the RefCounted sim drops with its last reference
		has_trace_stats_sampling_ = false;
		return 0;
	}
	// The world's load runs the boot's phase B after its device stages; an
	// isolated root (tests, tools) ends the load here, the start pending.
	if (!options->get_mission_start_deferred()) sim_->finish_load_without_environment();
	// The shared render/PANM presentation DWORD — re-stamped after the boot
	// because the load reset cleared it (an order-free scalar, not a boot step).
	if (presentation_time_ms_ >= 0) {
		sim_->set_panm_time_ms(presentation_time_ms_);
	}
	// The SIM is a RefCounted this root owns: only this root advances it; it
	// drops with the last reference in _exit_tree.
	// This MissionRoot node itself IS in the tree — GameWorld adds it and
	// drives advance_session_frame() explicitly (ADR 0025: the game shell is the only live runtime owner).
	index_.instantiate();
	const Ref<MissionObjectPlacer> registry_placer = options->get_placer();
	registry_placer_ = registry_placer;
	TypedArray<ObjectModel> placed_models;
	if (registry_placer.is_valid()) {
		placed_models = registry_placer->get_placed_models();
	}
	index_->build(placed_models, p_mission);
	// The registry present drives whichever authored mission nodes actually exist. A
	// production joiner owns only the 616-byte wire header, so its index is empty: the
	// native sim separately materializes streamed pools 1-3 at exact packed handles for
	// world-side consumers, while the wire walk below renders their decoded live state.
	// Complete-BMS/debug joins still retain authored nodes and the ordinary defer identity.
	// ONE presenter node runs both walks over the same snapshot (ADR 0043 d9); output
	// channels keep its native OUTPUT_ALL default. Visibility ownership is the two bits
	// on each ObjectModel: the placed walk writes the sim's intent
	// (set_present_visible), the occlusion frame its claim (set_occlusion_hidden), and
	// the node's visible flag is their product, so neither writer fights the other.
	EntityPresenter *presenter = memnew(EntityPresenter);
	presenter->set_name(kEntitiesName);
	add_child(presenter);
	entities_id_ = presenter->get_instance_id();
	presenter->setup(sim_.ptr(), index_.ptr(), registry_placer);
	// Co-op renders decoded remote rows WIRE-DIRECT. On a host that principally covers
	// dynamically admitted players; on a header-only joiner it covers every remote row,
	// because none has an authored placed node. Complete-BMS/debug roles still defer any
	// row resolved by the index so the placed and wire walks never double-render.
	const bool full_wire_present = is_joiner || sim_->is_host_listening();
	const bool sp_attachment_present = !full_wire_present && options->get_placer().is_valid();
	if (full_wire_present || sp_attachment_present) {
		// A retail network join has no local BMS identity table at load: the
		// index is empty until the host's world stream settles, when GameWorld
		// places the streamed statics through the same placer and
		// rebind_placed_entities() fills this SAME index. Rows carrying a placed
		// identity defer with or without a resolvable node, so the wire walk
		// keeps only organics and runtime spawns on every role.
		presenter->setup_wire(sim_.ptr(), options->get_placer(),
				Object::cast_to<Node3D>(p_container), index_);
		presenter->set_synthetic_origin_only(sp_attachment_present);
	}
	// The Stop -> Play boundary: the one connect resets the wire registry AND
	// the presenter's four passes (destruction, throwable, scars reset their
	// runtime state inside reset_wire_runtime_state).
	connect(kSignalSimulationRestarted, Callable(presenter, kResetWireRuntimeState));
	// The four tick-driven present passes are EntityPresenter's own (ADR 0043
	// d9): the fire pass (AI/remote fire sound + muzzle effect + tracers off
	// the sim's fired/tracer drains; a joiner re-runs decoded S2C tag-2 rounds
	// through the same visual RoundSim, so it drains this queue too), the
	// destruction pass (husk swaps, death-piece debris, wreck fire/smoke,
	// destruction sounds off world/destruction.h; every viewing peer), the
	// throwable pass (item models for flying grenades/satchels and placed
	// devices; world-wac-ai-re §27) and the scar pass (world-wac-ai-re §24.9;
	// every viewing peer). Their typed collaborators — the mission audio, the
	// effect world, the light director, the environment node and the
	// owner-anchor registry — exist only after the later load stages, so the
	// load binds them through setup_passes; the pass inputs known now are
	// captured here.
	container_id_ = p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	item_db_ = options->get_item_db();
	resource_root_ = options->get_resource_root();
	// ADR 0035: the native session owns lifecycle/cadence and invokes one
	// synchronous per-tick presentation sink (this root). The camera remains a
	// Godot device; a dedicated host has none.
	// Capture the authored node transforms now (pre-tick) so Stop restores them whether the caller
	// played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	capture_transforms();
	return sim_->get_entity_count();
}

Variant MissionRoot::entity_effect_transform_for_ssn(int p_ssn) const {
	if (sim_.is_null() || p_ssn <= 0) {
		return Variant();
	}
	// Once a logic tick has completed, follow the exact client-view pose that the
	// render pass will present for that tick. Before the first tick there is no
	// such snapshot, so retain the authoritative registry lookup as the seed.
	if (has_current_present_effect_snapshot()) {
		return effect_transform_from_state(sim_->get_present_effect_state_for_ssn(p_ssn));
	}
	return effect_transform_from_state(sim_->get_entity_effect_state_for_ssn(p_ssn));
}

bool MissionRoot::has_current_present_effect_snapshot() const {
	return sim_.is_valid() && effect_pose_snapshot_tick_ >= 0;
}

Variant MissionRoot::presented_entity_effect_transform(const Ref<EntityRef> &p_entity_ref) const {
	if (p_entity_ref.is_null() || !has_current_present_effect_snapshot()) {
		return Variant();
	}
	const int wire_handle = p_entity_ref->get_wire_handle();
	PackedVector3Array state;
	if (wire_handle >= 0 && wire_handle <= 0xffff) {
		state = sim_->get_present_effect_state_for_wire_handle(wire_handle);
	} else {
		const int native_bms_id = p_entity_ref->get_bms_id();
		if (native_bms_id > 0) {
			state = sim_->get_present_effect_state_for_bms_id(native_bms_id);
		} else {
			const int native_kind = p_entity_ref->get_kind() >= 0
					? p_entity_ref->get_kind()
					: p_entity_ref->get_origin_kind();
			const int native_index = p_entity_ref->get_index();
			if (native_kind >= 0 && native_index >= 0) {
				state = sim_->get_present_effect_state_for_origin(native_kind, native_index);
			}
		}
	}
	return effect_transform_from_state(state);
}

Variant MissionRoot::effect_transform_from_state(const PackedVector3Array &p_state) const {
	if (p_state.size() != Simulation::EFFECT_STATE_COUNT) {
		return Variant();
	}
	return Transform3D(
			MissionObjectPlacer::bms_to_godot_basis(p_state[Simulation::EFFECT_STATE_ROTATION_DEG]),
			p_state[Simulation::EFFECT_STATE_POSITION]);
}

void MissionRoot::clear_present_effect_poses() {
	effect_pose_snapshot_tick_ = -1;
}

void MissionRoot::begin_present_effect_tick(int p_logic_tick) {
	effect_pose_snapshot_tick_ = p_logic_tick;
}

bool MissionRoot::has_player() const {
	return sim_.is_valid() && sim_->has_local_player();
}

Ref<PlayerAimOverlay> MissionRoot::local_player_aim_overlay() const {
	return sim_.is_valid() ? sim_->get_local_player_aim_overlay() : Ref<PlayerAimOverlay>();
}

void MissionRoot::set_frame_stats(const Ref<FrameStats> &p_board) {
	if (p_board == frame_stats_) {
		return;
	}
	if (sim_.is_valid()) {
		sim_->set_frame_stats(p_board);
	}
	if (frame_stats_.is_valid()) {
		const Callable old_capture_changed(this, kOnCaptureChanged);
		if (frame_stats_->is_connected(kSignalCaptureChanged, old_capture_changed)) {
			frame_stats_->disconnect(kSignalCaptureChanged, old_capture_changed);
		}
	}
	frame_stats_ = p_board;
	if (frame_stats_.is_valid()) {
		const Callable capture_changed(this, kOnCaptureChanged);
		if (!frame_stats_->is_connected(kSignalCaptureChanged, capture_changed)) {
			frame_stats_->connect(kSignalCaptureChanged, capture_changed);
		}
	}
	sync_runtime_profiling();
}

void MissionRoot::set_runtime_profiling_enabled(bool p_enabled) {
	runtime_probe_enabled_ = p_enabled;
	sync_runtime_profiling();
}

void MissionRoot::_on_frame_stats_capture_changed(bool p_active) {
	(void)p_active;
	sync_runtime_profiling();
}

void MissionRoot::sync_runtime_profiling() {
	if (sim_.is_null()) {
		return;
	}
	const bool stats_active = frame_stats_.is_valid() && frame_stats_->is_capture_active();
	sim_->set_runtime_profiling_enabled(runtime_probe_enabled_ || stats_active);
}

Ref<MissionPresentStats> MissionRoot::get_mission_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_stats_record();
	}
	Ref<MissionPresentStats> empty;
	empty.instantiate();
	return empty;
}

void MissionRoot::setup_passes(MissionAudio *p_audio, EffectWorld *p_fx,
		EffectLightDirector *p_lights, MissionEnvironment *p_environment,
		ItemEffectDirector *p_anchors) {
	EntityPresenter *presenter = entities();
	if (presenter == nullptr) {
		return;
	}
	presenter->setup_passes(container(), item_db_, resource_root_, p_audio, p_fx, p_lights,
			p_environment, p_anchors);
}

Ref<FirePresentStats> MissionRoot::get_fire_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_fire_present_stats();
	}
	Ref<FirePresentStats> empty;
	empty.instantiate();
	return empty;
}

Ref<DestructionPresentStats> MissionRoot::get_destruction_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_destruction_present_stats();
	}
	Ref<DestructionPresentStats> empty;
	empty.instantiate();
	return empty;
}

Ref<WirePresentStats> MissionRoot::get_wire_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_wire_stats_record();
	}
	Ref<WirePresentStats> empty;
	empty.instantiate();
	return empty;
}

int MissionRoot::join_wire_present_pending() const {
	EntityPresenter *presenter = entities();
	return presenter != nullptr ? presenter->pending_spawn_count() : 0;
}

void MissionRoot::warm_present_pipelines(const Vector3 &p_at_position) {
	if (EntityPresenter *presenter = entities()) {
		presenter->warm_fire_pipelines(p_at_position);
	}
}

Ref<ThrowablePresentStats> MissionRoot::get_throwable_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_throwable_present_stats();
	}
	Ref<ThrowablePresentStats> empty;
	empty.instantiate();
	return empty;
}

Ref<ScarPresentStats> MissionRoot::get_scar_present_stats() const {
	if (EntityPresenter *presenter = entities()) {
		return presenter->get_scar_present_stats();
	}
	Ref<ScarPresentStats> empty;
	empty.instantiate();
	return empty;
}

void MissionRoot::set_presentation_time_ms(int64_t p_value_ms) {
	presentation_time_ms_ = p_value_ms < 0
			? -1
			: static_cast<int64_t>(static_cast<uint64_t>(p_value_ms) & 0xffffffffULL);
	if (sim_.is_valid()) {
		sim_->set_panm_time_ms(presentation_time_ms_);
	}
}

EntityPresenter *MissionRoot::get_entity_presenter() const {
	return entities();
}

void MissionRoot::rebind_placed_entities(const Ref<MissionObjectPlacer> &p_placer) {
	if (index_.is_null() || p_placer.is_null()) {
		return;
	}
	registry_placer_ = p_placer;
	index_->build(p_placer->get_placed_models(), Ref<MissionData>());
}

void MissionRoot::retire_placed_rows() {
	if (sim_.is_null() || !sim_->is_joiner()) {
		return;
	}
	const PackedInt32Array retired = sim_->take_retired_placement_ids();
	for (int64_t i = 0; i < retired.size(); ++i) {
		const int bms_id = retired[i];
		if (index_.is_valid()) {
			if (ObjectModel *node = index_->resolve_single(bms_id)) {
				node->set_present_visible(false);
			}
		}
		if (registry_placer_.is_valid()) {
			registry_placer_->hide_static_instance(bms_id);
		}
	}
}

int MissionRoot::entity_count() const {
	return sim_.is_valid() ? sim_->get_entity_count() : 0;
}

bool MissionRoot::is_playing() const {
	return sim_.is_valid() && sim_->is_playing();
}

void MissionRoot::present_entity_rows(bool p_stats_on) {
	EntityPresenter *presenter = entities();
	if (sim_.is_null() || presenter == nullptr) {
		return;
	}
	const int stride = sim_->get_present_stride();
	if (stride <= 0) {
		return;
	}
	// Both walks consume the same immutable row buffer. Fetching it once also
	// makes their topology revision refer to exactly the same layout.
	const auto snapshot = sim_->build_present_snapshot();
	const opennova::world::PresentRowsView rows{
			snapshot->rows.data(), static_cast<int64_t>(snapshot->rows.size())};
	const auto &door_phases = snapshot->door_phases;
	if (p_stats_on) {
		// The native buffer build the fetch above just paid for.
		frame_stats_->add(FrameStats::PRESENT_SNAPSHOT, sim_->get_last_present_snapshot_us());
	}
	const int64_t layout_revision = static_cast<int64_t>(snapshot->layout_revision);
	const int64_t mission_start = p_stats_on ? now_usec() : 0;
	if (p_stats_on) {
		const PackedInt64Array profile = presenter->profile_present_snapshot(
				rows, stride, layout_revision, door_phases.data(),
				static_cast<int64_t>(door_phases.size()));
		if (profile.size() >= EntityPresenter::MISSION_PROFILE_SLOT_COUNT) {
			frame_stats_->add(FrameStats::PRESENT_MISSION_CORE,
					profile[EntityPresenter::MISSION_PROFILE_CORE_US]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_AIM,
					profile[EntityPresenter::MISSION_PROFILE_AIM_US]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_CONTROLS,
					profile[EntityPresenter::MISSION_PROFILE_CONTROLS_US]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_VISIBILITY,
					profile[EntityPresenter::MISSION_PROFILE_VISIBILITY_US]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_BODY,
					profile[EntityPresenter::MISSION_PROFILE_BODY_US]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_ROWS,
					profile[EntityPresenter::MISSION_PROFILE_ROWS]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_SUBMITTED_ROWS,
					profile[EntityPresenter::MISSION_PROFILE_SUBMITTED_ROWS]);
			frame_stats_->add(FrameStats::PRESENT_MISSION_BODY_ROWS,
					profile[EntityPresenter::MISSION_PROFILE_BODY_ROWS]);
		}
		frame_stats_->add(FrameStats::PRESENT_MISSION, now_usec() - mission_start);
	} else {
		presenter->present_rows(rows, stride, layout_revision, door_phases.data(),
				static_cast<int64_t>(door_phases.size()));
	}
	// The wire walk is a no-op until setup_wire ran (a placer-less preview).
	const int64_t wire_start = p_stats_on ? now_usec() : 0;
	presenter->present_wire_rows_view(rows, stride, layout_revision);
	if (p_stats_on) {
		frame_stats_->add(FrameStats::PRESENT_WIRE, now_usec() - wire_start);
		const Ref<WirePresentStats> wire_stats = presenter->get_wire_stats_record();
		frame_stats_->add(FrameStats::PRESENT_WIRE_LIVE, wire_stats->live);
		frame_stats_->add(FrameStats::PRESENT_WIRE_PENDING, wire_stats->pending);
	}
}

void MissionRoot::present_frame(bool p_stats_on) {
	const int64_t present_start = now_usec();
	retire_placed_rows();
	present_entity_rows(p_stats_on);
	// After the entity rows, the presenter's four passes in drive order (fire ->
	// destruction -> throwable -> scars: the entity-ring meshes parent under
	// section nodes the row walks may have just built).
	if (EntityPresenter *presenter = entities()) {
		if (p_stats_on) {
			const PackedInt64Array spans = presenter->profile_present_passes();
			if (spans.size() >= EntityPresenter::PASS_PROFILE_SLOT_COUNT) {
				frame_stats_->add(FrameStats::PRESENT_FIRE,
						spans[EntityPresenter::PASS_PROFILE_FIRE_US]);
				frame_stats_->add(FrameStats::PRESENT_DESTRUCTION,
						spans[EntityPresenter::PASS_PROFILE_DESTRUCTION_US]);
				frame_stats_->add(FrameStats::PRESENT_THROWABLE,
						spans[EntityPresenter::PASS_PROFILE_THROWABLE_US]);
				frame_stats_->add(FrameStats::PRESENT_SCARS,
						spans[EntityPresenter::PASS_PROFILE_SCARS_US]);
			}
		} else {
			presenter->present_passes();
		}
	}
	perf_present_us_ = now_usec() - present_start;
}

bool MissionRoot::tick() {
	perf_effects_us_ = 0;
	if (sim_.is_null()) {
		perf_tick_us_ = 0;
		perf_sim_us_ = 0;
		perf_present_us_ = 0;
		perf_effects_us_ = 0;
		perf_did_tick_ = false;
		ticks_last_frame_ = 0;
		return false;
	}
	Ref<MissionFrameInput> input;
	input.instantiate();
	input->set_delta_seconds(Simulation::tick_dt());
	Ref<MissionFrameOutcome> outcome;
	{
		TickSinkScope sink(sim_.ptr(), this);
		outcome = sim_->step_session_frame(input);
	}
	read_frame_perf(outcome);
	if (outcome.is_valid() && !outcome->is_terminal() && outcome->did_tick()) {
		present_frame(stats_capture_on());
	}
	return outcome.is_valid() && outcome->did_tick();
}

// --- Per-tick presentation sink (ADR 0035) -----------------------------------

bool MissionRoot::on_session_tick(const opennova::inmatch::TickOutcome &p_tick) {
	if (sim_.is_null() || p_tick.terminal()) {
		return false;
	}
	begin_present_effect_tick(p_tick.logic_tick);
	// The throwable pass's fixed-tick half: the round-bound move groups
	// reconcile BEFORE the effect world advances on this tick.
	if (EntityPresenter *presenter = entities()) {
		presenter->sync_fixed_tick_effects();
	}
	const int64_t effects_start = now_usec();
	const TypedArray<MissionEffect> effects = sim_->drain_effects();
	perf_effects_us_ += now_usec() - effects_start;
	if (!effects.is_empty()) {
		emit_signal(kSignalEffectsDrained, effects);
	}
	feed_projectile_trace_stats();
	emit_signal(kSignalFixedTickCompleted, p_tick.logic_tick);
	return true;
}

bool MissionRoot::stats_capture_on() const {
	return frame_stats_.is_valid() && frame_stats_->is_capture_active();
}

void MissionRoot::read_frame_perf(const Ref<MissionFrameOutcome> &p_outcome) {
	ticks_last_frame_ = p_outcome.is_valid() ? p_outcome->get_ticks_run() : 0;
	perf_did_tick_ = ticks_last_frame_ > 0;
	perf_tick_us_ = p_outcome.is_valid() ? p_outcome->get_tick_us() : 0;
	perf_sim_us_ = 0;
	if (!sim_->is_runtime_profiling_enabled()) {
		return;
	}
	perf_sim_us_ = sim_->get_last_session_sim_us();
	if (ticks_last_frame_ > 0 && stats_capture_on()) {
		frame_stats_->add(FrameStats::EFFECTS_DRAIN, perf_effects_us_);
		frame_stats_->add(FrameStats::SIM_TICKS, ticks_last_frame_);
		// The sim row's counter cells and the Net row's peer count, folded from
		// the sim this root already owns (frame_stats_slots.h).
		frame_stats_->add(FrameStats::SIM_ENTITY_COUNT, sim_->get_entity_count());
		frame_stats_->add(FrameStats::SIM_ROLE, stats_role_value());
		frame_stats_->add(FrameStats::NET_PEER_COUNT, sim_->get_host_peer_count());
	}
}

int MissionRoot::stats_role_value() const {
	switch (sim_->session_role()) {
		case Simulation::ROLE_JOINER:
			return STATS_ROLE_JOINER;
		case Simulation::ROLE_LISTEN_HOST:
		case Simulation::ROLE_DEDICATED_HOST:
			return STATS_ROLE_HOST;
		default:
			return STATS_ROLE_SINGLE_PLAYER;
	}
}

void MissionRoot::feed_projectile_trace_stats() {
	if (frame_stats_.is_null() || !frame_stats_->is_capture_active() ||
			!has_trace_stats_sampling_) {
		return;
	}
	// Three value-type native reads preserve each logic tick's snapshot without
	// allocating a Dictionary on the capture hot path. In a catch-up render
	// frame the board folds each tick into one complete-frame peak.
	const Vector4i counts = sim_->get_last_projectile_trace_counts();
	if (counts.x <= 0) {
		return;
	}
	const Vector4i times = sim_->get_last_projectile_trace_times_us();
	const Vector2i faces = sim_->get_last_projectile_trace_faces();
	frame_stats_->add(FrameStats::TRACE_TERRAIN, times.x);
	frame_stats_->add(FrameStats::TRACE_STATIC, times.y);
	frame_stats_->add(FrameStats::TRACE_DYNAMIC, times.z);
	frame_stats_->add(FrameStats::TRACE_PERSON, times.w);
	frame_stats_->add(FrameStats::TRACE_CALLS, counts.x);
	frame_stats_->add(FrameStats::TRACE_STATIC_SURVIVORS, counts.y);
	frame_stats_->add(FrameStats::TRACE_DYNAMIC_SURVIVORS, counts.z);
	frame_stats_->add(FrameStats::TRACE_PERSON_SURVIVORS, counts.w);
	frame_stats_->add(FrameStats::TRACE_STATIC_FACES, faces.x);
	frame_stats_->add(FrameStats::TRACE_DYNAMIC_FACES, faces.y);
}

Ref<MissionFrameOutcome> MissionRoot::advance_session_frame(
		const Ref<MissionFrameInput> &p_input) {
	perf_effects_us_ = 0;
	if (sim_.is_null()) {
		ticks_last_frame_ = 0;
		return Ref<MissionFrameOutcome>();
	}
	// The presenting shell's listener (the camera sample the pipeline stamped
	// into this frame's input; the sim's fire-sound gate reads the same
	// sample): the fire pass's ribbon camera and the scar pass's fog cull. A
	// frame with no camera (a dedicated serve, an isolated test) pushes
	// nothing and the passes keep their last listener.
	EntityPresenter *presenter = entities();
	if (presenter != nullptr && p_input.is_valid() && p_input->is_listener_valid()) {
		presenter->set_listener_position(p_input->get_camera_position());
	}
	Ref<MissionFrameOutcome> outcome;
	{
		TickSinkScope sink(sim_.ptr(), this);
		outcome = sim_->advance_session_frame(p_input);
	}
	read_frame_perf(outcome);
	if (outcome.is_null() || outcome->is_terminal()) {
		return outcome;
	}
	if (outcome->did_tick()) {
		present_frame(stats_capture_on());
	} else {
		present_entity_rows(stats_capture_on());
	}
	return outcome;
}

Ref<MissionPerfCounters> MissionRoot::get_perf_counters() const {
	Ref<MissionPerfCounters> out;
	out.instantiate();
	out->set_tick_us(perf_tick_us_);
	out->set_sim_us(perf_sim_us_);
	out->set_present_us(perf_present_us_);
	out->set_effects_us(perf_effects_us_);
	out->set_did_tick(perf_did_tick_);
	out->set_ticks(ticks_last_frame_);
	out->set_sim_counters(sim_.is_valid() ? sim_->get_runtime_perf_counters() : Dictionary());
	return out;
}

// --- Debug/tooling transport (Play / Step / Stop) -----------------------------

bool MissionRoot::is_transport_locked() const {
	// Delegates to the native predicate (S14) — one home for the rule.
	if (sim_.is_null()) {
		return false;
	}
	return sim_->is_transport_locked();
}

bool MissionRoot::play() {
	if (sim_.is_null()) {
		return false;
	}
	return sim_->resume_session();
}

bool MissionRoot::pause() {
	if (is_transport_locked()) {
		return false;
	}
	if (sim_.is_null()) {
		return false;
	}
	return sim_->pause_session();
}

bool MissionRoot::step_once() {
	// Stepping pauses real-time cadence (and any opt-in tooling self-tick), which starves
	// the socket between steps just as pause() does. See is_transport_locked().
	if (is_transport_locked()) {
		return false;
	}
	if (sim_.is_null()) {
		return false;
	}
	sim_->pause_session();
	return tick();
}

void MissionRoot::stop() {
	// Worse than pause on a live session: the restart below rewinds the world under
	// peers that are still streaming against it. See is_transport_locked().
	if (is_transport_locked()) {
		return;
	}
	if (sim_.is_valid()) {
		sim_->reset_session();
	}
	clear_present_effect_poses();
	restore_transforms();
	emit_signal(kSignalSimulationRestarted);
}

void MissionRoot::capture_transforms() {
	orig_transforms_.clear();
	for_each_present_node([this](ObjectModel *p_node) {
		orig_transforms_.insert(p_node->get_instance_id(), p_node->get_transform());
	});
}

void MissionRoot::restore_transforms() {
	for (const KeyValue<uint64_t, Transform3D> &entry : orig_transforms_) {
		ObjectModel *node = Object::cast_to<ObjectModel>(ObjectDB::get_instance(ObjectID(entry.key)));
		if (node != nullptr) {
			node->set_transform(entry.value);
			node->set_present_visible(true); // undo any visibility intent the placed walk wrote
		}
	}
	orig_transforms_.clear();
}

void MissionRoot::for_each_present_node(const std::function<void(ObjectModel *)> &p_fn) {
	if (sim_.is_null() || index_.is_null()) {
		return;
	}
	const int stride = sim_->get_present_stride();
	if (stride <= 0) {
		return;
	}
	const PackedFloat32Array snap = sim_->get_present_snapshot();
	const int64_t count = snap.size() / stride;
	for (int64_t i = 0; i < count; ++i) {
		const int64_t base = i * stride;
		ObjectModel *node = index_->resolve(
				static_cast<int64_t>(snap[base + Simulation::PF_BMS_ID]),
				static_cast<int64_t>(snap[base + Simulation::PF_KIND]),
				static_cast<int64_t>(snap[base + Simulation::PF_INDEX]));
		if (node != nullptr) {
			p_fn(node);
		}
	}
}

// The explicit detach order (verbatim from the GDScript driver): the stats
// board hook, the profiling gate, the session close, the pose latch, the
// Stop -> Play connect, the presenter's teardown, then the captured pass
// inputs and the sim itself. In the game the placer's MissionObjects
// container is this root's child and unload() queue_free()s it FIRST, so the
// teardown below finds the container's nodes already gone exactly as it did
// when the container was the world's own child.
Ref<Simulation> MissionRoot::release_simulation() {
	Ref<Simulation> sim = sim_;
	if (sim.is_valid()) sim->set_runtime_profiling_enabled(false);
	sim_.unref();
	has_trace_stats_sampling_ = false;
	return sim;
}

void MissionRoot::_exit_tree() {
	if (frame_stats_.is_valid()) {
		const Callable capture_changed(this, kOnCaptureChanged);
		if (frame_stats_->is_connected(kSignalCaptureChanged, capture_changed)) {
			frame_stats_->disconnect(kSignalCaptureChanged, capture_changed);
		}
	}
	runtime_probe_enabled_ = false;
	has_trace_stats_sampling_ = false;
	if (sim_.is_valid()) {
		sim_->set_runtime_profiling_enabled(false);
		sim_->close_session();
	}
	clear_present_effect_poses();
	if (EntityPresenter *presenter = entities()) {
		const Callable reset_wire(presenter, kResetWireRuntimeState);
		if (is_connected(kSignalSimulationRestarted, reset_wire)) {
			disconnect(kSignalSimulationRestarted, reset_wire);
		}
		// Frees the wire bodies + held weapons and the tracer mesh under the
		// container, the husk models + effect anchors, the throwable models and
		// the scar meshes under the owner models.
		presenter->teardown();
		entities_id_ = ObjectID(); // the "Entities" child itself dies with this node
	}
	container_id_ = ObjectID();
	item_db_.unref();
	resource_root_.unref();
	sim_.unref();
	mission_.unref();
}

} // namespace godot
