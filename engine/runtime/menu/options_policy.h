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
#include <cstdlib>
#include <string>
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
    // The authored 3..16 rows are placeholders; mode 2 is the highest
    // multisample mode supported by the retail device contract.
    {"ANTIALIAS", "2"},
    {"SHADERUSAGE", "2"},
    {"WATERQUALITY", "3"},
    {"SHADOWQUALITY", "3"},
    {"FBEFFECTS", "3"},
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
// Options_HandleAcceptOrBack @0x55a820..0x55a83b (the ACCEPT's read)]; game.mnu's in-game
// options author the older alias OBJECTDETAIL [orig: UI_OptionsScreenInit
// @0x554997..0x5549b6 (the select by value);
// UI_IngameOptionsDialogEventHandler @0x554e40 — "OBJECTDETAIL" read
// @0x554efb..0x554f0f]. Neither Accept touches the session copy the draw
// reads, so the word reaches the draw at the next mission start.
// The rows: kOptionsWordControls' OBJECTPOLY and OBJECTDETAIL.

// The texture-filter row is served, not pinned: game.cfg's `texfilter_level`
// (renderer/texture_filter.h). The front-end OPTIONS screen's TEXFILTER
// combobox selects the row whose item value is the persisted word and writes
// the selected row's value back [orig: UI_PopulateRenderAndAudioSettings
// @0x55d059..0x55d0a1 locks only the rows above the device's own maximum
// (g_CfgTexFilterLevelMax, 3 on a shader-model-2 device past 120 MiB:
// RenderSettings_ComputeFromGPUCaps @0x587969..0x587993), so every row is open
// on the device OpenNova ports; UI_SyncRenderSettingsToWidgets
// @0x55a300..0x55a31f (the select by value); Options_HandleAcceptOrBack @0x55a97d..0x55a998
// (the ACCEPT's read), @0x55ad65..0x55adb1 (a changed word reloads every
// effect at once)]. The in-game options author no such row.
// The row: kOptionsWordControls' TEXFILTER.

// The particle-density rows are served, not pinned: game.cfg's
// `particle_density` (renderer/particle_density.h). The front-end OPTIONS
// screen's PARTICLES combobox and the in-game options' PARTICLES row select
// the row whose item value is the persisted word and write the selected row's
// value back [orig: UI_PopulateRenderAndAudioSettings @0x55cfb3..0x55d001
// locks only the rows above the device's own maximum (g_GameConfigState+0x158
// @0x2550810, 2 on every pixel-shader device: RenderSettings_ComputeFromGPUCaps
// @0x587983, @0x587a15), so every row is open on the device OpenNova ports;
// UI_SyncRenderSettingsToWidgets @0x55a2a6..0x55a2cb (the select by value);
// Options_HandleAcceptOrBack @0x55a905..0x55a926 (the ACCEPT's read); UI_OptionsScreenInit
// @0x554a0c..0x554a31 and UI_IngameOptionsDialogEventHandler
// @0x554f14..0x554f35 (the in-game pair)]. Neither Accept touches the session
// copy, so the word reaches the particles at the next mission start.
// The row: kOptionsWordControls' PARTICLES.

// The texture-compression row is served, not pinned: game.cfg's
// `texcompression_level` (renderer/texture_compression.h). The front-end
// OPTIONS screen's TEXCOMPRESSION combobox selects the row whose item value is
// the persisted word and writes the selected row's value back
// [orig: UI_PopulateRenderAndAudioSettings @0x55d0f3..0x55d141 locks only the
// rows above g_GameConfigState+0x170 @0x2550828, 2 on every device
// (RenderSettings_ComputeFromGPUCaps @0x587a43); UI_SyncRenderSettingsToWidgets
// @0x55a34e..0x55a373 (the select by value); Options_HandleAcceptOrBack @0x55a957..0x55a972
// (the ACCEPT's read)]. The in-game options author no such row. The word
// reaches the textures the next mission builds.
// The row: kOptionsWordControls' TEXCOMPRESSION.

// The terrain-texture-detail row is served, not pinned: game.cfg's
// `terrain_texdetail` (renderer/device_texture.h). The front-end OPTIONS
// screen's TERRAINTEX combobox selects the row whose item value is the
// persisted word and writes the selected row's value back
// [orig: UI_PopulateRenderAndAudioSettings @0x55ce2a..0x55ce71 locks only the
// rows above g_GameConfigState.recommendedGraphicsQuality_140+0x04, 3 on a
// shader-model-2 device past 120 MiB (RenderSettings_ComputeFromGPUCaps
// @0x587986..0x58798a), so every row is open on the device OpenNova ports;
// UI_SyncRenderSettingsToWidgets @0x55a1c0..0x55a1cf (the select by value);
// Options_HandleAcceptOrBack @0x55a80a..0x55a815 (the ACCEPT's read)]. The
// in-game options author no such row. The word reaches the terrain the next
// mission builds.
// The row: kOptionsWordControls' TERRAINTEX.

// The object-texture-detail row is served, not pinned: game.cfg's
// `object_texdetail` (renderer/device_texture.h). The front-end OPTIONS
// screen's OBJECTTEX combobox selects the row whose item value is the
// persisted word and writes the selected row's value back
// [orig: UI_PopulateRenderAndAudioSettings @0x55cec9..0x55cf02 locks only the
// rows above g_GameConfigState.recommendedGraphicsQuality_140+0x0C, 3 on a
// shader-model-2 device past 120 MiB with 511 MB of memory
// (RenderSettings_ComputeFromGPUCaps @0x587990, @0x587a6c..0x587a6e), so every
// row is open on the device OpenNova ports; UI_SyncRenderSettingsToWidgets
// @0x55a204..0x55a223 (the select by value); Options_HandleAcceptOrBack
// @0x55a846..0x55a861 (the ACCEPT's read)]. The in-game options author no such
// row. The word reaches the model textures the next mission loads
// (Game_StartMission @0x524a77..0x524a7d -> Material_SetTextureDetailLoadFlags).
// The row: kOptionsWordControls' OBJECTTEX.

