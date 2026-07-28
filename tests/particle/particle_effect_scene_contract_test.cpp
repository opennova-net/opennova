#include <particle/effect_scene.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace p = opennova::particle;

bool check(bool ok, const char *message) {
	if (!ok) {
		std::fputs(message, stderr);
		std::fputc(10, stderr);
	}
	return ok;
}

bool near(double actual, double expected) {
	return std::fabs(actual - expected) <= 0.00001;
}

p::ParticleDef particle(std::string id) {
	p::ParticleDef value;
	value.id = std::move(id);
	value.emit_dur = 4.0f;
	value.emit_rate = 10.0f;
	value.emit_burst = 1;
	value.age = 2.0f;
	value.graphics[0].present = true;
	value.graphics[0].index = 1;
	value.graphics[0].scale = 2.0f;
	value.graphics[0].alpha = 1.0f;
	return value;
}

p::EffectCatalogDocument document(std::string source,
		std::vector<p::ParticleDef> particles,
		std::vector<p::EffectDef> effects) {
	p::EffectCatalogDocument value;
	value.source = std::move(source);
	value.file.particles = std::move(particles);
	value.file.effects = std::move(effects);
	return value;
}

p::EffectSceneConfig one_effect(std::string effect = "flash",
		std::string definition = "spark") {
	p::EffectSceneConfig config;
	config.documents.push_back(document("primary.ptl",
			{particle(definition)}, {{effect, {definition}}}));
	return config;
}

p::EffectSpawnRequest spawn_request(p::EffectHandle effect) {
	p::EffectSpawnRequest request;
	request.effect = effect;
	return request;
}

const p::EffectGroupDebugSnapshot *debug_group(
		const p::EffectDebugSnapshot &snapshot, p::EffectGroupId id) {
	for (const auto &group : snapshot.groups)
		if (group.id == id)
			return &group;
	return nullptr;
}

const p::EffectGroupFrameSnapshot *frame_group(
		const p::ParticleFrameSnapshot &snapshot, p::EffectGroupId id) {
	for (const auto &group : snapshot.groups)
		if (group.id == id)
			return &group;
	return nullptr;
}

// Public EffectScene behavior is exercised without parsing fixture files.

bool catalog_and_stock_alias_contract() {
	p::EffectSceneConfig config;
	config.documents.push_back(document("first.ptl",
			{particle("first spark")},
			{{"StockEffect", {"first spark"}}, {"Flash", {"first spark"}}}));
	config.documents.push_back(document("second.ptl",
			{particle("second spark")}, {{"fLaSh", {"second spark"}}}));
	p::EffectScene scene;
	const auto report = scene.open(config);
	if (!check(report.effect_count == 2 &&
			report.duplicate_effect_count == 1,
			"case-insensitive duplicate is excluded and reported")) return false;
	const auto flash = scene.intern("FLASH");
	if (!check(flash && flash == scene.intern("flash") &&
			scene.effect_name(flash) == "Flash",
			"known intern is case-insensitive and keeps first spelling")) return false;
	const auto alias = scene.intern("CaseShellBurst");
	if (!check(alias && alias == scene.intern("caseshellburst") &&
			alias != scene.intern("ImpactBurst"),
			"stock aliases are stable and distinct")) return false;
	if (!check(scene.effect_name(alias) == "CaseShellBurst",
			"stock alias keeps requested spelling")) return false;
	const auto receipt = scene.spawn(spawn_request(flash));
	const auto alias_receipt = scene.spawn(spawn_request(alias));
	if (!check(receipt.spawned() && alias_receipt.spawned(),
			"first-win and stock-alias effects spawn")) return false;
	const auto frame = scene.advance({0.0f});
	if (!check(frame.groups.size() == 2 &&
			frame.groups[0].source == "first.ptl" &&
			frame.groups[1].source == "first.ptl",
			"first registration supplies both sources")) return false;
	const auto first_def =
			frame.emitters[frame.groups[0].first_emitter].definition_index;
	const auto alias_def =
			frame.emitters[frame.groups[1].first_emitter].definition_index;
	return check(frame.definitions &&
			first_def < frame.definitions->size() &&
			alias_def < frame.definitions->size() &&
			(*frame.definitions)[first_def].id == "first spark" &&
			(*frame.definitions)[alias_def].id == "first spark" &&
			frame.groups[1].effect_name == "CaseShellBurst",
			"first definition wins and alias stays observable");
}

