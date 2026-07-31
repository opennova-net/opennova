extends RefCounted

# THE mission present pass: applies the client-view entity state onto placed scene nodes for
# MissionRuntime. It consolidated the historical game's MissionCommandHost (PANM part-anim only) and
# the editor's MissionSimDriver._apply (position + yaw only); those divergent paths remain retired.
# `GameWorld`, under `MainGame`, is the sole live mission runtime and ONED has no PIE runtime. Tests and
# non-gameplay tooling previews may still instantiate MissionRuntime and this RefCounted pass directly.
#
# Hybrid split (the engine decides, the shell draws): it pulls ONE batched snapshot from the sim
# (NovaSimulation.get_present_snapshot -- a flat PackedFloat32Array) and the NATIVE row walk
# (NovaPresentApplier, godot/engine/simulation) owns the plan + per-row reads + change-gated
# dispatch; each resolved node's duck-typed NovaEntityVisual surface (ADR 0007) keeps its GDScript
# implementation:
#   transform (Node3D), set_part_phase(channel, phase),
#   clear_part_phase(channel), visible (Node3D),
#   play_body_clip_at(key, phase_ticks) / play_body_anim_at(slot, phase_ticks).
#
# Two animation systems, distinct on purpose:
#  - Procedural part-anim (PANM): the vehicle/emplacement part system (turret/dish), PLAYPARTANIM
#    case 0x22, integrated in-engine; applied via set_part_phase. IMPLEMENTED.
#  - Main-body skeletal (.bad via .adm): the primary infantry/view-model animation, selected by AI
#    state; the pass poses the model to the same .bad phase that produced root motion.
#    PF_BODY_ANIM_SLOT remains the coarse fallback for non-infantry/compat nodes.
#
# Targets resolve through ONE shared index (MissionEntityRegistry.resolve: bms_id primary, (kind,index)
# fallback). Shell-agnostic, RefCounted, preload-referenced (same convention as MissionObjectPlacer).
# The per-leg behavioral semantics and their [orig] witnesses are documented at the native walk
# (nova_present_applier.cpp) — this facade owns wiring, options, and the duck-typed sim queries.

const OUTPUT_TRANSFORM := 1
const OUTPUT_PART_ANIM := 2
const OUTPUT_VISIBILITY := 4
const OUTPUT_BODY_ANIM := 8
const OUTPUT_ALL := OUTPUT_TRANSFORM | OUTPUT_PART_ANIM \
		| OUTPUT_VISIBILITY | OUTPUT_BODY_ANIM

var _sim                    # NovaSimulation (or a compatible snapshot source)
var _index                  # MissionEntityRegistry: resolve(bms_id, kind, index) -> Node
var _applier: NovaPresentApplier = null


## options: { drive_transform, drive_part_anim, drive_visibility,
## drive_body_anim } (all default true), plus the shell's shared
## occlusion_hidden_ids and present_visibility maps (shared BY REFERENCE:
## GameWorld mutates the hidden set in place; the release lands on the sim's
## current intent so neither visibility writer fights the other).
func setup(sim, index, options: Dictionary = {}) -> void:
	_sim = sim
	_index = index
	_applier = NovaPresentApplier.new()
	_applier.setup(sim, index)
	var channels := OUTPUT_ALL
	if not bool(options.get("drive_transform", true)):
		channels &= ~OUTPUT_TRANSFORM
	if not bool(options.get("drive_part_anim", true)):
		channels &= ~OUTPUT_PART_ANIM
	if not bool(options.get("drive_visibility", true)):
		channels &= ~OUTPUT_VISIBILITY
	if not bool(options.get("drive_body_anim", true)):
		channels &= ~OUTPUT_BODY_ANIM
	_applier.set_output_channels(channels)
	var occlusion_ids: Variant = options.get("occlusion_hidden_ids")
	var present_visibility: Variant = options.get("present_visibility")
	_applier.set_shared_visibility_maps(
			occlusion_ids if occlusion_ids is Dictionary else {},
			present_visibility if present_visibility is Dictionary else {})


## Public A/B surface used by performance probes; one mask update changes a
## coherent set of presenter outputs without exposing implementation fields.
func set_output_channels(channels: int) -> void:
	if _applier != null:
		_applier.set_output_channels(channels & OUTPUT_ALL)


func get_output_channels() -> int:
	return _applier.get_output_channels() if _applier != null else OUTPUT_ALL


func get_stats() -> Dictionary:
	return _applier.get_stats() if _applier != null else {}


func get_stats_record() -> MissionPresentStats:
	return MissionPresentStats.new(get_stats())


## Apply the current sim state onto every resolved animated node. Called once per logic tick by the
## runtime driver (after the sim advances).
func present() -> void:
	if _sim == null or _index == null:
		return
	var stride: int = _sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = _sim.get_present_snapshot()
	var layout_revision := -1
	if _sim.has_method("get_present_layout_revision"):
		layout_revision = int(_sim.get_present_layout_revision())
	present_snapshot(snap, stride, layout_revision)


## Apply a snapshot already fetched by MissionRuntime. Compatible callers may
## keep using present(); a source without a layout revision simply rebuilds the
## routing plan every call.
func present_snapshot(
		snap: PackedFloat32Array, stride: int, layout_revision: int = -1) -> void:
	if _index == null or stride <= 0 or _applier == null:
		return
	_applier.present_snapshot(snap, stride, layout_revision)
