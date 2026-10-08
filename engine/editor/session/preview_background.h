#pragma once

#include <cstdint>
#include <string>

namespace opennova::editor {

// What the editor's previews of the project's own data draw behind their picture (the Preview background
// preference, ADR 0046 "The preview background"): the model's (and so a clip's and an animation table's), the
// texture's, the particle effect's, the definition's and the HUD's, each a viewport kind whose row says so
// (ViewportKindRow::backdrop). A picture of the game's own sky or screen (a menu's, a mission's, an
// environment's, a terrain's) draws the game's, never this. The editor's tool, not the game's look: no parity
// question. Dark is each picture's own background as it was before the preference (a 3D picture's near-black
// clear, the texture's 0.16 grey around the image, the HUD's mid grey), Grey a neutral mid grey falling a little
// from top to bottom (the default: a dark texture, camouflage or dark metal, still reads against it), Light a
// pale grey, Checker a checkerboard of two greys, through which a texture's alpha shows. Spelled by its token in
// the editor's settings file and on the wire.
enum class PreviewBackground : uint8_t { Dark, Grey, Light, Checker };

inline constexpr PreviewBackground kPreviewBackgrounds[] = {PreviewBackground::Dark, PreviewBackground::Grey,
                                                            PreviewBackground::Light, PreviewBackground::Checker};
inline constexpr PreviewBackground kDefaultPreviewBackground = PreviewBackground::Grey;

// A background's token: "dark", "grey", "light" or "checker".
constexpr const char *preview_background_token(PreviewBackground background) {
	switch (background) {
	case PreviewBackground::Dark: return "dark";
	case PreviewBackground::Light: return "light";
	case PreviewBackground::Checker: return "checker";
	case PreviewBackground::Grey: break;
	}
	return "grey";
}

// Its words, as the menu names it: "Dark", "Grey", "Light", "Checker".
constexpr const char *preview_background_words(PreviewBackground background) {
	switch (background) {
	case PreviewBackground::Dark: return "Dark";
	case PreviewBackground::Light: return "Light";
	case PreviewBackground::Checker: return "Checker";
	case PreviewBackground::Grey: break;
	}
	return "Grey";
}

// The background `token` names; false for a token none has (`out` left as it was).
inline bool preview_background_from_token(const std::string &token, PreviewBackground &out) {
	for (const PreviewBackground background : kPreviewBackgrounds) {
		if (token == preview_background_token(background)) {
			out = background;
			return true;
		}
	}
	return false;
}

// How a picture draws a background (each colour 0xRRGGBB, as shown on screen): `own` the picture's own
// background (Dark: nothing drawn over what it drew before); else a fill from `top` at the picture's top edge
// to `bottom` at its bottom edge, or, `checker`, squares of `check_px` pixels alternating `check_a` and
// `check_b` from the top left. What the overlays over it do to stay readable: `halo`, each line, dot and word
// drawn over a dark edge of its own (the canvas's shapes); `dark_lines`, a 3D picture's guides (its ground grid)
// drawn dark rather than light; and the canvas's frame around the picture.
struct PreviewBackdrop {
	bool own = false;
	uint32_t top = 0;
	uint32_t bottom = 0;
	bool checker = false;
	uint32_t check_a = 0;
	uint32_t check_b = 0;
	int check_px = 8;
	bool halo = false;
	bool dark_lines = false;
	uint32_t frame = 0x5A5A5A;
};

constexpr PreviewBackdrop preview_backdrop(PreviewBackground background) {
	PreviewBackdrop out;
	switch (background) {
	case PreviewBackground::Dark:
		out.own = true;
		return out;
	case PreviewBackground::Grey:
		out.top = 0x5A5A5A;
		out.bottom = 0x3C3C3C;
		break;
	case PreviewBackground::Light:
		out.top = 0xD2D2D2;
		out.bottom = 0xB4B4B4;
		out.dark_lines = true;
		break;
	case PreviewBackground::Checker:
		// The texture viewport's alpha board's two greys (S18), 8 pixels a square.
		out.checker = true;
		out.check_a = 0x666666;
		out.check_b = 0x9E9E9E;
		out.top = out.check_b;
		out.bottom = out.check_a;
		out.dark_lines = true;
		break;
	}
	out.halo = true;
	out.frame = 0x1C1C1C;
	return out;
}

} // namespace opennova::editor