bool pdef_reference_resolution_is_case_insensitive_contract() {
	// [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0 → _stricmp
	// @ 0x5e420c, called from the EFFDEF→PARDEF resolve @ 0x5e4920] — the
	// pdefs member resolve and particle-def identity fold case.
	p::EffectSceneConfig config;
	config.documents.push_back(document("first.ptl",
			{particle("Spark Dust")}, {{"flash", {"sPaRk dUsT"}}}));
	config.documents.push_back(document("second.ptl",
			{particle("SPARK DUST")}, {}));
	p::EffectScene scene;
	const auto report = scene.open(config);
	if (!check(report.unresolved_particle_reference_count == 0,
			"case-different pdef reference resolves")) return false;
	if (!check(report.duplicate_particle_count == 1,
			"case-insensitive duplicate particle def is excluded")) return false;
	const auto flash = scene.intern("flash");
	const auto receipt = scene.spawn(spawn_request(flash));
	if (!check(receipt.spawned(), "case-resolved effect spawns")) return false;
	const auto frame = scene.advance({0.0f});
	if (!check(frame.groups.size() == 1, "one group spawned")) return false;
	const auto def_index =
			frame.emitters[frame.groups[0].first_emitter].definition_index;
	return check(frame.definitions && def_index < frame.definitions->size() &&
			(*frame.definitions)[def_index].id == "Spark Dust",
			"pdef resolves to the first-registered spelling");
}

bool effect_resolve_is_all_or_nothing_contract() {
	// [orig: CEffectBank_ResolveAllEntries @ 0x5e4920 — the first missing
	// pdefs member breaks the resolve @ 0x5e495d and ClearAll @ 0x5e49be
	// empties the whole resolved list]. An effect with any missing PARDEF
	// stays registered by name but spawns nothing — never a partial subset.
	p::EffectSceneConfig config;
	config.documents.push_back(document("broken.ptl",
			{particle("smoke"), particle("flame")},
			{{"pyre", {"smoke", "no such pardef", "flame"}},
			 {"intact", {"flame"}}}));
	p::EffectScene scene;
	const auto report = scene.open(config);
	if (!check(report.unresolved_particle_reference_count == 1,
			"failed effect counts once, at its first missing member")) return false;
	const auto pyre = scene.intern("pyre");
	if (!check(bool(pyre), "unresolved effect stays registered by name")) return false;
	const auto receipt = scene.spawn(spawn_request(pyre));
	if (!check(!receipt.spawned() &&
			receipt.status == p::EffectSpawnStatus::EmptyEffect,
			"unresolved effect spawns nothing, not a partial subset")) return false;
	const auto intact = scene.intern("intact");
	const auto intact_receipt = scene.spawn(spawn_request(intact));
	return check(intact_receipt.spawned(),
			"a sibling effect with resolvable pdefs is unaffected");
}

