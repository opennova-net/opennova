extends GutTest

# =============================================================================
# ENG-1 — environment parity vectors (docs/maturity-program.md, ENG track).
#
# Pins the GDScript-visible outputs of the environment stack —
# NovaEnvironment / NovaWeather / NovaSky / NovaWater (+ the reachable
# NovaCelestial math via EnvFile statics), godot/engine/environment/*.gd —
# against committed vectors over a deterministic in-code corpus, so ENG-2 can
# port the math into libs/env and delete the GDScript with these staying green
# pre/post. Structure mirrors tests/novaworld/nw_codec_identity_test.cpp (the
# NET-0 identity gate): deterministic corpus, committed inline table, dump
# mode, and the same update discipline.
#
# POLICY (VERBATIM grade — docs/env/env-tod-re.md is the witness record):
# - These vectors were dumped ONCE from the current GDScript port, which is
#   itself the [orig]-cited port (citations live in the files under pin).
# - A vector divergence during a port/refactor means RE-GRILL against the
#   binary (grill-ida, Jointops.exe) — NEVER widen a tolerance, NEVER re-dump
#   to make a refactor pass. Updating any vector requires a witness citation
#   ([orig: Name @ 0xADDR] or a D-/divergence entry) in the same commit.
# - Divergences are pinned AS SHIPPED, never "corrected" (env-tod-re.md
#   Divergences table + docs/env/env-honored-matrix.md):
#     #14 glare occlusion held at full brightness (dot^32 curve only),
#     #17 iris auto-exposure unconsumed (getters pinned, no consumer),
#     #19 get_terrain_lighting_attenuation returns identity Vector3.ONE,
#     #20 sky dome as the C7 structural port (builder formula pinned).
#   #21 CLOSED 2026-07-05: the frame-clear horizon blend is ported and
#   pinned (get_frame_clear_color; [orig: Environment_UpdateWeatherTick
#   @ 0x57e9b0 blend @ 0x57f037..0x57f0a1]) - the re-dump in the same
#   commit carries that witness, per the policy above.
#   #22-#24 CLOSED 2026-07-05 (the ENG-2 weather-core port): the wa/wb/we
#   sway states and wc/long_* keys were re-dumped under fresh witnesses -
#   the PRNG signed carry [orig: Environment_UpdateWeatherTick
#   @ 0x57e9fc..0x57ea16; seed @ Environment_SnapStateToTargets @ 0x57d1e0],
#   lightning SET-per-epoch + integer additives [orig: @ 0x57ec6f/@ 0x57ed0a;
#   Environment_SetLightningFlash @ 0x57d320], the witnessed ambient
#   Env_WindScale 256 + stable-envelope strength scale
#   [orig: Environment_InitDefaults @ 0x57c1d1], and keyframe-target chasing
#   [orig: Environment_ComputeTimeOfDayColors @ 0x57de40]. wa/* is now the
#   witnessed ambient-256 series, byte-equal to the env_render_unit ctest
#   landmarks; wc/short_seq and every color-grid/sky/water/celestial/
#   smoother key were UNCHANGED by the port. Details: env-tod-re.md #22-#24.
#   #25 CLOSED 2026-07-06 (the ENG-2 sky-leg re-grill): the PRNG seed is
#   0x12333333 [orig: the mov imm32 @ 0x57d2ff in Environment_SnapStateToTargets
#   @ 0x57d1e0] - 0x12345633 was a transcription error shared with the WAC RNG
#   (same constant @ 0x4f966b, libs/wac fixed in the same commit). The
#   sway-bearing keys re-dumped under that witness: wa/k004..k256, wb/k016..k096,
#   wc/long_k01/k09/k12, we/k008..k064 (the sway token only; wa/k001 is
#   seed-invariant - both seeds share low-12 bits at tick 1). Every level-only
#   (wc/*_seq), color, float, and non-weather key was UNCHANGED.
#   #26 CLOSED 2026-07-06 (the ENG-2 sky binding slice): sky/k001 + sky/k064
#   re-dumped under the witnessed scroll model - the RAMPING rate (snap
#   refreshes only the target @ 0x57d2da; smooth-eighth @ 0x57eecc), integer
#   accumulators [orig: @ 0x57f1a5..0x57f1d1], and the render-side UV
#   translation with the accumulator NEGATIVE on U
#   [orig: render_skybox @ 0x5791de..0x579260] (the old float port added it
#   positively on both axes and skipped the ramp). Key shape is now the four
#   pushed offsets. sky/verts re-dumped: the mesh comes from
#   libs/env build_sky_dome_mesh (float32-stored, v22.z last-digit shift);
#   sky/mesh (counts + witnessed winding) byte-identical.
#
# TOLERANCE POLICY (stated here, enforced in the compare helpers — these are
# the ONLY two tolerances):
# - BYTE-EXACT for integer-math outputs: all smoothed/interpolated colors,
#   lightning intensity, and PRNG/sway-derived values compare via the
#   int(x * 255.0 + 0.5) idiom (nova_color_smoother_test.gd) or via exact
#   recovery of the underlying integer (sway amount/phase invert to the
#   16-bit sway state and the 8-bit ring index). Encoded as hex strings in
#   EXPECTED_BYTES; compared with assert_eq.
# - EPSILON 1e-4 via assert_almost_eq for float outputs: directions, scroll
#   accumulators/offsets, heights, fog distances, dome vertex samples, phase
#   blends. Encoded as float arrays in EXPECTED_FLOATS.
#
# NOT PINNED (honest gaps, no fake greens):
# - NovaCelestial node-level placement/visibility/glare application: needs a
#   NovaResourceRoot with retail 3DI models (asset-gated; with no bodies the
#   process path returns before the math). The separable math is pinned via
#   public surface instead: EnvFile.compute_sun_glare sweeps and the
#   EnvRenderConstants.DOME_DISTANCE constant. height_scale
#   (sky_height / 175.69) and the dir.y > -0.1 visibility threshold live only
#   inside that gated path — header-noted, not vectored.
# - NovaWater terrain-fallback height rung: needs a loaded NovaTerrainData
#   (asset). The env-driven and override rungs ARE pinned.
# - NovaWeather internal state (PRNG word, sway rings, fade timers) is
#   private; pinned only through public getters and the colors written back
#   to NovaEnvironment (ADR 0018 — no private pokes; ratchet stays flat).
#
# REGEN (the dumped-ONCE event, or after a witnessed change):
#   OPENNOVA_ENV_VECTORS_DUMP=1 "$GODOT_BIN" --headless --path godot \
#     -s addons/gut/gut_cmdln.gd -gtest=res://tests/env_parity_vectors_test.gd -gexit
# prints the replacement EXPECTED_BYTES/EXPECTED_FLOATS blocks, then FAILS
# loudly so a dump run is never mistaken for a green run.
# =============================================================================

