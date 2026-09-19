#include "world/session_drive.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "mission/mission_data.h"
#include "mission/mission_root.h"
#include "network/novaworld_host.h"
#include "object/avatar_database.h"
#include "resource_index/launch_flags.h"
#include "world/game_world.h"
#include "world/loading_screen_info.h"

using namespace godot;

namespace {

constexpr const char *kSignalLoadFailed = "load_failed";
constexpr const char *kSignalJoinSessionIdentified = "join_session_identified";
constexpr const char *kSignalJoinAdmissionReady = "join_admission_ready";
constexpr const char *kSignalJoinDeployPickRequired = "join_deploy_pick_required";
constexpr const char *kSignalSessionLost = "session_lost";
// HostSessionOptions.channel for the NovaWorld browser/lobby: the LAN channel
// never reads or manufactures NovaWorld service configuration; the NovaWorld
// channel supplies the gate.
constexpr const char *kChannelNovaWorld = "NovaWorld";

int64_t now_ms() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
}

} // namespace

SessionDrive::SessionDrive(GameWorld *p_world) :
		world_(p_world) {
	policy_.instantiate();
}

int SessionDrive::load_as_host(const Ref<HostSessionOptions> &p_options) {
	if (p_options.is_null()) {
		world_->emit_signal(kSignalLoadFailed, "host start: no host configuration");
		return ERR_INVALID_PARAMETER;
	}
	pending_host_ = p_options;
	// The host screen's rotation resolves its first pick into mission_file
	// before the request crosses (HostSessionConfig.to_session_options).
	const String bms = p_options->get_mission_file();
	if (bms.is_empty()) {
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed, "host start: no mission selected");
		return ERR_INVALID_PARAMETER;
	}
	const int err = world_->load_mission(bms, p_options->get_dir());
	if (err != OK) {
		clear_pending_session();
	}
	return err;
}

void SessionDrive::clear_pending_session() {
	pending_host_.unref();
	pending_join_.unref();
}

// The joiner's profile-to-wire projection: retail does not invent a
// network-only player id, it packs the two profile character selections from
// Avatars.def plus the two class and avatar bytes. The projection, its
// per-side defaults and its witnesses are the engine's
// (runtime/inmatch/join_character_profile.h via AvatarDatabase); `loadout`
// is the typed spawn record the world stages (null = no side selected, the
// per-side defaults).
Ref<CharacterJoinProfile> SessionDrive::build_join_character_profile(
		const Ref<ResourceRoot> &p_resource_root, const Ref<PlayerSpawnLoadout> &p_loadout) {
	if (p_resource_root.is_null()) {
		return Ref<CharacterJoinProfile>();
	}
	Ref<AvatarDatabase> db;
	db.instantiate();
	if (db->load_from_resource_root(p_resource_root, "Avatars.def") != OK || !db->is_loaded()) {
		UtilityFunctions::push_warning(vformat(
				"SessionDrive: Avatars.def not loaded for LAN join profile (%s)", db->get_last_error()));
		return Ref<CharacterJoinProfile>();
	}
	return db->character_join_profile_from_loadout(p_loadout);
}