bool slot_owner_and_admission_contract() {
	p::EffectScene scene;
	scene.open(one_effect());
	const auto effect = scene.intern("flash");
	p::EffectOwnerPoseUpdate owner_pose;
	owner_pose.owner = p::EffectOwnerToken{100};
	owner_pose.pose.position = {10.0f, 0.0f, 0.0f};
	scene.apply_owner_poses({owner_pose});
	auto first = spawn_request(effect);
	first.admission = p::EffectAdmission::SuppressWhileOwned;
	first.binding = p::EffectBinding::FollowOwner;
	first.slot = p::EffectSlotToken{7};
	first.owner = p::EffectOwnerToken{100};
	first.owner_relative_pose.position = {2.0f, 0.0f, 0.0f};
	const auto admitted = scene.spawn(first);
	if (!check(admitted.spawned(), "first slot admission spawns")) return false;
	auto other_owner = first;
	other_owner.owner = p::EffectOwnerToken{200};
	const auto suppressed = scene.spawn(other_owner);
	if (!check(suppressed.status == p::EffectSpawnStatus::Suppressed &&
			suppressed.group == admitted.group,
			"slot suppresses independently of owner")) return false;
	auto other_slot = first;
	other_slot.slot = p::EffectSlotToken{8};
	if (!check(scene.spawn(other_slot).spawned(),
			"same owner may occupy another slot")) return false;
	const auto active_owners = scene.active_owner_tokens();
	if (!check(active_owners.size() == 1 && active_owners[0].value == 100,
			"live owner query is unique across following groups")) return false;
	auto always = spawn_request(effect);
	always.admission = p::EffectAdmission::Always;
	always.slot = p::EffectSlotToken{7};
	const auto always_receipt = scene.spawn(always);
	if (!check(always_receipt.spawned() &&
			scene.spawn(other_owner).status == p::EffectSpawnStatus::Suppressed,
			"Always neither reads nor overwrites slots")) return false;
	const auto debug = scene.inspect();
	const auto *always_group = debug_group(debug, always_receipt.group);
	if (!check(always_group && !always_group->slot,
			"Always stores no admission slot")) return false;
	const auto frame = scene.advance({0.0f});
	const auto *followed = frame_group(frame, admitted.group);
	return check(followed && near(followed->pose.position.x, 12.0),
			"owner pose composes independently from slot");
}

bool replace_detach_and_validation_contract() {
	p::EffectScene scene;
	scene.open(one_effect());
	const auto effect = scene.intern("flash");
	auto request = spawn_request(effect);
	request.admission = p::EffectAdmission::ReplaceOwned;
	request.binding = p::EffectBinding::FollowOwner;
	request.slot = p::EffectSlotToken{31};
	request.owner = p::EffectOwnerToken{41};
	const auto original = scene.spawn(request);
	request.owner = p::EffectOwnerToken{42};
	const auto replacement = scene.spawn(request);
	if (!check(original.spawned() && replacement.spawned() &&
			replacement.replaced_group == original.group,
			"ReplaceOwned reports its prior occupant")) return false;
	auto debug = scene.inspect();
	const auto *old_group = debug_group(debug, original.group);
	const auto *new_group = debug_group(debug, replacement.group);
	if (!check(old_group && old_group->detached &&
			new_group && !new_group->detached,
			"replacement detaches old group without killing it")) return false;
	p::EffectOwnerPoseUpdate lost;
	lost.owner = p::EffectOwnerToken{42};
	lost.present = false;
	scene.apply_owner_poses({lost});
	if (!check(scene.active_owner_tokens().empty(),
			"detached groups leave the live owner query")) return false;
	debug = scene.inspect();
	new_group = debug_group(debug, replacement.group);
	if (!check(new_group && new_group->detached,
			"owner loss detaches a following group")) return false;
	auto reuse = spawn_request(effect);
	reuse.admission = p::EffectAdmission::SuppressWhileOwned;
	reuse.slot = p::EffectSlotToken{31};
	const auto reused = scene.spawn(reuse);
	if (!check(reused.spawned(), "detach releases slot admission")) return false;
	scene.detach_slot(p::EffectSlotToken{31});
	debug = scene.inspect();
	const auto *detached = debug_group(debug, reused.group);
	if (!check(detached && detached->detached,
			"explicit slot detach preserves draining group")) return false;
	if (!check(scene.spawn(reuse).spawned(),
			"explicit detach permits the next admission")) return false;
	auto missing_slot = spawn_request(effect);
	missing_slot.admission = p::EffectAdmission::ReplaceOwned;
	if (!check(scene.spawn(missing_slot).status ==
			p::EffectSpawnStatus::MissingSlot,
			"owned admission requires a slot")) return false;
	auto missing_owner = spawn_request(effect);
	missing_owner.binding = p::EffectBinding::FollowOwner;
	return check(scene.spawn(missing_owner).status ==
			p::EffectSpawnStatus::MissingOwner,
			"follow binding requires an owner");
}

