#include <editor/session/catalog_json.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include <editor/session/finding_codes.h>
#include <editor/session/record_batch.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view_json.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

const char *served_token(ServedBy by) {
	switch (by) {
		case ServedBy::Session:
			return "session";
		case ServedBy::Shell:
			return "shell";
		case ServedBy::ShellNeedsPerson:
			return "person";
	}
	return "session";
}

JsonValue fields_of(RequestFieldSet set) {
	JsonValue out = JsonValue::make_array();
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (set & field_bit(id))
			out.push(json_string(request_field(id).token));
	}
	return out;
}

} // namespace

void request_catalog_json(JsonValue &out) {
	JsonValue requests = JsonValue::make_array();
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const RequestKindRow &row = request_kind_row(static_cast<EditorRequestKind>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", json_string(row.token));
		entry.set("served_by", json_string(served_token(row.served_by)));
		entry.set("takes", fields_of(row.params.takes));
		entry.set("needs", fields_of(row.params.required));
		entry.set("doc", json_string(row.doc));
		requests.push(std::move(entry));
	}
	out.set("requests", std::move(requests));
	JsonValue fields = JsonValue::make_array();
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const RequestField &field = request_field(static_cast<RequestFieldId>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("field", json_string(field.token));
		entry.set("type", json_string(request_json_token(field.json)));
		entry.set("doc", json_string(field.doc));
		fields.push(std::move(entry));
	}
	out.set("fields", std::move(fields));
	// The batch form of a request's edits (record_batch.h): its forms, ops and members, from which
	// the editor MCP makes its edit schema.
	JsonValue batch = JsonValue::make_object();
	JsonValue forms = JsonValue::make_array();
	for (size_t i = 0; i < kRecordBatchFormCount; ++i) {
		const auto form = static_cast<RecordBatchForm>(i);
		JsonValue entry = JsonValue::make_object();
		entry.set("form", json_string(batch_form_token(form)));
		entry.set("doc", json_string(batch_form_doc(form)));
		forms.push(std::move(entry));
	}
	batch.set("forms", std::move(forms));
	JsonValue ops = JsonValue::make_array();
	for (const BatchOp &row : batch_ops()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("op", json_string(row.token));
		entry.set("form", json_string(batch_form_token(row.form)));
		entry.set("doc", json_string(row.doc));
		ops.push(std::move(entry));
	}
	batch.set("ops", std::move(ops));
	JsonValue members = JsonValue::make_array();
	for (const BatchMember &row : batch_members()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(row.name));
		entry.set("type", json_string(batch_json_token(row.json)));
		JsonValue read_by = JsonValue::make_array();
		for (size_t i = 0; i < kRecordBatchFormCount; ++i)
			if (row.forms & batch_form_bit(static_cast<RecordBatchForm>(i)))
				read_by.push(json_string(batch_form_token(static_cast<RecordBatchForm>(i))));
		entry.set("forms", std::move(read_by));
		if (row.minimum >= 0) entry.set("minimum", json_number(double(row.minimum)));
		if (row.only[0]) entry.set("only", json_string(row.only));
		entry.set("doc", json_string(row.doc));
		members.push(std::move(entry));
	}
	batch.set("members", std::move(members));
	out.set("batch", std::move(batch));
	// What the windows show of their own (session/workspace_parts.h): each part a set_workspace names, its
	// members with their JSON types, and the windows its focus brings forward, from which the editor MCP
	// makes the workspace field's schema.
	JsonValue workspace = JsonValue::make_object();
	JsonValue parts = JsonValue::make_array();
	size_t part_count = 0;
	const WorkspacePartRow *part_rows = workspace_parts(part_count);
	for (size_t p = 0; p < part_count; ++p) {
		const WorkspacePartRow &row = part_rows[p];
		JsonValue entry = JsonValue::make_object();
		entry.set("part", json_string(row.token));
		JsonValue part_members = JsonValue::make_array();
		for (size_t m = 0; m < row.member_count; ++m) {
			JsonValue member = JsonValue::make_object();
			member.set("name", json_string(row.members[m].token));
			member.set("type", json_string(workspace_json_token(row.members[m].json)));
			member.set("doc", json_string(row.members[m].doc));
			if (row.members[m].longest) member.set("max_length", json_number(double(row.members[m].longest)));
			part_members.push(std::move(member));
		}
		entry.set("members", std::move(part_members));
		entry.set("doc", json_string(row.doc));
		parts.push(std::move(entry));
	}
	workspace.set("parts", std::move(parts));
	JsonValue windows = JsonValue::make_array();
	for (size_t i = 0; const char *token = workspace_window_token(i); ++i) windows.push(json_string(token));
	workspace.set("focus", std::move(windows));
	out.set("workspace", std::move(workspace));
}

