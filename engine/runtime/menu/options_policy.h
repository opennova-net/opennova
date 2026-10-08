#pragma once

// The retail Options screen's authored controls, pinned: the CScrollWnd
// ranges every witnessed Options slider seeds, and the ONE renderer
// configuration OpenNova ports (the highest-quality pixel-shader path) as
// read-only VIDEO rows. The presenting shell applies these through its menu
// driver (widget selection and range writes are device work); the values are
// the engine's.

#include <formats/mnu/mnu.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::menu {

// The named Options CScrollWnd setup: {min, max, page}, page being the
// original inclusive-page field. PlayerOptions supplies the persisted current
// values after these ranges are installed through the driver's range setter.
// [orig: UI_OptionsScreenInit @0x554800;
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
    {"TERRAINPOLY", "3"},
    {"TERRAINTEX", "3"},
    {"OBJECTTEX", "3"},
    // The authored 3..16 rows are placeholders; mode 2 is the highest
    // multisample mode supported by the retail device contract.
    {"ANTIALIAS", "2"},
    {"SHADERUSAGE", "2"},
    {"WATERQUALITY", "3"},
    {"SHADOWQUALITY", "3"},
    {"PARTICLES", "2"},
    {"FBEFFECTS", "3"},
    // "Minimal" means minimal compression and therefore maximum fidelity.
    {"TEXCOMPRESSION", "2"},
};

// The object-detail rows are served, not pinned: game.cfg's
// `object_polydetail` (renderer/object_lod.h kObjectLodDetailLevelMax's note).
// Each surface selects the row whose item value is the persisted word and
// writes the selected row's value back. The front-end OPTIONS screen authors
// OBJECTPOLY [orig: UI_PopulateRenderAndAudioSettings @0x55ce79..0x55cec1
// locks only the rows above the device's own maximum (dword_2550800, 3 on a
// shader-model-2 device past 120 MiB: RenderSettings_ComputeFromGPUCaps
// @0x587986..0x58798d / @0x5879c2..0x5879d0), so every row is open on the
// device OpenNova ports;
// UI_SyncRenderSettingsToWidgets @0x55a1da..0x55a1f3 (the select by value);
// sub_55A710 @0x55a820..0x55a83b (the ACCEPT's read)]; game.mnu's in-game
// options author the older alias OBJECTDETAIL [orig: UI_OptionsScreenInit
// @0x554997..0x5549b6 (the select by value);
// UI_IngameOptionsDialogEventHandler @0x554e40 — "OBJECTDETAIL" read
// @0x554efb..0x554f0f]. Neither Accept touches the session copy the draw
// reads, so the word reaches the draw at the next mission start.
inline constexpr const char *kObjectDetailControls[] = {"OBJECTPOLY", "OBJECTDETAIL"};

// The texture-filter row is served, not pinned: game.cfg's `texfilter_level`
// (renderer/texture_filter.h). The front-end OPTIONS screen's TEXFILTER
// combobox selects the row whose item value is the persisted word and writes
// the selected row's value back [orig: UI_PopulateRenderAndAudioSettings
// @0x55d059..0x55d0a1 locks only the rows above the device's own maximum
// (g_CfgTexFilterLevelMax, 3 on a shader-model-2 device past 120 MiB:
// RenderSettings_ComputeFromGPUCaps @0x587969..0x587993), so every row is open
// on the device OpenNova ports; UI_SyncRenderSettingsToWidgets
// @0x55a300..0x55a31f (the select by value); sub_55A710 @0x55a97d..0x55a998
// (the ACCEPT's read), @0x55ad65..0x55adb1 (a changed word reloads every
// effect at once)]. The in-game options author no such row.
inline constexpr const char *kTextureFilterControls[] = {"TEXFILTER"};

// The registered retail comparison profile's gamma reference. Gamma is
// calibration, not a quality rung, so pushing it to the numeric maximum would
// deliberately distort the parity target.
inline constexpr int32_t kVideoGammaReference = 8;

// The three preset buttons the read-only policy disables.
inline constexpr const char *kVideoPresetButtons[] = {
    "VIDEODEFAULT", "VIDEOPERFORMANCE", "VIDEOQUALITY",
};

// The authored Options controls the reimpl does not service yet. Retail
// serves every one of them (UPDATE -> UI_LaunchUpdateProcess @0x55b0b0; the
// WDM channel/rate radios -> the Audio_ShutdownAll / Audio_InitSubsystems
// re-init and PunkBuster, read by the dialog's Accept
// [orig: UI_IngameOptionsDialogEventHandler @0x554e40]). The shell shows
// them read-only until each device leg lands — a tracked stand-in
// (D-MNU-21), never an invention. The JOYSTICK device radio is NOT here:
// it is served (the table shows the seeded joystick defaults, D-CTRL-1).
// Nor are the profile's controls words (INVERT_MOUSE, MOUSE_SENSITIVITY,
// ENABLE_JOYSTICK, INVERT_JOYSTICK, ENABLE_FORCE_FEEDBACK,
// OPTIONS_AUTORELOAD, OPTIONS_AUTOMEDIC): OptionsScreen seeds them from and
// writes them back to the current player.sav record
// (runtime/profile/profile_controls.h). Nor is the tip pair
// MR_CLIPPY_KEYBOARD / MR_CLIPPY_HINTS: served, seeded
// from and written back to the two tip words [orig: UI_OptionsScreenInit
// @0x554d79..0x554dc0 / UI_PopulateRenderAndAudioSettings @0x55d48f..0x55d4d6
// (seed), UI_IngameOptionsDialogEventHandler @0x5552c8..0x555301 / sub_55A710
// @0x55aca7..0x55ace0 (write back); hud/tip_system.h TipOptions].
inline constexpr const char *kOptionsUnsupportedControls[] = {
    "DIFFICULTY", "UPDATE",
    "WDM_AUDIO_2", "WDM_AUDIO_4", "WDM_AUDIO_6", "WDM_AUDIO_7",
    "WDM_AUDIO_8", "WDM_RATE",
    "CLIENT_PUNKBUSTER",
};

// The checked state those read-only rows show — what the ported paths do:
// the primary WDM channel on, the rest off.
struct OptionsForcedCheck {
    const char *control;
    bool checked;
};
inline constexpr OptionsForcedCheck kOptionsForcedChecks[] = {
    {"WDM_AUDIO_2", true},
    {"WDM_AUDIO_4", false},
    {"WDM_AUDIO_6", false},
    {"WDM_AUDIO_7", false},
    {"WDM_AUDIO_8", false},
};

// Retail seeds a spinlist BY VALUE: the selected row is the one whose
// authored item `value=` attribute equals the wanted value, and a miss
// selects row 0. The options screen rides it for XHAIR_COLOR (the shipped
// rows author the decimal RGB) and the write-back reads the same field.
// [orig: SpinList_SelectItemByValue @0x64ba50 — the item+4 (stride 56)
//  compare, the miss -> row 0 @0x64ba82; CSpinListWnd_GetSelectedValue
//  @0x64baf0]
inline int spinlist_row_for_value(const mnu::Items &items, std::string_view value) {
    for (size_t i = 0; i < items.items.size(); ++i)
        if (items.items[i].value == value) return static_cast<int>(i);
    return 0;
}

} // namespace opennova::menu