const NovaEnvironmentScript = preload("res://engine/environment/nova_environment.gd")
const NovaWeatherScript = preload("res://engine/environment/nova_weather.gd")
const NovaSkyScript = preload("res://engine/environment/nova_sky.gd")
const NovaWaterScript = preload("res://engine/environment/nova_water.gd")
const NovaCelestialScript = preload("res://engine/environment/nova_celestial.gd")

# The engine tick [docs/engine-primer.md: 62 Hz].
const TICK := 1.0 / 62.0
const FLOAT_EPSILON := 1e-4

# time_of_day grid: midnight, the 05:40->06:20 sunrise ramp (switch 06:00),
# noon, the 18:25->19:05 sunset ramp (switch 18:45), late night
# [orig: Environment_ComputeTimeOfDayColors @ 0x57de40 windows].
const GRID_TIMES: Array[int] = [0, 550, 600, 615, 1200, 1830, 1845, 1900, 2200]

# Glare sweep inputs (EnvFile.compute_sun_glare pins the dot^32 curve
# [orig: compute_sun_glare_and_fog_blend @ 0x5ad610]).
const GLARE_DOTS: Array[float] = [-1.0, -0.5, 0.0, 0.25, 0.5, 0.75, 0.9, 0.95, 0.975, 0.99, 1.0]
const GLARE_BRIGHTNESS: Array[int] = [0, 32, 64, 128, 192, 255]

# ---------------------------------------------------------------------------
# Committed vectors — regenerate ONLY per the policy header (dumped ONCE).

