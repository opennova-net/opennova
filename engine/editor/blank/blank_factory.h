#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The blank factories (ADR 0046 d7/d8): from-scratch files for the requirements a new
// project must satisfy, each produced through the engine's own serializer (rtxt, mnu,
// mns, fnt ...) or, where no writer exists yet, a constant CRLF template. No retail
// bytes: the font is drawn in code, the menu is authored here, the tables are empty or
// carry only the keys the blank menu needs. A factory is keyed by the manifest role it
// fills (`main_menu`, `gametext`, `font_arial14b`); the kind lookup serves a
// free-form "new file of this kind" later.
struct BlankRequest {
	std::string logical_name;  // the file the engine requires (its case as required)
	std::string role;          // the manifest role token, "" for a free-form asset
	std::string project_title; // for headers and the startup screen's title
};

using BlankMaker = bool (*)(const BlankRequest &request, std::vector<uint8_t> &out,
                            Diagnostic &error);

struct BlankFactory {
	const char *role;   // the manifest role this factory fills
	AssetKind kind;
	BlankMaker make;
	const char *summary; // plain language: what the created file is
};

size_t blank_factory_count();
const BlankFactory *blank_factory_at(size_t index);
const BlankFactory *find_blank_factory_for_role(std::string_view role);
// The first factory producing this kind (a generic file of the kind), or nullptr.
const BlankFactory *find_blank_factory_for_kind(AssetKind kind);

// Dispatch by role, then by kind; false with `error` when nothing can make it.
bool make_blank(const BlankRequest &request, AssetKind kind, std::vector<uint8_t> &out,
                Diagnostic &error);

// Where a created file goes inside the project tree, by kind ("menus", "fonts", ...;
// "" for the root). Organization only: the engine sees the flat name.
const char *blank_placement_dir(AssetKind kind);

// What the blank tables carry, for the tests and the requirements UI: the sections
// the game reads from gametext.bin by name, and the "Menu" keys the blank startup
// screen resolves its labels through.
const std::vector<std::string> &blank_gametext_sections();
const std::vector<std::pair<std::string, std::string>> &blank_menutxt_keys();

} // namespace opennova::editor