int SessionDrive::load_as_joiner(const Ref<JoinTarget> &p_target) {
	if (p_target.is_null()) {
		world_->emit_signal(kSignalLoadFailed, "join: no join target");
		return ERR_INVALID_PARAMETER;
	}
	cancel_join_preload();
	policy_->reset_for_join();
	pending_join_ = p_target;
	Ref<ResourceRoot> resource_root = world_->resolve_root(p_target->get_dir());
	if (resource_root.is_null()) {
		clear_pending_session();
		return ERR_CANT_OPEN;
	}
	join_preload_sim_.instantiate();
	// `--capture-pcap <path>` (LaunchFlags) records this process's datagrams.
	join_preload_sim_->set_capture_pcap_path(LaunchFlags::capture_pcap());
	// Retail builds g_CharAttr from the boot-soft charattr.def before any
	// network receive can deliver the 0x41 property clears or 0x39 challenge.
	// A missing file deliberately leaves the inactive all-zero table.
	join_preload_sim_->load_charattr_challenge(resource_root);
	Ref<CharacterJoinProfile> join_profile = build_join_character_profile(
			resource_root, world_->player_visuals_->spawn_loadout());
	if (join_profile.is_valid()) {
		auto profile = join_profile->value();
		profile.team_request = p_target->get_team_request();
		join_profile->assign(profile);
	}
	join_preload_sim_->set_join_character_profile(join_profile);
	if (!p_target->get_integrity_profile().is_empty() &&
			!join_preload_sim_->set_join_integrity_profile(p_target->get_integrity_profile())) {
		join_preload_sim_.unref();
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed, vformat(
				"join: unknown integrity profile '%s'", p_target->get_integrity_profile()));
		return ERR_INVALID_PARAMETER;
	}
	// The APPID join token (decoded .joi CK) a NovaWorld host validates in
	// ClientAuth (reject code 9), and the CD identity cookie (packed PUB*
	// blob) it validates in the 0x00 JOIN (codes 23/24/25). Both empty/"0"
	// for LAN.
	join_preload_sim_->set_app_id(p_target->get_app_id());
	join_preload_sim_->set_join_cd_cookie(p_target->get_cd_cookie());
	if (!join_preload_sim_->enable_join(p_target->get_host_ip(), p_target->get_port(),
				p_target->get_player_name(), p_target->get_join_role(),
				p_target->get_spectator_password(), p_target->get_server_password(),
				p_target->get_join_password())) {
		join_preload_sim_.unref();
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed, "join: could not open the LAN session socket");
		return ERR_CANT_CONNECT;
	}
	join_preload_sim_->set_join_world_ready(false);
	// The JOIN VERSIONCRCSTRING checksum reads the loose
	// expansion/<name>/version.txt under this install root (D-NET-166).
	join_preload_sim_->set_join_expansion_version_root(resource_root->get_root_dir());
	join_preload_root_ = resource_root;
	policy_->arm_preload(now_ms());
	world_->set_process(true);
	return OK;
}

void SessionDrive::step_preload() {
	if (join_preload_sim_.is_null()) {
		world_->set_process(false);
		return;
	}
	if (!join_preload_sim_->is_join_preload_ready()) {
		join_preload_sim_->poll_join_preload();
		const int step = policy_->preload_step(join_preload_sim_->get_join_error(), now_ms());
		if (step == NetSessionPolicy::STEP_FAIL) {
			fail_join_preload(policy_->fail_reason());
		}
		return;
	}
	// Stop the frame pump before the synchronous load transfers or releases
	// the preload simulation. Every failure below owns its own
	// cleanup/reporting leg.
	world_->set_process(false);
	policy_->disarm_preload();
	// Reconcile the mount with the host's data set BEFORE any referenced
	// assets are resolved through it. The mission body itself comes from the
	// host's world stream; the mount supplies shared terrain/environment/model
	// definitions (D-NET-178/194).
	if (!reconcile_join_expansion()) {
		return;
	}

	if (!policy_->validate_promote_mission_file(join_preload_sim_->get_join_mission_file())) {
		fail_join_preload(policy_->fail_reason());
		return;
	}
	const String bms = policy_->promoted_mission_file();
	Ref<ResourceRoot> resource_root = join_preload_root_;
	if (resource_root.is_null()) {
		fail_join_preload("join: resource root is unavailable after session identification");
		return;
	}
	// Retail does not open the advertised .bms on a joining client. The
	// server already copied its exact 0x268-byte header into S2C 0x0B, then
	// streamed the live world and optional mission .til overlay. Requiring the
	// map locally made retail-host -> OpenNova-client fail for custom maps
	// even though the reverse direction worked (D-NET-194).
	const PackedByteArray wire_header = join_preload_sim_->get_join_mission_header();
	if (!policy_->validate_promote_header(wire_header.size())) {
		fail_join_preload(policy_->fail_reason());
		return;
	}
	Ref<MissionData> mission;
	mission.instantiate();
	if (mission->open_wire_header(wire_header) != OK) {
		fail_join_preload(vformat("join: failed to parse host S2C 0x0B mission header for %s: %s",
				bms, mission->get_last_error()));
		return;
	}

	// The authoritative session identity. None of it came from discovery;
	// every value here came from 0x7B.
	join_preload_root_.unref();
	world_->emit_signal(kSignalJoinSessionIdentified,
			LoadingScreenInfo::make(bms, true, join_preload_sim_->get_join_server_name(),
					join_preload_sim_->get_join_mission_name(),
					static_cast<int>(join_preload_sim_->get_join_game_type()), String()));
	const int err = world_->load_mission_internal(mission, bms, resource_root);
	if (err != OK) {
		// load_mission_internal emitted the specific resource/load failure.
		cancel_join_preload();
		clear_pending_session();
		return;
	}
	// Post-load joiner watchdog: the admission tail (C2S 0x0A -> world stream
	// -> loadout grants) is server-driven with no protocol-level timeout, so a
	// stalled or incompatible host would leave the player loaded but hidden
	// forever. The policy reuses the retail ConnectOrHost window from
	// world-ready and composes a stage-named failure -- the reachable analog of
	// retail's post-load network-wait failure returns [orig:
	// NapiClient_WaitForGameStart @ 0x42cc10 failure legs -> "Mission loading
	// aborted"]. It covers only SERVER-owed transitions: at the player-paced
	// deployment pick the watchdog ends (retail's DEATH screen simply waits;
	// net-re 5.61).
	policy_->arm_admission_watch(now_ms());
}