bool fixed_age_and_order_contract() {
	p::EffectScene scene;
	auto config = one_effect();
	config.simulation_tick_seconds = 1.0f / 62.5f;
	scene.open(config);
	const auto effect = scene.intern("flash");
	auto first = spawn_request(effect);
	first.initial_age_ticks = 2;
	first.source_tick = 500;
	first.source_order = 9;
	first.render_domain = p::EffectRenderDomain::FirstPerson;
	const auto first_receipt = scene.spawn(first);
	auto second = spawn_request(effect);
	second.source_tick = 499;
	second.source_order = 3;
	const auto second_receipt = scene.spawn(second);
	if (!check(first_receipt.spawned() && second_receipt.spawned(),
			"ordered events spawn")) return false;
	auto frame = scene.advance({0.010f});
	if (!check(frame.groups.size() == 2 &&
			frame.groups[0].id == first_receipt.group &&
			frame.groups[1].id == second_receipt.group,
			"snapshot retains deterministic spawn order")) return false;
	if (!check(frame.groups[0].source_tick == 500 &&
			frame.groups[0].source_order == 9 &&
			frame.groups[1].source_tick == 499 &&
			frame.groups[1].source_order == 3,
			"source ordering metadata is preserved exactly")) return false;
	if (!check(frame.groups[0].render_domain ==
			p::EffectRenderDomain::FirstPerson,
			"render domain survives into snapshot")) return false;
	if (!check(near(frame.emitters[0].age, 0.032) &&
			near(frame.emitters[1].age, 0.0),
			"initial age replays whole 0.016-second ticks")) return false;
	if (!check(near(frame.simulation_time_seconds, 0.0),
			"sub-tick delta remains accumulated")) return false;
	frame = scene.advance({0.005f});
	if (!check(near(frame.emitters[0].age, 0.032),
			"accumulated 0.015 seconds remains sub-tick")) return false;
	frame = scene.advance({0.002f});
	return check(near(frame.simulation_time_seconds, 0.016) &&
			near(frame.emitters[0].age, 0.048) &&
			near(frame.emitters[1].age, 0.016),
			"crossing threshold advances one whole fixed tick");
}

bool capacity_rejection_contract() {
	auto group_config = one_effect();
	group_config.max_live_groups = 1;
	group_config.max_live_emitters = 4;
	p::EffectScene group_scene;
	group_scene.open(group_config);
	const auto effect = group_scene.intern("flash");
	if (!check(group_scene.spawn(spawn_request(effect)).spawned(),
			"first group fits capacity")) return false;
	const auto rejected = group_scene.spawn(spawn_request(effect));
	if (!check(rejected.status == p::EffectSpawnStatus::GroupCapacityReached,
			"group capacity rejects explicitly")) return false;
	auto debug = group_scene.inspect();
	if (!check(debug.live_group_count == 1 &&
			debug.live_emitter_count == 1 &&
			debug.rejected_spawn_count == 1 &&
			debug.capacity_rejection_count == 1,
			"group rejection is atomic and counted")) return false;
	p::EffectSceneConfig emitter_config;
	emitter_config.max_live_groups = 4;
	emitter_config.max_live_emitters = 1;
	emitter_config.documents.push_back(document("multi.ptl",
			{particle("left"), particle("right")},
			{{"double", {"left", "right"}}}));
	p::EffectScene emitter_scene;
	emitter_scene.open(emitter_config);
	const auto double_effect = emitter_scene.intern("double");
	const auto emitter_rejection =
			emitter_scene.spawn(spawn_request(double_effect));
	if (!check(emitter_rejection.status ==
			p::EffectSpawnStatus::EmitterCapacityReached,
			"multi-emitter effect rejects as a whole")) return false;
	debug = emitter_scene.inspect();
	return check(debug.live_group_count == 0 &&
			debug.live_emitter_count == 0 &&
			debug.capacity_rejection_count == 1,
			"emitter rejection allocates no partial group and is counted");
}

