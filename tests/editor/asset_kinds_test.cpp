// S13 D5 (ADR 0046 S13): the rule the maintainer set for the editor's roadmap, that every
// resource format the game reads at runtime will be editable, made checkable. That every runtime
// format and every required file has a kind and a place is the engine's table's test
// (tests/resource_index/file_kind_test: base/resource_index/file_kind.h, the facts a row reads);
// here, what the editor's own columns say and their static_asserts cannot: every document type is a
// row's and opens it, a kind a document type edits is one the build packs but the project's notes,
// the build leaves out exactly the kinds of no slot (S13 A8: a file of no kind the game knows among
// them, its name bound by no archive limit), an import source binds the archives' name limit (its
// outputs take its name), and a new file's name ends with one of its kind's extensions where the
// kind's facts list them.
#include <cstdio>
#include <set>
#include <string>

#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/document_types.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

// What the static_asserts cannot say: every document type is a row's (and opens it); an import
// source packs nowhere and binds the archives' name limit (its outputs take its name); Unknown packs
// nowhere, its name bound by nothing (S13 A8), as an archive's; a value past the last kind reads the
// Unknown row.
static int test_the_table() {
	std::set<DocumentTypeId> edited;
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKind kind = AssetKind(i);
		const AssetKindRow &row = asset_kind_row(kind);
		TEST_EXPECT(row.kind == kind && asset_kind_from_token(row.token) == kind);
		TEST_EXPECT(&row.facts() == &file_kind_facts(kind));
		// A kind a document type edits is one the build packs, but the project's notes, which the text type
		// opens and the game never reads.
		if (row.document != DocumentTypeId::None) {
			edited.insert(row.document);
			const DocumentType *type = document_type(row.document);
			TEST_EXPECT(type && document_type_for(kind) == type && asset_kind_packed(kind) == (kind != AssetKind::Notes));
		} else {
			TEST_EXPECT(document_type_for(kind) == nullptr);
		}
		const bool left_out = kind == AssetKind::Unknown || kind == AssetKind::Archive || kind == AssetKind::ImportSource ||
		                      kind == AssetKind::ImportInput || kind == AssetKind::MissionText || kind == AssetKind::Notes;
		TEST_EXPECT(asset_kind_packed(kind) == !left_out);
	}
	TEST_EXPECT(edited.size() == kDocumentTypeCount);
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentType *type = document_type(DocumentTypeId(id));
		TEST_EXPECT(type && type->id == DocumentTypeId(id));
	}
	TEST_EXPECT(document_type(DocumentTypeId::None) == nullptr);
	// The project's notes: the text type opens them, no build packs them, and no archive's name limit binds them.
	TEST_EXPECT(document_type_for(AssetKind::Notes) == document_type_for(AssetKind::Text) &&
	            !asset_kind_packed(AssetKind::Notes) && !archive_name_limit_binds(AssetKind::Notes));
	TEST_EXPECT(archive_name_limit_binds(AssetKind::ImportSource));
	TEST_EXPECT(!asset_kind_packed(AssetKind::Unknown));
	TEST_EXPECT(!archive_name_limit_binds(AssetKind::Unknown) && !archive_name_limit_binds(AssetKind::Archive));
	TEST_EXPECT(&asset_kind_row(AssetKind::kCount) == &asset_kind_row(AssetKind::Unknown));
	return 0;
}

// A new file's name of a kind whose facts list its extensions ends with one of them (a kind the
// runtime's classifier types by its bytes has no list to hold it to).
static int test_new_names_fit() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKindRow &row = asset_kind_row(AssetKind(i));
		const char *const *extensions = row.facts().extensions;
		if (!*row.new_name || !extensions) continue;
		bool fits = false;
		for (const char *const *extension = extensions; *extension; ++extension)
			fits = fits || strutil::ends_with_icase(row.new_name, *extension);
		if (!fits) std::fprintf(stderr, "%s's new name %s ends with none of its extensions\n", row.token, row.new_name);
		TEST_EXPECT(fits);
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_the_table();
	failures += test_new_names_fit();
	if (failures == 0) std::printf("editor_asset_kinds: every kind's row and document type agree\n");
	return failures == 0 ? 0 : 1;
}
