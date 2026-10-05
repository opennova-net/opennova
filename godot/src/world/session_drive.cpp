#include "world/session_drive.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "mission/mission_data.h"
#include "mission/mission_root.h"
#include "network/novaworld_client.h"
#include <runtime/inmatch/mission_exit.h> // the post-mission router
#include "object/avatar_database.h"
#include "resource_index/launch_flags.h"
#include "util/string_convert.h"
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
// never reads or manufactures NovaWorld service configuration; a NovaWorld host
// is hosted by the NovaWorld session the shell hands over.
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
	last_join_target_ = p_target;
	reloading_join_ = false;
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
	join_preload_sim_->set_join_network_type(p_target->get_network_type());
	join_preload_sim_->set_join_cd_cookie(p_target->get_cd_cookie());
	// A join from a browse row connects with the row's session record: the 0x42
	// first, with the row's HK (JoinerConnection::DiscoveredSession).
	join_preload_sim_->set_join_discovered_session(p_target->get_discovered_session(),
			p_target->get_session_host_key(), p_target->get_session_password_required(),
			p_target->get_expansion());
	// A nonzero .joi LN asks for the LAN-discovered endpoint of the named
	// session instead of the NK relay the target carries. That endpoint has no
	// producer on this seam: the NovaWorld panel resolves a join without running
	// a LAN enumeration, and JoinTarget carries no second (LAN) ip/port, so the
	// NK endpoint stands. The switch belongs here once a join-by-name LAN lookup
	// fills one.
	if (p_target->get_lobby_number() != 0) {
		UtilityFunctions::print_verbose(vformat(
				"SessionDrive: join LN=%d asks for a LAN-discovered endpoint; none is available, dialing %s:%d",
				p_target->get_lobby_number(), p_target->get_host_ip(), p_target->get_port()));
	}
	last_connection_error_.unref();
	if (!join_preload_sim_->enable_join(p_target->get_host_ip(), p_target->get_port(),
				p_target->get_player_name(), p_target->get_join_role(),
				p_target->get_spectator_password(), p_target->get_server_password(),
				p_target->get_join_password())) {
		join_preload_sim_.unref();
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed, "join: could not open the LAN session socket");
		return ERR_CANT_CONNECT;
	}
	// The proxy-assisted NovaWorld join (the .joi NI/NP/BK): the role sends its
	// rendezvous datagram ahead of the first hello, so this lands before the
	// first preload poll. Empty on a LAN target.
	if (p_target->has_join_proxy()) {
		join_preload_sim_->set_join_proxy(p_target->get_proxy_node(), p_target->get_proxy_relay(),
				p_target->get_proxy_cookie());
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
	// A reload's admission tail has no window either (net-re §5.70.7).
	if (!reloading_join_) policy_->arm_admission_watch(now_ms());
	reloading_join_ = false;
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
	const Ref<ResourceRoot> resource_root = join_preload_root_;
	if (resource_root.is_null()) {
		return true;
	}
	const String error = switch_join_expansion(resource_root, join_preload_sim_->get_join_expansion());
	if (!error.is_empty()) {
		fail_join_preload(error);
		return false;
	}
	return true;
}

// The switch itself, shared by the pre-dial leg (the browse row's expansion,
// GameWorld::mount_join_expansion) and the post-auth reconcile above. Returns
// the failure text, or an empty string when the root holds the host's
// expansion (or is a loose authoring root that cannot switch).
String SessionDrive::switch_join_expansion(const Ref<ResourceRoot> &p_root,
		const String &p_host_expansion) {
	Ref<ResourceRoot> resource_root = p_root;
	if (resource_root.is_null()) {
		return String();
	}
	const int action = policy_->decide_expansion(p_host_expansion,
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
			p_host_expansion, resource_root->get_expansion(), decision_name));
	if (action == NetSessionPolicy::ACTION_KEEP) {
		return String();
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
				p_host_expansion, resource_root->get_expansion()));
		return String();
	}
	// A runtime mount that cannot supply the host's expansion aborts the join.
	// Retail's switch is a no-op when expansion\<name>\<name>.pff is missing
	// and it connects on its own data set anyway [orig: Expansion_SwitchTo
	// @ 0x5688c0, missing-.pff gate @ 0x568914] -- that is precisely the ADM
	// index-space corruption D-NET-178 records, so we refuse the join instead
	// (tracked divergence).
	if (action == NetSessionPolicy::ACTION_FAIL) {
		return policy_->decision_error();
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
		return vformat("join: could not mount host expansion '%s' from %s: %s",
				target_expansion, dir, mount_error);
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
		return vformat("join: host runs expansion '%s' but %s mounted '%s' (installed: %s)",
				target_expansion, dir, resource_root->get_expansion(),
				NetSessionPolicy::describe_installed(resource_root->list_expansions(dir)));
	}
	return String();
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
			last_connection_error_ = sim->get_connection_error();
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