bool lightweight_debug_contract() {
	p::EffectScene scene;
	auto config = one_effect();
	config.documents[0].file.particles[0].flags =
			p::particle_flag::ForeverEmit;
	scene.open(config);
	auto request = spawn_request(scene.intern("flash"));
	request.pose.position = {3.0f, 4.0f, 5.0f};
	request.source_tick = 17;
	request.source_order = 23;
	request.kill_plane = p::EffectKillPlane::KillAbove;
	request.kill_plane_y = 12.5f;
	if (!check(scene.spawn(request).spawned(),
			"lightweight debug effect spawns")) return false;
	scene.advance_simulation({0.032f});

	const p::EffectLiveCounts counts = scene.live_counts();
	if (!check(counts.group_count == 1 && counts.emitter_count == 1 &&
			counts.particle_count > 0,
			"live counts avoid materializing a value snapshot")) return false;

	const p::EffectDebugSnapshot debug = scene.inspect(false);
	if (!check(debug.live_group_count == counts.group_count &&
			debug.live_emitter_count == counts.emitter_count &&
			debug.live_particle_count == counts.particle_count &&
			debug.groups.size() == 1 && debug.groups[0].emitters.size() == 1,
			"lightweight inspect retains live topology and counts")) return false;
	const p::EffectGroupDebugSnapshot &group = debug.groups[0];
	const p::EffectEmitterDebugSnapshot &emitter = group.emitters[0];
	if (!check(!group.bounds.valid && !emitter.bounds.valid,
			"lightweight inspect skips per-particle bounds")) return false;
	if (!check(near(group.pose.position.x, 3.0) &&
			group.source_tick == 17 && group.source_order == 23,
			"lightweight group metadata replaces frame serialization")) return false;
	return check(emitter.definition_flags == p::particle_flag::ForeverEmit &&
			near(emitter.position.x, 3.0) && emitter.age > 0.0f &&
			emitter.kill_plane == p::EffectKillPlane::KillAbove &&
			near(emitter.kill_plane_y, 12.5),
			"lightweight emitter metadata replaces frame and catalog serialization");
}

bool deferred_snapshot_contract() {
	p::EffectScene scene;
	auto config = one_effect();
	config.simulation_tick_seconds = 1.0f / 62.5f;
	scene.open(config);
	const auto effect = scene.intern("flash");
	if (!check(scene.spawn(spawn_request(effect)).spawned(),
			"deferred snapshot effect spawns")) return false;
	scene.advance_simulation({0.016f});
	scene.advance_simulation({0.016f});
	p::ParticleFrameSnapshot frame;
	scene.write_snapshot(frame);
	if (!check(frame.frame_index == 2 && frame.groups.size() == 1 &&
			frame.emitters.size() == 1 && near(frame.emitters[0].age, 0.032),
			"batched simulation materializes only the latest frame")) return false;
	scene.write_snapshot(frame);
	if (!check(frame.frame_index == 2,
			"snapshot materialization does not advance frame identity")) return false;
	const auto compatibility_frame = scene.advance({0.0f});
	return check(compatibility_frame.frame_index == 3 &&
			near(compatibility_frame.emitters[0].age, 0.032),
			"value-returning advance preserves frame progression");
}

bool initial_age_is_bounded_contract() {
	p::EffectScene scene;
	auto config = one_effect();
	config.simulation_tick_seconds = 1.0f / 62.5f;
	scene.open(config);
	auto request = spawn_request(scene.intern("flash"));
	request.initial_age_ticks = p::kEffectInitialAgeTickLimit + 100;
	if (!check(scene.spawn(request).spawned(),
			"bounded pre-age effect spawns")) return false;
	const auto frame = scene.advance({0.0f});
	const double capped_age = static_cast<double>(p::kEffectInitialAgeTickLimit) *
			static_cast<double>(config.simulation_tick_seconds);
	return check(frame.emitters.size() == 1 &&
			std::fabs(static_cast<double>(frame.emitters[0].age) - capped_age) < 0.001,
			"initial age is clamped to the bounded catch-up window");
}

