// The role feeds (inmatch/role_feeds.h), pinned where they used to live in
// the Simulation binding (ADR 0040 ladder E8): the end-of-round session state
// and overlay input off the folded 0x1D header, the stat rows joined to the
// roster with the local row resolved through the board index, the tab
// filter, the DEATH screen status off the joiner's 0x0A / 0x6E facts, and the
// bare-role fallbacks.
// [orig: NapiNPClientMsg_0x01D @0x430840; StatScreen_PopulateStatResultsList
//  @0x562240; UI_UpdateDeathScreenContent @0x5536a0]
#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/effect_pose_index.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title
#include <runtime/hud/hud_math.h> // friendly_tag_fallback_name
#include <runtime/replication/client_world_materializer.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/angle.h>
#include <runtime/world/radio_call.h>

#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>

using namespace opennova;
using namespace opennova::inmatch;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// The weapon inset's friendly name (D-NET-655): Entity_GetDisplayName of the
// HUD target cursor, the aim lock else the head-look target, over the live
// rules word, the HUD's team and the role's S2C 0x4C slot table; none when
// the lookup names none. The cursor is the HUD pass's record as the scene frame
// found it (D-NET-NEW-L8-48): each read below runs one pass's build, then the
// next scene frame's read; the stale cases follow.
// [orig: Render_WeaponInsetScene -- @0x5CA0A4..0x5CA0B5; HUD_BuildEntityInfo
//  -- the lock @0x4B87F0, the head-look target @0x4B881F; Entity_GetDisplayName
//  @0x59BF70; HUD_RenderAllOverlays @0x5A8070 -- the build @0x5A80BC]
static void inset_target_name_cases() {
	auto runtime_heap = std::make_unique<ClientRuntime>("host");
	ClientRuntime &host_runtime = *runtime_heap;
	// The kernel on the heap: main's frame already holds several (1 MB stack).
	auto kernel_heap = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 8);
	world::Entity local;
	local.kind = world::EntityKind::Organic;
	local.team = 1;
	const world::EntityHandle me = kernel.world.registry.spawn(0, local);
	kernel.world.cached.local_player = me;
	kernel.world.ai.attach(me);
	world::Entity mate;
	mate.kind = world::EntityKind::Organic;
	mate.team = 1;
	mate.display_name = "Mate";
	const world::EntityHandle mate_h = kernel.world.registry.spawn(0, mate);
	world::Entity foe = mate;
	foe.team = 2;
	foe.display_name = "Foe";
	const world::EntityHandle foe_h = kernel.world.registry.spawn(0, foe);
	world::Entity unnamed = mate;
	unnamed.display_name.clear();
	const world::EntityHandle unnamed_h = kernel.world.registry.spawn(0, unnamed);
	world::Entity player = mate;
	player.display_name = "Body";
	player.flags |= world::kEntityFlagPlayer;
	const world::EntityHandle player_h = kernel.world.registry.spawn(0, player);
	RoleView host;
	host.runtime = &host_runtime;
	host.kernel = &kernel;
	world::AiEntity *body = kernel.world.ai.for_handle(me);
	CHECK(body != nullptr);
	if (body != nullptr) {
		// The targeting update writes the lock into both (local_player_targeting.cpp).
		const auto lock = [body](world::EntityHandle h) {
			body->inf.combat_target = h;
			body->slot.f[3] = world::AiSlot::target_word(world::TargetRef{h});
		};
		// One frame: the HUD pass builds the record, the next scene frame's inset reads it.
		const auto inset = [&]() {
			build_hud_entity_info(kernel, false);
			kernel.local.note_hud_scene_head();
			return hud_role_facts(host).inset_target_name;
		};
		CHECK(inset().empty()); // no cursor
		body->inf.head_look_target = mate_h;
		CHECK(inset() == "Mate");
		// The lock outranks the head-look target; another team names none.
		lock(foe_h);
		CHECK(inset().empty());
		// An unnamed teammate: '^' + the fallback at its pool-encoded id.
		lock(unnamed_h);
		CHECK(inset() ==
				hud::friendly_tag_fallback_name(unnamed_h.packed));
		// FriendlyTag 0 in the live rules word nulls every name.
		kernel.world.rules.mpattrib = 0x400;
		lock(mate_h);
		CHECK(inset().empty());
		kernel.world.rules.mpattrib = 0;
		// A player names the first 0x4C slot driving it, never its body's
		// name; with none it is unnamed.
		lock(player_h);
		CHECK(inset().empty());
		replication::ClientRosterSlot &slot = host_runtime.state().roster[3];
		slot.bound = true;
		slot.name = "Pat";
		slot.registry_clan = "TAG";
		slot.entity_slot = static_cast<int16_t>(player_h.slot());
		host_runtime.state().visible_players.push_back({3, player_h.packed});
		CHECK(inset() == "Pat <ch>TAG<co>");
		// A lock on a round (a flare the heat lock took, the +0xC word) is the cursor: no
		// pool holds a round, so the inset names nothing, whatever the head-look target
		// [orig: HUD_BuildEntityInfo @0x4B8440 (@0x4B87ED..0x4B882D); Render_WeaponInsetScene
		//  @0x5C9740 (the pool test @0x5C9F78)] (D-NET-64).
		lock(world::EntityHandle{});
		body->inf.head_look_target = mate_h;
		CHECK(inset() == "Mate");
		body->slot.f[3] = world::AiSlot::target_word(world::TargetRef::of_round(3));
		CHECK(inset().empty());
		// An entity lock in the same word names its entity again.
		lock(mate_h);
		CHECK(inset() == "Mate");
		// The cursor is the word alone, read once: a word the guided round's init wrote
		// without the combat target names the word's entity, and a combat target over a
		// zero word is no cursor, so the head-look target names itself
		// [orig: HUD_BuildEntityInfo @0x4B8440 - the word @0x4B87F0, the head-look store
		//  only for a zero word (`cmp dword ptr [esi+168h], 0` @0x4B8816, @0x4B881F)].
		body->inf.combat_target = world::EntityHandle{};
		body->slot.f[3] = world::AiSlot::target_word(world::TargetRef{unnamed_h});
		CHECK(inset() ==
				hud::friendly_tag_fallback_name(unnamed_h.packed));
		body->inf.combat_target = foe_h;
		body->slot.f[3] = 0;
		CHECK(inset() == "Mate");
		// The scene frame reads ahead of its own pass: the inset names the last pass's
		// cursor, and this frame's build shows on the next frame [orig: Render_ProcessMainSceneFrame
		//  @0x5CA0F0 - the inset @0x5CA949 ahead of the pass @0x5CACE3 / @0x5CAD04].
		body->inf.combat_target = world::EntityHandle{};
		body->inf.head_look_target = unnamed_h;
		kernel.local.note_hud_scene_head();
		CHECK(hud_role_facts(host).inset_target_name == "Mate");
		build_hud_entity_info(kernel, false);
		CHECK(hud_role_facts(host).inset_target_name == "Mate");
		kernel.local.note_hud_scene_head();
		CHECK(hud_role_facts(host).inset_target_name ==
				hud::friendly_tag_fallback_name(unnamed_h.packed));
		// A pass that returns at its head builds nothing: the round-over latch, the white hit
		// flash and a missing local player each leave the last build standing
		// [orig: HUD_RenderAllOverlays @0x5A8084 / @0x5A8098 / @0x5A80A3, ahead of the build
		//  @0x5A80BC].
		body->inf.head_look_target = mate_h;
		build_hud_entity_info(kernel, true);
		kernel.local.note_hud_scene_head();
		CHECK(hud_role_facts(host).inset_target_name ==
				hud::friendly_tag_fallback_name(unnamed_h.packed));
		kernel.local.view.flash.white = 40;
		CHECK(inset() == hud::friendly_tag_fallback_name(unnamed_h.packed));
		kernel.local.view.flash.white = 0;
		kernel.world.cached.local_player = world::EntityHandle{};
		build_hud_entity_info(kernel, false);
		kernel.world.cached.local_player = me;
		kernel.local.note_hud_scene_head();
		CHECK(hud_role_facts(host).inset_target_name ==
				hud::friendly_tag_fallback_name(unnamed_h.packed));
		CHECK(inset() == "Mate");
		// The destroy zeroes the cursor word: a target destroyed since the build names none
		// until the next build [orig: Entity_Destroy @0x43E810 - @0x43E89C..0x43E8A4].
		kernel.world.registry.despawn(mate_h);
		CHECK(hud_role_facts(host).inset_target_name.empty());
		// On a joiner the cursor may name a decoded remote person: the pool-0 entity retail's
		// client built for its row, named by the same lookup over the row's Flags word, team
		// byte and Name, a player through the 0x4C slot driving its slot, with no team gate
		// (D-NET-NEW-L8-53) [orig: Entity_GetDisplayName @0x59BF70 - the Player test
		//  @0x59BFA0, the team gate @0x59BFB2, the slot walk @0x59C07A..0x59C0BB].
		replication::ClientState &cs = host_runtime.state();
		replication::ClientEntityState &remote = cs.upsert(0x0006);
		remote.rm_entity_flags = world::kEntityFlagPlayer;
		remote.team = 2;
		remote.team_known = true;
		remote.display_name = "Body";
		replication::ClientEntityState &rifleman = cs.upsert(0x0007);
		rifleman.team = 1;
		rifleman.team_known = true;
		rifleman.display_name = "Rifleman";
		const auto scene_names = [&](uint16_t wire_handle) {
			world::HudTargetCursor cursor;
			cursor.wire_person = wire_handle;
			kernel.local.view_tracker.hud_cursor_at_scene = cursor;
			return hud_role_facts(host).inset_target_name;
		};
		CHECK(scene_names(0x0006).empty()); // a player no 0x4C slot drives
		replication::ClientRosterSlot &wire_slot = cs.roster[4];
		wire_slot.bound = true;
		wire_slot.name = "Remote";
		wire_slot.entity_slot = 6;
		cs.visible_players.push_back({4, 0x0006});
		CHECK(scene_names(0x0006) == "Remote"); // the other team's player too
		CHECK(scene_names(0x0007) == "Rifleman"); // a non-player of the HUD team
		rifleman.display_name.clear();
		CHECK(scene_names(0x0007) == hud::friendly_tag_fallback_name(0x0007));
		rifleman.team = 2;
		CHECK(scene_names(0x0007).empty()); // a non-player of another team
		kernel.world.rules.mpattrib = 0x400;
		CHECK(scene_names(0x0006).empty()); // FriendlyTag 0 nulls every name
		kernel.world.rules.mpattrib = 0;
		CHECK(scene_names(0x0009).empty()); // no row holds the handle
		kernel.local.view_tracker.hud_cursor_at_scene = world::HudTargetCursor{};
	}
}