Ref<JoinScreenStatus> SessionDrive::join_screen_status() const {
	return join_preload_sim_.is_valid() ? join_preload_sim_->get_join_screen_status()
	                                    : Ref<JoinScreenStatus>();
}

void SessionDrive::fail_join_preload(const String &p_reason) {
	if (join_preload_sim_.is_valid()) {
		last_connection_error_ = join_preload_sim_->get_connection_error();
	}
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
	if (kept_sim_.is_valid() && kept_sim_->is_host_listening()) {
		const Ref<HostSessionOptions> live = kept_sim_->get_host_session_config();
		return live.is_valid() && !live->get_serve_and_play();
	}
	return pending_host_.is_valid() && !pending_host_->get_serve_and_play();
}

bool SessionDrive::keep_session(MissionRoot *p_runtime) {
	if (p_runtime == nullptr) return false;
	kept_sim_ = p_runtime->release_simulation();
	return kept_sim_.is_valid();
}

int SessionDrive::load_next_host_mission(const String &p_bms_name) {
	if (kept_sim_.is_null() || !kept_sim_->is_host_listening() || p_bms_name.is_empty()) {
		kept_sim_.unref();
		world_->emit_signal(kSignalLoadFailed, "map change: no live host session");
		return ERR_UNAVAILABLE;
	}
	// The kept simulation rides stage_runtime_options into the runtime, which
	// boots the next map inside its session (Simulation::begin_host_map_change
	// latched it).
	const int err = world_->load_mission(p_bms_name, String());
	kept_sim_.unref();
	return err;
}

int SessionDrive::reload_as_joiner() {
	if (kept_sim_.is_null() || !kept_sim_->is_joiner()) {
		kept_sim_.unref();
		world_->emit_signal(kSignalLoadFailed, "reload: no live joiner session");
		return ERR_UNAVAILABLE;
	}
	Ref<ResourceRoot> resource_root = world_->resolve_root(
			last_join_target_.is_valid() ? last_join_target_->get_dir() : String());
	if (resource_root.is_null()) {
		kept_sim_.unref();
		return ERR_CANT_OPEN;
	}
	// The kept connection is the preload's: the reload's legs ran from
	// Simulation::begin_joiner_reload, and step_preload loads the next map at
	// its 0x11 as a first join's does, with no window armed.
	policy_->reset_for_join();
	pending_join_ = last_join_target_;
	join_preload_sim_ = kept_sim_;
	kept_sim_.unref();
	join_preload_sim_->set_join_world_ready(false);
	join_preload_root_ = resource_root;
	last_connection_error_.unref();
	reloading_join_ = true;
	world_->set_process(true);
	return OK;
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
		// The NovaWorld menu's host keeps the NovaWorld network type through the
		// match: its session is already hosting (the service's host verify
		// lands before the mission starts) and rides in with the load.
		NovaWorldClient *client = nw_client();
		pending_host_->set_network_type(
				pending_host_->get_channel() == kChannelNovaWorld && client != nullptr &&
								client->is_hosting()
						? opennova::inmatch::NetworkType::NovaWorld
						: opennova::inmatch::NetworkType::Lan);
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
	// A host's map change: the kept session's simulation (its live role,
	// socket and connections) is the runtime's.
	if (kept_sim_.is_valid()) {
		p_opts->set_simulation(kept_sim_);
		p_opts->set_net_transport("lan");
		kept_sim_.unref();
	}
}

