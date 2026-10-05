#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

// A value a blank takes from whoever asks for it (ADR 0046 S14: New > Mission... asks its terrain
// and its environment): its token (the create_file request's `values`), its label, what its
// picker offers (a reference kind that names a file: a file of the project of that kind;
// ReferenceKind::None, a text), and whether the blank cannot be made without it.
struct BlankParam {
	const char *token = "";
	const char *label = "";
	ReferenceKind reference = ReferenceKind::None;
	bool required = false;
};

// The blank factories (ADR 0046 d7/d8): from-scratch files for the requirements a new
// project must satisfy and for the new files an author adds, each produced through the
// engine's own serializer (rtxt, mnu, mns, fnt, tga, dds, pcx, bms ...) or, where no writer
// exists yet, a constant CRLF template. No retail bytes: the font is drawn in code, the
// menus are authored here, the tables are empty or carry only the keys the blank menu
// needs, a texture is the checkerboard the game itself draws for a texture it cannot load, a
// mission holds its header alone.
// A factory is keyed by the manifest role it
// fills (`main_menu`, `gametext`, `font_arial14b`); a request with no role (Create
// menu, Create table: a new file of a kind that fills no requirement) takes its kind's
// one free-form factory. A factory that takes values declares them (its `params`); a request
// carries each by its token.
struct BlankRequest {
	std::string logical_name;  // the file to make (the engine's required name, or the one chosen)
	std::string role;          // the manifest role token, "" for a free-form file
	std::string project_title; // for headers and the startup screen's title
	std::vector<std::pair<std::string, std::string>> values; // by the factory's param tokens
	// The value given for `token`; "" for none.
	const std::string &value(std::string_view token) const;
};

using BlankMaker = bool (*)(const BlankRequest &request, std::vector<uint8_t> &out,
                            Diagnostic &error);

struct BlankFactory {
	const char *role;    // the manifest role this factory fills; "" when it fills none
	AssetKind kind;
	BlankMaker make;
	const char *summary; // plain language: what the created file is
	bool free_form;      // the kind's file for a request with no role: exactly one per kind
	const BlankParam *params = nullptr; // the values it takes (null: none), `param_count` of them
	size_t param_count = 0;
};

// The role of the text table New > Mission... makes beside its mission (its title and an empty
// briefing: blank_strings.cpp); no manifest row has it.
inline constexpr const char *kBlankMissionTextRole = "mission_text";
// The title a new mission and its text table carry: the request's `title`, else its file's stem.
std::string blank_mission_title(const BlankRequest &request);

// The game's mouse pointer (blank_texture.cpp). The original game hides the system pointer for
// good as it starts [orig: Game_InitSubsystems @ 0x4a725a -> Game_HideCursorLoop @ 0x7612e0], so
// a menu's only pointer is the texture a CURSOR of the current screen's windows names, drawn last
// at the mouse [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60 over scene_end_frame @ 0x63e600's
// pick]: a screen whose windows name none, or name one that does not load, has no pointer at all.
// The blank menus name the game's own pointer file, the one its start-mission splash draws by
// name (hud::kSplashArrowImage, newarow1.tga, which every shipped screen's CURSOR names too), and
// it is made with them where the project has no file of that name (blank_companion). No manifest
// row has the role: the menus name the file, the game does not.
inline constexpr const char *kBlankPointerRole = "pointer";
// The pointer's file name: hud::kSplashArrowImage.
const char *blank_pointer_name();

// The file a blank names that the editor makes with it in `doc`'s project, where the project has
// none of that name: a menu's blank names the pointer (its root window's CURSOR), made by the
// pointer's factory; no other blank names a file it does not make. Null for none, else `name` is
// the file's name. A project that builds as an expansion takes none: its base game serves the
// pointer, which a file of the expansion's would replace.
const BlankFactory *blank_companion(const BlankFactory &factory, const ProjectDocument &doc, std::string &name);

// Whether `request`'s values fit the factory's params: each a param of it, every required one
// given. False with `why` in plain words. (A reference param's value is checked against the
// project by whoever makes the file: the factory reads no project.)
bool blank_values_fit(const BlankFactory &factory, const BlankRequest &request, std::string &why);

size_t blank_factory_count();
const BlankFactory *blank_factory_at(size_t index);
// The factory filling a manifest role; nullptr for "" or an unknown role.
const BlankFactory *find_blank_factory_for_role(std::string_view role);
// The kind's free-form factory (a new file of the kind), or nullptr.
const BlankFactory *find_blank_factory_for_kind(AssetKind kind);
// The factory a file of `kind` named `logical_name` takes, as make_blank picks it: its role's,
// else the one a blank's name asks for (the pointer's name, a texture: the pointer), else the
// kind's free-form one; nullptr when nothing makes it.
const BlankFactory *find_blank_factory(std::string_view role, const std::string &logical_name, AssetKind kind);

// Dispatch by role, then by the pointer's name, then by kind (find_blank_factory); false with
// `error` when nothing can make it.
bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error);

// Whether the texture factory makes a placeholder of this name (blank_texture.cpp): the
// checkerboard the game draws for a texture it cannot load, in the format the name's
// extension asks for (a .tga or an .mdt as a TGA, which the game reads both as, a .pcx as a
// PCX, a .dds as a DDS). False, with `reason` in plain words, for any other name.
bool can_make_blank_texture(const std::string &logical_name, std::string &reason);
// The reader that format is written for, the one a loader must pick for the placeholder to
// load: TGA, PCX or DDS by the name's extension; None for a name the factory does not make.
renderer::MaterialTextureReader blank_texture_reader(const std::string &logical_name);

// What the blank tables carry, for the tests: the sections the game reads from
// gametext.bin by name, and the "Menu" keys the blank startup screen resolves its labels
// through.
const std::vector<std::string> &blank_gametext_sections();
const std::vector<std::pair<std::string, std::string>> &blank_menutxt_keys();

} // namespace opennova::editor
