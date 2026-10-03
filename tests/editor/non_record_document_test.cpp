// A document type of another kind than records (ADR 0046 S13 D6), a test's (BlobDocument, the
// base alone) standing in for the stylesheet's type (DocumentTypeStandIn). The core's record
// readers leave its files alone and say so: the graph reads none (graph_reads_kind false,
// extract_from_bytes giving nothing), so an import lists the kind as not followed; the
// validation's input answers document.no_records for its file, open or closed, without reading
// it (S13 D4: the validation cache's own finding for the file, its document never made); a rename
// refuses a site in it with that reason, where a kind with no editor gets its own
// words. Through a session the file opens as a document of that kind: the lifecycle's callers
// find it (document_base_for), the rows' do not (document_for null); it takes an Apply of its
// type's payload and nothing else, undoes, redoes and saves; Copy, Cut, Paste, Duplicate and
// Revert to saved refuse it (document.no_records); the record batch refuses it without opening it
// again (S13 A5: a request's edits on the wire); the document query answers its lifecycle alone
// and the record query refuses it; its JSON is the base's alone. The stand-in gone, the
// stylesheet's own type answers again.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/document_types.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/blob_document.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using editor_test::BlobDocument;
using editor_test::BlobReplace;
using opennova::io::JsonValue;

namespace {

std::unique_ptr<DocumentBase> make_blob() { return std::make_unique<BlobDocument>(); }

std::vector<Diagnostic> validate_nothing(const DocumentBase &) { return {}; }

// The blob type, in the stylesheet type's place.
const DocumentType kBlobType{DocumentTypeId::Styles, "blob", make_blob, validate_nothing};

bool has_finding(const ActionOutcome &outcome, const char *code) {
	for (const Diagnostic &d : outcome.findings)
		if (d.code() == code) return true;
	return false;
}

std::string file_text(const std::string &path) {
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(path, bytes, message)) return "<unread>";
	return std::string(bytes.begin(), bytes.end());
}

} // namespace