const EXPECTED_BYTES := {
	"c0/t0000": "2F4256 0E1D2D 23243B 02040C 000000 2F4256 23243B 23243B 000000 23243B 0E1D2D 0E1D2D 02040C 010206 383B27 FFFFFF FFFFFF 808080 01 02",
	"c0/t0000/water": "1D2524 CC",
	"c0/t0550": "18222C 1F2A2D 3B3D4A 4C5A8C 535351 18222C 3B3D4A 3B3D4A 535351 3B3D4A 1F2A2D 1F2A2D 4C5A8C 262D46 383B27 FFFFFF FFFFFF 808080 01 02",
	"c0/t0550/water": "20271F CC",
	"c0/t0600": "555554 202A2E 3C3E4A 4E5E90 555554 18212B 3C3E4A 3C3E4A 555554 3C3E4A 202A2E 202A2E 4E5E90 272F48 383B27 FFFFFF FFFFFF 808080 00 02",
	"c0/t0600/water": "343828 CC",
	"c0/t0615": "595957 202B2E 3D3F4B 526096 595957 172029 3D3F4B 3D3F4B 595957 3D3F4B 202B2E 202B2E 526096 29304B 383B27 FFFFFF FFFFFF 808080 00 02",
	"c0/t0615/water": "353929 CC",
	"c0/t1200": "AAAAA7 31372E 545859 9AB6FF AAAAA7 000000 545859 545859 AAAAA7 545859 31372E 31372E 9AB6FF 4D5B8A 383B27 FFFFFF FFFFFF 808080 00 02",
	"c0/t1200/water": "595F3F CC",
	"c0/t1830": "4E4E4C 1E292D 393C49 485684 4E4E4C 19242F 393C49 393C49 4E4E4C 393C49 1E292D 1E292D 485684 242B42 383B27 FFFFFF FFFFFF 808080 00 02",
	"c0/t1830/water": "313526 CC",
	"c0/t1845": "1A2530 1D282D 383B48 445280 4A4A49 1A2530 383B48 383B48 4A4A49 383B48 1D282D 1D282D 445280 222940 383B27 FFFFFF FFFFFF 808080 01 02",
	"c0/t1845/water": "20271F CC",
	"c0/t1900": "1B2732 1D282D 373A47 424E7A 474745 1B2732 373A47 373A47 474745 373A47 1D282D 1D282D 424E7A 21273D 383B27 FFFFFF FFFFFF 808080 01 02",
	"c0/t1900/water": "202720 CC",
	"c0/t2200": "273748 14212C 2B2D40 1C2238 1C1C1C 273748 2B2D40 2B2D40 1C1C1C 2B2D40 14212C 14212C 1C2238 0E111C 383B27 FFFFFF FFFFFF 808080 01 02",
	"c0/t2200/water": "1E2622 CC",
	"c1/t0000": "17212B 070E16 11121D 000206 000000 17212B 11121D 11121D 000000 11121D 070E16 070E16 000206 000003 145078 C89664 FFFFFF 5A7896 01 03",
	"c1/t0000/water": "051937 80",
	"c1/t0550": "0C1116 0F1416 1D1F24 242C46 292928 0C1116 1D1F24 1D1F24 292928 1D1F24 0F1416 0F1416 242C46 121623 145078 C89664 FFFFFF 5A7896 01 03",
	"c1/t0550/water": "051A2F 80",
	"c1/t0600": "2B2B2A 101517 1E1F25 262E48 2B2B2A 0C1116 1E1F25 1E1F25 2B2B2A 1E1F25 101517 101517 262E48 131724 145078 C89664 FFFFFF 5A7896 00 03",
	"c1/t0600/water": "09263D 80",
	"c1/t0615": "2C2C2B 101517 1E2025 28304A 2C2C2B 0B1015 1E2025 1E2025 2C2C2B 1E2025 101517 101517 28304A 141825 145078 C89664 FFFFFF 5A7896 00 03",
	"c1/t0615/water": "09273E 80",
	"c1/t1200": "555553 181B17 2A2C2C 4C5A8A 555553 000000 2A2C2C 2A2C2C 555553 2A2C2C 181B17 181B17 4C5A8A 262D45 145078 C89664 FFFFFF 5A7896 00 03",
	"c1/t1200/water": "0F415F 80",
	"c1/t1830": "272726 0F1416 1C1E24 222A42 272726 0C1217 1C1E24 1C1E24 272726 1C1E24 0F1416 0F1416 222A42 111521 145078 C89664 FFFFFF 5A7896 00 03",
	"c1/t1830/water": "08233A 80",
	"c1/t1845": "0D1318 0E1416 1C1D24 222840 252524 0D1318 1C1D24 1C1D24 252524 1C1D24 0E1416 0E1416 222840 111420 145078 C89664 FFFFFF 5A7896 01 03",
	"c1/t1845/water": "051A30 80",
	"c1/t1900": "0D1319 0E1316 1B1D23 20263C 232323 0D1319 1B1D23 1B1D23 232323 1B1D23 0E1316 0E1316 20263C 10131E 145078 C89664 FFFFFF 5A7896 01 03",
	"c1/t1900/water": "051A30 80",
	"c1/t2200": "131C24 0A1016 15161F 0C101C 0E0E0E 131C24 15161F 15161F 0E0E0E 15161F 0A1016 0A1016 0C101C 06080E 145078 C89664 FFFFFF 5A7896 01 03",
	"c1/t2200/water": "051934 80",
	"c2/t0000": "2F4256 0E1D2D 23243B FFFFFF 000000 2F4256 23243B 23243B 000000 23243B 0E1D2D 0E1D2D 02040C A69586 1E3C5A FFFFFF FFFFFF 808080 01 01",
	"c2/t0000/water": "0F2653 59",
	"c2/t0550": "18222C 1F2A2D 3B3D4A FFFFFF 535351 18222C 3B3D4A 3B3D4A 535351 3B3D4A 1F2A2D 1F2A2D 4C5A8C AC9D91 1E3C5A FFFFFF FFFFFF 808080 01 01",
	"c2/t0550/water": "112749 59",
	"c2/t0600": "555554 202A2E 3C3E4A FFFFFF 555554 18212B 3C3E4A 3C3E4A 555554 3C3E4A 202A2E 202A2E 4E5E90 AC9D91 1E3C5A FFFFFF FFFFFF 808080 00 01",
	"c2/t0600/water": "1C395D 59",
	"c2/t0615": "595957 202B2E 3D3F4B FFFFFF 595957 172029 3D3F4B 3D3F4B 595957 3D3F4B 202B2E 202B2E 526096 AD9D91 1E3C5A FFFFFF FFFFFF 808080 00 01",
	"c2/t0615/water": "1C3A5F 59",
	"c2/t1200": "AAAAA7 31372E 545859 FFFFFF AAAAA7 000000 545859 545859 AAAAA7 545859 31372E 31372E 9AB6FF B3A59C 1E3C5A FFFFFF FFFFFF 808080 00 01",
	"c2/t1200/water": "2F6191 59",
	"c2/t1830": "4E4E4C 1E292D 393C49 FFFFFF 4E4E4C 19242F 393C49 393C49 4E4E4C 393C49 1E292D 1E292D 485684 AC9D90 1E3C5A FFFFFF FFFFFF 808080 00 01",
	"c2/t1830/water": "1A3558 59",
	"c2/t1845": "1A2530 1D282D 383B48 FFFFFF 4A4A49 1A2530 383B48 383B48 4A4A49 383B48 1D282D 1D282D 445280 AC9C90 1E3C5A FFFFFF FFFFFF 808080 01 01",
	"c2/t1845/water": "112749 59",
	"c2/t1900": "1B2732 1D282D 373A47 FFFFFF 474745 1B2732 373A47 373A47 474745 373A47 1D282D 1D282D 424E7A AB9C8F 1E3C5A FFFFFF FFFFFF 808080 01 01",
	"c2/t1900/water": "11274A 59",
	"c2/t2200": "273748 14212C 2B2D40 FFFFFF 1C1C1C 273748 2B2D40 2B2D40 1C1C1C 2B2D40 14212C 14212C 1C2238 A89889 1E3C5A FFFFFF FFFFFF 808080 01 01",
	"c2/t2200/water": "102650 59",
	"celestial/glare_occlusion": "0000 1805 300A 6014 901E C028",
	"celestial/glare_sweep": "0000 0000 0000 0000 0000 0000 0600 2500 5501 8B0B C028",
	"envfile/derived": "9AFFFF 9A9A9A 7A5F43",
	"sky/flat": "01 5A7896",
	"sky/mesh": "441 2400 0 22 21 0 1 22 1 23 22 1 2 23",
	"smoother/clamped": "FE0000 FD0000 FC0000 FB0000",
	"smoother/decay": "DF7038 C36231 AB562B 954B26 834221 72391D 643219 582C16",
	"smoother/rise": "201810 3C2D1E 543F2A 6A4F35 7C5D3E 8D6947 9B744E A77D54",
	"smoother/snap_get": "336699",
	"wa/k001": "0012 01 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wa/k004": "035F 04 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wa/k016": "56E5 10 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wa/k064": "7CDE 40 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wa/k256": "9787 00 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"water/mesh": "4225 24576 0 65 1 1 65 66 1 66 2 2 66 67",
	"wb/k001": "0009 01 00 09 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wb/k016": "429E 10 00 07 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wb/k064": "71F1 40 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wb/k096": "833D 60 00 00 31372E AAAAA7 9AB6FF 545859 31372E AAAAA7 9AB6FF",
	"wc/long_k01": "007B 02 C8 00 24313D 273748 3D435B 6D6F86 24313D 273748 3D435B",
	"wc/long_k09": "23AB 0A 64 00 1C2934 273748 2C3249 4C4E63 1C2934 273748 2C3249",
	"wc/long_k12": "3E47 0D 00 00 14212C 273748 1C2238 2B2D40 14212C 273748 1C2238",
	"wc/long_seq": "C8 C8 C8 96 96 C8 C8 96 64 32 32 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
	"wc/short_seq": "00 00 00 00 00 C8 C8 C8 C8 FF FF C8 C8 FF FF 00",
	"we/k008": "14B3 08 00 00 212B2D 5D5D5C 56659D 3E414B 212B2D 5D5D5C 56659D",
	"we/k016": "56E5 10 00 00 202A2D 585857 516094 3C3F4A 202A2D 585857 516094",
	"we/k032": "6404 20 00 00 1D282D 2A323A 44517F 383B48 1D282D 2A323A 44517F",
	"we/k064": "7CDE 40 00 00 17242D 23313F 2A3351 303243 17242D 23313F 2A3351",
}