// The player options' words the Options screens show and edit (the shell's PlayerOptions
// state; game.cfg's and the profile's words), in this order: the shell hands them over as
// one array by these indices.
enum class OptionsWord : uint8_t {
    SoundFxVolume,
    DialogVolume,
    MusicVolume,
    CrosshairStyle,
    CrosshairColor,
    CrosshairSpread,
    KeyboardTips,
    GameplayTips,
    AspectMode,
    ObjectPolyDetail,
    TexFilterLevel,
    ParticleDensity,
    TexCompressionLevel,
    TerrainTexDetail,
    ObjectTexDetail,
    kCount,
};

// How a control shows its word: a slider's value, a list's row by its item value (the select
// by value, spinlist_row_for_value), a check box's state.
enum class OptionsWordForm : uint8_t { Scroll, RowValue, Checked };

// Each control the word seeds when the screen shows and takes back when the player edits it
// [orig: the front end's seed UI_SyncRenderSettingsToWidgets @0x55a140 and
//  UI_PopulateRenderAndAudioSettings @0x55c830, the in-game dialog's UI_OptionsScreenInit
//  @0x554800; the reads Options_HandleAcceptOrBack @0x55ab5f..0x55ace5 and
//  UI_IngameOptionsDialogEventHandler @0x554e40 - each graphics row's own addresses at its
//  policy above (the object detail .. the object texture detail), the scroll ranges at
//  kOptionsScrollRanges, the crosshair rows @0x5551c3 / @0x555209 / @0x55522b (the style seeded
//  by value, SpinList_SelectItemByValue @0x554cc8 / @0x55d437, and read by value,
//  CSpinListWnd_GetSelectedValue @0x5551ce / @0x55ac53), the tip pair at
//  kOptionsUnsupportedControls' note]. The shell applies a word on its edit (D-MNU-19).
struct OptionsWordControl {
    const char *control;
    OptionsWord word;
    OptionsWordForm form;
};
inline constexpr OptionsWordControl kOptionsWordControls[] = {
    {"SOUNDFXVOLUME", OptionsWord::SoundFxVolume, OptionsWordForm::Scroll},
    {"DIALOGVOLUME", OptionsWord::DialogVolume, OptionsWordForm::Scroll},
    {"MUSICVOLUME", OptionsWord::MusicVolume, OptionsWordForm::Scroll},
    {"XHAIR_APPEARANCE", OptionsWord::CrosshairStyle, OptionsWordForm::RowValue},
    // The persisted RGB against the authored item values (value="..." on each row).
    {"XHAIR_COLOR", OptionsWord::CrosshairColor, OptionsWordForm::RowValue},
    {"XHAIR_SPREAD", OptionsWord::CrosshairSpread, OptionsWordForm::Checked},
    {"MR_CLIPPY_KEYBOARD", OptionsWord::KeyboardTips, OptionsWordForm::Checked},
    {"MR_CLIPPY_HINTS", OptionsWord::GameplayTips, OptionsWordForm::Checked},
    {"16x9DISPLAY", OptionsWord::AspectMode, OptionsWordForm::RowValue},
    {"OBJECTPOLY", OptionsWord::ObjectPolyDetail, OptionsWordForm::RowValue},
    {"OBJECTDETAIL", OptionsWord::ObjectPolyDetail, OptionsWordForm::RowValue},
    {"TEXFILTER", OptionsWord::TexFilterLevel, OptionsWordForm::RowValue},
    {"PARTICLES", OptionsWord::ParticleDensity, OptionsWordForm::RowValue},
    {"TEXCOMPRESSION", OptionsWord::TexCompressionLevel, OptionsWordForm::RowValue},
    {"TERRAINTEX", OptionsWord::TerrainTexDetail, OptionsWordForm::RowValue},
    {"OBJECTTEX", OptionsWord::ObjectTexDetail, OptionsWordForm::RowValue},
};

// The registered retail comparison profile's gamma reference. Gamma is
// calibration, not a quality rung, so pushing it to the numeric maximum would
// deliberately distort the parity target.
inline constexpr int32_t kVideoGammaReference = 8;

// The three preset buttons the read-only policy disables.
inline constexpr const char *kVideoPresetButtons[] = {
    "VIDEODEFAULT", "VIDEOPERFORMANCE", "VIDEOQUALITY",
};

// The authored Options controls the reimpl does not service yet. Retail
// serves every one of them (the WDM channel/rate radios -> the
// Audio_ShutdownAll / Audio_InitSubsystems re-init and PunkBuster, read by the dialog's Accept
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
// (seed), UI_IngameOptionsDialogEventHandler @0x5552c8..0x555301 / Options_HandleAcceptOrBack
// @0x55aca7..0x55ace0 (write back); hud/tip_system.h TipOptions].
inline constexpr const char *kOptionsUnsupportedControls[] = {
    "DIFFICULTY",
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
// The item's value is the number its VALUE reads (the model holds none for 0, the parse's default).
inline int spinlist_row_for_value(const mnu::Items &items, std::string_view value) {
    const long wanted = std::strtol(std::string(value).c_str(), nullptr, 10);
    for (size_t i = 0; i < items.items.size(); ++i)
        if (std::strtol(items.items[i].value.c_str(), nullptr, 10) == wanted) return static_cast<int>(i);
    return 0;
}

} // namespace opennova::menu