// Point the joiner's resource root at the HOST's expansion (S2C 0x7B field 7,
// net-re 5.32) before any of the host's data is resolved through it. The ADM
// weapon index space is expansion-scoped, so a joiner mounted on a different
// expansion than the host misreads every wire ADM index from the first
// diverging weapon.def row on -- in BOTH directions, and in both its own C2S
// 0x2F kit and the host's S2C 0x5A grant / round events (D-NET-178). Retail
// switches THE ONE global mount in place on this same leg -- it copies the
// session record's expansion over the pending name, switches, and only then
// connects [orig: UI_JoinSelectedSession @ 0x5699d0 (expansion copy
// @ 0x569afa, switch @ 0x569b02, connect @ 0x569ded) -> Expansion_SwitchTo
// @ 0x5688c0 -> PFF_CloseAllOpenArchives @ 0x4a4380 / PFF_OpenAllArchives
// @ 0x4a4310]. There is no second mount object, and the switch is sticky: the
// shell keeps running on the host's expansion after the session. The
// keep/remount/fail DECISION is native (inmatch::decide_join_expansion); this
// executes it against the live mount.
bool SessionDrive::reconcile_join_expansion() {
	Ref<ResourceRoot> resource_root = join_preload_root_;
	if (resource_root.is_null()) {
		return true;
	}
	const int action = policy_->decide_expansion(join_preload_sim_->get_join_expansion(),
			resource_root->get_expansion(),
			resource_root->list_expansions(resource_root->get_root_dir()));
	String decision_name = "keep";
	switch (action) {
		case NetSessionPolicy::ACTION_REMOUNT:
			decision_name = "remount";
			break;
		case NetSessionPolicy::ACTION_FAIL:
			decision_name = "fail";
			break;
		default:
			break;
	}
	UtilityFunctions::print_verbose(vformat(
			"SessionDrive: join expansion: host='%s' mounted='%s' decision=%s",
			join_preload_sim_->get_join_expansion(), resource_root->get_expansion(), decision_name));
	if (action == NetSessionPolicy::ACTION_KEEP) {
		return true;
	}
	// Only a runtime mount layers expansion archives at all. A loose authoring
	// root (an explicit --loose-root run mounts the loose game-data tree;
	// tests hand fixtures over) has no expansion to switch AND reports an
	// empty installed set by construction, so it can neither honour the
	// host's expansion nor prove it missing -- every decision below is
	// meaningless there. Report the mismatch and let the authored data stand.
	// This precedes the abort: policing an install we do not own would fail
	// every editor/fixture join against an expansion host. The shipping game
	// always arrives here on a runtime mount (main_game hands GameWorld its
	// live menu mount), so D-NET-178's protection is unaffected.
	if (!resource_root->is_runtime_mount()) {
		UtilityFunctions::push_warning(vformat(
				"SessionDrive: host expansion '%s' differs from the loose root's '%s'; the authoring mount stands",
				join_preload_sim_->get_join_expansion(), resource_root->get_expansion()));
		return true;
	}
	// A runtime mount that cannot supply the host's expansion aborts the join.
	// Retail's switch is a no-op when expansion\<name>\<name>.pff is missing
	// and it connects on its own data set anyway [orig: Expansion_SwitchTo
	// @ 0x5688c0, missing-.pff gate @ 0x568914] -- that is precisely the ADM
	// index-space corruption D-NET-178 records, so we refuse the join instead
	// (tracked divergence).
	if (action == NetSessionPolicy::ACTION_FAIL) {
		fail_join_preload(policy_->decision_error());
		return false;
	}
	const String target_expansion = policy_->decided_expansion();
	const String dir = resource_root->get_root_dir();
	const String previous = resource_root->get_expansion();
	// Switch THIS root rather than swapping in a second one, the same
	// in-place remount MenuShell._apply_expansion does for the Mods screen:
	// every holder (the menu shell, the loading screen) is meant to move with
	// it, and mount_runtime rebuilds the index and bumps the cache epoch, so
	// their caches self-clear. The persisted expansion setting is NOT written
	// -- the host owns this session's data set, not the local menu choice.
	// Same layering as GameWorld::mount_runtime_root (see it for the flag
	// rules); only the expansion differs.
	const String game_code = LaunchFlags::game(world_->resolver_game());
	if (resource_root->mount_runtime(dir, target_expansion, LaunchFlags::loose_override_enabled(),
				game_code) != OK) {
		// A hard mount failure clears the root, and the shell shares this
		// object, so put the previous expansion back before aborting to the
		// menu (MenuShell._apply_expansion rolls back the same way). The
		// failure surfaces through the preload's abort leg rather than a bare
		// load_failed, so the live session is torn down too.
		const String mount_error = resource_root->get_last_error();
		resource_root->mount_runtime(dir, previous, LaunchFlags::loose_override_enabled(), game_code);
		fail_join_preload(vformat("join: could not mount host expansion '%s' from %s: %s",
				target_expansion, dir, mount_error));
		return false;
	}
	// mount_runtime succeeds even when the expansion never layered
	// (opennova::Vfs::mount_game falls back to base game silently), so read
	// back what ACTUALLY mounted. Without this the abort leg above would be
	// bypassed by a root that is quietly base game again. No rollback here:
	// unlike the hard failure above, the root holds a valid mount of whatever
	// DID layer, so the shell survives the abort on it.
	if (resource_root->get_expansion().to_lower() != target_expansion.to_lower()) {
		// The installed set is rendered by the same native helper the
		// decision leg uses ("none -- base game only" when empty), so both
		// abort reasons read identically (one impl -- engine/runtime/inmatch
		// join_session_policy).
		fail_join_preload(vformat("join: host runs expansion '%s' but %s mounted '%s' (installed: %s)",
				target_expansion, dir, resource_root->get_expansion(),
				NetSessionPolicy::describe_installed(resource_root->list_expansions(dir))));
		return false;
	}
	return true;
}