const EXPECTED_FLOATS := {
	"c0/consts": [1000.000000000, 2.000000000, 15.000000000, 175.000000000, 0.800000012, 1.000000000, 1200.000000000, -1.000000000, 15.000000000, 1.000000000, 25.408004761, 0.000000000],
	"c0/t0000": [-0.500005603, 0.866022229, -0.000000076, 1.000000000, 500.000000000, 1000.000000000],
	"c0/t0550": [-0.500005543, 0.037776366, -0.865197897, 0.500137389, 500.000000000, 1000.000000000],
	"c0/t0600": [-0.342015058, 0.000000041, 0.939694524, 0.000000000, 500.000000000, 1000.000000000],
	"c0/t0615": [-0.342015028, 0.061458986, 0.937682509, 0.750183165, 500.000000000, 1000.000000000],
	"c0/t1200": [-0.342015058, 0.939694524, -0.000000082, 1.000000000, 500.000000000, 1000.000000000],
	"c0/t1830": [-0.342015028, -0.122654945, -0.931655228, 0.749633670, 500.000000000, 1000.000000000],
	"c0/t1845": [-0.500005603, 0.168952495, 0.849381864, 0.000549451, 500.000000000, 1000.000000000],
	"c0/t1900": [-0.500005603, 0.224142894, 0.836513221, 0.750732601, 500.000000000, 1000.000000000],
	"c0/t2200": [-0.500005603, 0.749997258, 0.433011025, 1.000000000, 500.000000000, 1000.000000000],
	"c0/water_params": [0.000000000, 0.003547668],
	"c1/consts": [400.000000000, 3.000000000, 30.000000000, 250.000000000, 0.500000000, 0.500000000, 1830.000000000, 3.000000000, 15.000000000, 1.000000000, 76.000000000, 0.000000000],
	"c1/t0000": [-0.500005603, 0.866022229, -0.000000076, 1.000000000, 100.000000000, 400.000000000],
	"c1/t0550": [-0.500005543, 0.037776366, -0.865197897, 0.500137389, 100.000000000, 400.000000000],
	"c1/t0600": [-0.342015058, 0.000000041, 0.939694524, 0.000000000, 100.000000000, 400.000000000],
	"c1/t0615": [-0.342015028, 0.061458986, 0.937682509, 0.750183165, 100.000000000, 400.000000000],
	"c1/t1200": [-0.342015058, 0.939694524, -0.000000082, 1.000000000, 100.000000000, 400.000000000],
	"c1/t1830": [-0.342015028, -0.122654945, -0.931655228, 0.749633670, 100.000000000, 400.000000000],
	"c1/t1845": [-0.500005603, 0.168952495, 0.849381864, 0.000549451, 100.000000000, 400.000000000],
	"c1/t1900": [-0.500005603, 0.224142894, 0.836513221, 0.750732601, 100.000000000, 400.000000000],
	"c1/t2200": [-0.500005603, 0.749997258, 0.433011025, 1.000000000, 100.000000000, 400.000000000],
	"c1/water_params": [1.500000000, 0.007095337],
	"c2/consts": [300.000000000, 1.000000000, 15.000000000, 175.000000000, 0.349999994, 1.000000000, 630.000000000, 2.000000000, 15.000000000, 1.000000000, 107.992004395, 0.000000000],
	"c2/t0000": [-0.500005603, 0.866022229, -0.000000076, 1.000000000, 0.500000000, 300.000000000],
	"c2/t0550": [-0.500005543, 0.037776366, -0.865197897, 0.500137389, 0.500000000, 300.000000000],
	"c2/t0600": [-0.342015058, 0.000000041, 0.939694524, 0.000000000, 0.500000000, 300.000000000],
	"c2/t0615": [-0.342015028, 0.061458986, 0.937682509, 0.750183165, 0.500000000, 300.000000000],
	"c2/t1200": [-0.342015058, 0.939694524, -0.000000082, 1.000000000, 0.500000000, 300.000000000],
	"c2/t1830": [-0.342015028, -0.122654945, -0.931655228, 0.749633670, 0.500000000, 300.000000000],
	"c2/t1845": [-0.500005603, 0.168952495, 0.849381864, 0.000549451, 0.500000000, 300.000000000],
	"c2/t1900": [-0.500005603, 0.224142894, 0.836513221, 0.750732601, 0.500000000, 300.000000000],
	"c2/t2200": [-0.500005603, 0.749997258, 0.433011025, 1.000000000, 0.500000000, 300.000000000],
	"c2/water_params": [1.000000000, 0.003547668],
	"celestial/dome_distance": [2000.000000000],
	"dir/t0000": [-0.342015058, -0.939694524, 0.000000000, -0.500005603, 0.866022229, -0.000000076],
	"dir/t0550": [-0.342015058, -0.040990192, 0.938800037, -0.500005543, 0.037776366, -0.865197897],
	"dir/t0600": [-0.342015058, 0.000000041, 0.939694524, -0.500005603, -0.000000010, -0.866022229],
	"dir/t0615": [-0.342015028, 0.061458986, 0.937682509, -0.500005543, -0.056640457, -0.864167988],
	"dir/t1200": [-0.342015058, 0.939694524, -0.000000082, -0.500005603, -0.866022229, 0.000000151],
	"dir/t1830": [-0.342015028, -0.122654945, -0.931655228, -0.500005603, 0.113038644, 0.858613253],
	"dir/t1845": [-0.342015058, -0.183325395, -0.921638548, -0.500005603, 0.168952495, 0.849381864],
	"dir/t1900": [-0.342015058, -0.243210807, -0.907675147, -0.500005603, 0.224142894, 0.836513221],
	"dir/t2200": [-0.342015058, -0.813799441, -0.469847023, -0.500005603, 0.749997258, 0.433011025],
	"envfile/fog_type0": [0.000000000, 0.004158883, 1000.000000000],
	"sky/anchor": [512.000000000, 32.000000000, -256.000000000, 175.000000000, 64.000000000],
	"sky/flat": [250.000000000],
	"sky/k001": [0.124992847, -0.062492847, 0.062495232, -0.031247616],
	"sky/k064": [0.121737681, -0.059237681, 0.060325161, -0.030162523],
	"sky/verts": [0.000000000, 175.690628052, 0.000000000, 15.821670532, 175.263931274, 48.694095612, -0.000044760, 132.723480225, -512.000000000, 0.000179042, -0.000005395, 1024.000000000],
	"water/override": [42.500000000, 7.000000000],
	"water/snap": [96.000000000, 7.000000000, -64.000000000],
	"we/k008": [1751.612903226],
	"we/k016": [1803.225806452],
	"we/k032": [1906.451612903],
	"we/k064": [2112.903225806],
}


