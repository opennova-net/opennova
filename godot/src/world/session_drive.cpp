#include "world/session_drive.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "mission/mission_data.h"
#include "mission/mission_root.h"
#include "network/novaworld_client.h"
#include <base/vfs/vfs.h> // vfs_country_code (the listing's GCC)
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/host_config.h> // run_host_server_command
#include <runtime/inmatch/mission_exit.h> // the post-mission router
#include <runtime/inmatch/mission_rotation.h> // HostRotation::round_count (the ID report's first mission)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h> // read_session_listing_columns (the listing's columns)
#include "object/avatar_database.h"
#include "object/item_database.h"
#include "resource_index/launch_flags.h"
#include "rtxt/rtxt_string_file.h" // game_text_lookup (the listing's GateTypeAbbrev text)
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

// The METPROTOCOL gate reports' sink over the NovaWorld session `p_client_id` names: its strings
// the registration's and the gate reply's, the GAMETYPE row the mounted gametext's, the send the
// session's; it resolves the session by identity on every call (engine:
// inmatch/gate_metrics_reports.h, D-NET-NEW-L9-22).
opennova::inmatch::GateMetricsSink nw_gate_metrics_sink(
		const ObjectID &p_client_id, const Ref<RtxtStringFile> &p_gametext) {
	opennova::inmatch::GateMetricsSink sink;
	sink.host = [p_client_id]() {
		NovaWorldClient *live = Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(p_client_id));
		return live != nullptr ? live->host_role().metrics_host() : opennova::GateMetricsHost{};
	};
	sink.game_text = game_text_lookup(p_gametext);
	sink.send = [p_client_id](const std::string &p_text) {
		NovaWorldClient *live = Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(p_client_id));
		if (live != nullptr)
			live->host_role().send_gate_metrics(p_text);
	};
	return sink;
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
// per-side defaults). With no Avatars.def the projection runs over the empty
// table, which keeps the record's voice override (engine
// join_character_profile_without_table).
Ref<CharacterJoinProfile> SessionDrive::build_join_character_profile(
		const Ref<ResourceRoot> &p_resource_root, const Ref<PlayerSpawnLoadout> &p_loadout,
		Ref<AvatarDatabase> *r_avatar_db) {
	if (p_resource_root.is_null()) {
		return Ref<CharacterJoinProfile>();
	}
	Ref<AvatarDatabase> db;
	db.instantiate();
	if (db->load_from_resource_root(p_resource_root, "Avatars.def") != OK || !db->is_loaded()) {
		UtilityFunctions::push_warning(vformat(
				"SessionDrive: Avatars.def not loaded for LAN join profile (%s)", db->get_last_error()));
		return AvatarDatabase::character_join_profile_without_table(p_loadout, true);
	}
	if (r_avatar_db != nullptr) *r_avatar_db = db;
	// A join's session start runs in a session (the class clamp).
	return db->character_join_profile_from_loadout(p_loadout, true);
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
	join_preload_sim_->load_charattr(resource_root);
	Ref<AvatarDatabase> avatar_db;
	Ref<CharacterJoinProfile> join_profile = build_join_character_profile(
			resource_root, world_->player_visuals_->spawn_loadout(), &avatar_db);
	join_preload_sim_->set_join_character_profile(join_profile);
	// Retail's g_AvatarDefs is loaded before any join: the joiner's decode view restamps a
	// player's NetId its own table lacks from the first 0x0C on (engine
	// ClientReplicaPipeline::set_character_registry, D-NET-NEW-L2-27).
	join_preload_sim_->set_character_avatar_database(avatar_db);
	// The APPID join token (decoded .joi CK) a NovaWorld host validates in
	// ClientAuth (reject code 9), and the CD identity cookie (packed PUB*
	// blob) it validates in the 0x00 JOIN (codes 23/24/25). Both empty/"0"
	// for LAN.
	join_preload_sim_->set_app_id(p_target->get_app_id());
	join_preload_sim_->set_join_network_type(p_target->get_network_type());
	join_preload_sim_->set_join_cd_cookie(p_target->get_cd_cookie());
	join_preload_sim_->set_join_glsvss_request(p_target->get_glsvss_request());
	// A join from a browse row connects with the row's session record: the 0x42
	// first, with the row's HK (JoinerConnection::DiscoveredSession).
	join_preload_sim_->set_join_discovered_session(p_target);
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
	// The title the mounted game speaks, the joiner's identity.
	std::string title_error;
	const opennova::TitleIdentity *title = resource_root->title_identity(title_error);
	if (title == nullptr) {
		join_preload_sim_.unref();
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed,
				String("join: no identity to join with: ") + opennova::to_gd(title_error));
		return ERR_UNCONFIGURED;
	}
	join_preload_sim_->set_title(title);
	// The process's client words: the 0x4C report count runs on across joins.
	join_preload_sim_->use_client_process(&world_->client_process_);
	// The process's log devices: the join's session key closes the connect log.
	join_preload_sim_->use_log_devices(world_->log_devices_.get());
	// The shell's binding table, which the console's SAVEKEYS dumps.
	join_preload_sim_->set_controls_model(world_->controls_model_);
	// The join's JSR and JSPP are the cfg block's join-screen words: this target's commit
	// (an answered choice) writes them, every join sends what they hold, and the next
	// main-menu entry's read zeroes them (engine: inmatch/game_config_block.h
	// commit_join_choice; D-NET-NEW-L2-12).
	int join_role = JoinTarget::ROLE_PLAYER;
	String spectator_password;
	GameConfigBlock::join_choice(world_->game_config_, p_target, join_role, spectator_password);
	if (!join_preload_sim_->enable_join(p_target->get_host_ip(), p_target->get_port(),
				p_target->get_player_name(), join_role, spectator_password,
				p_target->get_server_password(), p_target->get_join_password())) {
		join_preload_sim_.unref();
		clear_pending_session();
		world_->emit_signal(kSignalLoadFailed, "join: could not open the LAN session socket");
		return ERR_CANT_CONNECT;
	}
	// This shell loads the joiner's mission start in two parts: the 0x0B header's terrain at
	// the S2C 0x11 (step_preload's declaration, set_join_sync_tail_loading), the rest after
	// the S2C 0x1A releases it (the world load, the host's streamed assets, the loaded-model
	// freeze), whose end sends the two 0x2F and the 0x0B (engine:
	// inmatch/joiner_connection.h set_mission_start_loading, D-NET-531, D-NET-NEW-L2-19).
	join_preload_sim_->set_join_mission_start_loading(true);
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
	// The connect exchange, until the S2C 0x11 ends the sync-tail wait, inside the
	// ConnectOrHost window. A mission start the round-over gate abandoned restarts for the
	// host's next mission in the runtime (engine inmatch/joiner_connection.h
	// mission_start_aborted), so that mission is identified anew at its own 0x11.
	if (!join_preload_sim_->is_join_preload_ready()) {
		join_identified_ = false;
		// Every mission start's 0x11 runs this shell's terrain load ahead of the 0x0A (a first
		// join's, a reload's, an abandoned start's restart), so the load stands declared while
		// the 0x11 is awaited (engine inmatch/joiner_connection.h set_sync_tail_loading,
		// D-NET-NEW-L2-19).
		join_preload_sim_->set_join_sync_tail_loading(true);
		join_preload_sim_->poll_join_preload();
		// The abandoned start's game loop runs no window: it lingers until the round's end
		// stores exit 4 (retail's game loop has no clock; the next mission's identification
		// arms the stream wait's window again; engine inmatch::ClientRuntime::run_frame and
		// JoinerConnection::mission_start_aborted carry the witness).
		if (join_preload_sim_->is_join_mission_start_aborted()) policy_->disarm_preload();
		const int step = policy_->preload_step(join_preload_sim_->get_join_error(), now_ms());
		if (step == NetSessionPolicy::STEP_FAIL) {
			fail_join_preload(policy_->fail_reason());
			return;
		}
		// The 0x11's own pass goes straight on into its load.
		if (!join_preload_sim_->is_join_preload_ready()) return;
	}
	// The 0x11 ended the sync-tail wait: the session is identified once, the 0x0B header's
	// terrain loads, and the load's end releases the 0x0A, which the next poll queues at its
	// head; the world stream then runs until the S2C 0x1A releases the mission start and the
	// rest of its load (D-NET-NEW-L2-19; engine inmatch/joiner_connection.h preload_ready,
	// set_sync_tail_loading and set_mission_start_loading carry the witness).
	if (!join_identified_) {
		if (!identify_join_session()) return;
		join_identified_ = true;
		if (!load_join_terrain()) return;
	}
	if (!join_preload_sim_->is_join_mission_start_released()) {
		join_preload_sim_->poll_join_preload();
		const int step = policy_->preload_step(join_preload_sim_->get_join_error(), now_ms());
		if (step == NetSessionPolicy::STEP_FAIL) {
			fail_join_preload(policy_->fail_reason());
		}
		return;
	}
	// The 0x1A's mission start: its load. Stop the frame pump before the synchronous
	// load transfers or releases the preload simulation. Every failure below owns its
	// own cleanup/reporting leg.
	world_->set_process(false);
	policy_->disarm_preload();
	const String bms = policy_->promoted_mission_file();
	Ref<ResourceRoot> resource_root = join_preload_root_;
	if (resource_root.is_null()) {
		fail_join_preload("join: resource root is unavailable after session identification");
		return;
	}
	const Ref<MissionData> mission = open_join_mission_header(bms);
	if (mission.is_null()) return;
	join_preload_root_.unref();
	join_identified_ = false;
	const int err = world_->load_mission_internal(mission, bms, resource_root);
	if (err != OK) {
		// load_mission_internal emitted the specific resource/load failure.
		cancel_join_preload();
		clear_pending_session();
		return;
	}
	// Post-load joiner watchdog: the admission tail (the mission start's loadout and
	// status -> the grants -> the S2C 0x0F) is server-driven with no protocol-level
	// timeout, so a stalled or incompatible host would leave the player loaded but hidden
	// forever. The policy reuses the retail ConnectOrHost window from the load and
	// composes a stage-named failure -- the reachable analog of retail's post-load
	// network-wait failure returns (engine: runtime/inmatch/join_session_policy.h). It
	// covers only SERVER-owed transitions: at the player-paced deployment pick the
	// watchdog ends (retail's DEATH screen simply waits; net-re 5.61).
	// A reload's admission tail has no window either (net-re §5.70.7).
	if (!reloading_join_) policy_->arm_admission_watch(now_ms());
	reloading_join_ = false;
}

