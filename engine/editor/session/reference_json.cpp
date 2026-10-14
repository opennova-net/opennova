#include <editor/session/session_json.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <base/io/cp1252.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/text_document.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/requirements/requirement_words.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/record_batch.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/workspace_parts.h>
#include <editor/session/terrain_stroke.h>
#include <base/io/strutil.h>
#include <editor/session/session_json_support.h>


namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

} // namespace

JsonValue graph_edge_to_json(const AssetGraph &graph, const GraphEdge &edge) {
	JsonValue out = JsonValue::make_object();
	// Every name is UTF-8 in the graph (a text's and a native file's read from the game's code page,
	// extract_from_text and extract_from_bytes).
	const bool text = edge.span.line != 0;
	const auto written = [](const std::string &value) { return value; };
	out.set("source", json_string(edge.source));
	if (!edge.record.empty()) out.set("record", json_string(edge.record));
	if (!edge.locator.empty()) out.set("locator", json_string(edge.locator));
	if (edge.address.row) out.set("address", address_to_json(edge.address));
	if (text) {
		JsonValue span = JsonValue::make_object();
		span.set("line", json_number(double(edge.span.line)));
		span.set("column", json_number(double(edge.span.column)));
		span.set("length", json_number(double(edge.span.length)));
		out.set("span", std::move(span));
	}
	out.set("field", json_string(edge.field));
	out.set("kind", json_string(reference_row(edge.kind).token));
	out.set("value", json_string(written(edge.value)));
	if (!edge.fallback.empty()) out.set("fallback", json_string(written(edge.fallback)));
	out.set("target", json_string(written(edge.target)));
	if (!edge.scope.empty()) out.set("scope", json_string(edge.scope));
	if (!edge.scopes_after.empty()) {
		JsonValue after = JsonValue::make_array();
		for (const std::string &scope : edge.scopes_after) after.push(json_string(scope));
		out.set("scopes_after", std::move(after));
	}
	if (!edge.scope_alternate.empty()) out.set("scope_alternate", json_string(edge.scope_alternate));
	if (!edge.scope_owner.empty()) out.set("scope_owner", json_string(edge.scope_owner));
	if (edge.optional) out.set("optional", JsonValue::make_bool(true));
	if (!edge.needs.empty()) out.set("needs", json_string(edge.needs));
	out.set("rewritable", JsonValue::make_bool(graph.rewrites(edge)));
	if (edge.through != ReferenceKind::None) out.set("through", json_string(reference_row(edge.through).token));
	if (edge.loader_arg >= 0) out.set("loader_arg", json_number(double(edge.loader_arg)));
	set_texture_role(out, edge.kind, edge.loader_arg);
	std::string file;
	const ReferenceStatus status = edge.target.empty() ? ReferenceStatus::NotAReference : graph.resolve(edge, &file);
	out.set("status", json_string(reference_status_token(status)));
	if (!file.empty()) out.set("file", json_string(file));
	return out;
}

JsonValue graph_symbol_to_json(const GraphSymbol &symbol) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(reference_row(symbol.kind).token));
	out.set("name", json_string(symbol.display));
	out.set("file", json_string(symbol.file));
	if (!symbol.record.empty()) out.set("record", json_string(symbol.record));
	if (!symbol.locator.empty()) out.set("locator", json_string(symbol.locator));
	if (symbol.address.row) out.set("address", address_to_json(symbol.address));
	if (!symbol.field.empty()) out.set("field", json_string(symbol.field));
	if (!symbol.scope.empty()) out.set("scope", json_string(symbol.scope));
	if (!symbol.value.empty()) out.set("value", json_string(symbol.value));
	if (symbol.inert) out.set("inert", JsonValue::make_bool(true));
	if (!symbol.inert_reason.empty()) out.set("inert_reason", json_string(symbol.inert_reason));
	if (symbol.line) out.set("line", json_number(double(symbol.line)));
	return out;
}

JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address,
		const std::string &id, const SessionView &view, const JsonPage &page) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	// The names the Inspector's picker offers, each by what it names (S15: an item id by its catalog's
	// name, an SSN by its entity's title, the player first): picker_choices, a field holding the
	// player's SSN listed as an entity field (FieldUse::picks).
	std::optional<GraphNameSource> names;
	if (view.findings.graph) names.emplace(*view.findings.graph);
	field = picked_as(field);
	const std::vector<ReferenceChoice> choices =
	        picker_choices(view.findings.graph.get(), document, address, field, names ? &*names : nullptr);
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(choices.size()); i < page.last(choices.size()); ++i) {
		const ReferenceChoice &choice = choices[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(choice.name));
		entry.set("kind", json_string(reference_row(choice.kind).token));
		entry.set("file", json_string(choice.file));
		if (!choice.record.empty()) entry.set("record", json_string(choice.record));
		if (!choice.label.empty()) entry.set("label", json_string(choice.label));
		entry.set("status", json_string(reference_status_token(choice.status)));
		if (choice.inert) {
			entry.set("inert", JsonValue::make_bool(true));
			entry.set("reason", json_string(choice.reason));
		}
		// A texture's: the file the reference set to it loads, and what it is (ADR 0046 S18).
		if (!choice.served.empty()) entry.set("served", json_string(choice.served));
		if (view.findings.graph && is_texture_reference(field.reference) && is_texture_reference(choice.kind))
			entry.set("texture", texture_reference_json(view, texture_reference(*view.findings.graph, field.reference, choice.name,
			                                                                     field.scope, field.loader_arg)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", json_string(field.schema->id));
	out.set("reference", json_string(reference_row(field.reference).token));
	if (!field.scope.empty()) out.set("scope", json_string(field.scope));
	// A number that forms a text key: the key's prefix, each choice named by its number.
	if (field.key_prefix) out.set("key", json_string(field.key_prefix));
	set_page(out, page, choices.size());
	out.set("choices", std::move(list));
	return out;
}

JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address,
		const std::string &id, const SessionView &view, const JsonPage &page) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	// A number that forms a text key (a name index, a waypoint's name id): the key's string, as the
	// Inspector's Go to beside its picker leads.
	FieldUse keyed;
	Value key;
	if (field.reference == ReferenceKind::None && view.findings.graph &&
	    keyed_reference(*view.findings.graph, field, value, keyed, key)) {
		field = keyed;
		value = key;
	}
	std::vector<ReferenceTarget> targets = view.findings.graph
			? reference_targets(*view.findings.graph, *view.project.scan, field, value)
			: std::vector<ReferenceTarget>();
	// A name nothing resolves: where it belongs (DI-17, a Go to always lands).
	ReferenceTarget home;
	if (targets.empty() && missing_target(field, value, view, home)) targets.push_back(std::move(home));
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(targets.size()); i < page.last(targets.size()); ++i) {
		const ReferenceTarget &target = targets[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("label", json_string(target.label));
		entry.set("file", json_string(target.file));
		if (!target.locator.empty()) entry.set("locator", json_string(target.locator));
		if (!target.field.empty()) entry.set("field", json_string(target.field));
		entry.set("editable", JsonValue::make_bool(target.editable));
		if (target.missing) entry.set("missing", JsonValue::make_bool(true));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", json_string(field.schema->id));
	out.set("reference", json_string(reference_row(field.reference).token));
	out.set("value", value_to_json(value));
	set_page(out, page, targets.size());
	out.set("targets", std::move(list));
	return out;
}

namespace {

JsonValue reference_target_json(const ReferenceTarget &target) {
	JsonValue entry = JsonValue::make_object();
	entry.set("label", json_string(target.label));
	entry.set("file", json_string(target.file));
	if (!target.locator.empty()) entry.set("locator", json_string(target.locator));
	if (!target.field.empty()) entry.set("field", json_string(target.field));
	entry.set("editable", JsonValue::make_bool(target.editable));
	if (target.missing) entry.set("missing", JsonValue::make_bool(true));
	return entry;
}

// How a completion's name is seen or heard before it is picked (DI-09): a sound set or a wave played through the
// one player (as the Inspector's Play: a menu SOUND's set from its own bank), a texture's thumbnail (the
// texture_thumbnail query of the file its loader opens), else its document opened at what it names (the model's
// picture, the effect's preview, the record's definition picture). Null for a name of nothing the project has.
JsonValue completion_preview(const AssetGraph *graph, const FieldUse &field, const ReferenceChoice &choice) {
	JsonValue preview = JsonValue::make_object();
	if (choice.kind == ReferenceKind::Sound) {
		preview.set("play", editor_request_to_json(request::play_set(choice.name, field.scope.substr(0, field.scope.find('/')))));
		return preview;
	}
	if (choice.kind == ReferenceKind::Wave && !choice.file.empty()) {
		preview.set("play", editor_request_to_json(request::play_sound(choice.file)));
		return preview;
	}
	if (is_texture_reference(choice.kind)) {
		if (choice.served.empty()) return JsonValue::make_null();
		preview.set("thumbnail", json_string(choice.served));
		return preview;
	}
	// A definition at its record (the symbol a choice named otherwise stands for), a file as itself.
	std::string locator;
	if (graph && reference_row(choice.kind).names_symbol())
		if (const GraphSymbol *symbol = graph->resolve_symbol(choice.kind, choice.symbol.empty() ? choice.name : choice.symbol,
		                                                      choice.symbol.empty() ? field.scope : choice.symbol_scope))
			locator = symbol->locator;
	if (choice.file.empty()) return JsonValue::make_null();
	preview.set("open", editor_request_to_json(request::open_document(choice.file, locator)));
	return preview;
}

} // namespace

JsonValue reference_completion_to_json(const Document &document, const NodeAddress &address, const std::string &id,
		const std::string &typed, const SessionView &view, const JsonPage &page) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const AssetGraph *graph = view.findings.graph.get();
	std::optional<GraphNameSource> names;
	if (graph) names.emplace(*graph);
	const FieldUse picking = picked_as(field);
	// The picker's names (picker_choices: each by what it names), those completing what is typed first.
	const std::vector<ReferenceCompletion> completions =
	        complete_reference(picker_choices(graph, document, address, picking, names ? &*names : nullptr), picking, typed);
	JsonValue out = JsonValue::make_object();
	out.set("field", json_string(field.schema->id));
	out.set("reference", json_string(reference_row(picking.reference).token));
	if (!picking.scope.empty()) out.set("scope", json_string(picking.scope));
	if (const size_t limit = field_name_limit(picking)) out.set("limit", json_number(double(limit)));
	out.set("value", value_to_json(value));
	out.set("prefix", json_string(typed));
	// What is typed, as the field would hold it set to it: whether the game's lookup finds it, where its Go to
	// leads (DI-17: a name nothing resolves where it belongs) and what fixes it (DI-15: Add it there first).
	Value as_typed = typed;
	if (picking.schema->type != FieldType::Text) {
		const std::optional<int> number = strutil::parse_int(typed);
		as_typed = number ? Value(int64_t(*number)) : Value(typed);
	}
	if (!typed.empty() && picking.reference != ReferenceKind::None) {
		const ReferenceStatus status =
		        graph ? reference_status(*graph, picking, as_typed) : ReferenceStatus::Unverified;
		JsonValue typed_json = JsonValue::make_object();
		typed_json.set("status", json_string(reference_status_token(status)));
		const size_t limit = field_name_limit(picking);
		typed_json.set("fits", JsonValue::make_bool(!limit || strutil::utf8_length(typed) <= limit));
		std::vector<ReferenceTarget> targets =
		        graph && view.project.scan ? reference_targets(*graph, *view.project.scan, picking, as_typed)
		                                   : std::vector<ReferenceTarget>();
		ReferenceTarget home;
		if (targets.empty() && missing_target(picking, as_typed, view, home)) targets.push_back(std::move(home));
		JsonValue listed = JsonValue::make_array();
		for (const ReferenceTarget &target : targets) listed.push(reference_target_json(target));
		typed_json.set("targets", std::move(listed));
		JsonValue fixes = JsonValue::make_array();
		Diagnostic finding;
		if (graph && missing_finding(*graph, document, address, picking, as_typed, finding))
			for (const ProblemFix &fix : fixes_for(finding, view)) {
				const Document *open = nullptr;
				for (const auto &held : view.documents.open)
					if (held && held->path() == fix.request.path) open = records_of(*held);
				const std::unique_ptr<Document> blank =
				        open || fix.request.edits.empty() ? nullptr : blank_names_for(view, fix.request.path);
				fixes.push(problem_fix_to_json(fix, open ? open : blank.get()));
			}
		typed_json.set("fixes", std::move(fixes));
		out.set("typed", std::move(typed_json));
	}
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(completions.size()); i < page.last(completions.size()); ++i) {
		const ReferenceCompletion &completion = completions[i];
		const ReferenceChoice &choice = completion.choice;
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(choice.name));
		if (!choice.label.empty()) entry.set("label", json_string(choice.label));
		entry.set("kind", json_string(reference_row(choice.kind).token));
		entry.set("file", json_string(choice.file));
		if (!choice.record.empty()) entry.set("record", json_string(choice.record));
		entry.set("status", json_string(reference_status_token(choice.status)));
		entry.set("prefix", JsonValue::make_bool(completion.prefix));
		if (completion.exact) entry.set("exact", JsonValue::make_bool(true));
		entry.set("fits", JsonValue::make_bool(completion.fits));
		if (choice.inert) {
			entry.set("inert", JsonValue::make_bool(true));
			entry.set("reason", json_string(choice.reason));
		}
		JsonValue preview = completion_preview(graph, picking, choice);
		if (!preview.is_null()) entry.set("preview", std::move(preview));
		list.push(std::move(entry));
	}
	set_page(out, page, completions.size());
	out.set("choices", std::move(list));
	return out;
}

JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits, const JsonPage &page) {
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(hits.size()); i < page.last(hits.size()); ++i) {
		const DocumentHit &hit = hits[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("id",
		          json_number(double(hit.address.child ? hit.address.child : hit.address.row)));
		entry.set("address", address_to_json(hit.address));
		entry.set("record", json_string(hit.record));
		entry.set("locator", json_string(hit.locator));
		entry.set("field", json_string(hit.field));
		entry.set("label", json_string(hit.label));
		entry.set("text", json_string(hit.text));
		entry.set("at", json_number(double(hit.at)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	set_page(out, page, hits.size());
	out.set("hits", std::move(list));
	return out;
}

JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits, const JsonPage &page) {
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(hits.size()); i < page.last(hits.size()); ++i) {
		const GraphSearchHit &hit = hits[i];
		JsonValue entry = hit.symbol ? graph_symbol_to_json(*hit.symbol) : JsonValue::make_object();
		if (!hit.symbol) {
			entry.set("kind", json_string("file"));
			entry.set("name", json_string(hit.name));
			entry.set("file", json_string(hit.file));
			// Found by a record that names it (a model by its item's name, S17).
			if (!hit.via.empty()) {
				entry.set("via", json_string(hit.via));
				entry.set("via_file", json_string(hit.via_file));
			}
		}
		if (!hit.words.empty()) entry.set("words", json_string(hit.words));
		entry.set("usages", json_number(double(hit.usages)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	set_page(out, page, hits.size());
	out.set("hits", std::move(list));
	return out;
}

} // namespace opennova::editor
