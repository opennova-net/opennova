class_name FiredSoundset
extends RefCounted

## One positional one-shot MissionAudio fired (fire_soundset / slot_soundset):
## the value the recent-fires ring keeps for diagnostics and the GUT pins
## (ADR 0018 read seam; the players themselves live under the audio root).

var set_name := ""
var position := Vector3.ZERO
var source_bms_id := 0
var exclusive_key := ""  # the every-tick refire slot key (slot sounds only)
var slot := false  # slot_soundset (true) vs fire_soundset (false)
var played := false  # the bank found the set and spawned a player
