#include <editor/session/asset_queries.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_surfaces.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/jump_queries.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document_search.h>
#include <editor/preview/menu_report.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/catalog_json.h>
#include <editor/session/document_set.h>
#include <editor/session/particle_keys_query.h>
#include <editor/session/terrain_queries.h>
#include <editor/session/environment_uses.h>
#include <editor/session/terrain_uses.h>
#include <editor/session/file_card.h>
#include <editor/session/finding_codes.h>
#include <editor/session/mission_queries.h>
#include <editor/session/model_json.h>
#include <editor/session/problems_service.h>
#include <editor/session/query_support.h>
#include <editor/session/record_batch.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/script_assist.h>
#include <editor/session/session_core.h>
#include <editor/session/session_json.h>
#include <editor/session/sounds_playing.h>
#include <editor/session/texture_budget_list.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view_json.h>
#include <editor/session/workspace_parts.h>
#include <editor/session/row_table.h>


namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

using J = QueryJson;

} // namespace

// Every way the game uses a texture (ADR 0046 S18, documents/texture_roles), in the catalog's order.
JsonValue answer_texture_roles(const QueryContext &, const QueryArgs &args, std::string &) {
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	set_page(out, page, renderer::kTextureRoleCount);
	JsonValue roles = JsonValue::make_array();
	for (size_t i = page.first(renderer::kTextureRoleCount); i < page.last(renderer::kTextureRoleCount); ++i)
		roles.push(texture_role_json(texture_role_row(static_cast<renderer::TextureRoleId>(i))));
	out.set("roles", std::move(roles));
	return out;
}

const char *transform_choice(size_t index) {
	return index < size_t(TextureLoadTransform::kCount) ? texture_load_transform_token(static_cast<TextureLoadTransform>(index))
	                                                    : nullptr;
}

namespace {

// The texture file of the project `path` names, by its project-relative path or its logical name; null with the
// error where the project has none.
const AssetEntry *texture_entry(const SessionView &view, const std::string &path, std::string &error) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
	if (!entry && view.project.scan) entry = view.project.scan->find(path);
	if (entry && entry->kind == AssetKind::Texture) return entry;
	error = "no texture file " + path + " in the project.";
	return nullptr;
}

} // namespace

// A texture file as a small picture (ADR 0046 S18, preview/texture_thumbnails), made now when the cache
// lacks it: its facts and the picture as a PNG.
JsonValue answer_texture_thumbnail(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const AssetEntry *entry = texture_entry(view, args.text("path"), error);
	if (!entry) return JsonValue::make_null();
	TextureLoadTransform transform = TextureLoadTransform::None;
	for (size_t i = 0; i < size_t(TextureLoadTransform::kCount); ++i)
		if (args.text("transform") == transform_choice(i)) transform = static_cast<TextureLoadTransform>(i);
	const std::shared_ptr<const TextureThumbnail> thumbnail =
			view.documents.thumbnails ? view.documents.thumbnails->make_now(view, entry->relative_path, transform) : nullptr;
	if (!thumbnail) {
		error = entry->relative_path + " did not read.";
		return JsonValue::make_null();
	}
	return texture_thumbnail_json(*thumbnail, true);
}

// What uses a texture (ADR 0046 S18, graph/texture_uses over the session's index).
JsonValue answer_texture_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const AssetEntry *entry = texture_entry(view, args.text("path"), error);
	if (!entry) return JsonValue::make_null();
	static const std::vector<TextureUse> kNone;
	const std::vector<TextureUse> &uses =
			view.documents.texture_uses ? view.documents.texture_uses->uses_of(view, entry->relative_path) : kNone;
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("file", json_string(entry->relative_path));
	set_page(out, page, uses.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(uses.size()); i < page.last(uses.size()); ++i) list.push(texture_use_json(uses[i]));
	out.set("uses", std::move(list));
	return out;
}

// What the project's model textures cost the game (ADR 0046 S18, session/texture_budget_list).
JsonValue answer_texture_budget(const QueryContext &context, const QueryArgs &args, std::string &) {
	const TextureBudgetList list = texture_budget_list(context.core.view());
	const JsonPage page = page_of(args);
	JsonValue out = JsonValue::make_object();
	out.set("totals", texture_budget_totals_json(list));
	set_page(out, page, list.rows.size());
	JsonValue rows = JsonValue::make_array();
	for (size_t i = page.first(list.rows.size()); i < page.last(list.rows.size()); ++i)
		rows.push(texture_budget_row_json(list.rows[i]));
	out.set("textures", std::move(rows));
	return out;
}

// An import's options and what its uses ask of it (ADR 0046 S18, session/texture_import_state).
JsonValue answer_import_options(const QueryContext &context, const QueryArgs &args, std::string &error) {
	TextureImportState state;
	if (!texture_import_state(context.core.view(), args.text("path"), state, error)) return JsonValue::make_null();
	return texture_import_state_json(state);
}

namespace {

// The file a query's "path" names, else `fallback` (the page, the active document): "" with the error naming
// `what` and why there is no `fallback` (`none`) when neither names one.
std::string path_or(const QueryArgs &args, const std::string &fallback, const char *what, const char *none,
                    std::string &error) {
	const std::string path = args.text("path").empty() ? fallback : args.text("path");
	if (path.empty()) error = std::string("name the ") + what + " with \"path\" (" + none + ").";
	return path;
}

// Whether the project file `path` names is of `kind` (one the scan does not list passes: its answer holds nothing);
// false with the error naming `words`.
bool of_kind(const SessionView &view, const std::string &path, AssetKind kind, const char *words, std::string &error) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->named(path) : nullptr;
	if (!entry || entry->kind == kind) return true;
	error = path + " is not " + words + ".";
	return false;
}

} // namespace

// A file's card (session/file_card.h): what it is, what reads it, where a build puts it, what it defines, who
// names it and what it names, in words; the page showing with the record its Go to marked.
JsonValue answer_file_card(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = path_or(args, view.documents.page, "file", "no page shows", error);
	return path.empty() ? JsonValue::make_null() : file_card_json(shown_file_card(view, path));
}

JsonValue answer_used_by(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = path_or(args, view.documents.active, "file", "no document is active", error);
	return path.empty() ? JsonValue::make_null() : file_users_json(file_users(view, path));
}

JsonValue answer_terrain_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = path_or(args, view.documents.active, "terrain", "no document is active", error);
	if (path.empty() || !of_kind(view, path, AssetKind::Terrain, "a terrain (.trn)", error)) return JsonValue::make_null();
	return terrain_uses_json(terrain_uses(view, path));
}

JsonValue answer_environment_uses(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const SessionView &view = context.core.view();
	const std::string path = path_or(args, view.documents.active, "environment", "no document is active", error);
	if (path.empty() || !of_kind(view, path, AssetKind::Environment, "an environment (.env)", error))
		return JsonValue::make_null();
	return environment_uses_json(environment_uses(view, path));
}

// What the editor plays now (DI-36): the preview play, the previews' last sounds, each listening mission view.
JsonValue answer_sounds_playing(const QueryContext &context, const QueryArgs &, std::string &) {
	return sounds_playing_json(context.core);
}


} // namespace opennova::editor
