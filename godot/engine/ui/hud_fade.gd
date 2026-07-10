class_name HudFade
extends RefCounted

## The hudpos.def ALPHAFADE ramp shared by the ammo/clip indicator and the stance
## indicator cross-fade. The file carries three fields — base %, max %, seconds — that
## the original stores as base×2.55 / max×2.55 (0..255 alpha) and seconds×62 (ticks).
## [orig: alphafade parse @0x5a086c — dbl_7D9A20 = 2.55, dbl_7C88C0 = 62.0]
##
## On a state change the element restamps its change tick; the fade term then decays
## 255 → 0 over the ramp: fade = 255 − u8(u16((elapsed<<16)/ticks − 1) >> 8).
## [orig: draw_hud_ammo_indicator @0x599af9; draw-stance @0x599fc0]

const PERCENT_TO_ALPHA := 2.55
const SECONDS_TO_TICKS := 62.0


## The raw decay term: 255 right after the change (elapsed 1) falling to 0 at the end
## of the ramp. Exact integer translation, including the elapsed-0 wrap quirk (u16
## underflow makes elapsed 0 read as fully decayed — one 62 Hz tick of latency).
static func decay(elapsed_ticks: int, ramp_ticks: int) -> int:
	if ramp_ticks <= 0:
		return 0
	var e := clampi(elapsed_ticks, 0, ramp_ticks)
	var frac := (((e << 16) / ramp_ticks) - 1) & 0xFFFF
	return 255 - ((frac >> 8) & 0xFF)


## The ammo/clip indicator's flash: alpha jumps toward base+255 on a change and decays
## back to base, clamped by the ALPHAFADE max. [orig: draw_hud_ammo_indicator @0x599af9]
static func flash_alpha(elapsed_ticks: int, ramp_ticks: int, base_alpha: int, max_alpha: int) -> int:
	return mini(base_alpha + decay(elapsed_ticks, ramp_ticks), max_alpha)


## The stance cross-fade pair: the current frame draws at min(base+fade, 255); the
## previous frame ghosts on top at fade>>2 (the witnessed (fade<<22) alpha-byte trick,
## max 63) until the ramp ends. [orig: draw-stance @0x599fc2..0x59a2e3]
static func stance_current_alpha(elapsed_ticks: int, ramp_ticks: int, base_alpha: int) -> int:
	return mini(base_alpha + decay(elapsed_ticks, ramp_ticks), 255)


static func stance_prev_alpha(elapsed_ticks: int, ramp_ticks: int) -> int:
	return decay(elapsed_ticks, ramp_ticks) >> 2
