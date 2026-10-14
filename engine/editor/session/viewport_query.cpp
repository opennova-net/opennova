#include <editor/session/viewport_query.h>

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

// --- the viewport query's ops (S13 V7) -------------------------------------------------------------

// What an op answers from: the session's view, the viewport the args name (resolved and followed
// now), the args, and the page they ask.
struct ViewportReadContext {
	const SessionView &view;
	const ViewportModel &model;
	const QueryArgs &args;
	JsonPage page;
};
using ViewportAnswer = JsonValue (*)(const ViewportReadContext &read, std::string &error);

JsonValue viewport_state(const ViewportReadContext &read, std::string &) {
	return viewport_to_json(read.view, read.model, read.page);
}
JsonValue viewport_items(const ViewportReadContext &read, std::string &) {
	return viewport_items_to_json(read.view, read.model, read.page);
}
JsonValue viewport_hit(const ViewportReadContext &read, std::string &error) {
	// A kind with no canvas (the script's: its device is a control that owns the pointer) has no picture
	// a point of it names anything of: refused, never answered as a hit on nothing.
	if (!viewport_kind_row(read.model.kind()).canvas) {
		error = std::string("a ") + viewport_kind_token(read.model.kind()) +
				" viewport has no canvas: its device is a control that owns the pointer, so no point of it names "
				"anything (op items reads what it lists).";
		return JsonValue::make_null();
	}
	// The point checked as a number a float holds (check_args).
	const ViewportHit hit = read.model.hit(viewport_context(read.view, read.model), float(read.args.number("x")),
			float(read.args.number("y")));
	return viewport_hit_to_json(read.model, hit);
}
JsonValue viewport_box(const ViewportReadContext &read, std::string &error) {
	if (!viewport_kind_row(read.model.kind()).canvas) {
		error = std::string("a ") + viewport_kind_token(read.model.kind()) +
				" viewport has no canvas: no box of it takes anything (op items reads what it lists).";
		return JsonValue::make_null();
	}
	return viewport_box_to_json(read.view, read.model,
			read.model.box(viewport_context(read.view, read.model), float(read.args.number("x")),
					float(read.args.number("y")), float(read.args.number("x2")), float(read.args.number("y2"))));
}
JsonValue viewport_notes(const ViewportReadContext &read, std::string &) {
	return viewport_notes_to_json(read.view, read.model, read.page);
}
JsonValue viewport_render(const ViewportReadContext &read, std::string &error) {
	return read.model.render_json(viewport_context(read.view, read.model).input, NodeId(read.args.integer("row")),
			read.page, error);
}
JsonValue viewport_palette(const ViewportReadContext &read, std::string &error) {
	return read.model.palette_json(read.view, read.args.text("text"), read.page, error);
}

// The ops by name, in their order on the wire.
enum class ViewportOp : uint8_t { State, Items, Hit, Box, Notes, Render, Palette, kCount };

// What an op takes beside the params every op takes (path, kind, op): a page (offset, limit), a point
// of the picture (x, y), a row (row), a box's other corner (x2, y2), a search (text).
enum ViewportTakes : uint8_t {
	kViewportPage = 1u << 0,
	kViewportPoint = 1u << 1,
	kViewportRow = 1u << 2,
	kViewportCorner = 1u << 3,
	kViewportText = 1u << 4,
};

// One op: its token, what it takes and of that what it needs (each named, a point both its params),
// and what answers it.
struct ViewportOpRow {
	ViewportOp op;
	const char *token;
	uint8_t takes;
	uint8_t needs;
	ViewportAnswer answer;
};

constexpr ViewportOpRow kViewportOps[] = {
	{ ViewportOp::State, "state", kViewportPage, 0, viewport_state },
	{ ViewportOp::Items, "items", kViewportPage, 0, viewport_items },
	{ ViewportOp::Hit, "hit", kViewportPoint, kViewportPoint, viewport_hit },
	{ ViewportOp::Box, "box", kViewportPoint | kViewportCorner, kViewportPoint | kViewportCorner, viewport_box },
	{ ViewportOp::Notes, "notes", kViewportPage, 0, viewport_notes },
	{ ViewportOp::Render, "render", kViewportPage | kViewportRow, kViewportRow, viewport_render },
	{ ViewportOp::Palette, "palette", kViewportPage | kViewportText, 0, viewport_palette },
};

// The flag of a viewport query param: 0 for those every op takes (path, kind, op), 0xFF for one no
// op takes (none: static_asserted).
constexpr uint8_t viewport_param_flag(const char *name) {
	return same_text(name, "path") || same_text(name, "kind") || same_text(name, "op") ? 0
			: same_text(name, "offset") || same_text(name, "limit")                    ? kViewportPage
			: same_text(name, "x") || same_text(name, "y")                             ? kViewportPoint
			: same_text(name, "x2") || same_text(name, "y2")                           ? kViewportCorner
			: same_text(name, "row")                                                   ? kViewportRow
			: same_text(name, "text")                                                  ? kViewportText
																					   : 0xFF;
}

} // namespace

