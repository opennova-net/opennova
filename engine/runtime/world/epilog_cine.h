#pragma once

// The single-player end-of-round cine: the round end's SP tail starts the
// WIN epilog or the LOSE screen, the entity update steps the cine timeline
// and its stage machine, the frame's render pass marks a cine frame drawn,
// and the stage machines build the end screen (the image, the letterbox, the
// text and counter lines), raise g_EpilogScreenActive and time the mission
// out. A session never runs it: the round end starts it only out of one.
//
// The timeline is a list of cine events, each live while its start frame <=
// the timeline frame <= start + duration; the shell draws the live ones (the
// device half), the engine owns the schedule, the stage machines and every
// value a line shows.
// [orig: Cinematic_EpilogUpdate @0x577950 (from Entity_UpdateAllEntities
//  @0x4C2230 / @0x4C2634); Cine_InitPlayback @0x578390; Cine_StartPlayback
//  @0x577840; Cine_EpilogStateMachineUpdate @0x576240; the render pass
//  sub_575A50 @0x575A50 from Render_ProcessMainSceneFrame @0x5CADFC]

#include <runtime/world/geom.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

class World;
struct Entity;

// The retail CineEvent subclasses the epilog builds (their vtables
// ??_7CineEvent*@@6B@ @0x7D64E4..0x7D69E4).
enum class CineEventKind : uint8_t {
	// Holds the view at a cached pose: the round-end player position, yaw and
	// pitch [orig: CineEventCacheCamera, vtable @0x7D67EC].
	CacheCamera = 0,
	// The cinematic bars [orig: CineEventLetterbox, CCineEventLetterbox_Construct
	//  @0x571260].
	Letterbox,
	// [orig: CineEventEditFade, vtable @0x7D682C]
	EditFade,
	// A full-screen fade, or a full-screen image when it names one
	// [orig: CineEventImageFade, CCineEventImageFade_Construct @0x570C80].
	ImageFade,
	// A text line in its box [orig: CineEventTextFade, CCineEventTextFade_Construct
	//  @0x572D50].
	TextFade,
	// A score line: a label and its value [orig: CineEventEpilogCounter,
	//  CineEventEpilogCounter_Construct @0x5732C0, _Draw @0x573440].
	EpilogCounter,
};

// Where a TextFade line's text comes from: a gametext string fetched at the
// build, or the round's stored banner (the WAC Lose cause, g_BannerText @0x28E3DA0),
// which the embedder holds.
enum class CineTextSource : uint8_t { GameText = 0, Banner };

// The ImageFade +0x124 fade modes [orig: CCineEventImageFade_InitFadeState
// @0x570FE0; its update sub_571020 @0x571020].
enum class CineFadeMode : int32_t {
	In = 1,   // alpha 0 -> 1 at the rate, clamped
	Out = 2,  // alpha 1 -> 0 at the rate, clamped
	Hold = 3, // alpha 1
};

// What an ImageFade quad shows (+0x128): the named image, the +0x120 solid
// colour, or (any other value) the default 0xFF303060 fill
// [orig: CinematicFadeEvent_LoadTexture @0x570D00 — the select @0x570D66].
enum class CineFadeSource : int32_t { Image = 0, SolidColor = 1 };

// One timeline event, the retail node's fields by kind (unused fields stay 0),
// then its live state the timeline walk advances.
struct CineEvent {
	CineEventKind kind = CineEventKind::Letterbox;
	int32_t start = 0;    // +0x04 the first live timeline frame
	int32_t duration = 0; // +0x08 live through start + duration
	// +0x14: bit 0 draws the node in the second pass (the lines over the
	// images and fades) [orig: sub_570000 @0x570017 / @0x570053].
	int32_t flags = 0;
	// CacheCamera: the cached pose, the start and end keyframes alike (the
	// local player's Position, Yaw and Pitch at the round end).
	Vec3 camera_position;
	int16_t camera_yaw = 0;
	int16_t camera_pitch = 0;
	// Letterbox (+0x24): 1 fades the bars in over the node, 2 fades them out.
	int32_t letterbox_mode = 0;
	// ImageFade: the image, the fade, the frames the fade waits, the source.
	std::string image;                          // +0x20
	CineFadeMode fade_mode = CineFadeMode::Hold; // +0x124
	int32_t fade_delay = 0;                      // +0x12C
	CineFadeSource fade_source = CineFadeSource::Image; // +0x128
	uint32_t solid_color = 0;                    // +0x120 (the SolidColor pixels)
	// TextFade: the text and its box in the cine's 1024 x 768 space, drawn
	// word-wrapped and centred in the box from its top (font -2 = the large
	// HUD label font, Impac22b), the colour's half-bright fold under the
	// node's alpha. `text_key` is the gametext key (section `text_section`) a
	// GameText line was fetched from; the mission-text section "CineText" the
	// constructor resolves it through comes back empty for a gametext
	// string, so the line draws the gametext.
	// [orig: sub_573020 @0x573020 -> HUD_DrawWrappedText @0x580C00 (mode 4)]
	CineTextSource text_source = CineTextSource::GameText;
	std::string text_section;
	std::string text_key;
	int32_t x = 0;           // +0xA8 (float)
	int32_t y = 0;           // +0xAC (float)
	int32_t font = 0;        // +0xA4
	int32_t fade_in = 0;     // +0xB8
	int32_t fade_out = 0;    // +0xBC
	int32_t box_width = 0;   // +0xB0 (float)
	int32_t box_height = 0;  // +0xB4 (float)
	uint32_t color_mask = 0; // +0xC0 (& 0xFFFFFF)
	// EpilogCounter: the label (gametext Epilog) and its value columns, in
	// the cine's 1024 x 768 space: the label left-aligned at label_x, the
	// value right-aligned at value_x, the count-up right-aligned at
	// count_x, on row y, all in the large HUD label font, half-bright.
	std::string label_key;
	int32_t label_x = 0;      // +0x430
	int32_t value_x = 0;      // +0x434
	int32_t count_x = 0;      // +0x438
	int32_t row_y = 0;        // +0x43C
	int32_t points_target = -1; // +0x420
	int32_t points_rate = -1;   // +0x424
	int32_t value = 0;          // +0x428
	int32_t max = -1;           // +0x42C