bool initial_age_reaps_exhausted_group_contract() {
	auto config = one_effect();
	config.documents[0].file.particles[0].emit_dur = 0.05f;
	config.documents[0].file.particles[0].age = 0.05f;
	p::EffectScene scene;
	scene.open(config);
	auto request = spawn_request(scene.intern("flash"));
	request.admission = p::EffectAdmission::SuppressWhileOwned;
	request.slot = p::EffectSlotToken{71};
	request.initial_age_ticks = 32;
	if (!check(scene.spawn(request).spawned(),
			"fully pre-aged effect is accepted")) return false;
	if (!check(scene.live_counts().group_count == 0 &&
			scene.live_counts().emitter_count == 0,
			"initial-age replay reaps an exhausted group immediately")) {
		return false;
	}
	return check(scene.spawn(request).spawned(),
			"exhausted pre-aged group never retains its suppression slot");
}

bool immutable_snapshot_contract() {
	p::EffectScene scene;
	scene.open(one_effect("first", "first particle"));
	const auto first = scene.intern("first");
	if (!check(scene.spawn(spawn_request(first)).spawned(),
			"first catalog effect spawns")) return false;
	const auto old_snapshot = scene.advance({0.0f});
	if (!check(old_snapshot.definitions &&
			old_snapshot.definitions->size() == 1,
			"old snapshot owns compiled definitions")) return false;
	scene.open(one_effect("second", "second particle"));
	const auto second = scene.intern("second");
	if (!check(scene.spawn(spawn_request(second)).spawned(),
			"replacement catalog effect spawns")) return false;
	const auto new_snapshot = scene.advance({0.0f});
	if (!check(old_snapshot.definitions.get() !=
			new_snapshot.definitions.get(),
			"reopen publishes new immutable definitions")) return false;
	if (!check((*old_snapshot.definitions)[0].id == "first particle" &&
			old_snapshot.groups[0].effect_name == "first",
			"old snapshot values survive reopen")) return false;
	return check((*new_snapshot.definitions)[0].id == "second particle" &&
			new_snapshot.groups.size() == 1 &&
			new_snapshot.groups[0].effect_name == "second",
			"new snapshot contains replacement scene values");
}

bool runtime_reset_preserves_catalog_identity_contract() {
	p::EffectScene scene;
	auto config = one_effect();
	config.simulation_tick_seconds = 0.25f;
	scene.open(config);
	const auto effect = scene.intern("flash");

	p::EffectOwnerPoseUpdate owner_pose;
	owner_pose.owner = p::EffectOwnerToken{91};
	owner_pose.pose.position = {9.0f, 8.0f, 7.0f};
	scene.apply_owner_poses({owner_pose});
	auto guarded = spawn_request(effect);
	guarded.pose.position = {1.0f, 2.0f, 3.0f};
	guarded.admission = p::EffectAdmission::SuppressWhileOwned;
	guarded.binding = p::EffectBinding::FollowOwner;
	guarded.slot = p::EffectSlotToken{71};
	guarded.owner = owner_pose.owner;
	if (!check(scene.spawn(guarded).spawned() &&
			scene.spawn(guarded).status == p::EffectSpawnStatus::Suppressed,
			"reset fixture has live admission and owner state")) return false;
	scene.spawn(spawn_request(p::EffectHandle{999}));
	scene.advance_simulation({0.5f});

	p::ParticleFrameSnapshot before;
	scene.write_snapshot(before);
	const auto before_debug = scene.inspect();
	if (!check(before.definitions && before.groups.size() == 1 &&
			before_debug.suppressed_spawn_count == 1 &&
			before_debug.rejected_spawn_count == 1,
			"reset fixture exercises live clocks and counters")) return false;
	const auto *definition_identity = before.definitions.get();

	scene.reset_runtime_state();

	p::ParticleFrameSnapshot after;
	scene.write_snapshot(after);
	const auto after_debug = scene.inspect();
	if (!check(after.definitions.get() == definition_identity &&
			after.definitions->size() == 1 &&
			(*after.definitions)[0].id == "spark",
			"runtime reset preserves the compiled definition identity")) {
		return false;
	}
	if (!check(after.groups.empty() && after.emitters.empty() &&
			after.particles.empty() && after.frame_index == 0 &&
			near(after.simulation_time_seconds, 0.0),
			"runtime reset clears live values and timing")) return false;
	if (!check(after_debug.load.effect_count == 1 &&
			after_debug.interned_effect_count == 1 &&
			after_debug.group_pool_high_water == 0 &&
			after_debug.emitter_pool_high_water == 0 &&
			after_debug.suppressed_spawn_count == 0 &&
			after_debug.rejected_spawn_count == 0 &&
			after_debug.capacity_rejection_count == 0,
			"runtime reset preserves catalog metadata and clears counters")) {
		return false;
	}
	if (!check(scene.intern("FLASH") == effect &&
			scene.effect_name(effect) == "flash",
			"runtime reset preserves stable interned handles")) return false;

	const auto respawned = scene.spawn(guarded);
	if (!check(respawned.spawned(),
			"runtime reset clears admission state so the effect respawns")) {
		return false;
	}
	const auto respawn_frame = scene.advance({0.0f});
	return check(respawn_frame.groups.size() == 1 &&
			near(respawn_frame.groups[0].pose.position.x, 1.0) &&
			near(respawn_frame.groups[0].pose.position.y, 2.0) &&
			near(respawn_frame.groups[0].pose.position.z, 3.0),
			"runtime reset clears cached owner poses before respawn");
}

