extends RefCounted

## The mounted-vehicle panel lane of GameHudPresenter: while the local player
## rides something, join the root vehicle's items.def sid to its hudpos
## VEHICLE_HUD block and hand the overlay the block + the rider's stance; the
## hull band and the seat rows never round-trip through script (the overlay
## pulls them natively through Simulation.fill_vehicle_panel).
## [orig: HUD_DrawVehicleHealthBars @0x5a4fd0 over the HUD entity-info root;
##  the VEHICLE_END commit keys the block by sid @0x59f370; the panel's one
##  witnessed gate is the block's interface texture @0x5a5038]
##
## The sid join is the shell's because the item database is the shell's: the
## sim reports the root's item id, the database carries the authored sid, and
## the hudpos owns the block.

var _pushed := false


## Clears with the mission (the HUD entity-info block is rebuilt per mission).
func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, hud_pos: HudPos, item_db: ItemDatabase,
		sim: Simulation, stance: int) -> void:
	if hud == null:
		return
	var view: Dictionary = sim.get_vehicle_panel_view() if sim != null else {}
	if not bool(view.get("shown", false)) or hud_pos == null or item_db == null:
		_hide(hud)
		return
	var sid := item_db.get_sid(int(view.get("item_id", 0)))
	var block: Dictionary = hud_pos.get_vehicle_hud(sid) if not sid.is_empty() else {}
	if block.is_empty():
		# No authored block for this vehicle: retail draws no panel for it
		# (the shipped sid whose art is missing behaves the same).
		_hide(hud)
		return
	hud.set_vehicle_panel(true, block, stance, sim)
	_pushed = true


func _hide(hud: HudOverlay) -> void:
	if _pushed:
		hud.set_vehicle_panel(false, {}, 0, null)
		_pushed = false
