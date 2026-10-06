#pragma once

// The HUD's elements as the frame compiler's walk draws them (hud_frame.h): which element of the
// walk emitted each run of a draw list, kept beside the draws (HudDrawList::element_spans), so a
// tool can say what lies under a point of the HUD from the game's own compile, never from a layout
// of its own (the OpenNova Editor's HUD preview, ADR 0046 DI-20). Not a port: no original keeps
// such a record; the walk it records is HUD_RenderOverlays' [orig: HUD_RenderAllOverlays @0x5a8070
// -> HUD_RenderOverlays @0x5a7bb0, docs/interface/hud-re.md].

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::hud {

struct HudDrawList;

// One per element call of the walk (HudFrameCompiler's element_*), the weapon cluster's four draws
// apart (its ammo count, its weapon name, its clip indicator and its crosshair each place by a
// hudpos line or a texture of their own).
enum class HudElement : uint8_t {
	SightsCard,
	BreathBar,
	ServicePrompt,
	InsetCues,
	GameInfo,
	Frame,
	Health,
	Instruments,
	OpticalCues,
	Stance,
	AmmoCount,
	WeaponName,
	ClipIndicator,
	Targeting,
	Crosshair,
	Heat,
	Clock,
	Power,
	Waypoint,
	TeamIdLine,
	WeaponSlotBar,
	ScopeDetails,
	CapturePointLabels,
	LfpPanel,
	VehicleBayLogos,
	Spinmap,
	AttachLabels,
	FriendlyTags,
	VehiclePanel,
	Feed,
	SquadOrders,
	Tip,
	EndRoundStatistics,
	MessageLog,
	Scoreboard,
	EndRoundOverlay,
	VoiceMenus,
	PausedText,
	ChatInput,
	KillAnnouncement,
	TipAlternate,
	Briefing,
	Objectives,
	HelpScreen,
	QuitDialog,
	NetQuality,
	kCount
};
inline constexpr size_t kHudElementCount = static_cast<size_t>(HudElement::kCount);

// An element's token ("frame", "ammo_count", ...; "" past the last), and the element a token names
// (false for none).
const char *hud_element_token(HudElement element);
bool hud_element_from_token(const char *token, HudElement &out);

// Where the walk stood in each of a draw list's flat runs.
struct HudDrawCursor {
	size_t quads = 0;
	size_t tris = 0;
	size_t lines = 0;
	size_t glyphs = 0;
	size_t underlines = 0;
};

// One element's draws: the runs [begin, end) of each flat list it emitted, and whether it drew the
// corner map or the big map (the spinmap's passes, which the flat lists do not hold). Only an element
// that drew something has one.
struct HudElementSpan {
	HudElement element = HudElement::kCount;
	HudDrawCursor begin;
	HudDrawCursor end;
	bool map = false;
	bool big_map = false;
};

// An element's box on the surface the list was compiled at, in its pixels: the bounds of every quad,
// triangle, line, glyph and underline it emitted, a map pass's its clip rect. One per element span, in
// the walk's order (a later one draws over an earlier one).
struct HudElementBox {
	HudElement element = HudElement::kCount;
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
};
std::vector<HudElementBox> hud_element_boxes(const HudDrawList &list);

} // namespace opennova::hud
