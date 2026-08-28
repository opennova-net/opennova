#pragma once

// The retail Options screen's authored controls, pinned: the CScrollWnd
// ranges every witnessed Options slider seeds, and the ONE renderer
// configuration OpenNova ports (the highest-quality pixel-shader path) as
// read-only VIDEO rows. The presenting shell applies these through its menu
// driver (widget selection and range writes are device work); the values are
// the engine's.

#include <cstddef>
#include <cstdint>

namespace opennova::menu {

// The named Options CScrollWnd setup: {min, max, page}, page being the
// original inclusive-page field. The reimplementation does not yet own
// persisted render/audio/input settings, so current = min is the
// deterministic seed; a settings owner replaces it through the driver's
// range setter. [orig: options_screen_init @0x554800;
// UI_PopulateRenderAndAudioSettings @0x55c830; the untouched page=10 default
// in CScrollWnd_Construct @0x64c450]
struct OptionsScrollRange {
    const char *control;
    int32_t minimum;
    int32_t maximum;
    int32_t page;
};
inline constexpr OptionsScrollRange kOptionsScrollRanges[] = {
    {"GAMMA", 5, 20, 2},
    {"SOUNDFXVOLUME", 0, 255, 10},
    {"DIALOGVOLUME", 0, 255, 10},
    {"MUSICVOLUME", 0, 255, 10},
    {"MOUSE_SENSITIVITY", 4, 511, 10},
};

// OpenNova ports exactly one retail renderer configuration: the
// highest-quality, pixel-shader path. The authored JO VIDEO controls stay
// visible as documentation, pinned to that contract and read-only. These are
// semantic item values, not row numbers. [orig: the options.mnu VIDEO rows
// are populated by UI_PopulateRenderAndAudioSettings @0x55c830 (TERRAINPOLY
// @0x55cddc, FBEFFECTS @0x55d009), synced by UI_SyncRenderSettingsToWidgets
// @0x55a140, the VIDEODEFAULT/VIDEOPERFORMANCE/VIDEOQUALITY presets
// registered at UI_RegisterOptionsCallbacks @0x55d697..0x55d6d3, and every
// value range pinned by Settings_ClampGraphicsOptions @0x54d4a0 (0..3 rungs,
// ANTIALIAS 0..16 @0x54d575..0x54d584)]
struct VideoQualityControl {
    const char *control;
    const char *semantic_value;
};
inline constexpr VideoQualityControl kVideoQualityControls[] = {
    {"16x9DISPLAY", "1"},
    {"TERRAINPOLY", "3"},
    {"TERRAINTEX", "3"},
    {"OBJECTPOLY", "3"},
    {"OBJECTTEX", "3"},
    // The authored 3..16 rows are placeholders; mode 2 is the highest
    // multisample mode supported by the retail device contract.
    {"ANTIALIAS", "2"},
    {"SHADERUSAGE", "2"},
    {"WATERQUALITY", "3"},
    {"SHADOWQUALITY", "3"},
    {"PARTICLES", "2"},
    {"FBEFFECTS", "3"},
    {"TEXFILTER", "3"},
    // "Minimal" means minimal compression and therefore maximum fidelity.
    {"TEXCOMPRESSION", "2"},
};

// The registered retail comparison profile's gamma reference. Gamma is
// calibration, not a quality rung, so pushing it to the numeric maximum would
// deliberately distort the parity target.
inline constexpr int32_t kVideoGammaReference = 8;

// The three preset buttons the read-only policy disables.
inline constexpr const char *kVideoPresetButtons[] = {
    "VIDEODEFAULT", "VIDEOPERFORMANCE", "VIDEOQUALITY",
};

} // namespace opennova::menu