void SessionDrive::on_runtime_started(const Ref<MissionSetupOptions> &p_opts, const String &p_bms_name) {
	(void)p_bms_name;
	bind_nw_host(p_opts);
}

void SessionDrive::observe_tick(MissionRoot *p_runtime) {
	update_joiner_admission_signals();
	Ref<Simulation> sim = p_runtime != nullptr ? p_runtime->get_sim() : Ref<Simulation>();
	if (sim.is_null()) {
		return;
	}
	// A hosting NovaWorld session follows the hosted match: one PlayerList slot
	// per admitted player, the live round clock the TimeLeft column reads at
	// every refresh, the session's GSID as the in-match host's 0x81 SUS1
	// (cleared when its connection tears down, re-supplied by a re-host), and
	// the cookie-key ring as it advances.
	NovaWorldClient *client = nw_client();
	if (client != nullptr && nw_host_bound_ && !sim->is_joiner()) {
		opennova::NwuHostRole &host = client->host_role();
		sync_nw_host_roster(host, sim);
		host.set_round_time_remaining_ticks(sim->round_time_remaining_ticks());
		sim->set_novaworld_registration(opennova::cp1252_to_gd(host.gsid()), host.app_id(), host.cookie_keys(),
				opennova::to_gd(client->get_login_pcid()));
	}
	sync_nwu_session(sim);
}

void SessionDrive::sync_nwu_session(const Ref<Simulation> &p_sim) {
	opennova::NwuLobbySession::MatchFacts facts;
	bool node = false;
	if (NovaWorldClient *client = nw_client()) {
		facts = client->nwu_match_facts();
		node = true;
	}
	if (!nwu_feed_live_) {
		if (!node || (facts.role != opennova::ClientSession::kSessionRoleHosting &&
						facts.role != opennova::ClientSession::kSessionRolePlaying)) {
			return;
		}
		nwu_feed_live_ = true;
	}
	if (!node) {
		facts = opennova::NwuLobbySession::MatchFacts{};
		facts.in_use = true;
	}
	p_sim->set_nwu_session(facts.in_use, facts.flags, facts.role, facts.exit_reason);
}

// A retail host SetOrCreates the five per-slot PlayerList vars when a player is
// added and sends ClientHostPlayerAdded / Removed on each roster change. The
// PlayerPCID is the joiner's decrypted PUBPCID (the in-match host's account
// fields) [orig: CNapiGameSession_SendPlayerAdded @0x4d006c]; PlayerType (the
// player type id) has no in-match source on this seam and rides as "0".
// The ClientPlayerEnterRequest is NOT sent from here: the in-match host's
// join-phase watchdog announces a validating joiner itself (the hook bound in
// bind_nw_host), before the player is ever added to this roster.
void SessionDrive::sync_nw_host_roster(opennova::NwuHostRole &p_host, const Ref<Simulation> &p_sim) {
	std::map<int, std::string> live;
	for (const Simulation::HostPeerSlot &slot : p_sim->host_peer_slots()) {
		const std::string signature = opennova::to_std(slot.player_name) + "|" +
				opennova::to_std(slot.ip_and_port) + "|" + opennova::to_std(slot.pcid) + "|" +
				opennova::to_std(slot.team);
		live[slot.slot] = signature;
		auto sent = nw_roster_sent_.find(slot.slot);
		if (sent != nw_roster_sent_.end() && sent->second == signature) {
			continue;
		}
		opennova::HostPlayerSlot player;
		player.slot = slot.slot;
		player.player_name = opennova::to_std(slot.player_name);
		player.ip_and_port = opennova::to_std(slot.ip_and_port);
		player.pcid = opennova::to_std(slot.pcid);
		player.team = opennova::to_std(slot.team);
		player.type = "0";
		p_host.set_player_slot(player);
	}
	for (const auto &sent : nw_roster_sent_) {
		if (live.count(sent.first) == 0) {
			p_host.clear_player_slot(sent.first);
		}
	}
	nw_roster_sent_ = std::move(live);
}

