#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

// The blank factories (ADR 0046 d7/d8): from-scratch files for the requirements a new
// project must satisfy and for the new files an author adds, each produced through the
// engine's own serializer (rtxt, mnu, mns, fnt, tga, dds, pcx ...) or, where no writer
// exists yet, a constant CRLF template. No retail bytes: the font is drawn in code, the
// menus are authored here, the tables are empty or carry only the keys the blank menu
// needs, a texture is the checkerboard the game itself draws for a texture it cannot load.
// A factory is keyed by the manifest role it
// fills (`main_menu`, `gametext`, `font_arial14b`); a request with no role (Create
// menu, Create table: a new file of a kind that fills no requirement) takes its kind's
// one free-form factory.
struct BlankRequest {
	std::string logical_name;  // the file to make (the engine's required name, or the one chosen)
	std::string role;          // the manifest role token, "" for a free-form file
	std::string project_title; // for headers and the startup screen's title
};

using BlankMaker = bool (*)(const BlankRequest &request, std::vector<uint8_t> &out,
                            Diagnostic &error);

struct BlankFactory {
	const char *role;    // the manifest role this factory fills; "" when it fills none
	AssetKind kind;
	BlankMaker make;
	const char *summary; // plain language: what the created file is
	bool free_form;      // the kind's file for a request with no role: exactly one per kind
};

size_t blank_factory_count();
const BlankFactory *blank_factory_at(size_t index);
// The factory filling a manifest role; nullptr for "" or an unknown role.
const BlankFactory *find_blank_factory_for_role(std::string_view role);
// The kind's free-form factory (a new file of the kind), or nullptr.
const BlankFactory *find_blank_factory_for_kind(AssetKind kind);

// Dispatch by role, then by kind; false with `error` when nothing can make it.
bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error);

// Where a created file goes inside the project tree, by kind ("menus", "fonts", ...;
// "" for the root). Organization only: the engine sees the flat name.
const char *blank_placement_dir(AssetKind kind);

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