# ---------------------------------------------------------------------------
# Corpus: three deterministic EnvFile configs (no fixture files, no assets).

func _make_cfg(index: int) -> EnvFile:
	var env := EnvFile.new()
	env.reset_to_default()
	match index:
		1:
			# Exercises: envscale getter-side scaling, fog_type 3 (quarter
			# start), env-driven water height, flat cloud pass, resized dome.
			env.set_envscale(0.5)
			env.set_fog_type(3)
			env.set_fog_level(400.0)
			env.set_water_height(3.0)
			env.set_sky_height(250.0)
			env.set_sky_speed(30.0)
			env.set_advanced_clouds(0)
			env.set_water_murk(0.5)
			env.set_cloud_tint(Color8(90, 120, 150))
			env.set_water_color(Color8(20, 80, 120))
			env.set_lightning_color(Color8(120, 110, 200))
			env.set_ceiling_color(Color8(70, 60, 50))
			env.set_floor_color(Color8(30, 20, 10))
			env.set_terrain_tint(Color8(200, 150, 100))
			env.set_curtime(1830)
		2:
			# Exercises: fog_type 1 (0.5-unit start) + the BMS mission
			# override layer as a live view (env_file_test.gd precedent).
			env.set_fog_type(1)
			env.apply_mission_overrides({
				"water_height": 2.0,
				"fog_level": 300.0,
				"fog_color": Color8(200, 180, 160),
				"water_color": Color8(30, 60, 90),
				"water_murk": 0.35,
				"start_time": 630,
			})
	return env


func _add_env_node(cfg: EnvFile, node_name: String) -> Node:
	var env_node: Node = NovaEnvironmentScript.new()
	env_node.name = node_name
	env_node.environment_data = cfg
	add_child_autofree(env_node)
	return env_node


func _add_weather_node(env_name: String, node_name: String) -> Node:
	var weather: Node = NovaWeatherScript.new()
	weather.name = node_name
	weather.environment_path = NodePath("../" + env_name)
	add_child_autofree(weather)
	return weather


func _add_camera(position: Vector3) -> Camera3D:
	var cam: Camera3D = add_child_autofree(Camera3D.new())
	cam.global_position = position
	cam.make_current()
	return cam


# ---------------------------------------------------------------------------
# Encoding helpers (the tolerance policy in executable form).

static func _hex_color(value: Vector3) -> String:
	return "%02X%02X%02X" % [
		int(value.x * 255.0 + 0.5),
		int(value.y * 255.0 + 0.5),
		int(value.z * 255.0 + 0.5),
	]


static func _hex_color_c(value: Color) -> String:
	return _hex_color(Vector3(value.r, value.g, value.b))


static func _hex_byte(value: int) -> String:
	return "%02X" % value


static func _byte_of(value: float) -> int:
	return int(value * 255.0 + 0.5)


# Sway getters invert exactly to the integer state they expose:
# amount = (s - 0x8000) / 0x8000 * 2  ->  s = round(amount * 0x4000) + 0x8000
# phase = index * (TAU / 256)         ->  index = round(phase * 256 / TAU)
static func _sway_state_hex(weather: Node) -> String:
	var sway := int(roundf(weather.get_sway_amount() * 16384.0)) + 32768
	var index := int(roundf(weather.get_sway_phase() * 256.0 / TAU))
	return "%04X %02X" % [sway, index]


static func _weather_checkpoint(weather: Node, env_node: Node) -> String:
	var parts := PackedStringArray()
	parts.append(_sway_state_hex(weather))
	parts.append(_hex_byte(_byte_of(weather.get_lightning_intensity())))
	parts.append(_hex_byte(weather.get_wind_duration()))
	parts.append(_hex_color(weather.get_smooth_fill()))
	parts.append(_hex_color(weather.get_smooth_sun()))
	parts.append(_hex_color(weather.get_smooth_fog()))
	parts.append(_hex_color(weather.get_smooth_sky()))
	# The colors the tick writes back onto the env node (fill/sun/fog).
	parts.append(_hex_color(env_node.get_fill_light()))
	parts.append(_hex_color(env_node.get_sun_light()))
	parts.append(_hex_color(env_node.get_fog_color()))
	return " ".join(parts)


# Per-cell env-node color snapshot. Token order:
# sun_light fill_light sky_ambient fog_color sun moon skybase skybright
# skyhighlight cloudbase cloudhighlight cloudedge skyfog water terrain_tint
# terrain_atten cloud_tint is_night fog_type
static func _env_cell_bytes(env_node: Node) -> String:
	var parts := PackedStringArray()
	var colors := [
		env_node.get_sun_light(),
		env_node.get_fill_light(),
		env_node.get_sky_ambient(),
		env_node.get_fog_color(),
		env_node.get_sun_color(),
		env_node.get_moon_color(),
		env_node.get_sky_base(),
		env_node.get_sky_bright(),
		env_node.get_sky_highlight(),
		env_node.get_cloud_base(),
		env_node.get_cloud_highlight(),
		env_node.get_cloud_edge(),
		env_node.get_skyfog_color(),
		env_node.get_frame_clear_color(),
		env_node.get_water_color(),
		env_node.get_terrain_tint(),
		env_node.get_terrain_lighting_attenuation(),
		env_node.get_cloud_tint(),
	]
	for color in colors:
		parts.append(_hex_color(color))
	parts.append("01" if env_node.is_night_phase() else "00")
	parts.append(_hex_byte(env_node.get_fog_type()))
	return " ".join(parts)


