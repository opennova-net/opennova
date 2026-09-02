class_name ScoreFanfarePresenter
extends RefCounted

## The S2C 0x81 hit-confirm tone lane: one 2D interface play per positive
## score delta that clears the EXP_FANFARE thresholds, behind the retail cfg
## key `enable_slotmachine` (default 0). The tone itself is the sim's decision
## (engine/runtime/hud/score_fanfare.h); this lane only plays it.
## [orig: NapiNPClientMsg_ScoreDeltaSound @0x42a0b0 -> Sound_PlayInterfaceTriggerSet
##  @0x527be0 (ex "PlaySoundOnDedicatedServer") when g_EnableSlotMachine
##  @0x25508ac; Config_SetDefaults @0x54d165 stores 0; parsed by atol @0x54fdfb]

const CONFIG_PATH := "user://settings.cfg"
const CONFIG_SECTION := "hud"
const CONFIG_KEY := "enable_slotmachine"

var _enabled: bool = int(ConfigStore.read(CONFIG_PATH, CONFIG_SECTION, CONFIG_KEY, 0)) != 0


## Consume the sim's score-feedback edge and play its tone set when enabled.
func update(sim: Simulation, world: GameWorld) -> void:
	if sim == null:
		return
	var feedback: Dictionary = sim.take_score_feedback()
	if feedback.is_empty():
		return
	var tone := String(feedback.get("tone", ""))
	if tone.is_empty() or not _enabled or world == null:
		return
	var audio: MissionAudio = world.get_mission_audio()
	if audio != null:
		audio.ui_soundset(tone)


func set_enabled(enabled: bool) -> void:
	_enabled = enabled
	ConfigStore.write(CONFIG_PATH, CONFIG_SECTION, CONFIG_KEY, 1 if enabled else 0)


func is_enabled() -> bool:
	return _enabled
