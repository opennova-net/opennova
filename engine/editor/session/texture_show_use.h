#pragma once

#include <cstdint>
#include <string>

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
// the mission opened, or the texture opened with its viewport's as_used set to the use. Refused, nothing
// opened (texture.show_use): references not read yet, a referrer that does not use it, a place there is not.
void show_texture_use(SessionCore &core, const EditorRequest &request);

} // namespace opennova::editor
