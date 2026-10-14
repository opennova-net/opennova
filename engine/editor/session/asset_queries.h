#pragma once

// The queries about a project file and what uses it (S18, DI-05, DI-19a, DI-30, DI-36): a texture's roles,
// picture, uses and cost, an import's options, a file's card and users, a terrain's and an environment's
// missions, the sounds the editor plays; their params and choices, kept apart from the query table's
// other rows (session/editor_queries.cpp, which names these in its rows).

#include <cstddef>
#include <string>

#include <base/io/json.h>
#include <editor/session/editor_queries.h>
#include <editor/session/query_support.h>

namespace opennova::editor {

const char *transform_choice(size_t index);

inline constexpr QueryParam kThumbnailParams[] = {
	{ "path", QueryJson::String, true, nullptr,
			"A texture file of the project, by its project-relative path or its logical name." },
	{ "transform", QueryJson::String, false, "none",
			"What the use's loader makes of the texels first (a texture reference's texture.transform): none, "
			"luminance_alpha, white_alpha_from_blue, alpha_only, normal_from_height or blue_alpha_only." },
};

inline constexpr QueryChoices kThumbnailChoices[] = { { "transform", transform_choice } };

inline constexpr QueryParam kTextureUsesParams[] = {
	{ "path", QueryJson::String, true, nullptr,
			"A texture file of the project, by its project-relative path or its logical name." },
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr QueryParam kTextureBudgetParams[] = {
	{ "offset", QueryJson::Integer, false, "0", kQueryOffsetDoc },
	{ "limit", QueryJson::Integer, false, "100", kQueryLimitDoc },
};

inline constexpr QueryParam kImportOptionsParams[] = {
	{ "path", QueryJson::String, true, nullptr,
			"An import source of the project (a file holding its .import record), or a file an import "
			"makes, by its project-relative path or its logical name." },
};

inline constexpr QueryParam kFileCardParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"A project file: its project-relative path, or its name alone (the file the name resolves to); "
			"left out, the file whose page the Document window shows." },
};

// Who names a file, in one look (the deep-integration plan's DI-05): the Inspector's Used by.
inline constexpr QueryParam kUsedByParams[] = {
	{ "path", QueryJson::String, false, nullptr,
			"A project file, as file_card takes it; left out, the active document's file." },
};

// The missions that run on an environment (DI-19a): a .env named by its path, else the active document.
inline constexpr QueryParam kEnvironmentUsesParams[] = {
	{ "path", QueryJson::String, false, nullptr, "An environment (.env); left out, the active document's file." },
};

// A particle file's blocks and keys as the game's reader reads them (S23 B): a particle file open as a document by
// its path, else the active one.
inline constexpr QueryParam kParticleKeysParams[] = {
	{ "path", QueryJson::String, false, nullptr, "A particle file open as a document; left out, the active one." },
};

// The missions that run on a terrain and the import it comes from (DI-30): a .trn by its path, else the active
// document.
inline constexpr QueryParam kTerrainUsesParams[] = {
	{ "path", QueryJson::String, false, nullptr, "A terrain (.trn); left out, the active document's file." },
};

io::JsonValue answer_texture_roles(const QueryContext &, const QueryArgs &args, std::string &);
io::JsonValue answer_texture_thumbnail(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_texture_uses(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_texture_budget(const QueryContext &context, const QueryArgs &args, std::string &);
io::JsonValue answer_import_options(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_file_card(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_used_by(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_terrain_uses(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_environment_uses(const QueryContext &context, const QueryArgs &args, std::string &error);
io::JsonValue answer_sounds_playing(const QueryContext &context, const QueryArgs &, std::string &);

} // namespace opennova::editor