// The expansion's override table is searched before vmacros.bin: its entries
// win, a key it lacks reads vmacros.bin's, and the radio chat line resolves the
// same way [orig: HUD_DrawRadioTitleMenu @0x5BFBD5 / @0x5BFC5D, HUD_DrawEmotesMenu
// @0x5BFF45 / @0x5BFFCD, VMacros_GetMacroText @0x5B7196 ->
// TextResource_FindEntryBySectionAndKey @0x75D27B].
static void voice_macro_override_cases() {
	auto runtime_heap = std::make_unique<ClientRuntime>("macros");
	// The kernel on the heap: main's frame already holds several (1 MB stack).
	auto kernel_heap = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 8);
	world::Entity seed;
	seed.kind = world::EntityKind::Organic;
	seed.team = 2;
	kernel.world.cached.local_player = kernel.world.registry.spawn(0, seed);
	RoleView view;
	view.runtime = runtime_heap.get();
	view.kernel = &kernel;
	view.joiner = true;
	auto macro_table = [](std::initializer_list<std::pair<const char *, const char *>> rows) {
		rtxt::File table;
		table.sections.push_back(rtxt::Section{"macrotext", static_cast<uint32_t>(rows.size())});
		for (const auto &row : rows) {
			rtxt::Entry entry;
			entry.key = row.first;
			entry.text = row.second;
			table.entries.push_back(entry);
		}
		return table;
	};
	kernel.world.tables.voice_macros =
			macro_table({{"RADIO_TITLE", "Radio"}, {"EMOTES_TITLE", "Emotes"}, {"EMO_1", "Hello"}});
	const rtxt::File override_table = macro_table({{"RADIO_TITLE", "Expansion Radio"}, {"EMO_1", "Wave"}});
	kernel.world.tables.text_override = &override_table;
	const auto over = std::make_unique<HudRoleFacts>(
			hud_role_facts(view, kHudVoiceMenuEmotes | kHudVoiceMenuRadio));
	CHECK(over->radio_menu.title == "Expansion Radio" && over->emotes_menu.title == "Emotes");
	CHECK(over->emotes_menu.texts[0] == "Wave" && over->emotes_menu.texts[1] == "EMO_2");
	CHECK(world::radio_call_text(kernel.world, "EMO_1", "Pat", nullptr) == "Pat: Wave");
	CHECK(world::radio_call_text(kernel.world, "RADIO_TITLE", "Pat", nullptr) == "Pat: Expansion Radio");
	kernel.world.tables.text_override = nullptr;
	CHECK(world::radio_call_text(kernel.world, "EMO_1", "Pat", nullptr) == "Pat: Hello");
}

// The joiner's lighting wire walk takes the decoded row whose wire handle equals
// its native L's packed handle like any other (H and L are separate domains; the
// self row is the self handle's): a cached context there is re-evaluated and
// emitted, where the L comparison erased it and emitted nothing (D-NET-698)
// [orig: Terrain_SetupEffectForEntity @0x5C74A0 per drawn entity].
static void lighting_walk_keeps_the_row_at_l_cases() {
	auto runtime_heap = std::make_unique<ClientRuntime>("LightAtL");
	auto kernel_heap = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_heap;
	world::World &w = kernel.world;
	w.registry.configure_pool(0, 8);
	world::Entity seed;
	seed.kind = world::EntityKind::Organic;
	w.cached.local_player = w.registry.spawn_at(world::EntityHandle::make(0, 3), seed);
	replication::ClientEntityState &row = runtime_heap->state().upsert(w.cached.local_player.packed);
	row.type_id = 0x0011;
	row.cls = EntityClass::Player;
	RoleView jv;
	jv.kernel = &kernel;
	jv.runtime = runtime_heap.get();
	jv.joiner = true;
	EntityLightingFeed feed;
	feed.layout_revision_seen = 0; // the layout this cache was built for
	feed.last_by_wire[w.cached.local_player.packed].quality = 2;
	const int32_t step[3] = { 0, 0, 65536 * 200 };
	std::vector<EntityLightingChange> changes;
	feed.collect(jv, step, {}, {}, 0, changes);
	// The row keeps its cached context (re-evaluated in place), where the L
	// comparison erased it as a self row.
	CHECK(feed.last_by_wire.count(w.cached.local_player.packed) == 1);
}

