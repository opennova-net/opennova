class_name ItemSeatSpecs
extends RefCounted

# The witnessed attach-command seat-selection MIRROR for the MCP mission tools
# and probes: command gating, seat priority weights, and the per-candidate
# prediction card. The sim performs the real selection; production seat-spec
# EXTRACTION is native (S16: simassets::extract_item_seat_specs via
# Simulation's boot install + ItemDatabase.extract_seat_specs_for_item
# for tooling) — the GDScript extraction that lived here is gone.

# Local aliases of the binding's single-source codes — Simulation pins
# these to engine/runtime/world by static_assert, so a native change can never
# silently drift past this file's command walks.
const SEAT_NONE := Simulation.SEAT_NONE
const SEAT_PASSENGER := Simulation.SEAT_PASSENGER
const SEAT_CONTROLLER := Simulation.SEAT_CONTROLLER
const SEAT_GUNNER := Simulation.SEAT_GUNNER
const SEAT_DRIVER := Simulation.SEAT_DRIVER

const COMMAND_PASSENGER_ONLY := Simulation.MOUNT_COMMAND_PASSENGER_ONLY
const COMMAND_SKIP_CONTROLLER := Simulation.MOUNT_COMMAND_SKIP_CONTROLLER
const COMMAND_ANY_SEAT := Simulation.MOUNT_COMMAND_ANY_SEAT


static func seat_type_label(seat_type: int) -> String:
	match seat_type:
		SEAT_PASSENGER:
			return "passenger"
		SEAT_CONTROLLER:
			return "controller"
		SEAT_GUNNER:
			return "gunner"
		SEAT_DRIVER:
			return "driver"
	return "none"


static func command_rule(command_id: int) -> Dictionary:
	match command_id:
		COMMAND_PASSENGER_ONLY:
			return {
				"id": command_id,
				"mode": "passenger_only",
				"description": "command 123 accepts sitex/passenger seats only",
			}
		COMMAND_SKIP_CONTROLLER:
			return {
				"id": command_id,
				"mode": "no_controller",
				"description": "command 124 rejects ctrlx/controller seats",
			}
		COMMAND_ANY_SEAT:
			return {
				"id": command_id,
				"mode": "any_seat",
				"description": "command 125 accepts passenger, controller, driver, and gunner seats",
			}
	return {
		"id": command_id,
		"mode": "not_mount_command",
		"description": "not an attach-to-seat command",
	}


static func command_allows_seat(command_id: int, seat_type: int) -> bool:
	if seat_type == SEAT_NONE:
		return false
	match command_id:
		COMMAND_PASSENGER_ONLY:
			return seat_type == SEAT_PASSENGER
		COMMAND_SKIP_CONTROLLER:
			return seat_type != SEAT_CONTROLLER
		COMMAND_ANY_SEAT:
			return true
	return false


static func seat_priority_weight(seat: Dictionary) -> int:
	match int(seat.get("type", SEAT_NONE)):
		SEAT_CONTROLLER, SEAT_DRIVER:
			return 0x2000
		SEAT_GUNNER:
			return 0x20000
		SEAT_PASSENGER:
			return 0x200000
	return 0x7fffffff


static func predict_best_seat(seats: Array, command_id: int) -> Dictionary:
	var candidates: Array = []
	var best_index := -1
	var best_weight := 0x7fffffff
	for i in range(seats.size()):
		var seat: Dictionary = seats[i]
		var seat_type := int(seat.get("type", SEAT_NONE))
		var occupied := bool(seat.get("occupied", false))
		var allowed := command_allows_seat(command_id, seat_type)
		var weight := seat_priority_weight(seat)
		var status := "eligible"
		var reason := ""
		if occupied:
			status = "skipped"
			reason = "occupied"
		elif not allowed:
			status = "skipped"
			reason = "command_filter"
		elif weight < best_weight:
			best_weight = weight
			best_index = i
		var candidate := seat.duplicate(true)
		candidate["index"] = i
		candidate["type_label"] = seat_type_label(seat_type)
		candidate["eligible"] = allowed and not occupied
		candidate["weight"] = weight
		candidate["status"] = status
		candidate["skip_reason"] = reason
		candidates.append(candidate)
	if best_index >= 0:
		var selected: Dictionary = candidates[best_index]
		selected["status"] = "selected"
		candidates[best_index] = selected
		return {
			"command": command_rule(command_id),
			"seat_index": best_index,
			"seat": selected,
			"candidates": candidates,
		}
	return {
		"command": command_rule(command_id),
		"seat_index": -1,
		"seat": {},
		"candidates": candidates,
	}