// The per-frame admission/deploy/loss observer: read the joiner state,
// forward it to the native edge machine, execute what the returned flags
// say. The ordering, the latches, the windows, and every reason text live in
// the policy (loss wins over every admission edge; the deploy latch re-arms
// when pending clears; admission-ready is once per join and holds for the
// cold wire drain behind the loading hold [orig: the reap @ 0x4ca4a0 ->
// @ 0x4c63d0]).
void SessionDrive::update_joiner_admission_signals() {
	MissionRoot *runtime = world_->get_runtime();
	if (runtime == nullptr) {
		policy_->disarm_admission_watch();
		return;
	}
	Ref<Simulation> sim = runtime->get_sim();
	if (sim.is_null() || !sim->is_joiner()) {
		policy_->disarm_admission_watch();
		return;
	}
	const String loss_reason = sim->get_session_loss_reason();
	const bool deploy_pending = sim->is_join_deploy_pick_pending();
	// The first valid S2C 0x5A opens retail's independent gameplay gate and
	// can make is_joined_in_match true BEFORE 0x0F supplies the deployment
	// policy or the second initial grant makes a required DEATH pick ready.
	// Only the native initial-admission boundary distinguishes that split
	// ordering from a complete no-pick join; later redeploys leave the
	// predicate monotonically true.
	const bool initial_admission_complete = sim->is_join_initial_admission_complete();
	// The join error + admission stage only matter to the armed watchdog --
	// read them exactly when the old inline machine did.
	String join_error;
	String admission_stage;
	if (policy_->is_admission_watch_active()) {
		join_error = sim->get_join_error();
		admission_stage = sim->get_join_admission_stage();
	}
	const int pre = policy_->begin_admission_frame(loss_reason, deploy_pending,
			initial_admission_complete, join_error, admission_stage, now_ms());
	if (pre & NetSessionPolicy::EMIT_SESSION_LOST) {
		// MainGame handles this signal synchronously and unloads the world.
		// All drive/policy mutation is already finished, so no owner method
		// runs after teardown.
		world_->emit_signal(kSignalSessionLost, policy_->session_loss_reason());
	}
	if (pre & NetSessionPolicy::FRAME_DONE) {
		if (pre & NetSessionPolicy::LOAD_FAILED) {
			world_->emit_signal(kSignalLoadFailed, policy_->fail_reason());
		}
		return;
	}
	bool settle_ok = true;
	if (pre & NetSessionPolicy::SETTLE_REQUIRED) {
		settle_ok = world_->settle_join_wire_assets();
	}
	// The revealed world must not race the budgeted cold wire materialization
	// (EntityPresenter.DEFAULT_COLD_SPAWN_BUDGET): the policy holds the
	// no-pick reveal -- with its watchdog deadline still armed -- until the
	// presenter's deferred-spawn queue drains behind the loading hold.
	const int post = policy_->finish_admission_frame(settle_ok, world_->is_join_wire_present_drained());
	if (post & NetSessionPolicy::SETTLE_FAILED) {
		world_->report_join_wire_asset_failure(policy_->fail_reason());
		return;
	}
	if (post & NetSessionPolicy::LOAD_FAILED) {
		world_->emit_signal(kSignalLoadFailed, policy_->fail_reason());
		return;
	}
	if (post & NetSessionPolicy::EMIT_DEPLOY_PICK) {
		world_->emit_signal(kSignalJoinDeployPickRequired);
	}
	if (post & NetSessionPolicy::EMIT_ADMISSION_READY) {
		world_->emit_signal(kSignalJoinAdmissionReady);
	}
}

