class_name MusicVolumeMeter
extends Control

# Persistent L/R volume bars fed by MusicDirector.volume_changed, which emits
# the script-domain 0..255 volumes directly (the VM hook's 16.16 fixed is
# decoded at the binding's trampoline — witness at that engine home,
# Jointops.exe!Intrinsic_GSV @ 0x6720E0 / GSDV @ 0x672120). Holds the last
# value so the bars don't flicker between the script's volume calls. GSDV
# passes left=0 to mean "left unchanged" (a known fidelity quirk in
# engine/formats/mus); we display channels as received.

const MAX_VOL := 255.0

@onready var _bar_l: ProgressBar = %BarL
@onready var _bar_r: ProgressBar = %BarR
@onready var _readout: Label = %Readout


func _ready() -> void:
	set_volume(0, 0)


# left / right are the signal args (script-domain 0..255).
func set_volume(left: int, right: int) -> void:
	var l: float = float(left)
	var r: float = float(right)
	if _bar_l != null:
		_bar_l.value = l
	if _bar_r != null:
		_bar_r.value = r
	if _readout != null:
		_readout.text = "L %.0f   R %.0f" % [l, r]