# ---------------------------------------------------------------------------
# Collection groups. Each writes stable string keys into the two tables.

func _collect_env_grid(bytes: Dictionary, floats: Dictionary) -> void:
	for cfg_index in 3:
		var cfg := _make_cfg(cfg_index)
		var env_name := "EnvC%d" % cfg_index
		var env_node := _add_env_node(cfg, env_name)

		var water: Node = NovaWaterScript.new()
		water.name = "WaterC%d" % cfg_index
		water.environment_path = NodePath("../" + env_name)
		add_child_autofree(water)

		var water_height_view := cfg.get_water_height() if cfg.has_water_height() else -1.0
		floats["c%d/consts" % cfg_index] = [
			cfg.get_fog_level(),
			float(cfg.get_fog_type()),
			cfg.get_sky_speed(),
			cfg.get_sky_height(),
			cfg.get_water_murk(),
			cfg.get_envscale(),
			float(cfg.get_curtime()),
			water_height_view,
			cfg.get_iris_percent(),
			cfg.get_iris_center(),
			cfg.get_fog_end_underwater(),
			cfg.get_fog_density(),
		]

		for t in GRID_TIMES:
			env_node.time_of_day = float(t)
			var cell := "c%d/t%04d" % [cfg_index, t]
			bytes[cell] = _env_cell_bytes(env_node)
			var light_dir: Vector3 = env_node.get_light_direction()
			floats[cell] = [
				light_dir.x, light_dir.y, light_dir.z,
				env_node.get_day_phase_blend(),
				env_node.get_fog_start(),
				env_node.get_fog_level(),
			]
			# Node-level lit water color for the cell (delegates the EnvFile
			# statics; pinned at the NovaWater output).
			simulate(water, 1, TICK)
			var lit: Vector3 = water.water_material.get_shader_parameter("u_water_color")
			var alpha: float = water.water_material.get_shader_parameter("u_water_alpha")
			bytes[cell + "/water"] = "%s %s" % [_hex_color(lit), _hex_byte(_byte_of(alpha))]

		floats["c%d/water_params" % cfg_index] = [
			water.water_height,
			water.water_material.get_shader_parameter("u_scroll_speed"),
		]


func _collect_directions(floats: Dictionary) -> void:
	var cfg := _make_cfg(0)
	for t in GRID_TIMES:
		var sun := cfg.compute_sun_direction(float(t))
		var moon := cfg.compute_moon_direction(float(t))
		floats["dir/t%04d" % t] = [sun.x, sun.y, sun.z, moon.x, moon.y, moon.z]


func _collect_weather(bytes: Dictionary, floats: Dictionary) -> void:
	# WA — still air: the sway spring settling from rest, colors snapped at
	# noon and stable (target == smoothed after the writeback loop).
	var env_a := _add_env_node(_make_cfg(0), "EnvWA")
	env_a.time_of_day = 1200.0
	var weather_a := _add_weather_node("EnvWA", "WeatherA")
	var ticks_done := 0
	for checkpoint in [1, 4, 16, 64, 256]:
		simulate(weather_a, checkpoint - ticks_done, TICK)
		ticks_done = checkpoint
		bytes["wa/k%03d" % checkpoint] = _weather_checkpoint(weather_a, env_a)

	# WB — wind: strength 50 => intensity 4096, duration 10 s => 60 ticks,
	# then the (x*31)>>5 decay. PRNG seed is the compile-time constant, so the
	# sway sequence is a pure function of tick count.
	var env_b := _add_env_node(_make_cfg(0), "EnvWB")
	env_b.time_of_day = 1200.0
	var weather_b := _add_weather_node("EnvWB", "WeatherB")
	weather_b.wind_strength = 50.0
	weather_b.set_wind_duration(10)
	ticks_done = 0
	for checkpoint in [1, 16, 64, 96]:
		simulate(weather_b, checkpoint - ticks_done, TICK)
		ticks_done = checkpoint
		bytes["wb/k%03d" % checkpoint] = _weather_checkpoint(weather_b, env_b)

	# WC — lightning at night (2200): the short 16-tick and long 32-tick
	# sequencers, plus the additive injection into sky/fog/fill (not sun).
	var env_c := _add_env_node(_make_cfg(0), "EnvWC")
	env_c.time_of_day = 2200.0
	var weather_c := _add_weather_node("EnvWC", "WeatherC")
	simulate(weather_c, 1, TICK) # snap tick before triggering
	weather_c.trigger_lightning_short()
	var short_seq := PackedStringArray()
	for _i in 16:
		simulate(weather_c, 1, TICK)
		short_seq.append(_hex_byte(_byte_of(weather_c.get_lightning_intensity())))
	bytes["wc/short_seq"] = " ".join(short_seq)

	var env_d := _add_env_node(_make_cfg(0), "EnvWD")
	env_d.time_of_day = 2200.0
	var weather_d := _add_weather_node("EnvWD", "WeatherD")
	simulate(weather_d, 1, TICK)
	weather_d.trigger_lightning_long()
	var long_seq := PackedStringArray()
	for i in 32:
		simulate(weather_d, 1, TICK)
		long_seq.append(_hex_byte(_byte_of(weather_d.get_lightning_intensity())))
		var after := i + 1
		if after == 1 or after == 9 or after == 12:
			bytes["wc/long_k%02d" % after] = _weather_checkpoint(weather_d, env_d)
	bytes["wc/long_seq"] = " ".join(long_seq)

	# WE — day-advance chase: day_speed drives the TOD forward every tick
	# (env first, weather second — the process-priority order), so the
	# smoothers chase a moving keyframe-interpolated target across the sunset
	# switch. Start 1700, day_speed 400 => ~+6.45 HHMM units per tick.
	var env_e := _add_env_node(_make_cfg(0), "EnvWE")
	env_e.time_of_day = 1700.0
	env_e.day_speed = 400.0
	var weather_e := _add_weather_node("EnvWE", "WeatherE")
	ticks_done = 0
	for checkpoint in [8, 16, 32, 64]:
		for _i in checkpoint - ticks_done:
			simulate(env_e, 1, TICK)
			simulate(weather_e, 1, TICK)
		ticks_done = checkpoint
		bytes["we/k%03d" % checkpoint] = _weather_checkpoint(weather_e, env_e)
		floats["we/k%03d" % checkpoint] = [env_e.time_of_day]