bool child_reaping_and_group_suppression_lifetime_contract() {
	// Retail attaches the action-slot clear callback to CEffectGroup
	// [orig: CEffectGroup_SetDeathCallback @ 0x5e1940] and invokes it from
	// CEffectGroup_Destroy @ 0x5e3460. Individual dead children are reaped by
	// CEffectGroup_AdvanceChildrenAndReap @ 0x5e59a0 without clearing the slot.
	p::ParticleDef quick = particle("quick flash");
	quick.emit_dur = 0.05f;
	quick.emit_rate = 20.0f;
	quick.age = 0.05f;
	p::ParticleDef slow = particle("slow smoke");
	slow.emit_dur = 0.75f;
	slow.age = 0.25f;
	p::ParticleDef spare = particle("spare spark");
	spare.emit_dur = 0.05f;
	spare.age = 0.05f;
	p::EffectSceneConfig config;
	config.max_live_emitters = 2;
	config.documents.push_back(document("muzzle.ptl",
			{quick, slow, spare},
			{{"muzzle", {"quick flash", "slow smoke"}},
					{"single", {"spare spark"}}}));
	p::EffectScene scene;
	scene.open(config);
	const auto effect = scene.intern("muzzle");
	const auto single = scene.intern("single");

	auto guarded = spawn_request(effect);
	guarded.admission = p::EffectAdmission::SuppressWhileOwned;
	guarded.slot = p::EffectSlotToken{7};
	const auto admitted = scene.spawn(guarded);
	if (!check(admitted.spawned(),
			"the first guarded spawn is admitted")) return false;
	if (!check(scene.spawn(guarded).status == p::EffectSpawnStatus::Suppressed,
			"a same-tick follow-up is suppressed")) return false;
	if (!check(scene.spawn(spawn_request(single)).status ==
			p::EffectSpawnStatus::EmitterCapacityReached,
			"both live child emitters initially consume capacity")) return false;
	// Run past the quick child's whole life (emit 0.05 s + particle age 0.05 s)
	// while the slow child keeps emitting.
	scene.advance({0.5f});
	const p::EffectLiveCounts partially_drained = scene.live_counts();
	const auto debug = scene.inspect();
	const auto *original = debug_group(debug, admitted.group);
	if (!check(partially_drained.group_count == 1 &&
			partially_drained.emitter_count == 1 &&
			original != nullptr && original->emitters.size() == 1 &&
			original->emitters[0].definition_name == "slow smoke",
			"finished child is reaped while its live sibling remains")) return false;
	const auto still_suppressed = scene.spawn(guarded);
	if (!check(still_suppressed.status == p::EffectSpawnStatus::Suppressed &&
			still_suppressed.group == admitted.group,
			"slot remains suppressed for the complete effect group lifetime")) {
		return false;
	}
	if (!check(scene.spawn(spawn_request(single)).spawned(),
			"reaping a child immediately returns its emitter capacity")) {
		return false;
	}

	scene.advance({2.0f});
	if (!check(scene.live_counts().group_count == 0 &&
			scene.live_counts().emitter_count == 0,
			"groups are destroyed after their final children drain")) return false;
	return check(scene.spawn(guarded).spawned(),
			"slot re-arms only after the complete effect group dies");
}