	// --- the live state ------------------------------------------------------
	// The node's alpha: an ImageFade's (+0x13C), a Letterbox's (+0x28) and a
	// TextFade's (+0xC4).
	float alpha = 0.0f;
	// An ImageFade's and a Letterbox's per-frame step (+0x130 / +0x20).
	float rate = 0.0f;
	// An ImageFade's remaining delay (+0x138); a TextFade's frame count (+0xC8).
	int32_t counter = 0;

	bool live_at(int32_t frame) const { return frame >= start && frame <= start + duration; }
};

// The cine's timing words, named once (the stage machines' literals).
// The win epilog's state-0 target with no <mission>.end cine
// [orig: Cine_InitPlayback @0x5785D5 `mov dword_2696DC0, 95`].
inline constexpr int32_t kEpilogWinBuildFrame = 95;
// The cached-camera hold and the opening letterbox of the win
// [orig: Cine_InitPlayback @0x5784F3 (300) / @0x578616 (62)].
inline constexpr int32_t kEpilogWinCameraFrames = 300;
inline constexpr int32_t kEpilogWinLetterboxFrames = 62;
// The lose cine's opening letterbox and edit fade
// [orig: Cine_StartPlayback @0x5778B3 (100) / @0x5778E0 (1)].
inline constexpr int32_t kEpilogLoseLetterboxFrames = 100;
inline constexpr int32_t kEpilogLoseEditFadeFrames = 1;
// The lose screen starts 249 frames past its build frame; the win screen 1
// [orig: Cinematic_EpilogUpdate @0x574512 `add esi, 249`;
//  Cine_EpilogStateMachineUpdate @0x5764F6 (+1)].
inline constexpr int32_t kEpilogLoseScreenDelay = 249;
inline constexpr int32_t kEpilogWinScreenDelay = 1;
// The fade pair, then the image and the lines held for 223200 frames
// [orig: the 48-frame CCineEventImageFade_Construct pairs @0x574539 /
//  @0x574579, @0x57651C / @0x57655D; the 223200 holds @0x5745B7 / @0x57659B].
inline constexpr int32_t kEpilogFadeFrames = 48;
inline constexpr int32_t kEpilogHoldFrames = 223200;
// The second letterbox at the screen's line start [orig: @0x5745E7 /
//  @0x5765CB — CCineEventLetterbox_Construct(start, 100, 2, 0)].
inline constexpr int32_t kEpilogScreenLetterboxFrames = 100;
inline constexpr int32_t kEpilogScreenLetterboxMode = 2;
// The exit timeout: 18600 frames past the end screen's fade state
// [orig: Cinematic_EpilogUpdate @0x5744EA; Cine_EpilogStateMachineUpdate
//  @0x576822].
inline constexpr int32_t kEpilogExitTimeoutTicks = 18600;
// The fade states' per-frame step on the global fade quad's alpha: 0.01 on
// both screens. No live path raises that quad (only the unreachable flyaway
// state 3 does, @0x576432 / @0x5764A6), so the step only ever lowers an
// already-lowered quad. [orig: Cinematic_EpilogUpdate @0x5744BF;
//  Cine_EpilogStateMachineUpdate @0x5767E6 `fsub flt_7C56A8` (0x3C23D70A)]
inline constexpr float kEpilogFadeStep = 0.01f;

// The two g_MissionExitReason values the world-side writers store
// (World::mission_exit_reason): the quit to the Post Menu (the end screens'
// timeout, the round-over ESC) and the SP restart (the round-over RESTART key,
// the in-game RESTART). The session layer's inmatch/mission_exit.h names
// every value and pins these two; the world stays below that layer.
// [orig: Game_ProcessMainFrame @0x526822 (4 -> Game_RestartRoundSP out of a
//  session) / @0x526860 (any other reason -> the "Post Menu" scene)]
inline constexpr int32_t kWorldMissionExitQuit = 1;
inline constexpr int32_t kWorldMissionExitRestart = 4;