func _collect_sky(bytes: Dictionary, floats: Dictionary) -> void:
	var cam := _add_camera(Vector3(512.0, 64.0, -256.0))
	var env_node := _add_env_node(_make_cfg(0), "EnvSky0")
	env_node.time_of_day = 1200.0

	var sky: Node = NovaSkyScript.new()
	sky.name = "Sky0"
	sky.environment_path = NodePath("../EnvSky0")
	add_child_autofree(sky)

	# Dome mesh invariants [orig: build_sky_dome_mesh @ 0x578db0]: 21x21 =
	# 441 vertices, 20*20*2 = 800 triangles (2400 indices).
	var mesh: ArrayMesh = sky.mesh_instance.mesh
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	var index_head := PackedStringArray()
	for i in 12:
		index_head.append(str(indices[i]))
	bytes["sky/mesh"] = "%d %d %s" % [positions.size(), indices.size(), " ".join(index_head)]
	# Sampled dome vertices (apex row, first ring, mid dome, outer corner).
	var samples: Array[int] = [0, 22, 220, 440]
	var vertex_floats: Array = []
	for sample_index in samples:
		var v := positions[sample_index]
		vertex_floats.append_array([v.x, v.y, v.z])
	floats["sky/verts"] = vertex_floats

	# Scroll UV offsets after fixed ticks at sky_speed 15: the weather core's
	# RAMPING rate (the snap refreshes only the target — the rate climbs by
	# smooth-eighth from 0) + integer accumulators through the witnessed UV
	# translation, accumulator NEGATIVE on U — here via the standalone
	# fallback core (no weather node in this group)
	# [orig: rate ramp @ 0x57eecc; accumulators @ 0x57f1a5..0x57f1d1;
	#  consumption @ 0x5791de..0x579260].
	var ticks_done := 0
	for checkpoint in [1, 64]:
		simulate(sky, checkpoint - ticks_done, TICK)
		ticks_done = checkpoint
		var off1: Vector2 = sky.sky_material.get_shader_parameter("u_scroll_offset1")
		var off2: Vector2 = sky.sky_material.get_shader_parameter("u_scroll_offset2")
		floats["sky/k%03d" % checkpoint] = [off1.x, off1.y, off2.x, off2.y]
	# Dome anchor rides at half camera height [orig: render_skybox @ 0x5790d0].
	var dome_pos: Vector3 = sky.mesh_instance.global_position
	var sky_height: float = sky.sky_material.get_shader_parameter("u_sky_height")
	floats["sky/anchor"] = [dome_pos.x, dome_pos.y, dome_pos.z, sky_height, cam.global_position.y]

	# advanced_clouds 0 flat pass [orig: render_skybox @ 0x579b42]: the dome
	# flat-shades with cloud_tint (cfg1).
	var env_flat := _add_env_node(_make_cfg(1), "EnvSkyFlat")
	env_flat.time_of_day = 1200.0
	var sky_flat: Node = NovaSkyScript.new()
	sky_flat.name = "SkyFlat"
	sky_flat.environment_path = NodePath("../EnvSkyFlat")
	add_child_autofree(sky_flat)
	simulate(sky_flat, 1, TICK)
	var flat_pass: bool = sky_flat.sky_material.get_shader_parameter("u_flat_pass")
	var flat_color: Vector3 = sky_flat.sky_material.get_shader_parameter("u_flat_color")
	bytes["sky/flat"] = "%s %s" % ["01" if flat_pass else "00", _hex_color(flat_color)]
	var flat_height: float = sky_flat.sky_material.get_shader_parameter("u_sky_height")
	floats["sky/flat"] = [flat_height]


func _collect_water_mesh(bytes: Dictionary, floats: Dictionary) -> void:
	# Mesh invariants: 65x65 grid = 4225 vertices, 64*64*6 = 24576 indices.
	var water: Node = add_child_autofree(NovaWaterScript.new())
	var mesh: ArrayMesh = water.mesh_instance.mesh
	var arrays := mesh.surface_get_arrays(0)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	var index_head := PackedStringArray()
	for i in 12:
		index_head.append(str(indices[i]))
	bytes["water/mesh"] = "%d %d %s" % [positions.size(), indices.size(), " ".join(index_head)]

	# Height ladder, override rung: host-driven height wins; clearing (NAN)
	# hands control back (terrain_environment_preview_test.gd precedent).
	water.set_height_override(42.5)
	var override_height: float = water.water_height
	water.set_height_override(NAN)
	water.water_height = 7.0
	floats["water/override"] = [override_height, water.water_height]

	# Camera-following snap: the plane snaps to a 32-unit grid under the camera.
	var cam := _add_camera(Vector3(100.3, 7.0, -33.7))
	assert_not_null(cam, "headless camera injection must succeed")
	simulate(water, 1, TICK)
	var snap: Vector3 = water.mesh_instance.global_position
	floats["water/snap"] = [snap.x, snap.y, snap.z]


func _collect_celestial(bytes: Dictionary, floats: Dictionary) -> void:
	# Node-level celestial placement is asset-gated (see header). The
	# separable math is pinned through the public surface it delegates to.
	var sweep := PackedStringArray()
	for dot in GLARE_DOTS:
		var glare: Dictionary = EnvFile.compute_sun_glare(dot, 255)
		sweep.append("%02X%02X" % [int(glare.get("glare", 0)), int(glare.get("fog_whiten", 0))])
	bytes["celestial/glare_sweep"] = " ".join(sweep)

	var occlusion := PackedStringArray()
	for brightness in GLARE_BRIGHTNESS:
		var glare: Dictionary = EnvFile.compute_sun_glare(1.0, brightness)
		occlusion.append("%02X%02X" % [int(glare.get("glare", 0)), int(glare.get("fog_whiten", 0))])
	bytes["celestial/glare_occlusion"] = " ".join(occlusion)

	floats["celestial/dome_distance"] = [NovaCelestialScript.EnvRenderConstants.DOME_DISTANCE]