bool huge_delta_catch_up_is_bounded_contract() {
	const float huge_deltas[] = {
		1.0e9f,
		std::numeric_limits<float>::max(),
	};
	for (const float huge_delta : huge_deltas) {
		p::EffectScene scene;
		auto config = one_effect();
		config.simulation_tick_seconds = 1.0f / 62.5f;
		config.documents[0].file.particles[0].flags =
				p::particle_flag::ForeverEmit;
		scene.open(config);
		if (!check(scene.spawn(spawn_request(scene.intern("flash"))).spawned(),
				"forever emitter for bounded catch-up spawns")) return false;
		scene.advance_simulation({huge_delta});
		p::ParticleFrameSnapshot frame;
		scene.write_snapshot(frame);
		const double capped_age =
				static_cast<double>(p::kEffectAdvanceTickLimit) *
				static_cast<double>(config.simulation_tick_seconds);
		if (!check(frame.emitters.size() == 1 &&
				std::fabs(static_cast<double>(frame.emitters[0].age) -
						capped_age) < 0.001 &&
				std::fabs(frame.simulation_time_seconds - capped_age) < 0.001,
				"huge finite delta advances no more than 256 fixed ticks")) {
			return false;
		}
		const double age_after_huge = frame.emitters[0].age;
		const double time_after_huge = frame.simulation_time_seconds;
		scene.advance_simulation({0.0f});
		scene.write_snapshot(frame);
		if (!check(std::fabs(static_cast<double>(frame.emitters[0].age) -
						age_after_huge) < 0.00001 &&
				std::fabs(frame.simulation_time_seconds - time_after_huge) <
						0.00001,
				"discarded whole-step backlog never drains on zero delta")) {
			return false;
		}
	}

	p::EffectScene remainder_scene;
	auto remainder_config = one_effect();
	remainder_config.simulation_tick_seconds = 0.25f;
	remainder_config.documents[0].file.particles[0].flags =
			p::particle_flag::ForeverEmit;
	remainder_scene.open(remainder_config);
	if (!check(remainder_scene.spawn(
			spawn_request(remainder_scene.intern("flash"))).spawned(),
			"forever emitter for fractional remainder spawns")) return false;
	remainder_scene.advance_simulation({1000.125f});
	p::ParticleFrameSnapshot before_remainder;
	remainder_scene.write_snapshot(before_remainder);
	remainder_scene.advance_simulation({0.125f});
	p::ParticleFrameSnapshot after_remainder;
	remainder_scene.write_snapshot(after_remainder);
	return check(before_remainder.emitters.size() == 1 &&
			after_remainder.emitters.size() == 1 &&
			std::fabs(static_cast<double>(before_remainder.emitters[0].age) -
					64.0) < 0.001 &&
			std::fabs(static_cast<double>(after_remainder.emitters[0].age) -
					64.25) < 0.001,
			"bounded catch-up preserves only the fractional fixed-step remainder");
}

} // namespace

int main() {
	if (!catalog_and_stock_alias_contract()) return 1;
	if (!pdef_reference_resolution_is_case_insensitive_contract()) return 1;
	if (!effect_resolve_is_all_or_nothing_contract()) return 1;
	if (!slot_owner_and_admission_contract()) return 1;
	if (!child_reaping_and_group_suppression_lifetime_contract()) return 1;
	if (!replace_detach_and_validation_contract()) return 1;
	if (!fixed_age_and_order_contract()) return 1;
	if (!capacity_rejection_contract()) return 1;
	if (!lightweight_debug_contract()) return 1;
	if (!deferred_snapshot_contract()) return 1;
	if (!initial_age_is_bounded_contract()) return 1;
	if (!initial_age_reaps_exhausted_group_contract()) return 1;
	if (!huge_delta_catch_up_is_bounded_contract()) return 1;
	if (!immutable_snapshot_contract()) return 1;
	if (!runtime_reset_preserves_catalog_identity_contract()) return 1;
	return 0;
}
