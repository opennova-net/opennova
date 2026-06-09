class_name MusicVolumeMeter
extends Control

# Persistent L/R volume bars fed by NovaMusicDirector.volume_changed. The VM
# reports volume in 16.16 fixed-point (clamp(arg,0,255)<<16) per
# Jointops.exe!Intrinsic_GSV @ 0x6720E0 / GSDV @ 0x672120; we render the
# decoded 0..255 value. Holds the last value so the bars don't flicker between
# the script's volume calls. GSDV passes left=0 to mean "left unchanged" (a
# known fidelity quirk in libs/mus); we display channels as received.

const MAX_VOL := 255.0

@onready var _bar_l: ProgressBar = %BarL
@onready var _bar_r: ProgressBar = %BarR
@onready var _readout: Label = %Readout


func _ready() -> void:
	set_volume(0, 0)


# left_16_16 / right_16_16 are the raw signal args (16.16 fixed-point).
func set_volume(left_16_16: int, right_16_16: int) -> void:
	var l: float = left_16_16 / 65536.0
	var r: float = right_16_16 / 65536.0
	if _bar_l != null:
		_bar_l.value = l
	if _bar_r != null:
		_bar_r.value = r
	if _readout != null:
		_readout.text = "L %.0f   R %.0f" % [l, r]
