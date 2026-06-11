class_name EditorIconLibrary
extends RefCounted

## Single home for the editor's SVG icon set (modtools/editor/ui/icons/). Icons
## are addressed by a StringName id — WorkspaceDef.icon_id rows, the shell's
## action/toggle buttons — and resolve to the imported SVG texture, cached after
## the first load. An unknown id resolves to null (the button stays text-only),
## so a missing glyph degrades instead of erroring.
##
## The set is hand-authored in-repo (16x16, one consistent stroke style); adding
## an icon = dropping <id>.svg in the directory and referencing the id.

const ICON_DIR := "res://modtools/editor/ui/icons"

static var _cache: Dictionary = {}


static func resolve(icon_id: StringName) -> Texture2D:
	if icon_id == &"":
		return null
	if _cache.has(icon_id):
		return _cache[icon_id]
	var path := "%s/%s.svg" % [ICON_DIR, String(icon_id)]
	var texture: Texture2D = load(path) if ResourceLoader.exists(path) else null
	_cache[icon_id] = texture
	return texture