// A Serve Only host folds no 0x1D: its end-round words are the board its own round end built,
// the globals HUD_DrawEndRoundStatsOverlay reads, and its status page's end-round leg runs the
// transition on its g_RoundOver. Before the round ends nothing is announced; after, the header
// is known and the board never (only a client's 0x56 landing sets g_ScoreboardDirty, so stat.mnu
// never opens), and the overlay reads the board's rows: each name cut to the 32-byte entry.
// [orig: Server_ProcessRoundEnd @0x5164F0 (Server_BuildEndOfRoundScoreboard @0x516590,
//  g_RoundOver @0x5168E4); Server_BuildEndOfRoundScoreboard @0x508F30 (the name @0x5090D3, the
//  primary @0x509152); Server_DrawStatusScreen @0x50A2D0 (@0x50B286..0x50B2A3);
//  UI_ProcessEndRoundScreenTransition @0x5B8600 (`cmp g_ScoreboardDirty, 0` @0x5B8600)]
static void serve_only_end_round_cases() {
	auto kernel_heap = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_heap;
	world::World &w = kernel.world;
	w.rules.mp_session = true;
	w.registry.configure_pool(0, 8);
	world::MatchRules rules;
	rules.game_type = game_type::kDeathmatch;
	w.match.configure(rules);
	world::Entity body;
	body.kind = world::EntityKind::Organic;
	const world::EntityHandle ann = w.registry.spawn_at(world::EntityHandle::make(0, 1), body);
	const world::EntityHandle bob = w.registry.spawn_at(world::EntityHandle::make(0, 2), body);
	w.match.upsert_player({ann, 1, "Ann"});
	w.match.upsert_player({bob, 2, "A name longer than thirty-one characters"});
	auto ctx = std::make_unique<NapiNPServerCtx>();
	ctx->is_in_session = 1;
	RoleView serve_only;
	serve_only.host = ctx.get();
	serve_only.kernel = &kernel;
	CHECK(!end_round_session_state(serve_only).header_known);
	CHECK(end_round_overlay_input(serve_only).player_names[0].empty());
	CHECK(w.match.finish(0, w));
	const EndRoundSessionState s = end_round_session_state(serve_only);
	CHECK(s.header_known && !s.board_known && s.session_open && !s.team_mode);
	CHECK(s.game_type == game_type::kDeathmatch && s.winner == 0 && s.draw); // two zero rows tie
	const hud::EndRoundOverlayInput in = end_round_overlay_input(serve_only);
	CHECK(in.game_type == game_type::kDeathmatch && in.draw && in.winner_team == 0);
	CHECK(in.local_team == 0 && !in.death_screen);
	const std::string &n0 = in.player_names[0];
	const std::string &n1 = in.player_names[1];
	CHECK((n0 == "Ann" && n1 == "A name longer than thirty-one c") ||
	      (n1 == "Ann" && n0 == "A name longer than thirty-one c"));
	CHECK(in.player_names[2].empty() && in.player_scores[0] == 0 && in.player_scores[1] == 0);
	// A listen host or a joiner keeps its client's folded words (none here).
	ClientRuntime own("ServeOnlyOwn");
	RoleView listen = serve_only;
	listen.runtime = &own;
	CHECK(!end_round_session_state(listen).header_known);
	// The folded 0x1D runs the end-round screens in a session alone: single player's own client
	// folds the one its round end sends, and no end-round screen runs out of a session.
	// [orig: HUD_DrawOverlayPanels @0x5C0060 - `g_RoundOver && is_in_session` @0x5C0069]
	own.view().state().end_round.header_known = true;
	own.view().state().end_round.known = true;
	CHECK(end_round_session_state(listen).header_known && end_round_session_state(listen).board_known);
	w.rules.mp_session = false;
	CHECK(!end_round_session_state(listen).header_known &&
	      !end_round_session_state(listen).board_known);
	w.rules.mp_session = true;
}

// The scene frame's call of its HUD pass (the session's radar step runs on it): no call
// while a death.mnu trigger holds (the deploy-map overlay; the dead bit in session) or
// the open latch is left over; in camera mode 0 or under the death screen for a body
// without the dead bit, the F1 help shut; in the chase view from a control seat alone.
// [orig: Render_ProcessMainSceneFrame @0x5CAB2B..0x5CAB58, @0x5CAC81 (g_DeployScreenActive),
//  @0x5CAC8E (g_DeathMenuOpenLatch), @0x5CACA6..0x5CACDD (the mode, the death screen, the
//  seat), @0x5CACED..0x5CACFE (the dead bit, g_HelpScreenOpen); the calls @0x5CACE3 / @0x5CAD04]
static void hud_pass_call_site_cases() {
	auto runtime_heap = std::make_unique<ClientRuntime>("hud");
	ClientRuntime &runtime = *runtime_heap;
	auto kernel_heap = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_heap;
	kernel.world.registry.configure_pool(0, 8);
	world::Entity seed;
	seed.alive = true;
	seed.health = 100;
	const world::EntityHandle local = kernel.world.registry.spawn(0, seed);
	kernel.world.cached.local_player = local;
	RoleView view;
	view.kernel = &kernel;
	view.runtime = &runtime;
	world::PlayerViewState &v = kernel.local.view;
	CHECK(hud_overlays_called(view, false));
	CHECK(!hud_overlays_called(view, true)); // the F1 help
	runtime.state().deploy_overlay_active = true;
	CHECK(!hud_overlays_called(view, false)); // the menu's open leg
	runtime.state().deploy_overlay_active = false;
	runtime.state().death_menu_open_latch = true;
	CHECK(!hud_overlays_called(view, false)); // the close leg
	runtime.state().death_menu_open_latch = false;
	world::Entity &body = *kernel.world.registry.get(local);
	body.flags |= world::kEntityFlagDead;
	CHECK(!hud_overlays_called(view, false)); // out of session: the mode-0 leg's dead bit
	kernel.world.rules.mp_session = true;
	CHECK(!hud_overlays_called(view, false)); // in session: the death trigger
	kernel.world.rules.mp_session = false;
	body.flags &= ~world::kEntityFlagDead;
	v.camera_mode = 1;
	CHECK(!hud_overlays_called(view, false)); // the chase view on foot
	body.mounted = true;
	body.mount_type = world::SeatType::Gunner;
	CHECK(!hud_overlays_called(view, false));
	body.mount_type = world::SeatType::Driver;
	CHECK(hud_overlays_called(view, false)); // parentSlot 5
	body.mount_type = world::SeatType::Controller;
	CHECK(hud_overlays_called(view, true)); // parentSlot 2; this leg reads no help word
	body.mounted = false;
	v.death_screen_active = true;
	CHECK(hud_overlays_called(view, false)); // the death screen takes the mode-0 leg
	v.death_screen_active = false;
	v.camera_mode = 4;
	CHECK(!hud_overlays_called(view, false));
	RoleView bare;
	CHECK(!hud_overlays_called(bare, false));
}

