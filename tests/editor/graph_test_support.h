#pragma once
// The steps the asset graph's tests share (editor_asset_graph, editor_reference_queries): a field
// as it applies to its record and where its Go to leads, a Go to served as the Inspector raises
// it, a menu window's field set and its first APPEARANCE row made an image, a record added, the
// missing-reference findings, and a menu's text made of screens and windows.
#include <string>
#include <utility>
#include <vector>

#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>

#include "editor/menu_test_support.h"

namespace graph_test {

namespace editor = opennova::editor;

// A field as it applies to a record.
inline editor::FieldUse field_on(const editor::Document &document,
		const editor::NodeAddress &record, const std::string &id) {
	for (const editor::FieldSchema &schema : document.fields(record.kind))
		if (schema.id == id) return document.field_on(record, schema);
	return editor::FieldUse();
}

// Where a field's Go to leads (reference_targets), over the view's graph and scan.
inline std::vector<editor::ReferenceTarget> targets_of(const editor::Document &document,
		const editor::NodeAddress &record, const std::string &id, const editor::SessionView &view) {
	editor::Value value;
	if (!view.findings.graph || !document.get(record, id, value)) return {};
	return editor::reference_targets(
			*view.findings.graph, *view.project.scan, field_on(document, record, id), value);
}

// A Go to served as the Inspector raises it (window_requests::go_to): the file opened at the
// record by its locator, its field shown.
inline void go_to(editor::ProjectSession &session, const editor::ReferenceTarget &target) {
	session.handle(editor::request::open_document(target.file, target.locator, target.field));
}

inline bool has_missing(const std::vector<editor::Diagnostic> &diagnostics,
		const std::string &field, editor::DiagnosticSeverity severity) {
	for (const editor::Diagnostic &d : diagnostics)
		if (d.code == "reference.missing" && d.field == field && d.severity == severity) return true;
	return false;
}

// The missing-reference finding on a field that says what it misses: the reference's kind.
inline const editor::Diagnostic *missing_of(const std::vector<editor::Diagnostic> &diagnostics,
                                            const std::string &field, editor::ReferenceKind kind) {
	for (const editor::Diagnostic &d : diagnostics)
		if (const editor::ReferenceSubject *missing = editor::reference_subject(d);
		    missing && d.code == "reference.missing" && d.field == field && missing->kind == kind)
			return &d;
	return nullptr;
}

inline void edit_window(editor::ProjectSession &session, const editor::Document &document,
		const editor::NodeAddress &address, const char *field, editor::Value value) {
	editor::Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	session.handle(editor::request::edit_record(document.path(), std::move(edit)));
}

// A window's first APPEARANCE row made an image of `texture` (one batch).
inline void set_image(editor::ProjectSession &session, const editor::Document &document,
                      const editor::NodeAddress &window, const std::string &texture) {
	session.handle(editor::request::edit_record(
			document.path(), menu_test::image_edits(document, window, texture)));
}

// A new record of `token` inside `owner`: its address.
inline editor::NodeAddress add_record(editor::ProjectSession &session,
		const editor::Document &document, const editor::NodeAddress &owner, const char *token) {
	editor::Edit add;
	add.operation = editor::EditOperation::Add;
	add.address = {owner.row, document.kind_from_name(token), 0};
	add.parent = owner.child;
	session.handle(editor::request::edit_record(document.path(), std::move(add)));
	return document.address_of(document.last_added());
}

// A window with a POSITION (a WINDOW with no child element is never created).
inline std::string window(
		const char *type, const char *name, const std::string &body = std::string()) {
	std::string out = std::string("<WINDOW TYPE=\"") + type + "\"" +
	                  (*name ? std::string(" NAME=\"") + name + "\"" : "") + ">\r\n";
	return out + "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n" + body +
	       "</WINDOW>\r\n";
}

inline std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN>\r\n<NAME>") + name + "</NAME>\r\n" + body + "</SCREEN>\r\n";
}

} // namespace graph_test
