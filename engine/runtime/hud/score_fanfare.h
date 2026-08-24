#pragma once

// The S2C 0x81 hit-confirm tones (net-re 0x81): the client stores the
// requester-local accumulated points and, on a POSITIVE delta with the session
// var EXP_FANFARE armed (lo byte nonzero and lo < hi), picks one of three
// sound-bank trigger sets by the delta's size and plays it as a 2D interface
// sound behind the cfg key `enable_slotmachine` (default 0).
// [orig: NapiNPClientMsg_ScoreDeltaSound @0x42a0b0 — the compare ladder
//  @0x42a0e0..0x42a11d: delta >= hi -> HEADSHOTTONE (g_snd_HEADSHOTTONE
//  @0x24e09fc), delta >= lo -> KILLTONE (@0x24e09f8), else HITTONE
//  (@0x24e09f4); play Sound_PlayInterfaceTriggerSet @0x527be0 (ex
//  "PlaySoundOnDedicatedServer") gated on g_EnableSlotMachine @0x25508ac
//  (Config_SetDefaults @0x54d165 stores 0; config token "enable_slotmachine"
//  @0x54fdd8..0x54fdfb); the three names are rows 81..83 of the 84-row
//  {char name[32]; int *handle} registry @0x82f590 DialogSystem_Init
//  @0x527687 resolves through SoundBank_FindTriggerByName @0x75be90]
// The EXP_FANFARE var itself is the server-info VarList KV (u16: lo byte = the kill
// threshold, hi byte = the headshot threshold) the joiner lands with
// parse_server_session_variables @0x520478; the authority sources it from
// score.ini's EXP_FANFARE directive (word_24C1170).

#include <cstdint>

namespace opennova::hud {

enum class ScoreTone { None, Hit, Kill, Headshot };

// The exact selection order of the handler [orig: @0x42a0e0..0x42a11d].
inline ScoreTone score_delta_tone(int32_t delta, uint16_t exp_fanfare) {
	const uint32_t lo = exp_fanfare & 0xFFu;
	const uint32_t hi = (exp_fanfare >> 8) & 0xFFu;
	if (delta <= 0) return ScoreTone::None;
	if (lo == 0 || lo >= hi) return ScoreTone::None;
	const uint32_t d = static_cast<uint32_t>(delta);
	if (d >= lo) return d >= hi ? ScoreTone::Headshot : ScoreTone::Kill;
	return ScoreTone::Hit;
}

// The trigger-set names [orig: the 0x82f590 registry rows 81..83].
constexpr const char *score_tone_set_name(ScoreTone tone) {
	switch (tone) {
		case ScoreTone::Hit: return "HITTONE";
		case ScoreTone::Kill: return "KILLTONE";
		case ScoreTone::Headshot: return "HEADSHOTTONE";
		default: return "";
	}
}

} // namespace opennova::hud