bool SessionDrive::cancel_preload() {
	if (join_preload_sim_.is_null()) {
		return false;
	}
	fail_join_preload("Mission loading aborted");
	return true;
}

bool SessionDrive::cancel_admission_wait() {
	return policy_->request_admission_abort();
}

void SessionDrive::fail_join_preload(const String &p_reason) {
	cancel_join_preload();
	clear_pending_session();
	world_->emit_signal(kSignalLoadFailed, p_reason);
}

void SessionDrive::cancel_join_preload() {
	world_->set_process(false);
	policy_->disarm_preload();
	join_preload_sim_.unref();
	join_preload_root_.unref();
}

bool SessionDrive::pending_dedicated() const {
	return pending_host_.is_valid() && !pending_host_->get_serve_and_play();
}

void SessionDrive::stage_runtime_options(const Ref<MissionSetupOptions> &p_opts) {
	// A LAN host start threads its typed session request (HostSessionOptions)
	// through to the listen server. A LAN JOINER threads its typed dial
	// target (JoinTarget) and is NOT a listen server (ADR 0017). Both are
	// consumed once per load; absent for a normal single-player start, which
	// keeps the in-process (socketless) listen server.
	if (pending_host_.is_valid()) {
		// The sim-shaped host request; the root resolves game_type_auto and
		// stamps the mission identity on it.
		p_opts->set_host_session(pending_host_);
		p_opts->set_net_transport("lan");
		p_opts->set_bind_port(pending_host_->get_bind_port());
		p_opts->set_server_name(pending_host_->get_server_name());
		p_opts->set_player_name(pending_host_->get_player_name());
		p_opts->set_max_players(pending_host_->get_max_players());
		p_opts->set_channel(pending_host_->get_channel());
		if (pending_host_->get_channel() == kChannelNovaWorld) {
			p_opts->set_nw_gate_host(pending_host_->get_nw_gate_host());
			p_opts->set_nw_gate_port(pending_host_->get_nw_gate_port());
			p_opts->set_region(pending_host_->get_region());
			if (!pending_host_->get_advertise().is_empty()) {
				p_opts->set_advertise(pending_host_->get_advertise());
			}
		}
	} else if (pending_join_.is_valid()) {
		p_opts->set_join_target(pending_join_);
		p_opts->set_net_transport("lan-join");
		p_opts->set_join_character_profile(build_join_character_profile(
				p_opts->get_resource_root(), world_->player_visuals_->spawn_loadout()));
	}
	pending_host_.unref();
	pending_join_.unref();
	// Consume the already-authenticated joiner. MissionRoot adopts this
	// simulation like its usual freshly-created one; clearing our reference
	// before setup makes ownership singular even on a setup failure.
	if (join_preload_sim_.is_valid()) {
		p_opts->set_simulation(join_preload_sim_);
		join_preload_sim_.unref();
	}
}