void view_catalog_json(JsonValue &out, const std::vector<Diagnostic> &diagnostics) {
	JsonValue sections = JsonValue::make_array();
	for (size_t i = 0; i < kViewSectionCount; ++i) {
		const ViewSectionRow &row = view_section_row(static_cast<ViewSection>(i));
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(row.token));
		JsonValue concerns = JsonValue::make_array();
		for (size_t c = 0; c < kViewConcernCount; ++c)
			if (row.concerns & concern_bit(static_cast<ViewConcern>(c)))
				concerns.push(json_string(view_concern_token(static_cast<ViewConcern>(c))));
		entry.set("concerns", std::move(concerns));
		entry.set("doc", json_string(row.doc));
		sections.push(std::move(entry));
	}
	out.set("sections", std::move(sections));
	JsonValue concerns = JsonValue::make_array();
	for (size_t i = 0; i < kViewConcernCount; ++i)
		concerns.push(json_string(view_concern_token(static_cast<ViewConcern>(i))));
	out.set("concerns", std::move(concerns));
	// Every finding code the session and the types know (S13 A6: the rows of the editor's own
	// table, then each document type's), with what Problems does with a finding of it and how many
	// of the findings the session holds now (the Problems rows) carry it, counted by the row each
	// keeps; then, by token, any row held findings carry that no table lists (a test's), as table
	// none, and a Diagnostic no finding was made into (its code "").
	std::map<const FindingCodeRow *, size_t> held;
	for (const Diagnostic &d : diagnostics)
		++held[d.row()];
	JsonValue findings = JsonValue::make_array();
	const auto listed = [&findings](const FindingCodeRow &row, const char *table, size_t count) {
		JsonValue entry = JsonValue::make_object();
		entry.set("code", json_string(row.token ? row.token : ""));
		entry.set("table", json_string(table));
		entry.set("fixes", json_string(finding_fix_token(row.fixes)));
		if (row.rewrite_does)
			entry.set("rewrite_does", json_string(row.rewrite_does));
		entry.set("blocks_save", JsonValue::make_bool(row.blocks_save));
		entry.set("gates_build", JsonValue::make_bool(row.gates_build));
		entry.set("place", json_string(finding_place_token(row.place)));
		entry.set("group", json_string(finding_group_key(row.group)));
		entry.set("source", json_string(finding_source_token(row)));
		if (row.problem != FindingProblem::None)
			entry.set("problem", json_string(finding_problem_token(row.problem)));
		entry.set("count", json_number(double(count)));
		findings.push(std::move(entry));
	};
	for (const NamedFindingTable &table : finding_tables()) {
		for (const FindingCodeRow &row : table.rows) {
			const auto count = held.find(&row);
			listed(row, table.owner, count == held.end() ? 0 : count->second);
			if (count != held.end())
				held.erase(count);
		}
	}
	static const FindingCodeRow kNoRow;
	std::vector<std::pair<const FindingCodeRow *, size_t>> unlisted;
	for (const auto &[row, count] : held)
		unlisted.emplace_back(row ? row : &kNoRow, count);
	std::sort(unlisted.begin(), unlisted.end(), [](const auto &a, const auto &b) {
		return std::string(a.first->token ? a.first->token : "") <
		       std::string(b.first->token ? b.first->token : "");
	});
	for (const auto &[row, count] : unlisted)
		listed(*row, "none", count);
	out.set("finding_codes", std::move(findings));
}

} // namespace opennova::editor
