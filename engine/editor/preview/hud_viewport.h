#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/hud_layout_type.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <runtime/hud/hud_elements.h>
#include <runtime/hud/hud_layout_from_hudpos.h>

namespace opennova::editor {

class ProjectAssetSource;
class TextDocument;

// What a HUD viewport shows, and why not (the plan's DI-20): the kind's reason.
enum class HudViewStatus : uint8_t {
	NoProject, // no project is open
	NoLayout,  // no HUD layout is open at its path
	Ready,
};
// "no_project", "no_layout", "ready".
const char *hud_view_status_token(HudViewStatus status);

// The first-person view a HUD preview draws over: the plain view, the binoculars' or the night-vision
// goggles' (their masks the game's own view effects, godot/game/world/player_view_effects.gd).
enum class HudPreviewView : uint8_t { Normal, Binoculars, NightVision };
// "normal", "binoculars", "night_vision", and the view a token names (false for none).
const char *hud_preview_view_token(HudPreviewView view);
bool hud_preview_view_from_token(const std::string &token, HudPreviewView &out);

// The screens the toolbar offers, the game's own design size among them (the HUD's positions are
// authored in 1024 x 768 and scaled to the screen; the label fonts change at the 640 and 800 widths
// [orig: HUD_InitAllFonts @ 0x51EE20], and the HUD font at 640 [orig: HUD_SelectHudposFont @ 0x591890]).
struct HudScreenSize {
	int width;
	int height;
};
inline constexpr HudScreenSize kHudScreenSizes[] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
};
// The sizes a SetViewport takes: from 320 x 240 to 4096 x 4096.
inline constexpr int kHudScreenLeast = 240;
inline constexpr int kHudScreenMost = 4096;

// The state of the player the preview's HUD is drawn for, as the game's per-frame HUD info would
// carry it [orig: HUD_BuildEntityInfo @ 0x4B8440], and the screen it is drawn at: each a SetViewport
// of the viewport's options. `weapon` is a weapon.def row by its name ("" the first row the project's
// weapon.def holds, "NONE" no weapon, the game's own word for the empty slot), `clip` the rounds in
// its clip (-1 a full clip: the weapon's clipsize) and `reserve` the rounds carried; `stance` the
// HUD's stance (a HUDSTANCE id, 0 to 5); `health` in percent; `damage` the red damage vignette's
// alpha (0 none, to its cap of 192); `detail` the HUD detail level F6 steps (0 to 3: hudpos.def's
// HUDDECLUT rows by level, 3 the HUD hidden); `crosshair` the player's crosshair style (0 to 24,
// cross01.tga to cross25.tga); `picked` the element a click picked (its token, "" none).
struct HudViewportOptions {
	int width = 1024;
	int height = 768;
	int stance = 0;
	std::string weapon;
	int clip = -1;
	int reserve = 90;
	int health = 100;
	HudPreviewView view = HudPreviewView::Normal;
	int damage = 0;
	int detail = 0;
	int crosshair = 0;
	std::string picked;
	bool operator==(const HudViewportOptions &other) const;
	bool operator!=(const HudViewportOptions &other) const { return !(*this == other); }
};
inline constexpr int kHudDamageMost = 192;
// The JSON of `options` (options_json's), and the change a SetViewport makes to set them.
io::JsonValue hud_options_json(const HudViewportOptions &options);
std::string hud_options_change(const HudViewportOptions &options);

// A weapon the project's weapon.def holds, as the HUD's weapon cluster reads it: its name, its clip
// size, the art its HUD slice names (its hudicon, its clip and round graphics: what the HUD loads
// for it [orig: HUD_LoadAllTextures @ 0x59E246]).
struct HudPreviewWeapon {
	std::string name;
	int clipsize = 0;
	std::string hudicon;
	std::string clip_art;
	std::string round_art;
};

// A HUD element as the editor words it (runtime/hud/hud_elements.h's, one per element of the HUD's
// walk): its words, the hudpos.def keys that place or colour it (the first the line a Go to opens),
// and whether the HUD layout or the weapon names the art it draws.
struct HudElementWords {
	opennova::hud::HudElement element = opennova::hud::HudElement::kCount;
	const char *words = "";
	std::array<const char *, 5> keys{};
};
const HudElementWords &hud_element_words(opennova::hud::HudElement element);