func _collect_statics(bytes: Dictionary, floats: Dictionary) -> void:
	# NovaColorSmoother snap/step sequences [orig: interpolate_weather_color
	# @ 0x57d9e0] — guards the statics' GDScript-visible contract during the
	# ENG-2 host thinning.
	var decay := NovaColorSmoother.new()
	decay.snap(Color(1.0, 0.5, 0.25))
	var decay_seq := PackedStringArray()
	for _i in 8:
		decay_seq.append(_hex_color_c(decay.step(Color(0.0, 0.0, 0.0), 255.0)))
	bytes["smoother/decay"] = " ".join(decay_seq)

	var rise := NovaColorSmoother.new()
	rise.snap(Color(0.0, 0.0, 0.0))
	var rise_seq := PackedStringArray()
	for _i in 8:
		rise_seq.append(_hex_color_c(rise.step(Color(1.0, 0.75, 0.5), 255.0)))
	bytes["smoother/rise"] = " ".join(rise_seq)

	var clamped := NovaColorSmoother.new()
	clamped.snap(Color(1.0, 0.0, 0.0))
	var clamped_seq := PackedStringArray()
	for _i in 4:
		clamped_seq.append(_hex_color_c(clamped.step(Color(0.0, 0.0, 0.0), 1.0)))
	bytes["smoother/clamped"] = " ".join(clamped_seq)

	var snapper := NovaColorSmoother.new()
	snapper.snap(Color(0.2, 0.4, 0.6))
	bytes["smoother/snap_get"] = _hex_color_c(snapper.get_current())

	# EnvFile derived-color statics [orig: Environment_UpdateWeatherTick
	# @ 0x57f0b3..0x57f1b1].
	var doubled := EnvFile.double_saturate_color(Color(0.3, 0.5, 0.7))
	var combined := EnvFile.combine_terrain_light(Color(0.5, 0.5, 0.5), Color(0.25, 0.25, 0.25))
	var lit := EnvFile.lit_water_color(Color(0.4, 0.31, 0.22), combined)
	bytes["envfile/derived"] = "%s %s %s" % [
		_hex_color_c(doubled), _hex_color_c(combined), _hex_color_c(lit),
	]

	# fog_type 0 exponential policy (no grid config uses it; pinned directly).
	var exp_env := EnvFile.new()
	exp_env.reset_to_default()
	exp_env.set_fog_type(0)
	floats["envfile/fog_type0"] = [
		exp_env.get_fog_start(),
		exp_env.get_fog_density(),
		exp_env.get_fog_end_distance(),
	]


func _collect_all() -> Array:
	var bytes := {}
	var floats := {}
	_collect_env_grid(bytes, floats)
	_collect_directions(floats)
	_collect_weather(bytes, floats)
	_collect_sky(bytes, floats)
	_collect_water_mesh(bytes, floats)
	_collect_celestial(bytes, floats)
	_collect_statics(bytes, floats)
	return [bytes, floats]


# ---------------------------------------------------------------------------
# Dump mode (mirrors NW_CODEC_DUMP): print the replacement table, fail loudly.

static func _sorted_keys(table: Dictionary) -> Array:
	var keys := table.keys()
	keys.sort()
	return keys


static func _print_dump(bytes: Dictionary, floats: Dictionary) -> void:
	print("### BEGIN ENV VECTOR TABLE — paste over EXPECTED_BYTES/EXPECTED_FLOATS ###")
	print("const EXPECTED_BYTES := {")
	for key in _sorted_keys(bytes):
		print("\t\"%s\": \"%s\"," % [key, bytes[key]])
	print("}")
	print("")
	print("const EXPECTED_FLOATS := {")
	for key in _sorted_keys(floats):
		var parts := PackedStringArray()
		for value in floats[key]:
			parts.append("%.9f" % float(value))
		print("\t\"%s\": [%s]," % [key, ", ".join(parts)])
	print("}")
	print("### END ENV VECTOR TABLE ###")


func test_environment_parity_vectors() -> void:
	var tables := _collect_all()
	var bytes: Dictionary = tables[0]
	var floats: Dictionary = tables[1]

	var dump := OS.get_environment("OPENNOVA_ENV_VECTORS_DUMP")
	if dump != "" and dump != "0":
		_print_dump(bytes, floats)
		fail_test("OPENNOVA_ENV_VECTORS_DUMP run — table printed above; a dump run is never green. Re-run without the env var to verify.")
		return

	if EXPECTED_BYTES.is_empty() or EXPECTED_FLOATS.is_empty():
		fail_test("No committed vectors. Generate the dumped-ONCE table via OPENNOVA_ENV_VECTORS_DUMP=1 (see header) and paste it in.")
		return

	assert_eq(_sorted_keys(bytes), _sorted_keys(EXPECTED_BYTES),
		"Byte-vector key sets must match (corpus drifted or table stale — policy header applies).")
	assert_eq(_sorted_keys(floats), _sorted_keys(EXPECTED_FLOATS),
		"Float-vector key sets must match (corpus drifted or table stale — policy header applies).")

	for key in _sorted_keys(EXPECTED_BYTES):
		if not bytes.has(key):
			continue
		assert_eq(bytes[key], EXPECTED_BYTES[key],
			"byte-exact vector '%s' (a divergence means re-grill, never re-dump)" % key)

	for key in _sorted_keys(EXPECTED_FLOATS):
		if not floats.has(key):
			continue
		var actual: Array = floats[key]
		var expected: Array = EXPECTED_FLOATS[key]
		assert_eq(actual.size(), expected.size(), "float vector '%s' arity" % key)
		if actual.size() != expected.size():
			continue
		var worst := 0.0
		var worst_index := 0
		for i in actual.size():
			var delta: float = absf(float(actual[i]) - float(expected[i]))
			if delta > worst:
				worst = delta
				worst_index = i
		assert_almost_eq(float(actual[worst_index]), float(expected[worst_index]), FLOAT_EPSILON,
			"float vector '%s'[%d] (epsilon 1e-4 — a divergence means re-grill, never widen)" % [key, worst_index])
