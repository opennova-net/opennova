extends SceneTree

## D-PTL-24 evidence: the retail effect catalog spans `.ptl` PLUS one gore set.
##
## Retail picks `particleExtension = ".ptu"` (US), or `".ptg"` when the mount carries
## `fgn2.bin` (the German content marker), and loads it alongside every `.ptl` — the
## archive walk matches either extension and parses both through the same section
## callback. Loading only `.ptl` loses every effect defined in that set, and loses it
## SILENTLY: an unknown effect name interns as an invisible `stockeffect` clone
## (D-PTL-8), so the sole symptom is that flesh hits and .50cal impacts render nothing.
##
## [orig: CEffectSystem_Init @ 0x5f6070 — extension select @0x5f608b..0x5f6095, loose
##  legs @0x5f6228/@0x5f6356, archive match @0x5f64f3, shared callback
##  CEffectWorld_ParseSectionCallback @ 0x5ecb40; selector Game_LoadConfig
##  @ 0x5514e8..0x5514fa sets byte_24D4DF9 = FileSystem_FileExists("fgn2.bin") != 0]
##
## Asset-gated, never CI. Judge the printed verdict, not the exit code:
##   NW_RESOURCE_DIR=<retail JO install> \
##     "$GODOT_BIN" --headless --path godot -s res://tests/gore_set_catalog_probe.gd

const RESOURCE_ENV := "NW_RESOURCE_DIR"
const TICK_DT := 1.0 / 62.5

# The effects retail authors in the gore set. `ammo.def` names Effect_AmHitBody on
# both the `flesh` (23) and `player` (2) rows of every real small-arms ammo, and
# Effect_SGvBody on the shotgun's; the Effect_FX50Cal* family covers .50cal impacts
# against every surface, not just flesh.
const GORE_SET_EFFECTS := [
	"Effect_AmHitBody",
	"Effect_FX50CalBody",
	"Effect_FX50CalDirt",
	"Effect_FX50CalMetal",
]
# Effect_SGvBody (the shotgun body hit) must render NOTHING — faithfully, because
# retail's own data breaks it. `SHOTGNFX.PTL` declares `Effect_SGvbody` with
# `pdefs =GvBody_Drops, ...` — a dropped leading `S`; the real particledef is
# `SGvBody_Drops` in the same file. `US_BLOOD.PTU` ships a WORKING `Effect_SGvBody`,
# but it never gets a chance: effect names are case-insensitive so the two collide,
# PFF entries are walked in uppercased-name order (SHOTGNFX.PTL < US_BLOOD.PTU), and
# first registration wins — so the typo'd one is the survivor, and one unresolved
# member clears the whole effect. Retail reaches the identical state.
# [orig: PFF_Open @ 0x7682e0 sorts entries by uppercased name;
#  CEffectBank_ResolveAllEntries @ 0x5e4920 breaks on the first miss @0x5e495d and
#  ClearAll @0x5e49be]
const RETAIL_BROKEN_EFFECT := "Effect_SGvBody"
# Authored in a plain `.ptl` — the control. It must resolve both before and after
# the fix, so a failure here means the mount is wrong, not the gore set.
const CONTROL_EFFECT := "Effect_AmHitDirt"
# No effectdef anywhere. Interning it exercises the D-PTL-8 stockeffect clone, which
# is exactly what a gore-set effect degraded to while `.ptu` went unloaded.
const ABSENT_EFFECT := "Effect_NoSuchEffect_ProbeSentinel"


