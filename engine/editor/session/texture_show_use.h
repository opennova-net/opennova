#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

class SessionCore;
struct EditorRequest;
struct SessionView;
struct TextureUse;

// Where a texture's use is shown as the game draws it (ADR 0046 S18, show_use): the picture the referrer
// has in the editor, else the texture's own as the use's loader makes it.
enum class UsePicture : uint8_t {
	Model,   // the referring model in the Preview window, the use's row selected
	Menu,    // the referring menu's screen in the Preview window, the window that names it selected
	Mission, // the view of a mission that draws the referring terrain or environment
	AsUsed,  // the texture's own view with as_used naming the use (a definition's, the HUD's, a particle's)
};

struct UsePlace {
	UsePicture picture = UsePicture::AsUsed;
	std::string open;  // the document opened: the referrer, the mission, or the texture
	std::string label; // the gesture's words ("Show on tree.3di")
	int use = -1;      // the use's place among the texture's uses (as_used's)
};

// Where `use`, at `index` among the uses of the texture at `texture` (project-relative), is shown: a
// model's or a menu's own picture; a terrain's or an environment's in the view of a mission that names it
// (one open first, else the first by path); any other referrer's, the texture's own as the use draws it.
// False, with why: a name the game opens itself (no referrer), a terrain or an environment no mission of
// the project names.
bool texture_use_place(const SessionView &view, const std::string &texture, const TextureUse &use, int index,
                       UsePlace &out, std::string &why);

// ShowUse: the use of the texture `request.path` in the project file `request.paths[0]` (at its locator
// and field, when given: a referrer may use it more than once; else its first) shown where texture_use_place
// says: the referrer opened at the use and the Preview window brought forward (a RevealPreview view event),
// the mission opened, or the texture opened with its viewport's as_used set to the use. A model's flipbook
// frame row is shown at its frame (S23 C, flipbook_frame_change): the preview clock held at the time its
// frame shows, or, for a flipbook on a register, the register held at a value that shows it. Refused, nothing
// opened (texture.show_use): references not read yet, a referrer that does not use it, a place there is not.
void show_texture_use(SessionCore &core, const EditorRequest &request);

// The change that shows the flipbook frame `frame` of `material` (S23 C): for a flipbook on the clock
// (texanim.type 0), a set_viewport of the clock alone (`clock` true), paused at the first millisecond the game's
// frame law shows it; for one on a register (type 1), the model viewport's options holding that register
// (`clock` false) at the least value that shows it; each found by the engine's own frame law
// (renderer::compute_anim_frame [orig: Material_ApplyShaderParameters @ 0x58DB80]). False, with why, for a
// frame the game never shows (past the flipbook's frames, or a flipbook of another clock, which stays on frame
// 0).
struct FlipbookFrameChange {
	bool clock = true;
	std::string change;
	std::string words;
};
bool flipbook_frame_change(const threedi::ThreediMaterial &material,
		const std::vector<threedi::ThreediControlRegister> &registers, int frame, FlipbookFrameChange &out,
		std::string &why);

} // namespace opennova::editor