// The ops' tokens and the kinds', the choices of the query's op and kind (QueryChoices).
const char *viewport_op_choice(size_t index) {
	return index < std::size(kViewportOps) ? kViewportOps[index].token : nullptr;
}

const char *viewport_kind_choice(size_t index) {
	return index < kViewportKindCount ? viewport_kind_token(static_cast<ViewportKind>(index)) : nullptr;
}

JsonValue answer_viewport(const QueryContext &context, const QueryArgs &args, std::string &error) {
	// The op (one of its tokens: check_args refused any other), then a param it does not take refused,
	// naming those it takes, as is one it needs left out.
	const std::string op = args.text("op");
	const ViewportOpRow *read = &kViewportOps[0];
	for (const ViewportOpRow &row : kViewportOps)
		if (op == row.token) read = &row;
	const EditorQueryRow &row = editor_query_row(EditorQueryKind::Viewport);
	std::string takes;
	for (size_t i = 0; i < row.param_count; ++i) {
		const uint8_t flag = viewport_param_flag(row.params[i].name);
		if (!flag || (read->takes & flag)) takes += (takes.empty() ? "" : ", ") + std::string(row.params[i].name);
	}
	for (size_t i = 0; i < row.param_count; ++i) {
		const uint8_t flag = viewport_param_flag(row.params[i].name);
		if (args.has(row.params[i].name) && flag && !(read->takes & flag)) {
			error = "op " + op + " takes no \"" + row.params[i].name + "\" (it takes " + takes + ").";
			return JsonValue::make_null();
		}
	}
	if ((read->needs & kViewportPoint) && !(args.has("x") && args.has("y"))) {
		error = "op " + op + " needs \"x\" and \"y\", the point in the viewport's units (a menu's design units, a "
							 "model's picture pixels).";
		return JsonValue::make_null();
	}
	if ((read->needs & kViewportCorner) && !(args.has("x2") && args.has("y2"))) {
		error = "op " + op + " needs \"x2\" and \"y2\", the box's other corner in the viewport's units.";
		return JsonValue::make_null();
	}
	if ((read->needs & kViewportRow) && !args.has("row")) {
		error = "op " + op + " needs \"row\", the row by its identity (a menu's screen).";
		return JsonValue::make_null();
	}
	// The viewport of the kind named (else the one the document shows in), followed now, so what it
	// answers is the document as it is.
	SessionCore &core = context.core;
	ViewportKind kind = ViewportKind::kCount;
	if (args.has("kind")) viewport_kind_from_token(args.text("kind"), kind);
	const DocumentBase *document = open_document_of(context, args, error);
	std::string at = document ? document->path() : std::string();
	// A file Files selects that a kind draws open or not (S18: a texture), by its path or its name.
	if (!document && args.has("path") && core.view().project.open) {
		const AssetScan &scan = *core.view().project.scan;
		const AssetEntry *entry = scan.at_path(args.text("path"));
		if (!entry) entry = scan.find(args.text("path"));
		for (size_t i = 0; entry && i < kViewportKindCount; ++i)
			if ((kind == ViewportKind::kCount || kind == static_cast<ViewportKind>(i)) &&
			    draws_selected_file(core.view(), entry->relative_path, static_cast<ViewportKind>(i))) {
				at = entry->relative_path;
				error.clear();
			}
	}
	const ViewportModel *model = at.empty() ? nullptr : core.viewports().resolve(core.view(), at, kind, error);
	if (!model) return JsonValue::make_null();
	return read->answer(ViewportReadContext{ core.view(), *model, args, page_of(args) }, error);
}


namespace {

// The viewport query's ops (S13 V7): one row per ViewportOp in its order, each a token of its own
// and an answer, what it needs among what it takes, a point no page; every param of the query one
// every op takes or one an op takes by its flag (none taken unnamed).
constexpr bool viewport_ops_hold() {
	if (std::size(kViewportOps) != static_cast<size_t>(ViewportOp::kCount)) return false;
	for (size_t i = 0; i < std::size(kViewportOps); ++i) {
		const ViewportOpRow &op = kViewportOps[i];
		if (op.op != static_cast<ViewportOp>(i) || !op.token[0] || !op.answer) return false;
		if ((op.needs & ~op.takes) || ((op.takes & kViewportPoint) && (op.takes & kViewportPage))) return false;
		for (size_t j = i + 1; j < std::size(kViewportOps); ++j)
			if (same_text(op.token, kViewportOps[j].token)) return false;
	}
	for (const QueryParam &param : kViewportParams)
		if (viewport_param_flag(param.name) == 0xFF) return false;
	return true;
}
static_assert(viewport_ops_hold(),
		"the viewport ops follow ViewportOp, each a token, an answer and what it needs among what it takes, "
		"every param of the query taken by name");

} // namespace

} // namespace opennova::editor
