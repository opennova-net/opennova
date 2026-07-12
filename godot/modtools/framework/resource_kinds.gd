class_name ResourceKinds
extends RefCounted

## The shared resource-kind vocabulary (B12). Two translations every jump/label
## surface used to copy-paste:
##
## - jump_kind(): index/reference kinds -> the kind the owning workspace
##   registers under for open_in_workspace. The reference index speaks
##   "object_model"/"sbf"; the Object and Music workspaces answer as
##   "object"/"music".
## - label(): the artist-facing noun for a kind (empty-state copy, tooltips).
##
## Per-surface vocabularies stay local to their surface on purpose (e.g. the
## browser pane's filter dropdown) — only the shared translations live here.

const JUMP_KIND := {
	"object_project": "object",
	"object_model": "object",
	"object_scene": "object",
	"sbf": "music",
	"music_script": "music",
}


## The workspace-jump kind for an index/reference kind; identity when no
## translation applies.
static func jump_kind(kind: String) -> String:
	return JUMP_KIND.get(kind, kind)


## Artist-facing noun for a resource kind.
static func label(kind: String) -> String:
	match kind:
		"terrain":
			return "terrain"
		"environment":
			return "environment"
		"mission":
			return "mission"
		"strings":
			return "strings"
		"sound":
			return "sound"
		"font":
			return "font"
		"credits":
			return "credits"
		"menu":
			return "menu"
		"menu_style":
			return "menu style"
		"hudpos":
			return "HUD layout"
		"avatar":
			return "character"
		"particle":
			return "particle effect"
		"object", "object_project", "object_model", "object_scene":
			return "object"
		"music", "sbf", "music_script":
			return "music"
		_:
			return "resource"
