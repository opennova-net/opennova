// The HUD effects document (ADR 0046 S23 B; editor/documents/hudfx_document.h): hudfx.def's model lines as rows
// over formats/def/def_hudfx with the file's layout (a tag, a model by its file); the game's reading of the file as
// findings: a line after the first never read (and its fields read for nothing), the HUD model as the one line read
// (the power slots' draw then reads a slot with no model: a gating error citing the game's failure), a power
// slot's model loaded and never drawn, a long name's run into the next slots, a line of no model; a model changed
// changes its one line, a line added goes after the last; the blank opens clean. JO ships no hudfx.def.
#include <editor/blank/blank_factory.h>
#include <editor/documents/hudfx_document.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kLine = node_kind(HudFxKind::Line);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

const Diagnostic *find_code(const std::vector<Diagnostic> &findings, const char *code) {
	for (const Diagnostic &d : findings)
		if (d.code() == code) return &d;
	return nullptr;
}

int test_document() {
	const std::string file = "// the HUD's models\r\n3dihud   hud.3di   // the frame\r\n\r\n3DIPower2 p2.3di\r\n";
	HudFxDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(file), "defs/hudfx.def", AssetKind::HudFxDefs, "jo", error));
	TEST_EXPECT(document.rows().size() == 2 && document.serialize().text == file);
	const NodeAddress first{document.rows()[0]->id, kLine, 0}, second{document.rows()[1]->id, kLine, 0};
	Value value;
	TEST_EXPECT(document.get(first, "tag", value) && value == Value(int64_t(0)));
	TEST_EXPECT(document.get(second, "model", value) && value == Value(std::string("p2.3di")));
	std::vector<Diagnostic> findings = validate_hudfx_file(document);
	const Diagnostic *empty = find_code(findings, "hudfx.power_slots_empty");
	TEST_EXPECT(empty && empty->severity == DiagnosticSeverity::Error && empty->row_id == first.row);
	TEST_EXPECT(finding_code(HudFxFinding::PowerSlotsEmpty).gates_build &&
	            finding_code(HudFxFinding::PowerSlotsEmpty).game_refusal != nullptr);
	const Diagnostic *never = find_code(findings, "hudfx.never_read");
	TEST_EXPECT(never && never->row_id == second.row && !find_code(findings, "hudfx.power_unseen"));
	// The model changed: its one line, its spacing and comment kept.
	Edit edit;
	edit.address = first;
	edit.field = "model";
	edit.value = std::string("frame2.3di");
	TEST_EXPECT(document.apply(edit, error));
	TEST_EXPECT(document.serialize().text == "// the HUD's models\r\n3dihud   frame2.3di   // the frame\r\n\r\n3DIPower2 p2.3di\r\n");
	// A power slot first: loaded, never drawn; a name of 16 characters changes nothing, a longer one runs on.
	edit.field = "tag";
	edit.value = int64_t(3);
	TEST_EXPECT(document.apply(edit, error));
	edit.field = "model";
	edit.value = std::string("sixteen_char.3di");
	TEST_EXPECT(document.apply(edit, error));
	TEST_EXPECT(!find_code(validate_hudfx_file(document), "hudfx.name_runs"));
	edit.value = std::string("averyveryverylongmodel.3di");
	TEST_EXPECT(document.apply(edit, error));
	findings = validate_hudfx_file(document);
	TEST_EXPECT(find_code(findings, "hudfx.power_unseen") && !find_code(findings, "hudfx.power_slots_empty"));
	const Diagnostic *runs = find_code(findings, "hudfx.name_runs");
	TEST_EXPECT(runs && runs->message.find("3DIPower4 'gmodel.3di'") != std::string::npos);
	// A line added goes after the last, in the writer's form.
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kLine;
	TEST_EXPECT(document.apply(add, error) && document.rows().size() == 3);
	TEST_EXPECT(document.serialize().text.find("3DIPower2 p2.3di\r\n3DIHud\r\n") != std::string::npos);
	std::printf("document: the lines, the one read, the game's failure, a model, a slot, a long name, a line added\n");
	return 0;
}

int test_blank() {
	const BlankFactory *factory = find_blank_factory_for_role("hudfx_def");
	TEST_EXPECT(factory != nullptr);
	BlankRequest request;
	request.logical_name = "hudfx.def";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(factory->make(request, bytes, error) && !bytes.empty());
	HudFxDocument document;
	TEST_EXPECT(document.load_bytes(bytes, "hudfx.def", AssetKind::HudFxDefs, "jo", error) && document.rows().empty());
	TEST_EXPECT(validate_hudfx_file(document).empty() && document.serialize().text == std::string(bytes.begin(), bytes.end()));
	std::printf("blank: no model line, opened clean\n");
	return 0;
}

} // namespace

int main() {
	if (test_document() || test_blank()) return 1;
	return 0;
}
