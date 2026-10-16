class_name ScoreFanfarePresenter
extends RefCounted

## The S2C 0x81 hit-confirm tone lane: one 2D interface play per positive
## score delta that clears the EXP_FANFARE thresholds, behind the retail cfg
## key `enable_slotmachine` (default 0). The tone itself is the sim's decision
## (engine/runtime/hud/score_fanfare.h); this lane only plays it.
## [orig: NapiNPClientMsg_ScoreDeltaSound @0x42a0b0 -> Sound_PlayInterfaceTriggerSet
##  @0x527be0 (ex "PlaySoundOnDedicatedServer") when g_GameConfigState.enableSlotMachine_1F4
##  @0x25508ac; Config_SetDefaults @0x54d165 stores 0; parsed by atol @0x54fdfb]

# The word in the process's cfg block (GameCfgStore), read at each landing as
# the handler reads g_GameConfigState at the play [orig: the
# enableSlotMachine_1F4 test @0x42257d], so a block change after the HUD's
# construction applies (the Options screens read and write the same word,
# @0x554b67 / @0x55ab3d).
const CONFIG_KEY := "enable_slotmachine"


## Consume the sim's score-feedback edge and play its tone set when enabled.
func update(sim: Simulation, world: GameWorld) -> void:
	if sim == null:
		return
	var feedback := sim.take_score_feedback()
	if feedback == null:
		return
	var tone := feedback.tone
	if tone.is_empty() or GameCfgStore.block().get_word(CONFIG_KEY) == 0 or world == null:
		return
	var audio: MissionAudio = world.get_mission_audio()
	if audio != null:
		audio.ui_soundset(tone)