static int test_non_record_type() {
	editor_test::TempProjectDir dir("opennova_editor_non_record_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Blobs"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const AssetEntry asset = *style;
	const AssetKind kind = AssetKind::MenuStyle;
	const std::string &path = asset.relative_path;
	const std::string file = dir.file("project") + "/" + path;
	const std::string original = file_text(file);
	TEST_EXPECT(document_content(*document_type_for(kind)) == DocumentContent::Records);
	{
		const DocumentTypeStandIn stand_in(kBlobType);
		// The registry answers the stand-in; its documents hold neither records nor a text.
		TEST_EXPECT(document_type_for(kind) == &kBlobType && is_editable_kind(kind));
		TEST_EXPECT(document_content(kBlobType) == DocumentContent::Other);
		TEST_EXPECT(document_content(*document_type(DocumentTypeId::Menu)) == DocumentContent::Records);
		TEST_EXPECT(document_content(*document_type(DocumentTypeId::Script)) == DocumentContent::Text);
		// The graph reads none of its files; an import lists the kind as not followed.
		TEST_EXPECT(!graph_reads_kind(kind));
		Extracted extracted;
		Diagnostic error;
		const std::vector<uint8_t> bytes(original.begin(), original.end());
		TEST_EXPECT(extract_from_bytes(path, kind, bytes, "jo", extracted, error));
		TEST_EXPECT(extracted.edges.empty() && extracted.symbols.empty());
		TEST_EXPECT(references_unread(kind));
		// The validation: document.no_records for its file, closed and open, the file unread and
		// no document of the type made for it.
		const ProjectPaths paths = ProjectPaths::for_root(dir.file("project"));
		ValidationCache cache;
		std::vector<std::shared_ptr<const DocumentBase>> open;
		const auto no_records = [&] {
			const ValidationInput input{paths, *view.project.document, *view.project.scan, open};
			cache.begin();
			const std::vector<Diagnostic> findings = cache.file_findings(input, asset);
			cache.end();
			return findings.size() == 1 && findings.front().code() == "document.no_records" &&
			       cache.stats().files_loaded == 0 && !cache.records_checked(path);
		};
		TEST_EXPECT(no_records());
		auto blob = std::make_shared<BlobDocument>();
		TEST_EXPECT(blob->load(file, path, kind, "jo", error));
		open.push_back(blob);
		TEST_EXPECT(no_records());
		// A rename refuses a site in it with that reason; a kind with no editor gets its own
		// words.
		SymbolRenamePlan plan;
		plan.kind = ReferenceKind::StyleVar;
		plan.file = path;
		plan.old_name = "TRIM_COLOR";
		plan.new_name = "TRIM_COLOUR";
		RenameSite site;
		site.file = path;
		site.kind = kind;
		site.locator = "0";
		site.field = "name";
		site.before = "TRIM_COLOR";
		site.after = "TRIM_COLOUR";
		plan.sites.push_back(site);
		std::vector<Diagnostic> refusals;
		TEST_EXPECT(!check_symbol_rename(paths, *view.project.document, *view.project.scan,
		                                 AssetGraph(), plan, open, refusals));
		TEST_EXPECT(refusals.size() == 1 && refusals.front().code() == "rename.site" &&
		            refusals.front().message.find("holds no records") != std::string::npos);

		// Through a session: the lifecycle's callers find it, the rows' do not.
		const uint64_t entries = session.handle_entries();
		session.handle(request::open_document(path));
		TEST_EXPECT(session.outcome().done());
		DocumentBase *opened = session.document_base_for(path);
		TEST_EXPECT(opened && !opened->as_records() && !session.document_for(path));
		TEST_EXPECT(view.documents.active == path && !view.documents.selection.primary.row);
		TEST_EXPECT(!view.documents.open.empty() && !records_of(*view.documents.open.back()));
		// An Apply of its type's payload is taken, undone and redone; a Set is not.
		Edit replace;
		replace.operation = EditOperation::Apply;
		replace.payload = std::make_shared<BlobReplace>("BLOB FFFFFFFF\r\n");
		session.handle(request::edit_record(path, replace));
		TEST_EXPECT(session.outcome().done() && opened->dirty() && session.documents_dirty());
		Edit set;
		set.field = "name";
		set.value = std::string("X");
		session.handle(request::edit_record(path, set));
		TEST_EXPECT(!session.outcome().done());
		TEST_EXPECT(has_finding(session.outcome(), "document.payload"));
		session.handle(request::undo(path));
		TEST_EXPECT(!opened->dirty());
		session.handle(request::redo(path));
		TEST_EXPECT(opened->dirty());
		// What acts on records refuses it: it holds none.
		Edit field;
		field.field = "name";
		for (const EditorRequest &refused :
		     {request::copy(path), request::cut(path), request::paste(path),
		      request::duplicate(path), request::revert_to_saved(path, {field})}) {
			session.handle(refused);
			TEST_EXPECT(!session.outcome().done());
			TEST_EXPECT(has_finding(session.outcome(), "document.no_records"));
		}
		// A batch on the wire (S13 A5: an edit_record's edits) refuses it as it is read, and sends
		// no OpenDocument to try again.
		const uint64_t before_batch = session.handle_entries();
		JsonValue batch;
		std::string refusal;
		TEST_EXPECT(opennova::io::json_parse(
		        "{\"kind\":\"edit_record\",\"path\":\"" + path +
		                "\",\"open_first\":true,\"edits\":[{\"op\":\"set\",\"id\":1,"
		                "\"field\":\"name\",\"value\":\"X\"}]}",
		        batch, refusal));
		const JsonValue answer = session.handle_json(batch);
		TEST_EXPECT(!answer.get_bool("ok", true) &&
		            answer.get_string("error", "").find("holds no records") != std::string::npos);
		TEST_EXPECT(session.handle_entries() == before_batch);
		// The queries: the document answers its lifecycle alone; what reads records refuses it.
		JsonValue args = JsonValue::make_object();
		args.set("path", opennova::io::json_string(path));
		const JsonValue lifecycle = session.query("document", args, refusal);
		TEST_EXPECT(lifecycle.get_string("path", "") == path && lifecycle.get("revision") &&
		            !lifecycle.get("rows") && !lifecycle.get("row_count"));
		args.set("id", opennova::io::json_number(1.0));
		TEST_EXPECT(session.query("record", args, refusal).is_null() &&
		            refusal.find("holds no records") != std::string::npos);
		// Its JSON is the base's alone: the lifecycle, no rows.
		const JsonPage rows;
		const JsonValue json = document_to_json(*opened, &rows);
		TEST_EXPECT(json.get_string("path", "") == path && json.get_bool("dirty", false));
		TEST_EXPECT(json.get("revision") && json.get("issues"));
		TEST_EXPECT(!json.get("row_count") && !json.get("rows"));
		// Saved: the file holds the blob.
		session.handle(request::save(path));
		TEST_EXPECT(session.outcome().done() && !opened->dirty());
		TEST_EXPECT(file_text(file) == "BLOB FFFFFFFF\r\n");
		session.handle(request::close_document(path));
		TEST_EXPECT(!session.document_base_for(path) && session.handle_entries() > entries);
	}
	// The stand-in gone, the stylesheet's own type answers, its records read again.
	TEST_EXPECT(document_type_for(kind)->name == std::string("styles"));
	TEST_EXPECT(graph_reads_kind(kind) && !references_unread(kind));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_non_record_type();
	if (failures == 0) std::printf("editor_non_record_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
