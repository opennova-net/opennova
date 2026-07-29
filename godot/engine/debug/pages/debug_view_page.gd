class_name DebugViewPage
extends NovaDebugPage
## The last unhomed render-debug toggles. This page is being dissolved: each
## toggle moves to the page that owns its domain as those pages land
## (skeletons + user points -> Animation & models; hide foliage -> Terrain &
## foliage), and the page is deleted when it empties. Collision moved to
## Rounds & collision; the FP viewmodel experiments moved to Player.

func page_id() -> StringName:
	return &"View"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)
	add_option_check(&"show_skeletons")
	add_option_check(&"show_user_points")
	add_option_check(&"hide_foliage")
