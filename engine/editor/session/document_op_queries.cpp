#include <editor/session/document_op_queries.h>

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

// --- a model's surfaces (S17) --------------------------------------------------------------------

constexpr const char *kSurfaceOps[] = { "materials", "set", "flag", "faces" };

} // namespace

const char *surface_op_choice(size_t index) { return index < std::size(kSurfaceOps) ? kSurfaceOps[index] : nullptr; }

const char *surface_flag_choice(size_t index) {
	return index < model_face_flags().size() ? model_face_flags()[index].token : nullptr;
}

JsonValue answer_model_surfaces(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const Document *document = document_of(context, args, error);
	const auto *model = dynamic_cast<const ModelDocument *>(document);
	if (document && !model) error = document->path() + " is no model.";
	if (!model) return JsonValue::make_null();
	const std::string op = args.text("op");
	if (op == "materials") return model_surfaces_to_json(*model, page_of(args));
	if (!args.has("id")) {
		error = "op " + op + " needs \"id\", the material by its identity.";
		return JsonValue::make_null();
	}
	const NodeId material = NodeId(args.integer("id"));
	if (op == "faces") return model_differing_faces_to_json(*model, material, page_of(args));
	std::vector<Edit> edits;
	std::string refusal;
	bool ok = false;
	if (op == "set") {
		if (!args.has("surface")) {
			error = "op set needs \"surface\", a face byte.";
			return JsonValue::make_null();
		}
		ok = model_surface_edits(*model, material, args.integer("surface"), edits, refusal);
	} else {
		if (!args.has("flag")) {
			error = "op flag needs \"flag\": both_sides, bullets_pass or front_only.";
			return JsonValue::make_null();
		}
		uint32_t bit = 0;
		for (const ModelFaceFlag &flag : model_face_flags())
			if (args.text("flag") == flag.token) bit = flag.bit;
		ok = model_face_flag_edits(*model, material, bit, args.boolean("on"), edits, refusal);
	}
	return planned_edits(*model, ok, edits, refusal);
}

namespace {

// --- a script's help (S15) -----------------------------------------------------------------------

constexpr const char *kScriptOps[] = { "complete", "hover", "definition", "mission_script" };

} // namespace

const char *script_op_choice(size_t index) { return index < std::size(kScriptOps) ? kScriptOps[index] : nullptr; }

JsonValue answer_script_assist(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string op = args.text("op");
	const DocumentBase *open = open_document_of(context, args, error);
	if (!open) return JsonValue::make_null();
	const SessionView &view = context.core.view();
	JsonValue out = JsonValue::make_object();
	if (op == "mission_script") {
		if (!dynamic_cast<const MissionDocument *>(records_of(*open))) {
			error = open->path() + " is no mission.";
			return JsonValue::make_null();
		}
		const MissionScript script = mission_script(view, open->path());
		out.set("name", json_string(script.name));
		out.set("path", json_string(script.path));
		out.set("create_at", json_string(script.create_at));
		out.set("held", JsonValue::make_bool(!script.path.empty()));
		return out;
	}
	const TextDocument *script = text_of(*open);
	if (!script) {
		error = open->path() + " is no text document.";
		return JsonValue::make_null();
	}
	const size_t line = size_t(std::max<int64_t>(1, args.integer("line")));
	const size_t column = size_t(std::max<int64_t>(1, args.integer("column")));
	if (op == "complete") {
		const ScriptCompletions completions = script_completions(view, *script, line, column);
		out.set("column", json_number(double(completions.column)));
		out.set("typed", json_string(completions.typed));
		out.set("expected", json_string(completions.expected));
		const JsonPage page = page_of(args);
		set_page(out, page, completions.items.size());
		JsonValue items = JsonValue::make_array();
		for (size_t i = page.first(completions.items.size()); i < page.last(completions.items.size()); ++i) {
			const ScriptCompletion &item = completions.items[i];
			JsonValue entry = JsonValue::make_object();
			entry.set("label", json_string(item.label));
			entry.set("insert", json_string(item.insert));
			entry.set("kind", json_string(item.kind));
			entry.set("detail", json_string(item.detail));
			items.push(std::move(entry));
		}
		out.set("items", std::move(items));
		return out;
	}
	if (op == "hover") {
		ScriptHover hover;
		const bool known = script_hover(view, *script, line, column, hover);
		out.set("word", json_string(known ? hover.word : std::string()));
		out.set("column", json_number(double(known ? hover.column : column)));
		out.set("length", json_number(double(known ? hover.length : 0)));
		out.set("text", json_string(known ? hover.text : std::string()));
		return out;
	}
	EditorRequest request;
	out.set("request", script_definition(view, *script, line, column, request) ? editor_request_to_json(request)
	                                                                            : JsonValue::make_null());
	return out;
}


} // namespace opennova::editor