func _init() -> void:
	var failures: Array[String] = []

	var resource_dir := OS.get_environment(RESOURCE_ENV).strip_edges()
	if resource_dir.is_empty():
		print("[gore-set] SKIP — set %s to a retail JO install" % RESOURCE_ENV)
		quit(0)
		return

	var root := ResourceRoot.new()
	if root.mount_runtime(resource_dir, "", false, "jo") != OK:
		print("[gore-set] FAIL — could not mount %s=%s" % [RESOURCE_ENV, resource_dir])
		quit(1)
		return
	print("[gore-set] mounted %s" % resource_dir)

	# 1. The selector. This install has no fgn2.bin, so the US set is active.
	var ext := root.particle_extension()
	print("[gore-set] particle_extension() = %s (fgn2.bin present: %s)"
			% [ext, root.has_file("fgn2.bin")])
	if ext != ".ptu" and ext != ".ptg":
		failures.append("particle_extension() returned %s, expected .ptu or .ptg" % ext)

	# 2. The index must actually carry the gore set. Before the fix these files
	#    classified as no kind at all and never entered the index.
	var ptl_entries := root.list_file_entries(".ptl")
	var gore_entries := root.list_file_entries(ext)
	print("[gore-set] indexed %d .ptl + %d %s" % [ptl_entries.size(), gore_entries.size(), ext])
	if gore_entries.is_empty():
		failures.append("no %s files indexed — the gore set is invisible to the loader" % ext)
	for entry_v in gore_entries:
		print("[gore-set]   %s" % String((entry_v as Dictionary).get("logical_name", "?")))

	# 3. Load the catalog the way the mission start does.
	var fx := EffectWorld.new()
	get_root().add_child(fx)
	var effect_count := fx.load_from_resource_root(root)
	print("[gore-set] catalog: %d effects across %d files" % [effect_count, fx.file_count()])
	if effect_count <= 0:
		print("[gore-set] FAIL — mounted no particle effects at all")
		quit(1)
		return

	# 4. A missing name still interns and still SPAWNS (D-PTL-8): it clones
	#    `stockeffect`, which retail authors as a real, spawning effect. So the
	#    discriminator is the EMITTER COUNT, not whether anything spawned at all —
	#    a clone always carries stockeffect's emitter profile, never its own.
	var absent := _spawn_and_probe(fx, ABSENT_EFFECT)
	var clone_emitters: int = absent["emitters"]
	print("[gore-set] control: absent name %s -> handle %d, %d emitter(s) — this is the"
			% [ABSENT_EFFECT, absent["handle"], clone_emitters])
	print("[gore-set]          stockeffect clone every gore effect degraded to")

	var control := _spawn_and_probe(fx, CONTROL_EFFECT)
	print("[gore-set] control: .ptl effect %s -> %d emitter(s)"
			% [CONTROL_EFFECT, control["emitters"]])
	if int(control["emitters"]) <= 0:
		failures.append("%s (a plain .ptl effect) did not spawn — the mount is wrong"
				% CONTROL_EFFECT)

	# 5. The payload: every gore-set effect must resolve to its OWN definition set.
	for name in GORE_SET_EFFECTS:
		var got := _spawn_and_probe(fx, name)
		var emitters: int = got["emitters"]
		var ok := emitters > 0 and emitters != clone_emitters
		print("[gore-set] %s %s -> handle %d, spawned %s, %d emitter(s)"
				% ["PASS" if ok else "FAIL", name, got["handle"], got["spawned"], emitters])
		if emitters == 0:
			failures.append("%s spawned nothing — its pdefs did not resolve (D-PTL-8 clears an effect whole)"
					% name)
		elif emitters == clone_emitters:
			failures.append("%s has the stockeffect clone's %d-emitter profile; it is still the invisible clone"
					% [name, clone_emitters])

	# 6. The retail data bug, pinned so a future load-order change cannot silently
	#    "fix" it into a divergence.
	var broken := _spawn_and_probe(fx, RETAIL_BROKEN_EFFECT)
	var broken_ok := int(broken["emitters"]) == 0
	print("[gore-set] %s %s -> handle %d, spawned %s, %d emitter(s) — retail-faithful"
			% ["PASS" if broken_ok else "FAIL", RETAIL_BROKEN_EFFECT,
					broken["handle"], broken["spawned"], broken["emitters"]])
	print("[gore-set]          (SHOTGNFX.PTL's `pdefs =GvBody_Drops` typo wins on name order)")
	if not broken_ok:
		failures.append("%s resolved to %d emitter(s); retail clears it whole on SHOTGNFX.PTL's typo, so rendering it is a DIVERGENCE"
				% [RETAIL_BROKEN_EFFECT, broken["emitters"]])

	print("")
	if failures.is_empty():
		print("[gore-set] VERDICT: PASS — the %s gore set loads and every blood/.50cal"
				% ext)
		print("[gore-set]          effect resolves to a real, spawning definition.")
		quit(0)
	else:
		print("[gore-set] VERDICT: FAIL")
		for f in failures:
			print("[gore-set]   - %s" % f)
		quit(1)


# Spawn one effect by name into a clean world and report its handle, whether the
# spawn took, and how many emitters it brought up. The emitter count IS the
# identity check: one emitter per resolved pdef, and an effect with any
# unresolved pdef is cleared whole (D-PTL-8 all-or-nothing) so it spawns none.
func _spawn_and_probe(fx: EffectWorld, name: String) -> Dictionary:
	fx.reset_runtime_state()
	var out := {"handle": 0, "spawned": false, "emitters": 0}
	var handle := fx.intern_effect(name)
	out["handle"] = handle
	if handle <= 0:
		return out
	out["spawned"] = fx.spawn_effect_by_handle(handle, Vector3.ZERO, Vector3.FORWARD)
	if not bool(out["spawned"]):
		return out
	fx.advance_fixed_tick(TICK_DT)
	var emitters := 0
	for group_v in fx.get_debug_group_report():
		emitters += ((group_v as Dictionary).get("emitters", []) as Array).size()
	out["emitters"] = emitters
	return out