bool SessionDrive::identify_join_session() {
	// Reconcile the mount with the host's data set BEFORE any referenced
	// assets are resolved through it. The mission body itself comes from the
	// host's world stream; the mount supplies shared terrain/environment/model
	// definitions (D-NET-178/194).
	if (!reconcile_join_expansion()) {
		return false;
	}
	if (!policy_->validate_promote_mission_file(join_preload_sim_->get_join_mission_file())) {
		fail_join_preload(policy_->fail_reason());
		return false;
	}
	const String bms = policy_->promoted_mission_file();
	Ref<ResourceRoot> resource_root = join_preload_root_;
	if (resource_root.is_null()) {
		fail_join_preload("join: resource root is unavailable after session identification");
		return false;
	}
	if (!policy_->validate_promote_header(join_preload_sim_->get_join_mission_header().size())) {
		fail_join_preload(policy_->fail_reason());
		return false;
	}
	// The world stream's records resolve through the item list of the mount the
	// reconcile left, so the stream decodes before any mission load (the 0x0D's class by
	// items.def; engine inmatch/joiner_connection.h preload_ready carries the witness).
	Ref<ItemDatabase> items;
	items.instantiate();
	if (items->load_from_resource_root(resource_root, "items.def") == OK) {
		join_preload_sim_->resolve_item_traits(items);
	}
	// The authoritative session identity. None of it came from discovery;
	// every value here came from 0x7B.
	world_->emit_signal(kSignalJoinSessionIdentified,
			LoadingScreenInfo::make(bms, true, join_preload_sim_->get_join_server_name(),
					join_preload_sim_->get_join_mission_name(),
					static_cast<int>(join_preload_sim_->get_join_game_type()), String()));
	return true;
}

