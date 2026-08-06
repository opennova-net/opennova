class_name NovaDebugOptions
## The declarative registry of the original host-owned debug checks. The
## shared NovaDebugCatalog turns these rows into typed controls whose targets
## are resolved live and whose public setters/getters are called directly.
##
## Every mutation must route through the shared NovaDebugSession, including
## page-specific transport, vars, terrain, environment, and dynamic audio-bus
## controls. Pages may register controls and render their live state, but they
## never call engine setters directly. Option values are deliberately not
## persisted.

const KIND_CHECK := 0
const KIND_SLIDER := 1  # row carries "min"/"max"/"step"
const KIND_ENUM := 2    # row carries "choices": Array[String]; value = index
const KIND_ACTION := 3

## The live GameWorld host.
const TARGET_WORLD := &"world"
## The local player host (LocalPlayerPresenter).
const TARGET_PLAYER := &"player"

const OPTIONS: Array[Dictionary] = [
	{
		"id": &"show_skeletons",
		"label": "Show skeletons",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Animation",
		"getter": &"is_skeleton_debug",
		"setter": &"set_skeleton_debug",
		"expensive": true,
		"tooltip": "Draw character bones (joint-to-parent lines + axis crosses) over the world.",
	},
	{
		"id": &"show_user_points",
		"label": "Show user points",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Animation",
		"getter": &"is_user_point_debug",
		"setter": &"set_user_point_debug",
		"expensive": true,
		"tooltip": "Draw every named model user point as a cyan marker + label, following live animated bones and including static-batched mission objects.",
	},
	{
		"id": &"show_collision",
		"label": "Show collision",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Rounds",
		"getter": &"is_collision_debug",
		"setter": &"set_collision_debug",
		"expensive": true,
		"tooltip": "Draw object collision volumes (type-colored boxes) and the player's capsule test points over the world.",
	},
	{
		"id": &"hide_foliage",
		"label": "Hide foliage",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Terrain",
		"getter": &"is_foliage_hidden",
		"setter": &"set_foliage_hidden",
		"tooltip": "Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it.",
	},
	{
		"id": &"hide_particles",
		"label": "Hide particles",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Particles",
		"getter": &"is_particles_hidden",
		"setter": &"set_particles_hidden",
		"tooltip": "Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all.",
	},
	{
		"id": &"show_effect_boxes",
		"label": "Show effect boxes",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Particles",
		"getter": &"is_particle_debug",
		"setter": &"set_particle_debug",
		"expensive": true,
		"tooltip": "Draw a red wireframe box (retail's debug box color) and effect name over every live emitter. Missing textures stay in the Particles catalog issues report.",
	},
	{
		"id": &"show_portal_faces",
		"label": "Show portal faces",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Occlusion",
		"getter": &"is_occlusion_debug",
		"setter": &"set_occlusion_debug",
		"expensive": true,
		"tooltip": "Draw every nearby building's occlusion faces over the world — windows, portals and welded links as colored outlines with section labels, plain occluder faces in gray.",
	},
	{
		"id": &"show_round_trails",
		"label": "Show round trails",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Rounds",
		"getter": &"is_round_debug",
		"setter": &"set_round_debug",
		"expensive": true,
		"tooltip": "Draw the recent round outcomes over the world — flight segments and hit markers colored by result (green = face hit, amber = sphere stand-in, red ring = a graze whose face test missed and flew on).",
	},
	{
		"id": &"show_hit_meshes",
		"label": "Show hit meshes",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_WORLD,
		"page": &"Rounds",
		"getter": &"is_hitbox_debug",
		"setter": &"set_hitbox_debug",
		"expensive": true,
		"tooltip": "Hit geometry is sampled at 6 Hz. Draw nearby hit geometry within 80 mission units of the local player: object bullet meshes and broad-phase spheres, plus posed person bone spheres (local player omitted; up to 96 targets). Person colors show normal-infantry damage zones: orange = x1.25 (0-4), cyan = x1.0 (5-8), lime = x0.5 (9-12/15-18), magenta = x3.0 head (13-14), dark red = masked, amber = unresolved fallback.",
	},
	{
		"id": &"force_fp_arms",
		"label": "Always draw FP arms",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_PLAYER,
		"page": &"Player",
		"getter": &"is_debug_force_viewmodel",
		"setter": &"set_debug_force_viewmodel",
		"tooltip": "Keep the first-person arms + weapon drawn in every camera mode (debug experiment).",
	},
	{
		"id": &"body_in_first_person",
		"label": "Show body in first person",
		"kind": KIND_CHECK,
		"default": false,
		"target": TARGET_PLAYER,
		"page": &"Player",
		"getter": &"is_debug_body_in_first_person",
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