void SessionDrive::on_runtime_started(const Ref<MissionSetupOptions> &p_opts, const String &p_bms_name) {
	maybe_start_nw_host(p_opts, p_bms_name);
}

void SessionDrive::observe_tick(MissionRoot *p_runtime) {
	update_joiner_admission_signals();
	// Keep the gate's advertised occupancy current (host + admitted joiners).
	// set_player_count self-dedupes, so this is a no-op until the count
	// changes.
	NovaWorldHost *host = nw_host();
	if (host != nullptr && p_runtime != nullptr) {
		Ref<Simulation> sim = p_runtime->get_sim();
		if (sim.is_valid()) {
			host->set_player_count(1 + sim->get_host_peer_count());
		}
	}
}

void SessionDrive::reset() {
	cancel_join_preload();
	policy_->reset();
	clear_pending_session();
	// Gate registration teardown: tells the gate to drop the host row
	// (ClientStopHosting).
	NovaWorldHost *host = nw_host();
	if (host != nullptr) {
		host->stop();
		host->queue_free();
	}
	nw_host_id_ = ObjectID();
}

NovaWorldHost *SessionDrive::nw_host() const {
	return Object::cast_to<NovaWorldHost>(ObjectDB::get_instance(nw_host_id_));
}

// Register a browsable listen host with the NovaWorld gate (F1, ADR 0010).
// The host-direction sibling of the joiner's NovaWorldClient: it runs the NWU
// lobby handshake to the gate, then ClientHostRequest + ClientHostUpdate
// heartbeats so the host shows in /api/hosts + the retail server browser.
// Gated so it only fires for a real LAN listen server WITH a gate configured
// -- single-player, joiners, and pure-LAN play (no nw_gate_host) all skip it,
// unchanged.
void SessionDrive::maybe_start_nw_host(const Ref<MissionSetupOptions> &p_opts, const String &p_bms_name) {
	if (!GATE_REGISTRATION_ARMED) {
		return;
	}
	if (p_opts->get_net_transport() != "lan") {
		return;
	}
	// Register only when the explicit NovaWorld host flow supplied a gate. The
	// in-match wire is shared, but the LAN menu path never reads or
	// manufactures service configuration.
	const String gate_host = p_opts->get_nw_gate_host();
	if (gate_host.is_empty()) {
		if (p_opts->get_channel() == kChannelNovaWorld) {
			UtilityFunctions::push_warning(
					"SessionDrive: NovaWorld host requested but no gate address (nw_gate_host) -- gate registration skipped; host is LAN-reachable only");
		}
		return; // no gate configured -> pure LAN, nothing to register with
	}
	MissionRoot *runtime = world_->get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (sim.is_null() || !sim->is_host_listening()) {
		return; // the listen socket never came up; nothing reachable to advertise
	}
	NovaWorldHost *host = memnew(NovaWorldHost);
	world_->add_child(host);
	nw_host_id_ = ObjectID(host->get_instance_id());
	host->set_host(gate_host);
	host->set_gate_port(p_opts->get_nw_gate_port());
	host->set_server_name(p_opts->get_server_name());
	host->set_mission_name(p_bms_name.get_basename());
	host->set_max_players(p_opts->get_max_players());
	// The actually-bound game port the joiner will dial (not the requested
	// bind_port).
	host->set_game_port(sim->get_host_listen_port());
	host->set_region(p_opts->get_region());
	host->set_player_name(p_opts->get_player_name());
	if (!p_opts->get_advertise().is_empty()) {
		host->set_advertise_ip(p_opts->get_advertise());
	}
	host->connect("registered", callable_mp(world_, &GameWorld::on_nw_host_registered));
	host->connect("error_occurred", callable_mp(world_, &GameWorld::on_nw_host_error));
	host->start();
}

void SessionDrive::on_nw_host_registered() {
	UtilityFunctions::print_verbose(
			"SessionDrive: listen host registered with the NovaWorld gate (browsable)");
}

void SessionDrive::on_nw_host_error(const String &p_message) {
	UtilityFunctions::push_warning(vformat("SessionDrive: NovaWorld host registration error: %s", p_message));
}