// The S2C 0x11's load: the mission start's head and the terrain off the host's 0x0B header
// (GameWorld::load_join_terrain), then the declaration's end, which lets the next poll queue
// the 0x0A (engine inmatch/joiner_connection.h set_sync_tail_loading carries the witness). A
// reload's previous world is gone by now: the shell unloaded it before reload_as_joiner.
bool SessionDrive::load_join_terrain() {
	const String bms = policy_->promoted_mission_file();
	const Ref<MissionData> mission = open_join_mission_header(bms);
	if (mission.is_null()) return false;
	if (world_->load_join_terrain(mission, bms, join_preload_root_) != OK) {
		// load_join_terrain emitted the specific resource/load failure.
		cancel_join_preload();
		clear_pending_session();
		return false;
	}
	join_preload_sim_->set_join_sync_tail_loading(false);
	// The world stream's wait gets its own ConnectOrHost window from the load's end, as the
	// admission tail had from its world-ready (a reload's has none).
	if (!reloading_join_) policy_->arm_preload(now_ms());
	else policy_->disarm_preload();
	return true;
}

// Retail does not open the advertised .bms on a joining client. The server already copied its
// exact 0x268-byte header into S2C 0x0B, then streamed the live world and optional mission .til
// overlay. Requiring the map locally made retail-host -> OpenNova-client fail for custom maps
// even though the reverse direction worked (D-NET-194). Null after the join's failure.
Ref<MissionData> SessionDrive::open_join_mission_header(const String &p_bms) {
	Ref<MissionData> mission;
	mission.instantiate();
	if (mission->open_wire_header(join_preload_sim_->get_join_mission_header()) != OK) {
		fail_join_preload(vformat("join: failed to parse host S2C 0x0B mission header for %s: %s",
				p_bms, mission->get_last_error()));
		return Ref<MissionData>();
	}
	return mission;
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
// @ 0x4a4310]. There is no second mount object. Retail takes back the
// expansion the switch left at the return to the main menu; the shell keeps
// running on the host's expansion after the session (D-NET-622). The
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

void SessionDrive::restore_kept_expansion(const Ref<ResourceRoot> &p_root) {
	if (!expansion_kept_) return;
	expansion_kept_ = false;
	Ref<ResourceRoot> resource_root = p_root;
	if (resource_root.is_null() ||
			resource_root->get_expansion().to_lower() == kept_expansion_.to_lower()) {
		return;
	}
	const String game_code = LaunchFlags::game(world_->resolver_game());
	if (resource_root->mount_runtime(resource_root->get_root_dir(), kept_expansion_,
				LaunchFlags::loose_override_enabled(), game_code) != OK) {
		UtilityFunctions::push_warning(vformat(
				"SessionDrive: the return could not mount the menu's expansion '%s': %s",
				kept_expansion_, resource_root->get_last_error()));
		return;
	}
	// The cfg block takes the restored mount's weapon table and reads nothing; the menu
	// entry's read follows over it (engine: inmatch::GameConfigBlock::switch_expansion).
	Ref<GameConfigBlock> block = world_->get_game_config();
	if (block.is_valid()) {
		block->switch_expansion(resource_root);
	}
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
	// and it connects on its own data set anyway (engine: runtime/inmatch/join_session_policy.h) -- that is precisely the ADM
	// index-space corruption D-NET-178 records, so we refuse the join instead
	// (tracked divergence).
	if (action == NetSessionPolicy::ACTION_FAIL) {
		return policy_->decision_error();
	}
	const String target_expansion = policy_->decided_expansion();
	const String dir = resource_root->get_root_dir();
	const String previous = resource_root->get_expansion();
	// The switch keeps the expansion it leaves for the return to the main menu (the pre-dial
	// leg's switch, not the reconcile after it, is a join's first; D-NET-622).
	if (!expansion_kept_) {
		kept_expansion_ = previous;
		expansion_kept_ = true;
	}
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
	// The process's cfg block takes the mount's weapon table and reads
	// nothing, so the main-menu entry after the match is a plain read
	// (engine: inmatch::GameConfigBlock::switch_expansion).
	Ref<GameConfigBlock> block = world_->get_game_config();
	if (block.is_valid()) {
		block->switch_expansion(resource_root);
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
// cold wire drain behind the loading hold (engine: runtime/inmatch/joiner_connection.h)).
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
	// The S2C 0x1A released the joiner's mission start: its load runs now (the
	// host's streamed assets settle and the loaded-model list freezes), and its end
	// pushes the two 0x2F and the 0x0B (engine: inmatch/joiner_connection.h
	// set_mission_start_loading, D-NET-531).
	if (sim->is_join_mission_start_released()) {
		if (!world_->settle_join_wire_assets()) {
			world_->report_join_wire_asset_failure(
					"join: failed to settle the host's streamed mission assets");
			return;
		}
		sim->finish_join_mission_start();
	}
	const String loss_reason = sim->get_session_loss_reason();
	const bool deploy_pending = sim->is_join_deploy_pick_pending();
	// The first valid S2C 0x5A opens retail's independent gameplay gate and
	// can make is_joined_in_match true BEFORE the 0x0F completes the admission
	// and makes a required DEATH pick ready (a spectator's comes at the 0x0F).
	// Only the native initial-admission boundary distinguishes that split
	// ordering from a complete no-pick join; later redeploys leave the
	// predicate monotonically true.
	// An ESC that left the final wait ends the loading hold as the 0x0F does: the game
	// loop runs either way (SessionDrive::cancel_admission_wait).
	const bool initial_admission_complete =
			sim->is_join_initial_admission_complete() || sim->is_join_game_loop();
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
	// The user's cancel: ahead of the mission start the join screen's ABORT leaves with
	// its own record (Simulation::cancel_join); inside it the teardown below routes it.
	join_preload_sim_->cancel_join();
	fail_join_preload("Mission loading aborted");
	return true;
}

bool SessionDrive::cancel_admission_wait() {
	// The final wait for the S2C 0x0F: its ESC leaves the wait for the game loop and
	// nothing else (no exit reason, no teardown, no goodbye); the 0x0F still applies when
	// it comes. Only the load's ESC is the abort (engine inmatch::JoinerConnection::
	// cancel_final_wait carries the witness).
	// A join the drive already failed (the load's ESC, a stall) left its mission start: there
	// is no final wait to leave, whatever the doomed runtime reads until the shell's unload
	// (engine inmatch/join_session_policy.h fail_reason carries the witness).
	if (!policy_->fail_reason().is_empty()) return false;
	MissionRoot *runtime = world_->get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (sim.is_valid() && sim->cancel_join_final_wait()) {
		// The wait is over: a later ESC is the game loop's, never this watch's abort.
		policy_->disarm_admission_watch();
		return true;
	}
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
	join_identified_ = false;
}

bool SessionDrive::pending_session() const {
	return pending_host_.is_valid() || pending_join_.is_valid() || kept_sim_.is_valid() ||
			join_preload_sim_.is_valid();
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
	// its 0x1A as a first join's does, with no window armed.
	policy_->reset_for_join();
	pending_join_ = last_join_target_;
	join_preload_sim_ = kept_sim_;
	kept_sim_.unref();
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
		const bool novaworld = pending_host_->get_channel() == kChannelNovaWorld &&
				client != nullptr && client->is_hosting();
		pending_host_->set_network_type(novaworld ? opennova::inmatch::NetworkType::NovaWorld
		                                          : opennova::inmatch::NetworkType::Lan);
		// The match hosts on the session's own socket, the one the service saw
		// its NWU datagrams come from (D-NET-346).
		if (novaworld) p_opts->set_host_pump(client->get_game_pump());
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
	// per admitted player and the session's GSID as the in-match host's 0x81 SUS1
	// (cleared when its connection tears down, re-supplied by a re-host). The
	// listing's columns, the round clock's TimeLeft and the match's cookie-key
	// ring's PCIDKey among them, are read off the match at each update (the
	// column source bind_nw_host installs).
	NovaWorldClient *client = nw_client();
	if (client != nullptr && nw_host_bound_ && !sim->is_joiner()) {
		opennova::NwuHostRole &host = client->host_role();
		sync_nw_host_roster(host, sim);
		sim->set_novaworld_registration(opennova::cp1252_to_gd(host.gsid()), host.app_id(),
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
// added and sends ClientHostPlayerAdded / Removed on each roster change; a team
// change, a loadout's accept and a weapon-slot list rewrite the slot's
// PlayerTeam / PlayerType alone, which the match keeps per connection
// (NapiNPConnection::player_list_team / player_list_type) and the roster sync
// carries as vars (NwuHostRole::sync_roster). The PlayerPCID is the joiner's
// decrypted PUBPCID (the in-match host's account fields) (engine: runtime/inmatch/napi_np_connection.h). The ClientPlayerEnterRequest is
// NOT sent from here: the in-match host's join-phase watchdog announces a
// validating joiner itself (the hook bound in bind_nw_host), before the player
// is ever added to this roster.
void SessionDrive::sync_nw_host_roster(opennova::NwuHostRole &p_host, const Ref<Simulation> &p_sim) {
	std::vector<opennova::HostPlayerSlot> wanted;
	bool self_listed = false;
	for (const opennova::inmatch::HostPlayerListRow &row : p_sim->host_player_list_rows()) {
		opennova::HostPlayerSlot player;
		if (row.own) {
			if (!nw_self_slot_set_) continue;
			player = nw_self_slot_;
			player.team = row.slot.team;
			player.type = row.slot.type;
			self_listed = true;
		} else {
			player = row.slot;
		}
		wanted.push_back(std::move(player));
	}
	if (nw_self_slot_set_ && !self_listed) wanted.push_back(nw_self_slot_);
	p_host.sync_roster(wanted);
	for (const opennova::HostPlayerSlot &player : wanted) {
		if (nw_self_slot_set_ && player.slot == nw_self_slot_.slot) nw_self_slot_ = player;
	}
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
	// The process's one NovaWorld session takes the player teardowns no hosted match's roster
	// sync sees: a held server's, stopped by the next host's session reset (engine:
	// inmatch/client_runtime.h ClientProcessState::nwu_player_removed, D-NET-NEW-L9-21).
	const ObjectID client_id = nw_client_id_;
	world_->client_process_.nwu_player_removed = [client_id](int p_player_number) {
		NovaWorldClient *live = Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(client_id));
		if (live != nullptr)
			live->host_role().clear_player_slot(p_player_number);
	};
	// ... and that teardown's PLAYER gate report (engine: ClientProcessState::nwu_gate_metrics,
	// D-NET-NEW-L9-22).
	world_->client_process_.nwu_gate_metrics = nw_gate_metrics_sink(client_id, p_client->get_gametext());
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
	world_->client_process_.nwu_player_removed = nullptr;
	world_->client_process_.nwu_gate_metrics = opennova::inmatch::GateMetricsSink{};
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
	out->set_exits_game(route.exits_game);
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
	world_->client_process_.nwu_player_removed = nullptr;
	world_->client_process_.nwu_gate_metrics = opennova::inmatch::GateMetricsSink{};
}

// The hosting session meets its match once the runtime is live: the GSID and
// AppId the in-match host advertises, the join-ticket arm, and a listen host's
// own player in the roster (Server_PlayerAdd adds the local player like any
// other). Its PlayerIpAndPort is its loopback connection's address, which
// carries no peer address and an unset port: "0.0.0.0:0", never a reachable
// endpoint. The service learns the game endpoint from the NWU datagrams'
// source, the shared socket (D-NET-346; net-re "A Serve Only host's
// registration"). A Serve Only host publishes no slot 0.
void SessionDrive::bind_nw_host(const Ref<MissionSetupOptions> &p_opts) {
	NovaWorldClient *client = nw_client();
	MissionRoot *runtime = world_->get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (client == nullptr || !client->is_hosting() || sim.is_null() || !sim->is_host_listening()) {
		return;
	}
	opennova::NwuHostRole &host = client->host_role();
	nw_host_bound_ = true;
	// From here the listing's columns follow the match: each server-info update reads them off
	// the live session (the name and message, the game type, the mission, the caps, the join
	// lock, the rules, the round clock; D-NET-373), as retail's reads its globals at each run.
	// The source resolves the world and the session node by identity on every call, and a call
	// with no match (a map change's load) leaves the last live read standing; unbind_nw_host()
	// clears it. [orig: Lobby_UpdateServerInfo @0x4FE8C0]
	const ObjectID world_id = ObjectID(world_->get_instance_id());
	const ObjectID columns_client_id = nw_client_id_;
	// The listing's GCC and the first mission's METPROTOCOL ID report: the install's CC.BIN
	// (engine: HostRegistration::gcc, NwuHostRole::publish_mission_start; D-NET-NEW-L9-22).
	std::string country_code;
	if (p_opts.is_valid() && p_opts->get_resource_root().is_valid()) {
		country_code = opennova::vfs_country_code(
				opennova::to_std(p_opts->get_resource_root()->get_root_dir()));
	}
	host.set_column_source([world_id, columns_client_id, country_code](
								   opennova::HostRegistration &r) {
		GameWorld *live_world = Object::cast_to<GameWorld>(ObjectDB::get_instance(world_id));
		NovaWorldClient *live_client =
				Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(columns_client_id));
		const Ref<Simulation> live_sim =
				live_world != nullptr ? live_world->get_sim() : Ref<Simulation>();
		const opennova::inmatch::NapiNPServerCtx *ctx =
				live_sim.is_valid() ? live_sim->host_ctx() : nullptr;
		// A map change's load holds no sim (the kept session waits for the next runtime): the
		// last live read stands.
		if (ctx == nullptr)
			return false;
		r.gcc = country_code;
		opennova::inmatch::read_session_listing_columns(r, *ctx,
				game_text_lookup(live_client != nullptr ? live_client->get_gametext()
														: Ref<RtxtStringFile>()));
		return true;
	});
	// The match's server-info update, the join lock's toggle's (D-NET-659) and its 0x744 refresh's
	// (engine: inmatch::run_server_info_refresh), runs the hosting session's role; the hook
	// resolves the session node by identity on every call.
	// [orig: Server_ToggleJoinLock @0x50D750 -> Lobby_UpdateServerInfo(0) @0x50D762]
	if (opennova::inmatch::NapiNPServerCtx *ctx = sim->host_ctx()) {
		ctx->on_update_server_info = [columns_client_id]() {
			NovaWorldClient *live =
					Object::cast_to<NovaWorldClient>(ObjectDB::get_instance(columns_client_id));
			if (live != nullptr)
				live->host_role().update_server_info();
		};
		// The match's METPROTOCOL gate reports go out on the hosting session, its strings the
		// registration's and the gate reply's (engine: inmatch/gate_metrics_reports.h,
		// D-NET-NEW-L9-22).
		ctx->gate_metrics = nw_gate_metrics_sink(columns_client_id, client->get_gametext());
	}
	const std::string login_pcid = client->get_login_pcid();
	sim->set_novaworld_registration(opennova::cp1252_to_gd(host.gsid()), host.app_id(),
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
		// The host's own name as its roster carries it, the page's bytes (D-NET-661).
		self.player_name =
				opennova::utf8_to_retail_text(opennova::to_std(p_opts->get_player_name()));
		self.ip_and_port = opennova::peer_addr_to_string(opennova::PeerAddr{});
		self.pcid = login_pcid; // the host's own player's PCID is its login cookie
		// Its PlayerTeam / PlayerType are the match's, as for any added player.
		self.team = "0";
		self.type = "0";
		// The local player's slot: its add sends ClientHostPlayerAdded alone (engine:
		// HostPlayerSlot::local, D-NET-659).
		self.local = true;
		for (const opennova::inmatch::HostPlayerListRow &row : sim->host_player_list_rows()) {
			if (!row.own) continue;
			self.team = row.slot.team;
			self.type = row.slot.type;
		}
		host.set_player_slot(self);
		nw_self_slot_ = self;
		nw_self_slot_set_ = true;
	}
	// The mission start's update, at every mission start of a hosting session, map changes
	// included (NwuHostRole::publish_mission_start, which carries Game_StartMission's cite):
	// both lists whole, the columns the started mission's. A fresh host's first full Host list,
	// its HostKey included, is this one, since its host result publishes nothing (D-NET-465).
	// The session's first mission (the rotation's round count 0) then sends the gate its
	// METPROTOCOL ID report (D-NET-NEW-L9-22).
	const opennova::inmatch::NapiNPServerCtx *round_ctx = sim->host_ctx();
	host.publish_mission_start(round_ctx != nullptr && round_ctx->rotation != nullptr
					? round_ctx->rotation->round_count
					: 0);
}

void SessionDrive::unbind_nw_host() {
	if (!nw_host_bound_) {
		return;
	}
	nw_host_bound_ = false;
	nw_self_slot_ = opennova::HostPlayerSlot{};
	nw_self_slot_set_ = false;
	// The listing's columns stop following a match the session is leaving.
	if (NovaWorldClient *client = nw_client())
		client->host_role().set_column_source(nullptr);
	// The in-match host stops advertising the GSID and stops announcing joiners
	// to a session it is leaving.
	Ref<Simulation> sim = world_->get_sim();
	if (sim.is_valid()) {
		if (opennova::inmatch::NapiNPServerCtx *ctx = sim->host_ctx()) {
			ctx->on_update_server_info = nullptr;
			ctx->gate_metrics = opennova::inmatch::GateMetricsSink{};
		}
		sim->set_novaworld_join_tickets(false, Simulation::PlayerEnterRequestHook());
		sim->set_novaworld_registration(String(), 0, String());
	}
}

// A ServerCommand the hosting NovaWorld session received. Before the match
// has a host context (the hosting wait: the load before the session create)
// it runs on a session-less one, where SetMPReset alone passes; a config
// change lands in the process's cfg block, which is saved at once
// (engine: inmatch/host_config.h run_host_server_command,
// docs/net/novaworld-net-re.md D-NET-383).
void SessionDrive::on_nw_host_server_command(const String &p_verb, const String &p_target,
		const PackedStringArray &p_args) {
	Ref<Simulation> sim = world_->get_sim();
	Ref<GameConfigBlock> block = world_->get_game_config();
	Simulation::ServerCommandResult result;
	if (sim.is_valid()) {
		result = sim->execute_server_command(p_verb, p_target, p_args);
	} else {
		std::vector<std::string> args;
		for (int i = 0; i < p_args.size(); ++i)
			args.push_back(opennova::to_std(p_args[i]));
		const bool serve_and_play = pending_host_.is_null() || pending_host_->get_serve_and_play();
		const opennova::inmatch::ServerCommandOutcome outcome =
				opennova::inmatch::run_host_server_command(nullptr, nullptr,
						opennova::inmatch::host_connection_mode(serve_and_play),
						block.is_valid() ? &block->native().cfg() : nullptr,
						opennova::to_std(p_verb), opennova::to_std(p_target), args);
		result.handled = outcome.handled;
		result.stop_hosting = outcome.stop_hosting;
		result.config_changed = outcome.config_changed;
	}
	if (!result.handled) {
		UtilityFunctions::print_verbose(vformat(
				"SessionDrive: NovaWorld ServerCommand '%s%s' was not executed", p_verb, p_target));
		return;
	}
	if (result.config_changed && block.is_valid()) {
		block->save();
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
	// A changed name or message is the live session's, which the listing's next update reads.
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

