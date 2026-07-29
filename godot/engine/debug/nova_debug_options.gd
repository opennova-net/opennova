class_name NovaDebugOptions
## The declarative registry of every HOST-ACTIONABLE debug option: an option
## the overlay can only request (build/free a world debug view, flip
## host-owned player/render state), never perform itself. Pages build their
## controls from these rows (NovaDebugPage.add_option_check), the overlay
## re-emits changes as one generic `debug_option_changed(id, value)` signal,
## and each host maps `target` to its own object and calls `setter` — so the
## game shell and the editor can never drift apart on wiring.
##
## NOT in the registry: page-local actions that poke live objects directly
## (Sim transport, Vars edits, AudioServer mutes, terrain debug modes) — the
## existing precedent. Option values are deliberately not persisted.

const KIND_CHECK := 0
const KIND_SLIDER := 1  # row carries "min"/"max"/"step"
const KIND_ENUM := 2    # row carries "choices": Array[String]; value = index

## The world host (GameWorld or the PIE world).
const TARGET_WORLD := &"world"
## The local player host (LocalPlayerHost).
const TARGET_PLAYER := &"player"

const OPTIONS: Array[Dictionary] = [
	{
		"id": &"show_skeletons",
		"label": "Show skeletons",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_skeleton_debug",
		"tooltip": "Draw character bones (joint-to-parent lines + axis crosses) over the world.",
	},
	{
		"id": &"show_user_points",
		"label": "Show user points",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_user_point_debug",
		"tooltip": "Draw every named model user point as a cyan marker + label, following live animated bones and including static-batched mission objects.",
	},
	{
		"id": &"show_collision",
		"label": "Show collision",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_collision_debug",
		"tooltip": "Draw object collision volumes (type-colored boxes) and the player's capsule test points over the world.",
	},
	{
		"id": &"hide_foliage",
		"label": "Hide foliage",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_foliage_hidden",
		"tooltip": "Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it.",
	},
	{
		"id": &"hide_particles",
		"label": "Hide particles",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_particles_hidden",
		"tooltip": "Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all.",
	},
	{
		"id": &"show_effect_boxes",
		"label": "Show effect boxes",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_particle_debug",
		"tooltip": "Draw a red wireframe box (retail's debug box color) + effect name over every live emitter; effects with missing textures list them on the label.",
	},
	{
		"id": &"show_portal_faces",
		"label": "Show portal faces",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_occlusion_debug",
		"tooltip": "Draw every nearby building's occlusion faces over the world — windows, portals and welded links as colored outlines with section labels, plain occluder faces in gray.",
	},
	{
		"id": &"show_round_trails",
		"label": "Show round trails",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_round_debug",
		"tooltip": "Draw the recent round outcomes over the world — flight segments and hit markers colored by result (green = face hit, amber = sphere stand-in, red ring = a graze whose face test missed and flew on).",
	},
	{
		"id": &"show_hit_meshes",
		"label": "Show hit meshes",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"setter": &"set_hitbox_debug",
		"tooltip": "Hit geometry is sampled at 6 Hz. Draw nearby hit geometry within 80 mission units of the local player: object bullet meshes and broad-phase spheres, plus posed person bone spheres (local player omitted; up to 96 targets). Person colors show normal-infantry damage zones: orange = x1.25 (0-4), cyan = x1.0 (5-8), lime = x0.5 (9-12/15-18), magenta = x3.0 head (13-14), dark red = masked, amber = unresolved fallback.",
	},
	{
		"id": &"force_fp_arms",
		"label": "Always draw FP arms",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_PLAYER,
		"setter": &"set_debug_force_viewmodel",
		"tooltip": "Keep the first-person arms + weapon drawn in every camera mode (debug experiment).",
	},
	{
		"id": &"body_in_first_person",
		"label": "Show body in first person",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_PLAYER,
		"setter": &"set_debug_body_in_first_person",
		"tooltip": "Draw your own body in first person — look down to see your legs and feet (debug experiment; expect the head/shoulders to clip the camera).",
	},
]


## The registry row for `id`, or {} for unknown ids.
static func find(id: StringName) -> Dictionary:
	for option in OPTIONS:
		if option["id"] == id:
			return option
	return {}