int main() {
	hud_pass_call_site_cases();
	voice_macro_override_cases();
	lighting_walk_keeps_the_row_at_l_cases();
	serve_only_end_round_cases();
	// The bare local role: nothing folded, nothing gathered.
	{
		RoleView bare;
		CHECK(!local_death_screen_active(bare) && !local_player_dead(bare));
		const EndRoundSessionState s = end_round_session_state(bare);
		CHECK(!s.header_known && !s.board_known && !s.session_open);
		CHECK(end_round_rows(bare).empty());
		std::vector<world::FriendlyTagSource> tags;
		CHECK(!collect_friendly_tags(bare, tags) && tags.empty());
		const world::DeployScreenStatus status =
				deploy_screen_status(bare, world::SpawnZoneRegistry(), "M", hud::GameTextLookup());
		CHECK(status.penalty_seconds == 0 && status.line.kind == world::DeployStatusLine::Kind::None &&
				!status.statics.medic && status.respawn_text.empty());
		CHECK(status.statics_text.call_medic == "Press M to call a medic");
		std::vector<hud::HudLfpZone> zones;
		CHECK(!collect_lfp_zones(bare, world::SpawnZoneRegistry(), 1, zones) && zones.empty());
		// The entity lighting diff without a kernel emits nothing and keeps its
		// caches; a layout change forgets the wire-domain cache.
		EntityLightingFeed sun;
		sun.last_by_wire[7].quality = 2;
		sun.last_by_bms[3].quality = 1;
		const int32_t step[3] = { 0, 0, 65536 * 200 };
		std::vector<EntityLightingChange> changes;
		sun.collect(bare, step, {}, {}, 5, changes);
		CHECK(changes.empty() && sun.last_by_wire.size() == 1 && sun.layout_revision_seen == -1);
	}

	// The Tab board header's session variables: none on the bare role; the
	// authority's serializer copies, re-parsed through the client caps on a
	// session peer (a listen host); the stock Co-op selector names the map file.
	// [orig: Game_SerializeMissionInfoToDataStream @0x523620;
	//  SaveFile_SendAndWaitForServerAck @0x5204c4..0x5204f5;
	//  Client_ParseServerSessionVariables @0x5203b5 / @0x5203e0]
	{
		CHECK(scoreboard_session_vars(RoleView{}).server_name.empty());
		NapiNPServerCtx host;
		host.config.server_name = std::string(40, 'S');
		host.config.mission_name = "Dormant Volcano Isle";
		host.config.mission_file = "ASH_I5A.BMS";
		host.config.game_type = 0x10010;
		// The mission start's session title (g_SessionTitle): the map file with no override table,
		// which the stock Co-op MISSIONNAME advertises (D-NET-NEW-L8-16).
		host.session_title = mission_session_title(nullptr, host.config.mission_file);
		RoleView view;
		view.host = &host;
		host.is_mp_session_peer = 0;
		SessionVars vars = scoreboard_session_vars(view);
		CHECK(vars.server_name.size() == 40 && vars.mission_name == "Dormant Volcano Isle");
		host.is_mp_session_peer = 1;
		vars = scoreboard_session_vars(view);
		CHECK(vars.server_name == std::string(31, 'S') &&
				vars.mission_name == "Dormant Volcano Isle" && vars.game_type == 0x10010);
		host.config.game_type = 0x10020;
		CHECK(scoreboard_session_vars(view).mission_name == "ASH_I5A.BMS");
		host.config.game_type = 0x30020;
		CHECK(scoreboard_session_vars(view).mission_name == "Dormant Volcano Isle");
	}

	// A joiner's view: the 0x1D header + 0x56 board folded into its replica state.
	ClientRuntime runtime("RoleFeeds");
	replication::ClientState &cs = runtime.state();
	cs.end_round.header_known = true;
	cs.end_round.known = true;
	cs.end_round.header.winner_team = 2;
	cs.end_round.header.team_score_0 = 7;
	cs.end_round.header.team_score_1 = 9;
	cs.end_round.header.draw = 0;
	cs.end_round.header.player_index = 1;
	cs.end_round.header.player_names[0] = "Alice";
	cs.end_round.header.player_scores[0] = 12;
	cs.death_screen_active = true;
	cs.round_time_remaining_ticks = 620;
	RoleView joiner;
	joiner.runtime = &runtime;
	joiner.joiner = true;
	CHECK(local_death_screen_active(joiner));
	const EndRoundSessionState s = end_round_session_state(joiner);
	CHECK(s.header_known && s.board_known && s.winner == 2 && s.team_score_0 == 7 &&
			s.team_score_1 == 9 && !s.draw && s.my_index == 1 && s.round_ticks == 620 &&
			s.death_screen && s.session_open); // a live joiner runtime: not lost
	const hud::EndRoundOverlayInput in = end_round_overlay_input(joiner);
	CHECK(in.winner_team == 2 && in.team_scores[1] == 9 && in.player_names[0] == "Alice" &&
			in.player_scores[0] == 12 && in.death_screen && in.round_time_remaining_ticks == 620);
	// A negative round clock reads as 0 (the untimed arm).
	cs.round_time_remaining_ticks = -1;
	CHECK(end_round_session_state(joiner).round_ticks == 0);
	// The stat fill's local slot is the local player entity's slot id (+0x154:
	// the runtime's latched roster slot), not the 0x1D header's board index,
	// which here names board row 1 (slot 5) where the local slot is 3; the rows
	// carry no local flag (D-MNU-51).
	// [orig: StatScreen_PopulateStatResultsList @0x562240 - g_LocalPlayerEntity
	//  +0x154 read @0x562724]
	{
		ClientRuntime local("StatLocal");
		replication::ClientState &ls = local.state();
		ls.end_round = cs.end_round;
		ls.end_round.board.team_fields = { { 1, 1 } };
		EndRoundPlayerRow three;
		three.slot = 3;
		three.per_team = { 4 };
		EndRoundPlayerRow five;
		five.slot = 5;
		five.per_team = { 9 };
		ls.end_round.board.players = { three, five };
		ls.end_round.header.player_index = 1;
		for (const int slot : { 3, 5 }) {
			ls.roster[static_cast<size_t>(slot)].bound = true;
			ls.roster[static_cast<size_t>(slot)].team = 1;
		}
		local.view().set_local_player_slot(3);
		RoleView v;
		v.runtime = &local;
		v.joiner = true;
		CHECK(end_round_session_state(v).local_slot == 3);
		CHECK(end_round_session_state(RoleView{}).local_slot == -1);
		const std::vector<StatScreenRow> rows = end_round_rows(v);
		CHECK(rows.size() == 2 && rows[0].slot == 3 && rows[1].slot == 5);
	}
	// The joiner's local team is the S2C 0x04 latch; a listen host never
	// receives that record, so the authority reads its own player entity
	// [orig: g_LocalTeam @0x5b7f56, latched on the host from its loopback
	// 0x04 @0x425499].
	{
		ClientRuntime host_runtime("host");
		host_runtime.state().end_round = cs.end_round;
		mission::MissionKernel kernel;
		kernel.world.registry.configure_pool(0, 8);
		world::Entity seed;
		seed.kind = world::EntityKind::Organic;
		seed.team = 2;
		kernel.world.cached.local_player = kernel.world.registry.spawn(0, seed);
		RoleView host;
		host.runtime = &host_runtime;
		host.kernel = &kernel;
		CHECK(host_runtime.assigned_team() == 0);
		CHECK(end_round_session_state(host).local_team == 2);
		CHECK(end_round_overlay_input(host).local_team == 2);
		CHECK(end_round_session_state(joiner).local_team == 0); // the joiner's latch, unset here
	}

	// The host's HUD gates read the live rules word (g_RulesFlags: the mission-data block's,
	// as an admin SET moves it), not the shell's staged option word: FriendlyTag 0 (0x400)
	// set in the world word hides the friendly tags whatever the staged word holds, and the
	// reverse (D-NET-631).
	// [orig: HUD_DrawFriendlyTagsPass @0x5A4480 `test g_RulesFlags, 400h`; the FriendlyTags
	//  SET @0x405EF6 / @0x405F05]
	{
		ClientRuntime host_runtime("host");
		host_runtime.state().death_screen_active = true; // the pass gates admit every team
		mission::MissionKernel kernel;
		kernel.world.registry.configure_pool(0, 8);
		world::Entity local;
		local.kind = world::EntityKind::Organic;
		local.team = 1;
		kernel.world.cached.local_player = kernel.world.registry.spawn(0, local);
		world::Entity mate;
		mate.kind = world::EntityKind::Organic;
		mate.team = 1;
		mate.has_item_def = true;
		mate.display_name = "Mate";
		kernel.world.registry.spawn(0, mate);
		RoleView host;
		host.runtime = &host_runtime;
		host.kernel = &kernel;
		std::vector<world::FriendlyTagSource> tags;
		host.staged_mp_attributes = 0;
		kernel.world.rules.mpattrib = 0x400;
		CHECK(session_rules_word(host) == 0x400);
		collect_friendly_tags(host, tags);
		CHECK(tags.empty());
		host.staged_mp_attributes = 0x400;
		kernel.world.rules.mpattrib = 0x4204; // the out-of-session word
		CHECK(session_rules_word(host) == 0x4204);
		collect_friendly_tags(host, tags);
		CHECK(tags.size() == 1 && tags[0].name == "Mate");
		CHECK(session_rules_word(RoleView{}) == 0);
	}

	// The breath bar's facts: the replica's 0x0A breath samples and the 0x1D
	// latch on every role; the breath seconds from a joiner's sub-block-1 copy
	// or the authority's own WAC named value.
	// [orig: HUD_DrawBreathBar @0x59D6F0; NapiNPClientMsg_0x00A @0x430104]
	{
		cs.breath_samples = 40;
		cs.breathtime = 25;
		cs.spawn_success_gate = true; // the folded round-over latch
		const BreathBarFacts j = breath_bar_facts(joiner);
		CHECK(j.samples == 40 && j.breath_time == 25 && j.round_over);
		ClientRuntime host_runtime("host-breath");
		host_runtime.state().breath_samples = 12;
		mission::MissionKernel kernel;
		kernel.world.script.wac_values.breathtime = 30;
		RoleView host;
		host.runtime = &host_runtime;
		host.kernel = &kernel;
		const BreathBarFacts h = breath_bar_facts(host);
		CHECK(h.samples == 12 && h.breath_time == 30 && !h.round_over);
		const BreathBarFacts none = breath_bar_facts(RoleView{});
		CHECK(none.samples == 0 && none.breath_time == 20 && !none.round_over);
		// A Serve Only host (a host context, no client runtime): g_RoundOver is the
		// authority's own round-end latch, which its status page's end-round leg tests
		// [orig: Server_ProcessRoundEnd @0x5164F0 (@0x5168E4); Server_DrawStatusScreen
		//  @0x50A2D0 (`cmp g_RoundOver, 0` @0x50B286)].
		NapiNPServerCtx serve_only_ctx;
		RoleView serve_only;
		serve_only.host = &serve_only_ctx;
		serve_only.kernel = &kernel;
		CHECK(!breath_bar_facts(serve_only).round_over);
		kernel.world.match.latch_round_over();
		CHECK(breath_bar_facts(serve_only).round_over);
		// A listen host keeps its own client's folded latch.
		CHECK(!breath_bar_facts(host).round_over);
	}

	// The HUD's role facts: the session lines' globals off the replica (a
	// joiner's 0x08 time-limit copy, the 0x16 team rows read signed, the
	// UNclamped round clock) and the kernel (the session bit, the local
	// team, the A&D latch) [orig: HUD_DrawGameTimerOverlay @0x59cc80;
	// HUD_DrawScoreOverlay @0x593e50; HUD_DrawTeamIdLine @0x59aa30].
	{
		ClientRuntime rt("hud-facts");
		replication::ClientState &st = rt.state();
		st.permanent_death = true;
		st.session_time_limit_minutes = 15;
		st.round_time_remaining_ticks = -1;
		st.scoreboard.alive_player_count = 3;
		st.scoreboard.spectator_count = 2;
		st.scoreboard.rows.resize(6);
		st.scoreboard.teams.resize(3);
		st.scoreboard.teams[1].score1 = static_cast<uint16_t>(0xFFFEu); // -2 through movsx
		st.scoreboard.teams[1].koth_hold = 4;
		st.scoreboard.teams[2].score1 = 90;
		mission::MissionKernel kernel;
		kernel.world.rules.mp_session = true;
		kernel.world.registry.configure_pool(0, 8);
		world::Entity seed;
		seed.kind = world::EntityKind::Organic;
		seed.team = 2;
		kernel.world.cached.local_player = kernel.world.registry.spawn(0, seed);
		kernel.local.attack_defend_role = 1;
		RoleView view;
		view.runtime = &rt;
		view.kernel = &kernel;
		view.joiner = true;
		const HudRoleFacts f = hud_role_facts(view);
		CHECK(f.session.in_session && f.session.round_time_remaining == -1 &&
				f.session.permanent_death && f.session.remaining_count == 3 &&
				f.session.row_count == 6 && f.session.spectator_count == 2);
		CHECK(f.session.time_limit_minutes == 15 && f.session.team_score1[0] == -2 &&
				f.session.team_koth[0] == 4 && f.session.team_score1[1] == 90);
		CHECK(f.session.team == 2 && f.session.attack_defend == 1 && f.session.zone_coverage == 0);
		// No inventory: every slot-bar category empty.
		CHECK(f.slot_bar[6].adm_index == -1 && f.slot_bar[0].count == 0);
		// The F9 / F10 menus resolve only when asked: each row's voice-macro
		// key (flags 0xC / 6) and, with no vmacros table, the key itself and
		// the literal titles [orig: HUD_DrawEmotesMenu @0x5bff00 — the key
		// @0x5bff9e, the fallbacks @0x5bffdf / @0x5bff57; HUD_DrawRadioTitleMenu
		// @0x5bfb90 — @0x5bfc2e, @0x5bfc6f / @0x5bfbe7].
		CHECK(!f.emotes_menu.shown && !f.radio_menu.shown);
		const HudRoleFacts menus = hud_role_facts(view, kHudVoiceMenuEmotes | kHudVoiceMenuRadio);
		CHECK(menus.emotes_menu.shown && menus.radio_menu.shown);
		CHECK(menus.emotes_menu.title == "!EMOTES_Title" && menus.radio_menu.title == "!Radio_Title");
		CHECK(menus.emotes_menu.texts[0] == "EMO_1" && menus.emotes_menu.texts[6] == "EMO_7");
		CHECK(menus.emotes_menu.texts[7].rfind("EMO_", 0) == 0 && menus.emotes_menu.texts[7] != "EMO_8");
		CHECK(menus.radio_menu.texts[0] == "RAD_1" && menus.radio_menu.texts[7] == "RAD_8");
		CHECK(menus.radio_menu.texts[8].rfind("RAD_", 0) == 0 && menus.radio_menu.texts[8] != "RAD_9");
		const HudRoleFacts none = hud_role_facts(RoleView{});
		CHECK(!none.session.in_session && none.session.round_time_remaining == -1);
		// The death screen's spectate arm: with a target and a sub-mode the
		// info is rebuilt for the target (its team byte, name and health
		// ratio, a negative Health reading back as full); a missing sub-mode
		// or a missing row keeps the local build [orig: HUD_RenderOverlays
		// @0x5a7bdb..0x5a7c25; HUD_BuildEntityInfo @0x4b87c7..0x4b87d3].
		CHECK(!f.session.spectating);
		kernel.world.tables.player.item_hp = 200;
		replication::ClientEntityState &target = st.upsert(0x0005);
		target.team = 1;
		target.team_known = true;
		target.display_name = "Bravo";
		target.health_word = 50;
		target.health_known = true;
		st.death_screen_active = true;
		st.spectate_target = 0x0005;
		st.death_screen_submode = 0;
		CHECK(!hud_role_facts(view).session.spectating); // the gate needs a sub-mode
		st.death_screen_submode = 1;
		const HudRoleFacts spec = hud_role_facts(view);
		CHECK(spec.session.spectating && spec.session.team == 1 &&
				spec.session.spectated_name == "Bravo" &&
				spec.session.spectated_health_fraction == 0.25f);
		target.health_word = static_cast<uint16_t>(-5);
		CHECK(hud_role_facts(view).session.spectated_health_fraction == 1.0f);
		st.spectate_target = 0x0009;
		const HudRoleFacts gone = hud_role_facts(view);
		CHECK(!gone.session.spectating && gone.session.team == 2);
		st.death_screen_active = false;
		st.spectate_target = 0xFFFF;
		st.death_screen_submode = 0;
		// The A&D side latch: the first TARGET-attrib def in pool 2 (then 1)
		// against the local team — another team's target attacks (2), our own
		// defends (1); any other game type clears it [orig: Game_LatchAttackDefendRole @0x524110].
		kernel.world.registry.configure_pool(1, 4);
		kernel.world.registry.configure_pool(2, 4);
		world::Entity item;
		item.has_item_def = true;
		item.item_attrib = 0x8000u;
		item.team = 2;
		kernel.world.registry.spawn(1, item);
		kernel.local.latch_attack_defend_role(0x10002u);
		CHECK(kernel.local.attack_defend_role == 1);
		world::Entity building = item;
		building.team = 1;
		kernel.world.registry.spawn(2, building); // pool 2 is walked first
		kernel.local.latch_attack_defend_role(0x10002u);
		CHECK(kernel.local.attack_defend_role == 2);
		kernel.local.latch_attack_defend_role(0x10000u);
		CHECK(kernel.local.attack_defend_role == 0);
	}

	// The DEATH screen facts: the sub-block-0 timers and the being-revived latch.
	cs.respawn_penalty_seconds = 4;
	cs.local_revive_seconds = 30;
	cs.spawn_hold_seconds = 0;
	cs.local_medic_reviving = false;
	{
		std::map<std::string, std::string> rows = { { "Overlays/STROVER_PENALTYTIMER", "Penalty" },
			{ "Overlays/STROVER_MEDICTIMER", "Medic" } };
		const hud::GameTextLookup gametext = [rows](const char *section, const char *key,
													   const char *fallback) {
			const auto it = rows.find(std::string(section) + "/" + key);
			return it != rows.end() ? it->second : std::string(fallback);
		};
		const world::DeployScreenStatus status =
				deploy_screen_status(joiner, world::SpawnZoneRegistry(), "Q", gametext);
		CHECK(status.penalty_seconds == 4 && status.revive_seconds == 30 &&
				status.line.kind == world::DeployStatusLine::Kind::Penalty);
		CHECK(status.respawn_text == "Penalty  <cFF4040>4");
		CHECK(status.statics.medic && !status.statics.psp_respawn);
		CHECK(status.statics_text.medic_timer == "Medic  <cFF4040>30");
		CHECK(status.statics_text.call_medic == "Press Q to call a medic");
		cs.local_medic_reviving = true; // a medic already reviving hides the pair
		CHECK(!deploy_screen_status(joiner, world::SpawnZoneRegistry(), "Q", gametext).statics.medic);
	}

	// The parity harness's readiness: false on the bare role and on a joiner
	// runtime that never entered the protocol's InMatch phase (the positive
	// legs are the harness's own witness).
	{
		RoleView bare;
		CHECK(!joiner_deploy_hold_ready(bare) && !joiner_in_match_ready(bare, false));
		mission::MissionKernel kernel;
		RoleView jv;
		jv.kernel = &kernel;
		jv.runtime = &runtime;
		jv.joiner = true;
		CHECK(!joiner_deploy_hold_ready(jv)); // no pick owed, no self handle
		CHECK(!joiner_in_match_ready(jv, false)); // no local player, not in match
		// The DEATH trigger reads the entity's dead bit, not the health word: a
		// session joiner with no bound self record and no frame yet (health 0)
		// is not dead. [orig: Render_ProcessMainSceneFrame @0x5CAB54]
		kernel.world.rules.mp_session = true;
		CHECK(runtime.state().local_health <= 0 && !death_menu_triggered(jv));
		kernel.world.rules.mp_session = false;
	}

	// The DEATH screen's zone rows: a joiner's feed over the zones its minimap
	// banks hold (transient, persistent, special in that order; a 0x40 slot
	// skipped), each zone's local BMS facts (team, control) and SpawnZoneList
	// index (-1 lists as '@' / STRWPNAME000), with the 0x6E wave group's
	// occupants named through the roster. A zone with no timer entry lists
	// whatever its entity's control word holds; the authority's own client
	// lists the same way by its body's team [orig: UI_UpdateDeathScreenContent
	// @0x553b53..0x553c4e; the entry gate @0x553bce..0x553bdd].
	{
		mission::MissionKernel kernel;
		kernel.world.registry.configure_pool(2, 8);
		world::Entity zone;
		zone.kind = world::EntityKind::Item;
		zone.has_item_def = true;
		zone.is_spawn_point = true;
		zone.alive = true;
		zone.team = 0; // the runtime's assigned team before a joiner binds
		zone.zone_control = 0x10000;
		const world::EntityHandle secured = kernel.world.registry.spawn(2, zone);
		zone.zone_control = 0x8000;
		const world::EntityHandle contested = kernel.world.registry.spawn(2, zone);
		zone.team = 1;
		zone.zone_control = 0x10000;
		const world::EntityHandle theirs = kernel.world.registry.spawn(2, zone);
		zone.team = 0;
		const world::EntityHandle unlisted = kernel.world.registry.spawn(2, zone);
		world::SpawnZoneRegistry reg;
		reg.entries = { secured, contested, theirs };
		auto bank = [](replication::ClientMinimapOverlaySlot &slot, world::EntityHandle h,
							uint8_t flags) {
			slot.active = true;
			slot.handle = h.packed;
			slot.flags = flags;
		};
		bank(cs.minimap.transient[0], unlisted, 0x00);
		bank(cs.minimap.persistent[0], contested, 0x10);
		bank(cs.minimap.persistent[1], secured, 0x10);
		bank(cs.minimap.persistent[2], theirs, 0x10);
		bank(cs.minimap.special[0], secured, 0x40); // a local-person/probe slot
		SpawnWaveGroup group;
		group.zone_handle = secured.packed;
		group.wave_countdown = 42;
		group.members = { 3, 4 };
		cs.spawn_waves.known = true;
		cs.spawn_waves.value.groups = { group };
		replication::ClientRosterSlot ace;
		ace.bound = true;
		ace.entity_slot = 3;
		ace.name = "Ace";
		cs.roster[0] = ace;
		replication::ClientEntityState member;
		member.handle = 4;
		member.display_name = "Bravo";
		cs.entities.push_back(member);
		RoleView jv;
		jv.kernel = &kernel;
		jv.runtime = &runtime;
		jv.joiner = true;
		std::vector<world::DeployZoneRow> rows;
		CHECK(deploy_zone_rows(jv, reg, rows));
		CHECK(rows.size() == 3); // the other team's zone is not a row, the 0x40 slot is skipped
		CHECK(rows.size() == 3 && rows[0].index == -1 && rows[0].letter == '@' &&
				rows[0].name_key == "STRWPNAME000" && rows[0].secured);
		CHECK(rows.size() == 3 && rows[1].index == 1 && rows[1].letter == 'B' && rows[1].secured &&
				rows[1].occupants.empty()); // no entry: its 0x8000 control is never read
		CHECK(rows.size() == 3 && rows[2].index == 0 && rows[2].letter == 'A' &&
				rows[2].name_key == "STRWPNAME001" && rows[2].secured);
		CHECK(rows.size() == 3 && rows[2].wave_countdown == 42 && rows[2].occupants.size() == 2);
		CHECK(rows.size() == 3 && rows[2].occupants.size() == 2 && rows[2].occupants[0].name == "Ace" &&
				rows[2].occupants[1].name == "Bravo" && !rows[2].occupants[0].self);
		// No bank slots, no rows (nothing is listed before the first 0x40 lands).
		const replication::ClientMinimapState banks = cs.minimap;
		cs.minimap = replication::ClientMinimapState();
		CHECK(deploy_zone_rows(jv, reg, rows) && rows.empty());
		cs.minimap = banks;
		// A zone's team is its entity's Team byte as this client holds it: on a
		// joiner the replica row once a team store lands (here a 0x50 moving
		// `theirs` to team 0), over the placed team [orig: the team compare
		// @0x553bb5..0x553bc0; NapiNPClientMsg_TeamAssign @0x4319ee].
		replication::ClientEntityState retarget;
		retarget.handle = theirs.packed;
		retarget.team = 0;
		retarget.team_known = true;
		cs.entities.push_back(retarget);
		CHECK(deploy_zone_rows(jv, reg, rows) && rows.size() == 4 && rows[3].index == 2);
		// The DEATH map reads the same byte [orig: MapOverlay_DrawView `v12[354]`
		// @0x5a5ae9].
		auto map_zone = [](const hud::DeathMapFacts &f,
								world::EntityHandle h) -> const hud::DeathMapZone * {
			for (const hud::DeathMapZone &z : f.zones)
				if (z.handle == h.packed) return &z;
			return nullptr;
		};
		hud::DeathMapFacts map_facts;
		CHECK(death_map_facts(jv, reg, map_facts));
		const hud::DeathMapZone *moved = map_zone(map_facts, theirs);
		CHECK(moved != nullptr && moved->team == 0 && moved->timer_ready);
		cs.entities.pop_back();
		CHECK(death_map_facts(jv, reg, map_facts));
		moved = map_zone(map_facts, theirs);
		CHECK(moved != nullptr && moved->team == 1); // the placed team again
		// The authority's view (the listen host, the single player): its own
		// client's banks, by its own body's team byte, the body marked as self
		// in a wave group. The zone's own entity team decides, never a 0x6F
		// entry's mode (the handler stores only +544/+545), and an entry whose
		// value sits below its limit keeps the zone off the list.
		// [orig: the team compare @0x553bb5..0x553bc6; NapiNPClientMsg_ZoneTimerValue
		//  @0x428d60; the entry gate @0x553bce..0x553bdd; the self compare @0x553d64]
		kernel.world.registry.get(contested)->team = 1;
		kernel.world.registry.get(secured)->team = 1;
		kernel.world.registry.configure_pool(0, 4);
		world::Entity body;
		body.kind = world::EntityKind::Organic;
		body.team = 1;
		kernel.world.cached.local_player = kernel.world.registry.spawn(0, body);
		replication::LoopbackChannel host_loop;
		ClientRuntime host_view(host_loop);
		auto zone_value = [&host_loop](world::EntityHandle h, uint8_t team, int32_t value,
								  int32_t limit) {
			std::vector<uint8_t> b = { static_cast<uint8_t>(h.packed),
				static_cast<uint8_t>(h.packed >> 8), team };
			for (const int32_t word : { value, limit })
				for (int shift = 0; shift < 32; shift += 8)
					b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(word) >> shift));
			b.insert(b.end(), { 0, 0, 0, 0 }); // rate, +544, +545
			host_loop.host_send(0x6F, b);
		};
		zone_value(contested, 0, 0x8000, 0x10000); // a mode the list never reads
		zone_value(secured, 0, 0x10000, 0x10000);
		host_view.Client_ProcessNetworkFrame();
		replication::ClientState &hs = host_view.state();
		hs.minimap = banks;
		SpawnWaveGroup ours;
		ours.zone_handle = theirs.packed;
		ours.members = { kernel.world.cached.local_player.packed };
		hs.spawn_waves.known = true;
		hs.spawn_waves.value.groups = { ours };
		RoleView hv;
		hv.kernel = &kernel;
		hv.runtime = &host_view;
		CHECK(deploy_list_team(hv) == 1 && deploy_list_team(jv) == runtime.assigned_team());
		CHECK(deploy_zone_rows(hv, reg, rows));
		CHECK(rows.size() == 3); // the team-0 transient zone is not a row
		CHECK(rows.size() == 3 && rows[0].index == 1 && !rows[0].secured);
		CHECK(rows.size() == 3 && rows[1].index == 0 && rows[1].secured);
		CHECK(rows.size() == 3 && rows[2].index == 2 && rows[2].letter == 'C' && rows[2].secured);
		CHECK(rows.size() == 3 && rows[2].occupants.size() == 1 && rows[2].occupants[0].self);
		// The authority's DEATH map: each zone's own entity team under 0x6F
		// entries whose mode names the other team, and the entry's ready gate
		// [orig: MapOverlay_DrawView `v12[354]` @0x5a5ae9; the entry gate
		//  @0x5a5b6e..0x5a5b7f].
		CHECK(death_map_facts(hv, reg, map_facts));
		const hud::DeathMapZone *taking = map_zone(map_facts, contested);
		const hud::DeathMapZone *held = map_zone(map_facts, secured);
		const hud::DeathMapZone *untouched = map_zone(map_facts, theirs);
		CHECK(taking != nullptr && taking->team == 1 && !taking->timer_ready);
		CHECK(held != nullptr && held->team == 1 && held->timer_ready);
		CHECK(untouched != nullptr && untouched->team == 1 && untouched->timer_ready);
		// Without a replica runtime there is nothing to walk.
		hv.runtime = nullptr;
		CHECK(!deploy_zone_rows(hv, reg, rows) && rows.empty());
		cs.spawn_waves = replication::ClientSpawnWaveStatus();
		cs.roster[0] = replication::ClientRosterSlot();
		cs.entities.clear();
		cs.minimap = replication::ClientMinimapState();
	}

	// The per-drawn-entity lighting feed's CONTAINED leg: an entity whose first
	// blink hit names a building skips the sun rays (quality 4) and takes the
	// interior lerp with the building's interior light group; only a PERSON
	// takes the building's ItemDef+0x218 daylight — the non-person wave never
	// loads the aux, so a contained vehicle or prop lerps with t = 0
	// [orig: Terrain_RenderSectorEntitiesBySide @0x5c7f83..0x5c7f93 (the aux),
	//  @0x5c7fb6..0x5c7fc3 / Terrain_RenderSectorEntities @0x5c7c05..0x5c7c14
	//  (the 0x80 submit flag); RenderBatchCtx_BeginFrame zeroes the stack base
	//  aux @0x5d89b6..0x5d89b8].
	{
		mission::MissionKernel kernel;
		world::World &w = kernel.world;
		w.registry.configure_pool(0, 8);
		w.registry.configure_pool(1, 8);
		w.registry.configure_pool(2, 8);
		world::Entity building;
		building.kind = world::EntityKind::Building;
		building.item_type = 5;
		building.has_item_def = true;
		building.light_transfer = 0.4f;
		building.bms_id = 40;
		building.spawn_origin = world::spawn_origin_pack(2, 0);
		const world::EntityHandle house = w.registry.spawn(2, building);
		const uint32_t hit = (static_cast<uint32_t>(house.slot()) << 20) | (3u << 12);
		world::Entity person;
		person.kind = world::EntityKind::Organic;
		person.item_type = 3;
		person.bms_id = 41;
		person.spawn_origin = world::spawn_origin_pack(0, 1);
		person.blink_hits[0] = hit;
		w.registry.spawn(0, person);
		world::Entity crate;
		crate.kind = world::EntityKind::Item;
		crate.item_type = 4;
		crate.bms_id = 42;
		crate.spawn_origin = world::spawn_origin_pack(2, 2);
		crate.blink_hits[0] = hit;
		w.registry.spawn(2, crate);
		world::Entity outdoor = crate;
		outdoor.bms_id = 43;
		outdoor.blink_hits[0] = 0;
		w.registry.spawn(2, outdoor);
		RoleView view;
		view.kernel = &kernel;
		EntityLightingFeed feed;
		const int32_t step[3] = { 0, 0, 65536 * 200 };
		std::vector<EntityLightingChange> changes;
		feed.collect(view, step, {}, {}, 0, changes);
		const EntityLightingChange *person_change = nullptr;
		const EntityLightingChange *crate_change = nullptr;
		bool outdoor_emitted = false;
		for (const EntityLightingChange &c : changes) {
			if (!c.wire && c.bms_id == 41) person_change = &c;
			if (!c.wire && c.bms_id == 42) crate_change = &c;
			if (!c.wire && c.bms_id == 43) outdoor_emitted = true;
		}
		CHECK(person_change != nullptr && person_change->lighting.interior &&
				person_change->lighting.quality == 4 &&
				person_change->lighting.light_transfer == 0.4f &&
				person_change->lighting.interior_bms == 40 &&
				person_change->lighting.interior_section == 3);
		CHECK(crate_change != nullptr && crate_change->lighting.interior &&
				crate_change->lighting.quality == 4 &&
				crate_change->lighting.light_transfer == 0.0f &&
				crate_change->lighting.interior_bms == 40 &&
				crate_change->lighting.interior_section == 3);
		CHECK(!outdoor_emitted); // an outdoor static stays the default context
		// A steady frame emits nothing.
		feed.collect(view, step, {}, {}, 0, changes);
		CHECK(changes.empty());
		// Walking out restores the outdoor context.
		w.registry.get(world::EntityHandle::make(0, 0))->blink_hits[0] = 0;
		feed.collect(view, step, {}, {}, 0, changes);
		CHECK(changes.size() == 1 && changes[0].bms_id == 41 && !changes[0].lighting.interior &&
				changes[0].lighting.interior_bms == 0 && changes[0].lighting.light_transfer == 0.0f);
	}

	// The joiner's wire walk takes a decoded row's native twin by the replica
	// rule (D-NET-676): a header-only world's owned row lends the row its
	// contained context, and a same-type newcomer spawned at the handle after
	// that lifetime lends none (the row runs its own blink walk at its decoded
	// position). Retail keeps one entity per slot, the wire handle naming the
	// slot's entity [orig: NapiNPClientMsg_EntityDeath @0x42EBC6, the handle's
	// slot address].
	{
		mission::MissionKernel kernel;
		world::World &w = kernel.world;
		w.registry.configure_pool(1, 4);
		w.registry.configure_pool(2, 4);
		world::Entity building;
		building.kind = world::EntityKind::Building;
		building.item_type = 5;
		building.has_item_def = true;
		building.bms_id = 40;
		building.spawn_origin = world::spawn_origin_pack(2, 0);
		const world::EntityHandle house = w.registry.spawn(2, building);
		const uint32_t hit = (static_cast<uint32_t>(house.slot()) << 20) | (3u << 12);
		ClientRuntime decoded_runtime("LightReuse");
		replication::ClientEntityState decoded;
		decoded.handle = world::EntityHandle::make(1, 0).packed;
		decoded.type_id = 1296;
		decoded.spawn_revision = 1;
		decoded.cls = EntityClass::Vehicle;
		decoded_runtime.state().upsert(decoded.handle) = decoded;
		replication::ClientWorldMaterializer materializer;
		materializer.sync(decoded_runtime.state(), w);
		const world::EntityHandle h{decoded.handle};
		world::Entity *mine = w.registry.get(h);
		CHECK(mine != nullptr && materializer.owned(w, h) == mine);
		if (mine != nullptr) mine->blink_hits[0] = hit;
		RoleView jv;
		jv.kernel = &kernel;
		jv.runtime = &decoded_runtime;
		jv.joiner = true;
		jv.materializer = &materializer;
		EntityLightingFeed feed;
		const int32_t step[3] = { 0, 0, 65536 * 200 };
		std::vector<EntityLightingChange> changes;
		feed.collect(jv, step, {}, {}, 0, changes);
		CHECK(changes.size() == 1 && changes[0].wire && changes[0].handle == decoded.handle &&
				changes[0].lighting.interior && changes[0].lighting.interior_bms == 40);
		w.registry.despawn(h);
		world::Entity newcomer;
		newcomer.kind = world::EntityKind::Item;
		newcomer.item_id = 1296;
		newcomer.alive = true;
		newcomer.blink_hits[0] = hit;
		CHECK(w.registry.spawn_at(h, newcomer) == h && materializer.owned(w, h) == nullptr);
		feed.collect(jv, step, {}, {}, 0, changes);
		CHECK(changes.size() == 1 && changes[0].wire && changes[0].handle == decoded.handle &&
				!changes[0].lighting.interior && changes[0].lighting.interior_bms == 0);
	}

	// The present-effect pose index over a joiner's decoded rows: a lookup
	// resolves through the wire row (mission units, the yaw off the heading),
	// a miss is remembered for the epoch, and a newly applied replica frame
	// rebuilds the cache.
	{
		mission::MissionKernel kernel;
		RoleView jv;
		jv.kernel = &kernel;
		jv.runtime = &runtime;
		jv.joiner = true;
		replication::ClientEntityState row;
		row.handle = 5;
		row.type_id = 7;
		row.x = 3 * 65536;
		row.y = 4 * 65536;
		row.z = 5 * 65536;
		row.heading_bam = 0x4000;
		cs.entities.push_back(row);
		EffectPoseIndex index;
		const EffectPose *pose = index.for_handle(jv, 5);
		CHECK(pose != nullptr && pose->x == 3.0f && pose->y == 4.0f && pose->z == 5.0f);
		CHECK(pose != nullptr && pose->pitch_deg == 0.0f && pose->roll_deg == 0.0f &&
				pose->yaw_deg == static_cast<float>(world::mission_yaw_deg_from_bam_heading(0x4000)));
		CHECK(index.for_handle(jv, 6) == nullptr);
		// A joiner resolves wire identity only.
		world::BmsHandleIndex bms;
		CHECK(index.for_ssn(jv, 9) == nullptr && index.for_bms_id(jv, 9, bms) == nullptr &&
				index.for_origin(jv, 1, 2) == nullptr);
		// The epoch: the same frame keeps the miss, a new applied frame retries.
		cs.entities.push_back(replication::ClientEntityState());
		cs.entities.back().handle = 6;
		cs.entities.back().type_id = 7;
		CHECK(index.for_handle(jv, 6) == nullptr);
		cs.frames_applied += 1;
		CHECK(index.for_handle(jv, 6) != nullptr);
		// No runtime: nothing resolves and the cache empties.
		RoleView cold;
		cold.kernel = &kernel;
		CHECK(index.for_handle(cold, 5) == nullptr);
	}

	inset_target_name_cases();

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("role_feeds_test OK\n");
	return 0;
}
