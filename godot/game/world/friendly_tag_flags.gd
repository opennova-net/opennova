class_name FriendlyTagFlags
extends RefCounted

## The HudOverlay friendly-tag flag word (godot/src/hud/hud_overlay.h documents
## the bits): the presenter packs the sim's per-tag facts into it so the
## compiler's element (engine/runtime/hud hud_frame.cpp) reads one int per tag.

const MEDIC := 1            # the red-cross plate (charattr Medic)
const SPEAKING := 2         # the voice pulse
const PLAYER := 4           # a player-slot entry (empty callsign draws the bar)
const DEAD := 8             # entity Flags & 2
const HAS_SLOT := 16        # a connection slot owns the entity
const MEDIC_REQUEST := 32   # the slot's medic-request latch (the pulse)
const REVIVE_SHIFT := 8     # bits 8..15: the slot's revive countdown seconds
const REVIVE_MAX := 255


static func pack(tag: Dictionary) -> int:
	var flags := 0
	if bool(tag.get("medic", false)):
		flags |= MEDIC
	if bool(tag.get("player", false)):
		flags |= PLAYER
	if bool(tag.get("dead", false)):
		flags |= DEAD
	if bool(tag.get("has_slot", false)):
		flags |= HAS_SLOT
	if bool(tag.get("medic_request", false)):
		flags |= MEDIC_REQUEST
	flags |= clampi(int(tag.get("revive_seconds", 0)), 0, REVIVE_MAX) << REVIVE_SHIFT
	return flags