// The g_CineMode values [orig: Cine_InitPlayback @0x57864F (1),
//  Cine_StartPlayback @0x577922 (2), Cine_DestroyAllNodesAndFont @0x57685D
//  and the intro-cine load sub_578270 @0x5782B8 (0)].
enum class EpilogCineMode : int32_t { None = 0, Win = 1, Lose = 2 };

struct EpilogCine {
	// The cine runs: the entity update steps it while set, the render pass
	// clears it once the timeline passes its last event [orig: dword_26970FC].
	bool active = false;
	// The timeline frame, advanced once per dispatch [orig: dword_2697100;
	//  sub_56FF40 @0x56FF52].
	int32_t frame = -1;
	EpilogCineMode mode = EpilogCineMode::None; // [orig: g_CineMode @0x2696DC4]
	int32_t win_state = 0;  // [orig: g_EpilogWinState @0x2696DB4]
	int32_t lose_state = 0; // [orig: g_EpilogLoseState @0x2696D9C]
	// The win's state-0 target frame [orig: dword_2696DC0].
	int32_t win_build_frame = 0;
	// The fade states' frame counts the timeouts compare [orig: dword_2696DAC
	//  (win) / dword_2696D98 (lose)].
	int32_t win_age = 0;
	int32_t lose_age = 0;
	// The end screen is up: the script tick, the BMS quarter pass and most of
	// the entity update hold [orig: g_EpilogScreenActive @0xA87054].
	bool screen_active = false;
	// A cine frame has rendered since the process started: the end screens
	// build only once one has. Process-lifetime: only the cine render pass
	// writes it (sub_570BB0 @0x570C66), so it crosses every load.
	// [orig: dword_26970F4]
	bool frame_drawn = false;
	// The fade states' full-screen quad and its alpha: the live path never
	// raises the quad (only the unreachable flyaway state 3 does)
	// [orig: dword_2696E1C / flt_26970E0].
	bool fade_quad = false;
	float fade_alpha = 0.0f;
	// The cinematic bars over the frame (the 16:9 band's top and bottom
	// remainder, black): fading while a letterbox node runs (its alpha),
	// held opaque after a fade-in node ends, gone after a fade-out node ends
	// [orig: dword_26970EC (fading) / dword_26970E8 (held) / flt_26970F0 (the
	//  alpha), written by the letterbox update CCineEventFade_StepAlpha
	//  @0x5712D0 and its end sub_571330 @0x571330; drawn by sub_570390
	//  @0x570390 over the bars sub_570240 @0x570240 lays].
	bool bars_fading = false;
	bool bars_held = false;
	float bars_alpha = 0.0f;
	// The timeline, in construction order (retail inserts each node before
	// its start word is stored, so its list order is unspecified; the port
	// keeps the build order).
	std::vector<CineEvent> events;

	// The mission start's cine legs [orig: Game_StartMission — sub_577940
	// @0x525DA8 (destroy every node, mode 0); on a first start out of a
	// session the <mission>.cin intro load sub_578270 @0x525DC9 (active = 1,
	// mode 0, the fade quad down; no shipped mission carries a .cin), the
	// frame reset sub_56FB50 @0x525DD1 and one Cinematic_EpilogUpdate
	// @0x525DD6; Game_ResetSessionHudState @0x434BDC clears the screen].
	void mission_start(bool first_start, bool in_session, World &world);
	// The WIN epilog with no <mission>.end cine (the only form a shipped
	// mission takes): the cached camera at the local player's pose, the
	// opening letterbox, state 0 [orig: Cine_InitPlayback @0x578390].
	void begin_win(const Entity *local_player);
	// The LOSE cine: the opening letterbox and edit fade, state 0
	// [orig: Cine_StartPlayback @0x577840].
	void begin_lose();
	// One entity update's dispatch: the timeline step, then the mode's stage
	// machine [orig: Cinematic_EpilogUpdate @0x577950].
	void update(World &world);
	// The frame's render pass: while the cine runs, a timeline before its
	// last event draws a cine frame, else the cine stops
	// [orig: sub_575A50 @0x575A50 -> sub_570BB0 @0x570BB0].
	void render_pass();
	// The timeline's last frame: the largest start + duration, 0 for an empty
	// list [orig: CineNode_RecalcTimeRange @0x5701C0].
	int32_t end_frame() const;

private:
	void clear_events();
	// The timeline step: the frame advances, then every live node activates
	// on its start frame, updates, and ends on its last frame, in list order
	// [orig: sub_56FF40 @0x56FF40].
	void step_timeline();
	void activate_event(CineEvent &e);
	void update_event(CineEvent &e);
	void end_event(const CineEvent &e);
	void update_lose(World &world);
	void update_win(World &world);
	void build_lose_screen();
	void build_win_screen(const World &world);
};

} // namespace opennova::world
