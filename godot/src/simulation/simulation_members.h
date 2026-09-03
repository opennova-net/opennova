// Class-body fragment of godot::Simulation — #include'd exactly once by
// simulation/simulation.h INSIDE the class. Not a standalone header: no
// include guard on purpose, so a second inclusion fails loudly with
// redefinition errors. It OPENS its own `private:` and the main header
// re-opens `public:` right after the include. New private state belongs
// here, not in the main header.
//
// The private state block. The P7 net-runtime declaration order
// (host_state_ -> aliases -> runtime_) is a load-bearing contract — never
// reorder it.
private:
	// The engine's ONE mission boot + state + no-net tick (ADR 0042 d3):
	// world, systems (AI/WAC/BMS events/collision/occlusion), the sim asset
	// caches, terrain field store, seat specs, the weapon/ammo tables, and
	// the local-player frame state all live inside. Recreated per load
	// (reset_world) and NEVER null after construction; this binding converts
	// Godot Refs into the kernel's sources and orders device work around it.
	std::unique_ptr<opennova::mission::MissionKernel> kernel_;
	// The Weather node bound through set_weather_render_owner; released
	// whenever the kernel (and the WeatherState it owns) is replaced or dies.
	ObjectID weather_owner_id_;
	void _release_weather_owner();
	opennova::renderer::PrecipitationDrawState precipitation_draw_;
	// The compiled streak frame, reused across frames (its vertex capacity
	// survives clear()).
	opennova::renderer::PrecipitationDrawFrame precipitation_frame_;
	void apply_collision_to_ai();
	// The shell input the sweep reads (its retained items.def rows feed the
	// engine resolve). RefCounted, so retaining it also keeps its object/ADM
	// caches alive for a later RoundSim or F3 query.
	Ref<ItemDatabase> collision_item_db_;
	// The sim's asset source (ADR 0028): the mounted root pinned for its
	// index lifetime; the kernel's parse-once model cache reads through it
	// (set_asset_index). Render caches stay render-only.
	Ref<ResourceRoot> asset_root_;
	// Portable mission lifecycle and cadence. During one advance call the Godot
	// adapter holds a single typed tick sink so presentation consumes every
	// catch-up tick before the next simulation tick.
	opennova::inmatch::Session session_;
	Callable session_tick_sink_;
	int64_t frame_net_us_ = 0;
	int64_t frame_sim_us_ = 0;
	int64_t frame_sink_us_ = 0;
	// The dev tools' frame-stats board (ADR 0039): fold_frame_stats() lands the
	// kernel's tick profile (every touched SIM_* slot, ADR 0043 d5) plus the
	// shell-side step/sink spans on it natively at the end of a session frame.
	// Ordinary play leaves runtime_profiling_enabled_ false, so the profile is
	// inactive and no producer reads a clock.
	Ref<FrameStats> frame_stats_;
	void fold_frame_stats(const opennova::inmatch::FrameOutcome &p_outcome);
	opennova::inmatch::Role configured_session_role() const;
	bool begin_session_load();
	void complete_session_load();
	void fail_session_load(const char *p_message);
	bool advance_world_tick();
	void restore_world_baseline();
	opennova::inmatch::TickOutcome advance_mission_tick(
			const opennova::inmatch::TickInput &p_input) override;
	bool reset_mission_to_baseline(opennova::inmatch::SessionError &r_error) override;
	void close_mission() override;
	// Wire-side collision initialization for decoded rows, sharing the exact
	// typed engine result and by-graphic caches used by local entities.
	std::unordered_map<uint16_t, opennova::world::ResolvedCollisionShape>
			wire_collision_shape_by_type_;
	// Per-frame entity render-gate verdicts (bms_id -> culled), rebuilt by
	// run_occlusion_frame; consumed via get_render_culled_bms_ids.
	std::vector<int32_t> occlusion_culled_bms_;
	// The same render gate over the decoded rows WirePresentPass draws (no
	// placed identity): culled wire handles this frame, the applied baseline,
	// and each row's persistent three-ray latch.
	std::vector<int32_t> occlusion_culled_wire_;
	std::vector<int32_t> occl_apply_culled_wire_last_;
	std::unordered_map<uint16_t, uint8_t> wire_occlusion_latch_;
	PackedInt32Array get_wire_render_culled_changes();
	// Reused probe scratch (cleared per frame, capacity retained).
	std::vector<opennova::world::EntityHandle> occlusion_probe_handles_;
	// Delta baselines for the render-occlusion apply path: what the shell last
	// applied, so steady frames emit nothing. Cleared on world reset and via
	// reset_occlusion_apply_baseline() (the occlusion A/B seam re-arms a full
	// re-emit).
	std::unordered_map<uint32_t, int64_t> occl_apply_building_last_;
	std::vector<int32_t> occl_apply_culled_last_;
	// Per-entity sun-visibility quality last emitted to the shell, split by
	// identity domain. Wire handle zero and a placed BMS id zero are both valid
	// sentinels in their own schemas, so they must never share one integer map.
	// Unlisted entities are quality 4 (factor 1.0), the node default.
	std::unordered_map<int32_t, uint8_t> sun_quality_last_by_bms_;
	std::unordered_map<uint16_t, uint8_t> sun_quality_last_by_wire_;
	int64_t sun_quality_present_layout_revision_ = -1;
	uint8_t local_sun_quality_ = 4;
	// Mutable retail Lighting_SetInteriorLightGroup state left by the marched
	// iris samples. Outdoor samples clear it, indoor samples with interior data
	// replace it, and indoor-no-data samples intentionally retain the previous
	// pair. OpenNova's actual draw selection carries groups explicitly, but this
	// state preserves the witnessed sequence instead of erasing the side effect.
	opennova::world::EntityHandle iris_interior_group_entity_;
	int32_t iris_interior_group_section_ = 0;
	// The pool-2 building a packed blink hit names, as a bms_id (0 = none).
	int blink_hit_owner_bms_id(uint32_t p_hit) const;
	// The installed script program. Held as a Ref so it survives reset_world();
	// each (re)load re-applies it onto the fresh kernel WacSystem when the
	// kernel's own layered load installed none.
	Ref<WacProgram> wac_program_;
	// Resource-install invariant only. Public lifecycle is
	// session_.state(); this prevents partially constructed worlds from
	// serving data while Loading/Failed transitions are in flight.
	bool world_installed_ = false;

	// --- in-match net runtime (P7, ADR 0009/0011): the SP / LAN host in-process listen server. OFF
	// by default, so an explicit non-network fixture uses the direct AI-pool present. When
	// enabled (before load), bringup_host_runtime stands up the npruntime ctx_ + host_loop_ + runtime_
	// (declared in the P7 block below) and the present pass reads the client-decoded ClientState
	// (ADR 0011 Decision 1) instead of the AI pool. Server_TickUpdate owns the per-frame tick.
	bool listen_server_ = false;
	uint64_t last_sim_tick_us_ = 0;
	uint64_t last_net_tick_us_ = 0;
	// The two halves of run_occlusion_frame: the portal/section build
	// (OcclusionWorld::build_frame) and the per-entity render-gate probe loop.
	uint64_t last_occlusion_build_us_ = 0;
	uint64_t last_occlusion_probe_us_ = 0;
	// One opt-in gate for every native runtime timer/counter. Retail play keeps
	// this false; F3 Stats and the manual probe share the public ownership seam.
	bool runtime_profiling_enabled_ = false;
	mutable uint64_t last_present_snapshot_us_ = 0;
	mutable int last_present_entity_count_ = 0;
	// Exact identity/order of the most recently returned PF_* buffer. Dynamic
	// values (pose, animation, visibility) deliberately do not participate:
	// GDScript row plans may keep their offsets while reading fresh values.
	struct PresentRowIdentity {
		int32_t wire_handle = 0;
		int32_t type_id = 0;
		int32_t bms_id = 0;
		int32_t kind = -1;
		int32_t index = -1;

		bool operator==(const PresentRowIdentity &p_other) const {
			return wire_handle == p_other.wire_handle &&
			       type_id == p_other.type_id &&
			       bms_id == p_other.bms_id &&
			       kind == p_other.kind &&
			       index == p_other.index;
		}
	};
	mutable std::vector<PresentRowIdentity> present_layout_;
	mutable uint64_t present_layout_revision_ = 0;

	// FollowOwner consumes the same wire-decoded pose as the present pass, but it
	// does so once per fixed tick inside a catch-up batch. Keep the identity index
	// native and generation-bound so GDScript does not rebuild the full PF_* buffer
	// plus four Dictionary indexes for every catch-up tick.
	struct PresentEffectPose {
		Vector3 position;
		Vector3 rotation_deg;
	};
	mutable bool present_effect_pose_cache_valid_ = false;
	mutable uint32_t present_effect_pose_cache_logic_tick_ = 0;
	mutable uint32_t present_effect_pose_cache_client_frame_ = 0;
	mutable const opennova::np::ClientRuntime *present_effect_pose_cache_runtime_ = nullptr;
	mutable std::unordered_map<uint16_t, PresentEffectPose> present_effect_poses_by_handle_;
	mutable std::unordered_map<int, uint16_t> present_effect_handles_by_bms_id_;
	mutable std::unordered_map<int, opennova::world::EntityHandle> bms_handle_index_;
	mutable uint64_t bms_handle_index_serial_ = 0;
	mutable const opennova::world::World *bms_handle_index_world_ = nullptr;
	mutable std::unordered_map<int, uint16_t> present_effect_handles_by_ssn_;
	mutable std::unordered_map<uint64_t, uint16_t> present_effect_handles_by_origin_;
	// A missing owner is also stable for one decoded-client epoch. Remember
	// misses so stale effect attachments cannot turn lazy lookup into one full
	// entity scan per fixed tick/query. Positive caches remain authoritative
	// when another alias materializes the same row.
	mutable std::unordered_set<uint16_t> present_effect_missing_handles_;
	mutable std::unordered_set<int> present_effect_missing_bms_ids_;
	mutable std::unordered_set<int> present_effect_missing_ssns_;
	mutable std::unordered_set<uint64_t> present_effect_missing_origins_;
	void invalidate_present_effect_pose_cache() const;
	void ensure_present_effect_pose_cache() const;
	bool cache_present_effect_pose(
			const opennova::netsim::ClientEntityState &p_entity_state) const;
	// The host's pool row (D-NET-140: the listen host never presents from ClientState).
	bool cache_present_effect_pose(const opennova::world::Entity &p_entity) const;
	PackedVector3Array cached_present_effect_state_for_handle(uint16_t p_handle) const;
	PackedVector3Array present_effect_state_for_handle(uint16_t p_handle) const;

	// --- co-op LAN host: a real UDP socket (UdpPump) over the npruntime runtime. enable_host_listen
	// binds the socket (it implies the listen server); host_pump drives the owner loop, and
	// dispatch_event/admit_peer admit joiners + stream the named dcb-bearing 0x0C. host_session_config_
	// holds the GDScript-facing session options (the Dictionary getter + the §5.1 reactive-reply config
	// consumed by create_session). Sockets live here, the protocol/crypto in libs (ADR 0010).
	bool host_listen_ = false;
	Ref<UdpPump> pump_;
	String capture_pcap_path_;
	opennova::np::GameConfig host_session_config_; // the ONE consolidated server-state config (ADR 0013)
	uint16_t host_bind_port_ = 64220;                      // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's own player and folds
	// host_loop_ into runtime_; a DEDICATED host (false) runs the listen server with NO local player and
	// lets host_session_pump discard the loopback (step 5). Mirrors HostConfig.serve_and_play /
	// start_host_session's gating (engine: runtime/session/listen_host.cpp).
	bool host_serve_and_play_ = true;
	uint32_t host_max_players_ = 16; // the lobby-advertised player cap; clamped host-side to the witnessed 1..65 [orig +0xC0]
	// The mission's raw terrain-tile (.til) file bytes, fed from the Godot shell (which owns the resource
	// root) before load; copied into ctx_.terrain_til_data at bring-up so the initial-state burst streams
	// the S2C 0x45 terrain-tile load (phase 5). Empty => 0x45 faithfully skipped. [§5.37]
	std::vector<uint8_t> terrain_til_data_;
	// MissionText briefing values parsed from the mounted <mission>.bin (or the
	// medmssn.bin fallback) before load. These remain the RTXT's original cp1252
	// bytes so the S2C 0x7E payload is byte-exact for localized text.
	bool mission_text_loaded_ = false;
	std::string mission_briefing3_;
	std::string mission_briefing2_;
	// Numeric suffix -> original cp1252 MissionText [Locations] value.
	// bringup_host_runtime resolves these against type-2044 BMS markers in
	// spawn order for the S2C 0x0F deploy-map label block.
	std::unordered_map<int32_t, std::string> mission_location_texts_;
	// Numeric suffix -> [PeopleNames] STRNAME%03i value; promote resolves an
	// entity's authored display name (D-HUD-20) from its BMS name_index here
	// (engine: runtime/mission/promote.cpp).
	std::unordered_map<int32_t, std::string> mission_people_names_;
	// Build the PF_* present buffer from the client-decoded ClientState (runtime_->state()):
	// the joiner's view of the host's stream.
	PackedFloat32Array present_snapshot_from_client_replicas() const;
	// Build the PF_* present buffer from the host's own pools: retail's listen
	// host/SP local client reads process memory and its loopback 0x0A carries no
	// entity records (engine: runtime/replication/connection_fan.cpp).
	PackedFloat32Array present_snapshot_from_world() const;
	// The decoded fold's dead->alive respawn revision, mirrored per pool row so
	// WirePresentPass sees the same PF_RESPAWN_REVISION edges on every role.
	struct PoolPresentLifecycle {
		uint64_t registry_spawn_id = 0;
		uint32_t respawn_revision = 0;
		bool dead_known = false;
		bool dead = false;
	};
	mutable std::unordered_map<uint16_t, PoolPresentLifecycle> pool_present_lifecycle_;

	// --- co-op LAN joiner: a pure non-authority np::ClientRuntime (Joiner role, built in enable_join /
	// the boot's role hook; runtime_ is in the P7 block below). joiner_pump drives the connect legs +
	// the per-frame S2C->ClientState fold + the C2S 0x0C uplink over a dialed UdpPump; its own player L
	// runs run_logic_tick(false), remotes render wire-direct. (engine: runtime/session/joiner_connection.h)
	bool joiner_ = false;
	// The F3 Weapon window's held-trigger latch (OR'd into per-tick weapon input).
	bool debug_weapon_fire_held_ = false;
	bool joiner_net_diagnostics_ = false;
	opennova::np::JoinRole join_role_ = opennova::np::JoinRole::Player;
	std::string join_spectator_password_;
	// The joiner's per-frame world<->net bridge (S10a, ADR 0028): frame sequence, latches
	// (started/spawned/redeploy/tripwire), wire-header materializer, and per-replica resolver
	// state live in engine/runtime/session; this binding supplies the shell legs as PumpHooks.
	opennova::np::JoinerWorldBridge joiner_bridge_;
	// The shell-asset leg of the bridge's materialize phase: rebuild the
	// collision/occlusion/trait/seat caches for the changed streamed rows.
	// The joiner's streamed pool-1..3 rows with a placed identity, as the
	// entity dictionaries MissionObjectPlacer.place_entities consumes (kind,
	// index, bms_id, item_id, position, rotation_deg, team, group, ai_flags).
	// Empty on a host or before the world stream's static pools completed.
	Array get_streamed_placement_records() const;
	// Placed identities retired since the last take (the slot vanished or was
	// re-typed): the shell hides their placed representation.
	PackedInt32Array take_retired_placement_ids();
	void on_replica_world_changed(
			const opennova::netsim::ClientWorldSyncResult &p_sync);
	// The env-gated ~1 Hz tripwire print (the bridge owns the sampled state).
	void print_joiner_net_diagnostic_sample();
	// Input-latch resets + adm resolution at L's spawn/redeploy edges.
	void on_joiner_local_player_spawned(int32_t p_look_heading_bam);
	void on_joiner_local_player_redeployed(int32_t p_look_heading_bam);
	// Retail authenticates with one packed Avatars.def selection for each side.
	// GameWorld resolves the active profile before enable_join; retain it here
	// because a direct-loaded join rebuilds ClientRuntime at load.
	opennova::np::CharacterJoinVars join_character_vars_{};
	bool join_character_vars_set_ = false;
	// The listen host's own type-2 connection consumes the same profile shape,
	// installed before its authoritative player spawn.
	opennova::np::CharacterJoinVars local_character_vars_{};
	bool local_character_vars_set_ = false;
	// Explicit resource-corpus identity for the retail anti-cheat 0x30/0x31
	// sources. Empty keeps safe silence. Retained across direct-load runtime
	// rebuilds just like the character and charattr profile data.
	std::string join_integrity_profile_id_;
	// The install root the joiner's JOIN VERSIONCRCSTRING checksum reads its
	// loose expansion/<name>/version.txt from (D-NET-166). Empty keeps the
	// golden "0". Retained across direct-load runtime rebuilds.
	std::string join_expansion_version_root_;
	// The game-session APPID join token (decoded .joi CK) a NovaWorld host
	// validates (reject code 9). "0" is the LAN default. Retained across rebuilds.
	std::string app_id_ = "0";
	// The CD identity cookie (packed PUB* blob) for the 0x00 JOIN (codes 23/24/25).
	// Empty for LAN. Retained across direct-load runtime rebuilds.
	std::vector<uint8_t> join_cd_cookie_;
	// The live environment owner consumes each decoded phase-2 edge once. The
	// ClientState revision is monotonic for one ClientRuntime; fresh runtimes
	// reset this cursor with their other receive-side cursors.
	// The 0x81 score-feedback edge cursor (ClientScoreFeedback::updates).
	uint32_t score_feedback_updates_seen_ = 0;
	// Last authoritative S2C 0x5A grant installed into the local slot pool.
	// Requests may rebuild optimistically, but only a newer host grant becomes
	// the durable spawn/respawn kit.
	uint64_t joiner_applied_loadout_revision_ = 0;
	// The shell applies the profile kit/class right after runtime setup — on a
	// joiner that is BEFORE L exists (L spawns on the name-match). The class
	// latch lives on the world-typed loadout aggregate
	// (kernel_->loadout.pending_player_class); L's spawn block stamps it with
	// the equipped weapon, the same Player_InitPlayer-time arm the host's own
	// spawn performs. (engine: runtime/session/host_session.h)
	// Send one framed datagram to the dialed host (the joiner's send_datagram).
	void ship_to_host(const std::vector<uint8_t> &dg);

	// The local-player frame input, look accumulators, stance latch and mouse
	// settings all live on the kernel (kernel_->input / look() /
	// request_stance / look_settings); this binding only converts device
	// input and routes the joiner's wire edges.
	// Retail's held-weapon draw gate, local-player branch — the weapon model is shown
	// iff the soldier may fire it. (engine: runtime/session/client_replica_present.h)
	bool local_held_weapon_visible(const opennova::world::Entity &p_entity) const;
	// The M-cycle map mode + the two radar zooms — the engine-side state
	// machine carries the retail lifecycle (cycle, zoom routing, spawn
	// reset, the dead-player clear); this class only routes requests and
	// tick edges into it (witness at hud::HudMapControl).
	opennova::hud::HudMapControl hud_map_control_;

	// --- the local player's equipped-weapon action FSM (net-re §5.62) ------------------
	// The 12-state action queue on the equipped slot, pumped once per logic tick after the
	// world advances (engine: runtime/mission/mission_kernel.cpp). The host
	// feeds the baked def via set_local_player_weapon and per-frame trigger state via
	// set_local_player_weapon_input. Presentation outputs accumulate as ordered
	// per-tick records because several logic ticks can run per render frame; the
	// snapshot's monotonic serials remain diagnostics/rebuild state.
	// The whole equipped-weapon state (def/slot/rings/serials/UseGun/
	// PowerThrow/presentation events) lives on the kernel (kernel_->weapon,
	// with the retained weapon.def rows and the clip index beside it); this
	// binding marshals installs, inputs, drains, and the two wire request
	// records.
	using LocalUseGunSwitch = opennova::world::LocalUseGunSwitch;
	// The UseGun borrow + slot selection live in world/player_weapon.h; these
	// inline wrappers keep the family's call sites unchanged.
	opennova::world::WeaponSlotState *active_local_weapon_slot() {
		return opennova::world::active_local_weapon_slot(
				kernel_->world, kernel_->weapon);
	}
	const opennova::world::WeaponSlotState *active_local_weapon_slot() const {
		return opennova::world::active_local_weapon_slot(
				kernel_->world, kernel_->weapon);
	}
	bool local_usegun_switch_is_instant() const {
		return opennova::world::local_usegun_switch_is_instant(
				kernel_->world, kernel_->weapon);
	}
	void sync_local_usegun_weapon_transition() {
		opennova::world::sync_local_usegun_weapon_transition(
				kernel_->world, kernel_->weapon);
	}
	void commit_local_usegun_weapon_switch() {
		opennova::world::commit_local_usegun_weapon_switch(
				kernel_->world, kernel_->weapon);
	}
	void queue_local_usegun_weapon_switch(bool p_same_category) {
		opennova::world::queue_local_usegun_weapon_switch(
				kernel_->world, kernel_->weapon, p_same_category);
	}
	void install_local_player_weapon(const Ref<WeaponDef> &p_def,
	                                 const Dictionary &p_clip_seconds,
	                                 bool p_preserve_slot_state,
	                                 bool p_allow_same_weapon_rebake);
	// The Dictionary/def-row feeders both build the world install payload.
	static opennova::world::WeaponInstallData install_data_from_def(
			const DefWeaponDef &p_def, const Dictionary &p_clip_seconds);
	void tick_local_player_weapon();

	// --- the local player's weapon slot pool + spawn kit + map rules -------------------
	// The slot pool, spawn kit, availability table and pre-spawn class latch
	// all live on the kernel (kernel_->inventory / kernel_->loadout, with the
	// world-typed rules in world/player_loadout.h); this binding converts
	// dictionaries and routes the joiner wire submissions.
	// The ACTIVE player weapon profile record — retail's g_charSelClass slot: two
	// side blocks (blue/red), each carrying the class byte that selects both the wire
	// class and one of five 2048-byte kit pages, plus the single-player page.
	// (engine: base/gameprofile/required_resources.c)
	// Seeded from the shipped defaults so it is NEVER empty even when weapon.sav is
	// absent (engine: formats/playersav/weapon_sav.cpp).
	opennova::playersav::Record weapon_profile_ =
			opennova::playersav::make_defaults().slots[0];
	bool weapon_profile_loaded_ = false;
	// The switch commit/outcome/gates moved to world/player_weapon.h (S7a);
	// wrappers keep the family's call sites unchanged.
	void commit_pending_weapon_switch() {
		opennova::world::commit_pending_weapon_switch(
				kernel_->world, kernel_->weapon,
				kernel_->inventory_valid ? &kernel_->inventory : nullptr);
	}
	void handle_weapon_switch_outcome(
			const opennova::world::WeaponSwitchOutcome &p_out) {
		opennova::world::handle_weapon_switch_outcome(
				kernel_->world, kernel_->weapon,
				kernel_->inventory_valid ? &kernel_->inventory : nullptr, p_out);
	}
	opennova::world::WeaponSwitchGates local_weapon_switch_gates() const {
		return opennova::world::local_weapon_switch_gates(
				kernel_->world, kernel_->weapon,
				kernel_->inventory_valid ? &kernel_->inventory : nullptr);
	}
	// Player_InitPlayer's weapon leg (engine: runtime/session/host_session.h); shared by table load,
	// respawn, and the ACCEPT apply (which passes the freshly stored kit).
	void rebuild_local_player_loadout(bool p_select_spawn_default);
	// Copies the assigned side's profile page into the resident kit buffer
	// (kernel_->loadout.spawn_kit) in a live session — retail's single restrictionData (engine: runtime/session/loadout_submit.h). False when
	// not in a session, before the catalog exists, or when the page resolves empty.
	bool seed_session_kit_from_profile();
	// Re-copies the page when the SIDE the team selector names stops matching the side
	// the resident buffer came from — the S2C 0x04 latch arriving after the catalog, or
	// a later S2C 0x50 reassignment (engine: runtime/session/joiner_connection.cpp).
	bool reseed_session_kit_on_side_change();
	// Which side the resident kit buffer was last copied from (-1 = never seeded).
	int weapon_profile_seeded_side_ = -1;
	// Push the submission content (class + ADM rows + equipped combo) into the joiner
	// runtime's 0x2F loadout-submission seam. No-op for hosts/SP.
	void push_joiner_loadout_kit();
	// Re-entry latch: rebuild_local_player_loadout re-pushes the seam once the live
	// equipped combo has settled, so the push must never drive a rebuild back.
	bool pushing_joiner_loadout_kit_ = false;
	bool apply_local_player_loadout_impl(
			const TypedArray<WeaponKitEntry> &p_kit, int p_player_class,
			bool p_submit_joiner_request);
	// The typed-rows core of the apply (the record overload converts, the 0x5A
	// grant path feeds np::kit_from_authoritative_grant's rows directly).
	bool apply_local_player_loadout_rows(
			std::vector<opennova::world::WeaponKitEntry> p_kit,
			int p_player_class, bool p_submit_joiner_request);
	// Fold the latest authoritative S2C 0x5A grant into the local slot pool at
	// the same recv-before-actions boundary as the retail handler.
	void apply_joiner_authoritative_loadout();
	// The deploy/spawn-zone registry (letters/pick-index space), built lazily per
	// load (engine: runtime/hud/hud_lfp_panel.h).
	const opennova::world::SpawnZoneRegistry &deploy_zone_registry();
	// The DEATH screen's zone rows over the registry: the record feed and
	// the compiled list builder both read this one walk.
	std::vector<opennova::world::DeployZoneRow> deploy_zone_rows();
	opennova::world::SpawnZoneRegistry deploy_zone_registry_;
	bool deploy_zone_registry_built_ = false;
	// --- the local player's view state (ADS ease + 3P anchor chase) --------------------
	// The view state and its trackers live on the kernel (kernel_->view /
	// kernel_->view_tracker; the witnessed gates in world/local_player_view.h);
	// this class converts frames and routes wire requests
	// (simulation_player_view.cpp).
	opennova::world::LocalViewSessionInputs local_view_session_inputs() const;
	void reset_local_player_view_effects();
	void refresh_local_player_view_effects();
	void tick_local_player_view();
	// The dead-player map-mode clear, run once per advanced tick (witness at
	// hud::HudMapControl::on_local_player_dead).
	void tick_hud_map_death_gate();

	// --- P7: the in-match runtime as a THIN ADAPTER over engine/runtime/session ----------------
	// One in-match runtime funnels every live path: the host/SP game is the §5.0 mode-3
	// listen server (NapiNPServerCtx ctx_ + its own loopback client over host_loop_, driven by
	// the npruntime owner loop = Server_TickUpdate + tick_connections + handle_server_datagram);
	// the joiner is a non-authority np::ClientRuntime. The Godot net bindings stay PURE socket
	// pumps — all protocol/crypto/framing lives in libs (ADR 0009-0012, .agents/network.md).
	// host_loop_ MUST be declared before runtime_: the HostClient ClientRuntime holds a
	// non-owning reference into host_loop_, so the loopback has to outlive (and not move under)
	// the runtime.
	// The SP/LAN listen session's net state (inmatch::ListenHostState): the
	// loopback + the np host owner the ONE listen frame
	// (inmatch::listen_host::frame) drives — ctx + per-peer transports +
	// now_tick + serve_and_play, shared with host_session_pump
	// (engine/runtime/session). MUST be declared before the aliases and before
	// runtime_ (the HostClient runtime references host_state_.host_loop). The
	// HostClient/Joiner ClientRuntime stays a binding member (runtime_ below,
	// ADR 0042 d3: no headless joiner consumer; the binding also folds the
	// host's own view with its perf clocks), so state.client_runtime is unused.
	opennova::inmatch::ListenHostState host_state_;
	opennova::np::HostOwner &host_owner_ = host_state_.host_owner;
	opennova::np::NapiNPServerCtx &ctx_ = host_state_.host_owner.ctx; // alias: host only (is_authority)
	opennova::netsim::LoopbackChannel &host_loop_ = host_state_.host_loop; // the host's own dcb-2 client; Server_TickUpdate's 0x0A target
	std::unique_ptr<opennova::np::ClientRuntime> runtime_;    // HostClient (host/SP) OR Joiner; the present-snapshot source
	// Retail loads this process-scoped table from charattr.def before joining.
	// Keep the byte image outside ClientRuntime so a direct mission load can
	// reinstall it when a load rebuilds an as-yet-unstarted joiner.
	opennova::np::CharAttrChallengeTable charattr_challenge_table_{};
	bool charattr_challenge_loaded_ = false;
	std::string joiner_player_name_;                          // persisted for the Joiner runtime ctor on (re)load
	// One immutable items.def catalog supplies both the authoritative entity stamp
	// and the decoded-client record-width resolver. The callback codec, physical
	// motion family, and allocation inputs remain independent traits.
	std::shared_ptr<const opennova::netsim::ItemReplicationCatalog>
			item_replication_catalog_;
	Ref<ItemDatabase> item_replication_catalog_db_;
	uint64_t item_replication_catalog_revision_ = 0;
	// Mission-scoped source for the authoritative half of the same contract.
	// World::restore rewinds registry entities to the pre-trait promotion baseline,
	// so restart reapplies this database before rebuilding decoded client replicas.
	Ref<ItemDatabase> item_traits_db_;
	// Install the catalog resolver on runtime_'s view (no-op until both exist). Called from
	// resolve_item_traits, finish_load (per-load runtime rebuild), and enable_join.
	void install_item_class_resolver();
	// Install or clear the retained boot charattr table on the current Joiner runtime.
	void install_charattr_challenge_table();
	// Copy the per-class ATTRIBUTES words into World::class_attribute_flags -- the
	// joiner's live table when one exists (S2C 0x41 mutates it in receive order),
	// else the boot copy. Runs at world creation, at every table install, and
	// after each net pump (engine: runtime/session/charattr_challenge.cpp).
	void sync_class_attribute_flags();
	// Install the retained retail player-profile join block on the current runtime.
	void install_character_join_vars();
	// Install or clear the explicitly selected retail integrity corpus profile.
	void install_join_integrity_profile();
	// Install the retained JOIN-checksum install root (D-NET-166).
	void install_expansion_version_root();
	// Install the retained APPID join token (decoded .joi CK) on the current runtime.
	void install_app_id();
	// Install the retained CD identity cookie (packed PUB* blob) on the runtime.
	void install_join_cd_cookie();
	// Per-load host bring-up: mode 3 -> create_session(&host_loop_) [connection-table reset +
	// Server_InitNewRoundState] -> the faithful host-player auto-spawn, over the kernel's
	// world/mission. Mirrors apps/nw_server; the Godot-fed context installs (mission text,
	// .til bytes, GameConfig from the UI host config) sit beside the shared core.
	void bringup_host_runtime();
	// The per-frame host owner loop: the ONE inmatch::listen_host::frame over the kernel
	// (drain -> pre-tick -> host_session_pump -> local pumps -> adm ground), with the
	// binding supplying the viewport-height seam, the local ClientState fold + perf
	// clocks, and the local reload relay.
	void host_pump();
	// The per-frame non-authority client loop, now the bridge's pump (S10a):
	// this binding builds the PumpContext/PumpHooks and delegates. The
	// pre-mission preload pump shares the bridge's hello latch + clock.
	// Deposit received framed datagrams for this frame's recv pump.
	void joiner_deposit_inbound();
	void joiner_pump();
	// Wire-side authored-shape resolution for one decoded runtime type id
	// (items.def graphic -> the shared by-graphic collision model cache).
	opennova::world::ResolvedCollisionShape wire_collision_shape_for_type(
			uint16_t type_id);

	// Terrain: the kernel owns the one cpt/trn(+charmap) field store
	// (kernel_->terrain_store, ADR 0042 d4); this binding retains the source
	// TerrainData Ref so a reload (which recreates the kernel) can rebuild it.
	Ref<TerrainData> terrain_data_;
	// The placed-tile surface override (D-SND-15): the mission .til entries
	// plus the tileset's .TSD-fed tile-index -> surface table, both resolved
	// engine-side (terrain_query surface_tiles.h — the witnesses live there).
	// Owned here like the heightmap so world.surface_map's raw pointers
	// survive reset_world; the zero table is retail's no-.TSD default.
	std::vector<opennova::terrain::SurfaceTileEntry> surface_tiles_;
	std::array<uint8_t, 256> tile_surface_table_{};
	// SndProf.def text + water plane held for (re)application on reset_world.
	std::vector<uint8_t> sndprof_text_;
	int32_t env_water_z_q16_ = 0;
	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	std::vector<CharacterSexRow> character_sex_rows_;
	void apply_terrain_to_ai();
	void apply_sound_state_to_world();
	void apply_character_traits_to_world();

	// Anim-driven soldier locomotion: the kernel owns the .adm/.bad-backed
	// root-motion source (kernel_->root_motion) and the per-entity resolution
	// sweep. This binding retains the Refs that pin the shell's sources (the
	// anim root and item db a joiner's decoded-row resolve reads through).
	void resolve_client_row_adm_ids();
	std::unordered_map<uint16_t, int> client_row_adm_by_type_;
	Ref<ResourceRoot> infantry_adm_resource_root_;
	// Retained score.ini parse; the row is re-resolved whenever the mission's
	// attrib flags change (either load order is legal).
	opennova::score::File score_config_;
	bool score_config_loaded_ = false;
	void refresh_score_rules();
	Ref<ItemDatabase> infantry_adm_item_db_;
	// The shared install tail (both install orders): sort for the per-frame
	// binary search, stamp turret clamps, refresh live pool-1 rows, and re-sync
	// the header-only materializer image.
	void finalize_installed_seat_specs();
	void refresh_item_seat_spec(opennova::world::Entity &p_entity);
	// Resolve each spec's turret clamp window from its primary weapon's
	// weapon.def rows. Called from BOTH install orders (specs-then-table and
	// table-then-specs); all-zero = not authored, no clamp.
	void stamp_seat_spec_turret_limits();

	void reset_world();