// One element the picture drew: its box on the screen (the resolution's pixels), the hudpos.def lines
// that place it (each key's line the game takes, a HUDSTANCE's of the stance shown), and the textures
// it draws, each with the project file the game's lookup finds it in ("" none).
struct HudPreviewElement {
	opennova::hud::HudElement element = opennova::hud::HudElement::kCount;
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	struct Line {
		std::string key;
		size_t line = 0;
		size_t column = 0;
		std::string text;
	};
	std::vector<Line> lines;
	struct Art {
		std::string name;
		std::string path;
	};
	std::vector<Art> textures;
};

// A HUD layout's viewport (the plan's DI-20; ViewportKind::Hud, the Preview role of the HUD layout
// type): the game's own HUD drawn over hudpos.def as Save would write it now, at a screen size, for a
// player whose state its options choose, by the Shell's device (godot/src/authoring/
// hud_viewport_applier: the runtime's HudOverlay, which compiles the HUD through the engine's layout
// fill and frame compiler and draws its list, and the game's view effects) over the project's files.
// The device reports where each element of the HUD's walk drew (runtime/hud/hud_elements.h:
// ViewportDeviceReport::rects, one per HudElement, its box in the screen's pixels), so a point of the
// picture names the element under it (the smallest box holding it: an element inside the frame
// before the frame), its words, the hudpos.def lines that place it and the textures it draws; a
// click picks it (its options' `picked`), and the Go to of its line or its texture is the window's
// (window_requests::go_to). It changes nothing in the document: a drag and a command are refused.
class HudViewport final : public ViewportModel {
public:
	explicit HudViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	HudViewStatus view_status() const { return reason_; }
	const HudViewportOptions &options() const { return options_; }
	// The weapons the project's weapon.def holds, in its order, and the one shown (null for none:
	// `weapon` "NONE", a name the file lacks, or no weapon.def).
	const std::vector<HudPreviewWeapon> &weapons() const { return weapons_; }
	const HudPreviewWeapon *weapon_shown() const;
	// The names the HUD layout hands its loader (the fonts, the static frame, the stance art), and its
	// stances' names (a HUDSTANCE line's last word, "" where it names none) by id, as the text holds
	// them now.
	const opennova::hud::HudLayoutAssets &assets() const { return assets_; }
	const std::array<std::string, 6> &stance_names() const { return stance_names_; }
	// The elements the device last said its picture drew, in HudElement's order.
	const std::vector<HudPreviewElement> &elements() const { return elements_; }
	// The element under (x, y), the screen's pixels (null for none); the one picked (null for none).
	const HudPreviewElement *element_at(float x, float y) const;
	const HudPreviewElement *picked() const;
	// An element's words with its lines ("Ammo count: AMMOCOUNTPOS, line 15"), what a hover says.
	std::string element_words(const HudPreviewElement &element) const;

	ViewportStatus status() const override;
	const char *reason() const override { return hud_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout{ options_.width, options_.height }; }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	// The element under the point (the screen's pixels): index its HudElement, name its words, kind
	// its token; never a record (id 0).
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	bool click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
			std::string &error) const override;
	io::JsonValue options_json() const override;
	// The screen, the HUD font the HUD loads at its width, the stance and weapon shown, the weapons the
	// project's weapon.def holds, the stances' names.
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The elements the picture drew: each `element`, `words`, `rect` [x0, y0, x1, y1], `lines` [{key,
	// line, column, locator, text}], `textures` [{name, path}] and `picked`.
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(HudViewStatus reason);
	// The layout's lines and names, read from the text as it stands.
	void read_layout_(const TextDocument &text);
	// The weapons of the project's weapon.def, read again when its stamp moves.
	void read_weapons_(const FileSource &files);
	// The elements from the device's boxes, worded from the layout and the weapon shown.
	void make_elements_();

	HudViewStatus reason_ = HudViewStatus::NoProject;
	std::string detail_;
	HudViewportOptions options_;
	PreviewFollow picture_;
	// The text the layout was read from (its document's identity, load and revision) and what it read.
	uint64_t read_identity_ = 0, read_load_ = 0, read_revision_ = UINT64_MAX;
	std::vector<HudLayoutLine> lines_;
	std::string text_;
	opennova::hud::HudLayoutAssets assets_;
	std::array<std::string, 6> stance_names_{};
	// weapon.def as last read, by its stamp.
	bool weapons_read_ = false;
	uint64_t weapons_stamp_ = 0;
	std::vector<HudPreviewWeapon> weapons_;
	// Where the device said each element drew (one per HudElement), and the project's files' paths by
	// the names the elements' textures are found by.
	std::vector<ViewportDeviceReport::Rect> boxes_;
	std::shared_ptr<const ProjectAssetSource> assets_source_;
	std::vector<HudPreviewElement> elements_;
};

} // namespace opennova::editor
