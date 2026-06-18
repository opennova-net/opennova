// Cross-resource reference extraction: which assets a file points at.
//
// NovaLogic assets reference each other by name (an .env names its sky textures
// and celestial models, items.def maps item ids to .3di graphics, a .kda credits
// file names its fonts). The engine resolves those names at load time but never
// exposes them; this library makes them queryable as flat edges so an editor (or
// any tool) can model the dependency graph.
//
// Extractors are pure bytes-in/edges-out: extract() dispatches on the source
// name (extension / well-known filename, case-insensitive) and parses through
// the format's own library (libs/env, libs/cbin, libs/def). They deliberately
// do NOT resolve targets — resolution semantics (VFS precedence, texture
// extension fallbacks) belong to the caller, so there is exactly one source of
// truth for "does this name exist".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::refs {

// One outgoing reference edge. Names are kept VERBATIM from the source bytes
// (the engine's lookups are case-insensitive; canonicalization is the graph
// layer's job, not the extractor's). Within one extract() call edges are
// deduplicated on (target_kind, lowercased target_name) — the first site wins —
// so a credits file naming one font 500 times yields one edge.
struct Reference {
    std::string source_path;  // the name/path handed to extract(), verbatim
    std::string source_kind;  // "environment" | "credits" | "item_defs" | "menu" | ... (matches resource_index vocabulary where one exists)
    std::string target_name;  // referenced asset name as written in the source
    std::string target_kind;  // "texture" | "object_model" | "anim_def" | "sound_profile" | "font" |
                              // "image" | "menu" | "sound" (.lwf bank file) | "strings" (table file) |
                              // "string_id" (key INSIDE a table - no file semantics) | "datasource" |
                              // "style_var" (menu_style.mns variable - no file semantics)
    std::string site;         // where in the source ("sky_map1", "entry[3].font", "item 451 graphic")
};

// True when `name` (by extension or well-known filename, case-insensitive on
// the basename) has an extractor: *.env, *.kda, *.3di, *.bms, *.mis, *.mnu, items.def.
bool can_extract(const std::string& name);

// Extract `source_path`'s outgoing references from its bytes into `out`
// (appended). Returns false only when a recognized format fails to parse
// (`error` filled); an unrecognized name is success with no edges, so callers
// can sweep a whole file list without pre-filtering.
bool extract(const std::string& source_path, const uint8_t* data, size_t size,
             std::vector<Reference>& out, std::string& error);

}  // namespace opennova::refs
