class_name MenuCreditsOverlays
extends RefCounted

# CBIN credits scrollers mounted over marquee widgets of the current screen.
# The scroller overlays are frame CHILDREN, outside the compiled draw walk, so
# this owner re-applies the walk's shown gate (widget + ancestors + overrides
# — MenuFrame.is_widget_shown) whenever widget visibility changes: a hidden
# tab hides its credits. Rows are {player, id}.

var _players: Array[Dictionary] = []


func clear() -> void:
	for row in _players:
		if is_instance_valid(row.get("player")):
			(row["player"] as CreditsPlayer).queue_free()
	_players.clear()


# Mount one scroller over the marquee widget `id` at `rect`, replaying its
# credits resource. Ownership stays here; the caller re-seeds per screen.
func mount(frame: MenuFrame, id: int, rect: Rect2,
		credits: CbinCreditsResource, autoplay: bool) -> void:
	var player := CreditsPlayer.new()
	player.set_name("Credits")
	player.set_credits_resource(credits)
	player.set_autoplay(autoplay)
	player.position = rect.position
	player.size = rect.size
	player.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(player)
	_players.append({"player": player, "id": id})


# Re-apply the draw walk's shown gate to every mounted overlay.
func sync(frame: MenuFrame, frame_index: Callable) -> void:
	for row in _players:
		var player: CreditsPlayer = row.get("player")
		if not is_instance_valid(player):
			continue
		var index := int(frame_index.call(int(row.get("id", -1))))
		player.visible = index >= 0 and frame.is_widget_shown(index)