void SessionDrive::reset() {
	cancel_join_preload();
	policy_->reset();
	clear_pending_session();
	reloading_join_ = false;
	// A kept session's NovaWorld session rides through the reload with it, as
	// retail keeps the NWU session playing or hosting across a map change.
	if (kept_sim_.is_valid()) return;
	stop_nw_client();
	nwu_feed_live_ = false;
}

void SessionDrive::adopt_nw_client(NovaWorldClient *p_client) {
	if (p_client == nullptr) {
		return;
	}
	if (nw_client() != p_client) {
		stop_nw_client();
	}
	Node *parent = p_client->get_parent();
	if (parent != world_) {
		if (parent != nullptr) {
			parent->remove_child(p_client);
		}
		world_->add_child(p_client);
	}
	nw_client_id_ = ObjectID(p_client->get_instance_id());
	// A hosting session's service traffic reaches the in-match host.
	const Callable command = callable_mp(world_, &GameWorld::on_nw_host_server_command);
	if (!p_client->is_connected("server_command", command)) {
		p_client->connect("server_command", command);
	}
	const Callable enter = callable_mp(world_, &GameWorld::on_nw_host_player_enter_result);
	if (!p_client->is_connected("player_enter_result", enter)) {
		p_client->connect("player_enter_result", enter);
	}
}

// A normal exit back to the NovaWorld menu keeps the session: the world lets go
// of the node (the hosted match's hooks unbound) and the shell re-enters the
// menu with it, whose re-entry stops the hosting and the play (the post-mission
// router's keep, engine: inmatch/mission_exit.h).
NovaWorldClient *SessionDrive::release_nw_client() {
	NovaWorldClient *client = nw_client();
	unbind_nw_host();
	nw_client_id_ = ObjectID();
	nwu_feed_live_ = false;
	if (client == nullptr) {
		return nullptr;
	}
	const Callable command = callable_mp(world_, &GameWorld::on_nw_host_server_command);
	if (client->is_connected("server_command", command)) {
		client->disconnect("server_command", command);
	}
	const Callable enter = callable_mp(world_, &GameWorld::on_nw_host_player_enter_result);
	if (client->is_connected("player_enter_result", enter)) {
		client->disconnect("player_enter_result", enter);
	}
	if (client->get_parent() == world_) {
		world_->remove_child(client);
	}
	return client;
}

Ref<PostMissionRoute> SessionDrive::post_mission_route(int p_reason) const {
	// The NovaWorld network type rides an adopted NovaWorld session: a NovaWorld
	// join or a NovaWorld host.
	const opennova::inmatch::PostMissionRoute route =
			opennova::inmatch::route_mission_exit(p_reason, nw_client() != nullptr);
	Ref<Simulation> sim = world_->get_sim();
	Ref<PostMissionRoute> out;
	out.instantiate();
	out->set_keep_session(route.keep_session);
	out->set_error(route.error != opennova::inmatch::PostMissionError::None);
	out->set_error_key(String(opennova::inmatch::post_mission_error_key(route.error)));
	if (route.error == opennova::inmatch::PostMissionError::DisconnectReason && sim.is_valid()) {
		out->set_error_text(sim->get_session_loss_reason());
		out->set_connection_error(sim->get_connection_error());
	}
	return out;
}

NovaWorldClient *SessionDrive::nw_client() const {
	return Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(nw_client_id_));
}

// A session the shell did not take back goes with the match: stop() leaves the
// play or the hosting (ClientStopPlaying / ClientStopHosting while still in
// them) and says goodbye.
void SessionDrive::stop_nw_client() {
	NovaWorldClient *client = nw_client();
	unbind_nw_host();
	if (client != nullptr) {
		client->stop();
		client->queue_free();
	}
	nw_client_id_ = ObjectID();
}

