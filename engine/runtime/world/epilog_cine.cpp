#include <runtime/world/epilog_cine.h>

#include <runtime/hud/hud_math.h>
#include <runtime/world/objectives_feed.h>
#include <runtime/world/world.h>

#include <algorithm>

namespace opennova::world {

namespace {

CineEvent letterbox(int32_t start, int32_t duration, int32_t mode, int32_t flags) {
	CineEvent e;
	e.kind = CineEventKind::Letterbox;
	e.start = start;
	e.duration = duration;
	e.letterbox_mode = mode;
	e.flags = flags;
	return e;
}

// The constructor, then the node's init the build runs right after it: the
// per-frame step over the frames after the delay (the image or the solid
// fill is the device's) [orig: CCineEventImageFade_Construct @0x570C80 (this,
// start, duration, image, +0x124, +0x12C, +0x128, +0x120, +0x14);
// CinematicFadeEvent_LoadTexture @0x570D00 — +0x130 = 1 / (duration - delay)
// @0x570D5B].
CineEvent image_fade(int32_t start, int32_t duration, const char *image, CineFadeMode mode,
		int32_t delay, CineFadeSource source, uint32_t solid_color, int32_t flags) {
	CineEvent e;
	e.kind = CineEventKind::ImageFade;
	e.start = start;
	e.duration = duration;
	e.image = image;
	e.fade_mode = mode;
	e.fade_delay = delay;
	e.fade_source = source;
	e.solid_color = solid_color;
	e.flags = flags;
	e.rate = static_cast<float>(1.0 / static_cast<double>(duration - delay));
	return e;
}

// [orig: CCineEventTextFade_Construct @0x572D50 (this, start, duration,
//  "CineText", text, x, y, +0xA4, +0xB8, +0xBC, +0xB0, +0xB4, +0x14,
//  +0xC0 & 0xFFFFFF)]
CineEvent text_fade(int32_t start, CineTextSource source, const char *section, const char *key,
		int32_t x, int32_t y, int32_t font, int32_t fade_in, int32_t fade_out, int32_t box_width,
		int32_t box_height, int32_t flags, int32_t color_mask) {
	CineEvent e;
	e.kind = CineEventKind::TextFade;
	e.start = start;
	e.duration = kEpilogHoldFrames;
	e.text_source = source;
	e.text_section = section;
	e.text_key = key;
	e.x = x;
	e.y = y;
	e.font = font;
	e.fade_in = fade_in;
	e.fade_out = fade_out;
	e.box_width = box_width;
	e.box_height = box_height;
	e.flags = flags;
	e.color_mask = static_cast<uint32_t>(color_mask) & 0xFFFFFFu;
	return e;
}

// [orig: CineEventEpilogCounter_Construct @0x5732C0 (this, start, duration,
//  +0x430, +0x434, +0x438, +0x43C, label, +0x420, +0x424, +0x428, +0x42C,
//  +0x14)]
CineEvent epilog_counter(int32_t start, const char *label_key, int32_t row_y, int32_t value,
		int32_t max) {
	CineEvent e;
	e.kind = CineEventKind::EpilogCounter;
	e.start = start;
	e.duration = kEpilogHoldFrames;
	e.label_key = label_key;
	e.label_x = 192;
	e.value_x = 692;
	e.count_x = 832;
	e.row_y = row_y;
	// The live epilog's every line passes -1 as its points target and rate,
	// so the count-up column never draws [orig: @0x576629 / @0x5766B4 /
	// @0x576715 / @0x576776].
	e.points_target = -1;
	e.points_rate = -1;
	e.value = value;
	e.max = max;
	e.flags = 1;
	return e;
}

} // namespace

uint32_t cine_event_draw_argb(const CineEvent &e) {
	const uint32_t alpha_byte = static_cast<uint32_t>(static_cast<int32_t>(e.alpha * 255.0f));
	switch (e.kind) {
	case CineEventKind::ImageFade: {
		if (e.fade_source == CineFadeSource::Image) return (alpha_byte << 24) | 0xFFFFFFu;
		// The solid source's pixels, or the default fill for any other source
		// [orig: CinematicFadeEvent_LoadTexture @0x570D56 (0xFF303060) /
		//  @0x570F10 (+0x120)].
		const uint32_t pixels =
				e.fade_source == CineFadeSource::SolidColor ? e.solid_color : 0xFF303060u;
		const uint32_t a = (alpha_byte * (pixels >> 24)) / 255u;
		return (a << 24) | (pixels & 0xFFFFFFu);
	}
	case CineEventKind::TextFade:
		return hud::half_bright_argb((alpha_byte << 24) + e.color_mask);
	case CineEventKind::EpilogCounter:
		return hud::half_bright_argb(0xFFFFFFFFu);
	default:
		return 0;
	}
}

int32_t EpilogCine::end_frame() const {
	// The largest start + duration over the list; an empty list ends at 0
	// [orig: CineNode_RecalcTimeRange @0x5701C6 (max 0) .. @0x5701FD].
	int32_t end = 0;
	for (const CineEvent &e : events) end = std::max(end, e.start + e.duration);
	return end;
}

void EpilogCine::clear_events() {
	// [orig: Cine_DestroyAllNodesAndFont @0x576850 — mode 0 @0x57685D, then
	//  every node's destructor and unlink]
	mode = EpilogCineMode::None;
	events.clear();
}

void EpilogCine::step_timeline(World &world) {
	++frame; // [orig: sub_56FF40 @0x56FF52]
	// [orig: @0x56FF8A..0x56FFE7 — start <= frame <= start + duration; the
	//  +0x0C progress the camera and blend nodes read; slot 1 on the start
	//  frame, slot 2 every live frame, slot 4 on the last]
	for (CineEvent &e : events) {
		if (!e.live_at(frame)) continue;
		if (frame == e.start) activate_event(e);
		update_event(e, world);
		if (frame == e.start + e.duration) end_event(e);
	}
}

void EpilogCine::activate_event(CineEvent &e) {
	switch (e.kind) {
	case CineEventKind::ImageFade:
		// [orig: CCineEventImageFade_InitFadeState @0x570FE0 — the delay
		//  @0x570FE6, alpha 0 for a fade-in @0x57100C, 1 for modes 2..3
		//  @0x571003]
		e.counter = e.fade_delay;
		if (e.fade_mode == CineFadeMode::In)
			e.alpha = 0.0f;
		else if (e.fade_mode == CineFadeMode::Out || e.fade_mode == CineFadeMode::Hold)
			e.alpha = 1.0f;
		break;
	case CineEventKind::Letterbox:
		// [orig: CCineEventFade_InitStep @0x5712A0 — the step 1 / duration
		//  @0x5712B1; alpha 0 for mode 1 @0x5712C3, 1 for mode 2 @0x5712BB]
		e.rate = static_cast<float>(1.0 / static_cast<double>(e.duration));
		if (e.letterbox_mode == 1)
			e.alpha = 0.0f;
		else if (e.letterbox_mode == 2)
			e.alpha = 1.0f;
		break;
	case CineEventKind::TextFade:
		// [orig: sub_572F70 @0x572F70]
		e.counter = 0;
		e.alpha = 0.0f;
		break;
	case CineEventKind::EpilogCounter:
		// The count-up column's start: the count at 0, the first step four
		// frames away [orig: CineEventEpilogCounter's start @0x573370 —
		//  +0x440 = 0, +0x444 = 4 @0x57337a].
		e.points = 0;
		e.points_cooldown = kEpilogCounterStepWait;
		break;
	default:
		// The cached camera's start is a nullsub (@0x571FB0); the edit fade's
		// camera-target blend (sub_5726B0 @0x5726B0) has no target in the lose
		// cine.
		break;
	}
}

void EpilogCine::update_event(CineEvent &e, World &world) {
	switch (e.kind) {
	case CineEventKind::ImageFade:
		// [orig: sub_571020 @0x571020 — the delay @0x571028; mode 1 up to 1
		//  @0x57107E, mode 2 down to 0 @0x571058, mode 3 at 1 @0x57104B]
		if (e.counter > 0) {
			--e.counter;
			break;
		}
		if (e.fade_mode == CineFadeMode::In) {
			e.alpha = e.rate + e.alpha;
			if (e.alpha > 1.0f) e.alpha = 1.0f;
		} else if (e.fade_mode == CineFadeMode::Out) {
			e.alpha = e.alpha - e.rate;
			if (!(e.alpha > 0.0f) && !(e.alpha == 0.0f)) e.alpha = 0.0f;
		} else if (e.fade_mode == CineFadeMode::Hold) {
			e.alpha = 1.0f;
		}
		break;
	case CineEventKind::Letterbox:
		// [orig: CCineEventFade_StepAlpha @0x5712D0 — mode 1 @0x5712FB, mode 2
		//  @0x5712E0, then the bars' globals @0x571313..0x571323]
		if (e.letterbox_mode == 1) {
			e.alpha = e.rate + e.alpha;
			if (e.alpha > 1.0f) e.alpha = 1.0f;
		} else if (e.letterbox_mode == 2) {
			e.alpha = e.alpha - e.rate;
			if (!(e.alpha > 0.0f) && !(e.alpha == 0.0f)) e.alpha = 0.0f;
		}
		bars_fading = true;
		bars_alpha = e.alpha;
		bars_held = false;
		break;
	case CineEventKind::TextFade: {
		// [orig: sub_572F90 @0x572F90 — the count @0x572F9A, the fade-in
		//  @0x572FB8, the hold @0x572FF7, the fade-out @0x572FDD..0x572FEB]
		const int32_t count = ++e.counter;
		if (count < e.fade_in)
			e.alpha = static_cast<float>(static_cast<double>(e.counter) / e.fade_in);
		else if (count <= e.duration - e.fade_out)
			e.alpha = 1.0f;
		else
			e.alpha = static_cast<float>(static_cast<double>(e.duration - count + 1) / e.fade_out);
		break;
	}
	case CineEventKind::EpilogCounter: {
		// The count-up step: after its wait the count moves toward the target
		// by the rate (a negative target subtracts it), clamped past the target,
		// and the step that lands on the target plays TEXT_END; every step
		// re-arms the wait. The live epilog's -1 target and rate step the count
		// up from 0, away from the target, so it never lands and never sounds.
		// (The step's busy flag dword_269721C, raised while the count is off
		// its target, has no reader.)
		// [orig: CineNode_CounterStep @0x573390 — the wait @0x5733ab..0x5733b8;
		//  the step @0x5733c0..0x5733fa; the arrival @0x573400..0x57340e ->
		//  Sound_PlayInterfaceTriggerSet(g_SndTextEnd); the re-arm @0x573416]
		if (e.points_cooldown != 0) {
			--e.points_cooldown;
			break;
		}
		const int32_t target = e.points_target;
		const uint32_t count = static_cast<uint32_t>(e.points);
		const uint32_t rate = static_cast<uint32_t>(e.points_rate);
		if (e.points != target) {
			if (target > 0) {
				e.points = static_cast<int32_t>(count + rate); // @0x5733da
				if (e.points > target) e.points = target;      // @0x5733fa
			} else if (target < 0) {
				e.points = static_cast<int32_t>(count - rate); // @0x5733ea
				if (e.points < target) e.points = target;      // @0x5733fa
			}
			if (e.points == target) {
				ScriptSoundEvent sound;
				sound.name = kEpilogCounterEndSoundset;
				sound.kind = ScriptSoundEvent::Kind::Interface;
				world.out.script_sounds.push_back(std::move(sound));
			}
		}
		e.points_cooldown = kEpilogCounterStepWait;
		break;
	}
	default:
		// The cached camera's interpolation (Cine_CameraInterpolateTransform
		// @0x5722C0) drives the cine view (D-AI-16); the edit fade's blend
		// (sub_5726D0 @0x5726D0) has no target.
		break;
	}
}

void EpilogCine::end_event(const CineEvent &e) {
	// The letterbox's end: a fade-in leaves the bars held opaque, a fade-out
	// clears them [orig: sub_571330 @0x571330 — mode 1 @0x571348 / @0x571352,
	//  mode 2 @0x57133D / @0x571342]. The other kinds' ends are nullsubs or
	//  the edit fade's target restore (sub_572750 @0x572750).
	if (e.kind != CineEventKind::Letterbox) return;
	if (e.letterbox_mode == 1) {
		bars_fading = false;
		bars_held = true;
	} else if (e.letterbox_mode == 2) {
		bars_fading = false;
		bars_held = false;
	}
}

void EpilogCine::mission_start(bool first_start, bool in_session, World &world) {
	clear_events(); // [orig: Game_StartMission @0x525DA8 -> sub_577940]
	screen_active = false; // [orig: Game_ResetSessionHudState @0x434BDC]
	// The restart takes the other arm (nullsub_32 @0x525DDD); a session never
	// loads the intro [orig: @0x525DAF..0x525DBF].
	if (!first_start || in_session) return;
	// The <mission>.cin intro load: no shipped mission carries a .cin, so the
	// load finds no file and only its head runs; the cine is flagged running
	// over an empty timeline, which the first render pass stops.
	// [orig: sub_578270 @0x578270 — dword_26970FC = 1 @0x578296, the fade
	//  quad down @0x5782A6, mode 0 @0x5782B8; the .pcx probe @0x57833B and the
	//  re-raise @0x578378]
	active = true;
	fade_quad = false;
	mode = EpilogCineMode::None;
	frame = -1; // [orig: sub_56FB50 @0x56FB50]
	update(world); // [orig: @0x525DD6]
}

void EpilogCine::begin_win(const Entity *local_player) {
	// [orig: Cine_InitPlayback @0x578390 — the destroy @0x5783A7, the frame
	//  reset @0x5783B3]. A <mission>.end cine would load here (@0x578411 ->
	//  sub_578270 when FileSystem_FileExists); no shipped mission carries one,
	//  so the timeline stays empty and the no-cine arm builds the camera.
	clear_events();
	frame = -1;
	// The cached camera at the round-end player pose, held 300 frames
	// [orig: @0x578479..0x578574 — X, Y, Z, Yaw, Pitch into the start and end
	//  keyframes; duration @0x5784F3], then the state-0 target
	//  [orig: @0x5785D5].
	CineEvent camera;
	camera.kind = CineEventKind::CacheCamera;
	camera.start = 0;
	camera.duration = kEpilogWinCameraFrames;
	if (local_player != nullptr) {
		camera.camera_position = local_player->position;
		camera.camera_yaw = local_player->yaw;
		camera.camera_pitch = local_player->pitch;
	}
	events.push_back(camera);
	win_build_frame = kEpilogWinBuildFrame;
	// The opening letterbox [orig: @0x5785F5..0x57861D — (0, 62), +0x24 = 1].
	events.push_back(letterbox(0, kEpilogWinLetterboxFrames, 1, 0));
	// [orig: @0x578628..0x57867C]
	active = true;
	mode = EpilogCineMode::Win;
	win_state = 0;
	win_age = 0;
}

void EpilogCine::begin_lose() {
	// [orig: Cine_StartPlayback @0x577840 — the destroy @0x577858, the frame
	//  reset @0x577864]
	clear_events();
	frame = -1;
	// The opening letterbox (0, 100, +0x24 = 1) and edit fade (0, 1)
	// [orig: @0x577892..0x5778E6].
	events.push_back(letterbox(0, kEpilogLoseLetterboxFrames, 1, 0));
	CineEvent fade;
	fade.kind = CineEventKind::EditFade;
	fade.start = 0;
	fade.duration = kEpilogLoseEditFadeFrames;
	events.push_back(fade);
	// [orig: @0x5778E9..0x577922]
	active = true;
	lose_state = 0;
	lose_age = 0;
	mode = EpilogCineMode::Lose;
}

void EpilogCine::update(World &world) {
	// [orig: Cinematic_EpilogUpdate @0x577961 — the cine-running gate]
	if (!active) return;
	step_timeline(world); // [orig: Cinematic_EpilogUpdate @0x577963 -> sub_56FF40]
	if (mode == EpilogCineMode::Win)
		update_win(world); // [orig: @0x57797C]
	else if (mode == EpilogCineMode::Lose)
		update_lose(world); // [orig: @0x577975 -> @0x574491]
}

void EpilogCine::update_lose(World &world) {
	switch (lose_state) {
	case 0:
		lose_state = 1; // [orig: @0x5747EB]
		break;
	case 1:
		// The screen builds on the first dispatch after a cine frame has
		// rendered [orig: `cmp dword_26970F4, 0` @0x5744FD].
		if (!frame_drawn) break;
		build_lose_screen();
		++lose_state; // [orig: @0x5747D4]
		// Then the dialog reset (its registry half; the waiting lines are the
		// shell's queue) and the audio channel shutdown, a device leg the port
		// does not run (D-HUD-46) [orig: Dialog_ResetAll @0x5747DA,
		// Audio_ShutdownChannelsAndDeviceTable @0x5747E6].
		world.script.dialog.reset();
		break;
	case 2:
		++lose_age; // [orig: @0x5744B9]
		fade_alpha -= kEpilogFadeStep; // [orig: @0x5744BF]
		if (fade_alpha < 0.0f) fade_quad = false; // [orig: @0x5744D4 / @0x5744D6]
		if (lose_age > kEpilogExitTimeoutTicks)
			world.mission_exit_reason = kWorldMissionExitQuit; // [orig: @0x5744EC]
		break;
	default:
		break;
	}
}

void EpilogCine::build_lose_screen() {
	// [orig: Cinematic_EpilogUpdate @0x57450C — the screen flag]
	screen_active = true;
	const int32_t t = frame + kEpilogLoseScreenDelay; // [orig: @0x574512]
	// The fade pair and the backdrop [orig: @0x574539 / @0x574579 / @0x5745B7].
	events.push_back(image_fade(t, kEpilogFadeFrames, "", CineFadeMode::In, 0,
			CineFadeSource::SolidColor, 0xFFFFFFu, 0));
	const int32_t image_start = t + kEpilogFadeFrames; // [orig: @0x574554]
	events.push_back(image_fade(image_start, kEpilogFadeFrames, "", CineFadeMode::Out, 0,
			CineFadeSource::SolidColor, 0xFFFFFFu, 0));
	events.push_back(image_fade(image_start, kEpilogHoldFrames, "jo_Epil2.tga",
			CineFadeMode::Hold, 0, CineFadeSource::Image, 0u, 0));
	const int32_t line_start = image_start + kEpilogFadeFrames; // [orig: @0x5745CF]
	events.push_back(letterbox(line_start, kEpilogScreenLetterboxFrames,
			kEpilogScreenLetterboxMode, 0)); // [orig: @0x5745E7]
	// MISSION FAILED [orig: @0x574623 / @0x574639 — (line_start, 223200,
	//  "CineText", Overlays/STROVER_MISSION_FAILED, 0, 120, -2, 100, 140,
	//  1024, 100, 1, 0xFFFFFF)].
	events.push_back(text_fade(line_start, CineTextSource::GameText, "Overlays",
			"STROVER_MISSION_FAILED", 0, 120, -2, 100, 140, 1024, 100, 1, 0xFFFFFF));
	// The banner line, built only when the round stored one; the embedder
	// holds the banner text and draws nothing for an empty one, so the node
	// rides every build [orig: `cmp g_BannerText, 0` @0x574645; sub_572E30
	//  @0x574685 — (line_start, 223200, g_BannerText, 0, 230, -2, 140, 140,
	//  1024, 100, 1, 0xFFFFFF)]. The saved-game list it would offset below it
	//  (sub_5B72E0 @0x574696) is the unported savegame record (D-SAVE-1).
	events.push_back(text_fade(line_start, CineTextSource::Banner, "", "", 0, 230, -2, 140, 140,
			1024, 100, 1, 0xFFFFFF));
	// The key help: 124 frames past the line start, y 600 [orig: @0x57471C /
	//  @0x574757 / @0x57476D]. Its second line (STREPILOG_KEYINFO2, y 632,
	//  @0x5747B6) builds only over a saved game (D-SAVE-1).
	events.push_back(text_fade(line_start + 124, CineTextSource::GameText, "Epilog",
			"STREPILOG_KEYINFO", 0, 600, -2, 140, 140, 1024, 100, 1, 0xFFFFFF));
}

void EpilogCine::update_win(World &world) {
	switch (win_state) {
	case 0:
		// The timeline reaches the target, then the SCORE SCREEN: no state
		// sets 1, so the flyaway states 1..3 (the altitude wait, the
		// acceleration, the steer at dword_2696FD8/FDC) are unreachable
		// [orig: Cine_EpilogStateMachineUpdate @0x57626F..0x576283 — `mov
		//  g_EpilogWinState, 4`; the writers of g_EpilogWinState: @0x576283,
		//  @0x5762A4, @0x57632B, @0x57649E, @0x5767D7, Cine_InitPlayback
		//  @0x578655 and the unreferenced Cine_ProcessEpilogSequence_Retail
		//  @0x575A90].
		if (frame >= win_build_frame) win_state = 4;
		break;
	case 4:
		// [orig: `cmp dword_26970F4, 0` @0x5764DB]
		if (!frame_drawn) break;
		build_win_screen(world);
		++win_state; // [orig: @0x5767D7]
		break;
	case 5:
		// The fade step is the same 0.01 as the lose screen's: the live
		// path never raises the quad (the flyaway's state 3 would), so only
		// the counter is observable here.
		fade_alpha -= kEpilogFadeStep; // [orig: @0x5767E6 fsub flt_7C56A8]
		++win_age;                         // [orig: @0x5767F7]
		if (fade_alpha < 0.0f) fade_quad = false; // [orig: @0x57680C / @0x57680E]
		// (The unreferenced sibling Cine_ProcessEpilogSequence_Retail stores the
		// same exit from its own fade state [orig: @0x57621d].)
		if (win_age > kEpilogExitTimeoutTicks)
			world.mission_exit_reason = kWorldMissionExitQuit; // [orig: @0x576824]
		break;
	default:
		break;
	}
}

void EpilogCine::build_win_screen(const World &world) {
	screen_active = true; // [orig: @0x5764EC]
	const int32_t t = frame + kEpilogWinScreenDelay; // [orig: @0x5764F6]
	events.push_back(image_fade(t, kEpilogFadeFrames, "", CineFadeMode::In, 0,
			CineFadeSource::SolidColor, 0xFFFFFFu, 0));
	const int32_t image_start = t + kEpilogFadeFrames; // [orig: @0x576537]
	events.push_back(image_fade(image_start, kEpilogFadeFrames, "", CineFadeMode::Out, 0,
			CineFadeSource::SolidColor, 0xFFFFFFu, 0));
	events.push_back(image_fade(image_start, kEpilogHoldFrames, "jo_Epil.tga",
			CineFadeMode::Hold, 0, CineFadeSource::Image, 0u, 0));
	const int32_t line_start = image_start + kEpilogFadeFrames; // [orig: @0x5765B3]
	events.push_back(letterbox(line_start, kEpilogScreenLetterboxFrames,
			kEpilogScreenLetterboxMode, 0)); // [orig: @0x5765CB]
	// The four counter lines, 124 frames past the line start and 60 apart,
	// their values frozen at the build: the won subgoals over the defined
	// ones, the six enemy buckets clamped to [0, the enemy total] over it,
	// and the team and friendly sums with no max.
	// [orig: @0x5765D0..0x576776 — y 160 / 208 / 256 / 304]
	const hud::EndRoundStatisticsInput in = end_round_statistics_input(world);
	const int32_t first = line_start + 124; // [orig: @0x5765E1]
	events.push_back(epilog_counter(first, "STREPILOG_OBJECTIVEBONUS", 160, in.subgoals_won,
			in.subgoals_defined));
	events.push_back(epilog_counter(first + 60, "STREPILOG_ENEMYUNITS", 208,
			hud::end_round_enemy_units(in), in.enemy_unit_total)); // [orig: @0x576649]
	events.push_back(epilog_counter(first + 120, "STREPILOG_TEAMUNITS", 256, in.team_unit_kills,
			-1)); // [orig: @0x5766C9]
	events.push_back(epilog_counter(first + 180, "STREPILOG_FRIENDLYUNITS", 304,
			in.friendly_unit_kills, -1)); // [orig: @0x57672A]
	// The key help, 120 frames past the last counter, y 700 [orig: @0x5767B9 /
	//  @0x5767D2].
	events.push_back(text_fade(first + 300, CineTextSource::GameText, "Epilog",
			"STREPILOG_KEYINFO", 0, 700, -2, 140, 140, 1024, 100, 1, 0xFFFFFF));
}

void EpilogCine::render_pass() {
	// [orig: sub_575A50 @0x575A57 — the running gate; the time-range recalc
	//  @0x575A59; the draw @0x575A8A -> sub_570BB0, whose tail raises
	//  dword_26970F4 @0x570C66; past the end the cine stops @0x575A6B]
	if (!active) return;
	if (frame < end_frame())
		frame_drawn = true;
	else
		active = false;
}

} // namespace opennova::world