// The hosting session meets its match once the runtime is live: the GSID and
// AppId the in-match host advertises, the join-ticket arm, and the host's own
// player in the roster (Server_PlayerAdd adds the local player like any other,
// its endpoint the bound game port).
void SessionDrive::bind_nw_host(const Ref<MissionSetupOptions> &p_opts) {
	NovaWorldClient *client = nw_client();
	MissionRoot *runtime = world_->get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (client == nullptr || !client->is_hosting() || sim.is_null() || !sim->is_host_listening()) {
		return;
	}
	opennova::NwuHostRole &host = client->host_role();
	nw_host_bound_ = true;
	const std::string login_pcid = client->get_login_pcid();
	sim->set_novaworld_registration(opennova::cp1252_to_gd(host.gsid()), host.app_id(), host.cookie_keys(),
			opennova::to_gd(login_pcid));
	// A service that asked for join tickets arms the in-match host's join-phase
	// watchdog: it announces each validating joiner through this hook and holds
	// the player until on_nw_host_player_enter_result answers. The hook resolves
	// the session node by identity on every call, so it stays safe after the
	// node is gone; unbind_nw_host() clears it on teardown.
	if (host.requires_join_ticket()) {
		const ObjectID client_id = nw_client_id_;
		sim->set_novaworld_join_tickets(true,
				[client_id](uint32_t p_connection_id, uint32_t p_ip_packed, uint16_t p_port,
						const String &p_join_ticket) {
					NovaWorldClient *live = Object::cast_to<NovaWorldClient>(
							ObjectDB::get_instance(client_id));
					if (live != nullptr) {
						live->host_role().request_player_enter(p_connection_id, p_ip_packed,
								p_port, opennova::to_std(p_join_ticket));
					}
				});
	}
	if (p_opts.is_valid() && p_opts->get_host_session().is_valid() &&
			p_opts->get_host_session()->get_serve_and_play()) {
		opennova::HostPlayerSlot self;
		self.slot = 0;
		self.player_name = opennova::to_std(p_opts->get_player_name());
		self.ip_and_port = ":" + std::to_string(sim->get_host_listen_port());
		self.pcid = login_pcid; // the host's own player's PCID is its login cookie
		self.team = "0";
		self.type = "0";
		host.set_player_slot(self);
	}
}

void SessionDrive::unbind_nw_host() {
	if (!nw_host_bound_) {
		return;
	}
	nw_host_bound_ = false;
	nw_roster_sent_.clear();
	// The in-match host stops advertising the GSID and stops announcing joiners
	// to a session it is leaving.
	Ref<Simulation> sim = world_->get_sim();
	if (sim.is_valid()) {
		sim->set_novaworld_join_tickets(false, Simulation::PlayerEnterRequestHook());
		sim->set_novaworld_registration(String(), 0, opennova::SessionIdRing{}, String());
	}
}

void SessionDrive::on_nw_host_server_command(const String &p_verb, const String &p_target,
		const PackedStringArray &p_args) {
	Ref<Simulation> sim = world_->get_sim();
	if (sim.is_null()) {
		return;
	}
	const Simulation::ServerCommandResult result =
			sim->execute_server_command(p_verb, p_target, p_args);
	if (!result.handled) {
		UtilityFunctions::print_verbose(vformat(
				"SessionDrive: NovaWorld ServerCommand '%s%s' was not executed", p_verb, p_target));
		return;
	}
	NovaWorldClient *client = nw_client();
	if (result.stop_hosting) {
		// The service punted the host's own slot: the session leaves NovaWorld
		// hosting (ClientStopHosting, the word back to verified), and the
		// NovaWorld exit ends the match on its next 62-frame block.
		if (client != nullptr) {
			client->stop_hosting();
		}
		return;
	}
	if (result.config_changed && client != nullptr) {
		// The changed columns ride the session's next refresh as its dirty delta.
		client->host_role().set_server_name(opennova::to_std(result.server_name));
		client->host_role().set_server_message(opennova::to_std(result.server_message));
	}
}

void SessionDrive::on_nw_host_player_enter_result(int64_t p_connection_id, int p_success,
		int p_msg_code, const String &p_player_ticket, const String &p_access_code_list) {
	(void)p_player_ticket;
	(void)p_access_code_list;
	Ref<Simulation> sim = world_->get_sim();
	if (sim.is_null()) {
		return;
	}
	if (!sim->apply_player_enter_result(static_cast<uint32_t>(p_connection_id), p_success != 0,
				static_cast<int32_t>(p_msg_code))) {
		UtilityFunctions::print_verbose(vformat(
				"SessionDrive: ServerPlayerEnterResult for connection %d matched no held joiner",
				p_connection_id));
	}
}

