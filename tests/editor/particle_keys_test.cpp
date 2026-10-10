// The particle key panel (ADR 0046 S23 B, documents/particle_keys and the particle_keys query): a particle file's
// blocks as the game's reader reads them, each key it writes at its span with what the reader takes it as, the keys
// of its kind it lacks; a set rewriting that one value's span (the rest of the text as it stood), a key the block
// lacks put on its own line before the block's closing brace in its indent and line end; a value the reader would
// take otherwise refused with why; the query over a session; the retail leg (OPENNOVA_JO_DIR): every particle file
// of the install read so, each key's text at its span its value.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/install_view.h>
#include <editor/documents/document_types.h>
#include <editor/documents/particle_keys.h>
#include <editor/model/text_document.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
using opennova::particle::BlockKind;

namespace {

const char *kText = "[particledef]\r\n{\r\n\tid = spark;\r\n\tgraphic1 = spark.tga, additive;\r\n\temit_burst = 2;\r\n"
                    "\tcolor1 = 255, 128, 0;\r\n}\r\n"
                    "[effectdef]\r\n{\r\n\tid = Hit;\r\n\tpdefs = spark;\r\n}\r\n";

std::unique_ptr<DocumentBase> loaded(const std::string &text) {
	std::unique_ptr<DocumentBase> document = document_type_for(AssetKind::Particles)->make();
	Diagnostic error;
	document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "particles/t.ptl", AssetKind::Particles, "jo",
	                     error);
	return document;
}

const ParticleKeyField *field_of(const ParticleKeyBlock &block, const char *key) {
	for (const ParticleKeyField &field : block.fields)
		if (opennova::strutil::iequals(field.key, key)) return &field;
	return nullptr;
}

int test_blocks_and_sets() {
	std::unique_ptr<DocumentBase> document = loaded(kText);
	const TextDocument &text = *text_of(*document);
	std::vector<ParticleKeyBlock> blocks = particle_key_blocks(text);
	TEST_EXPECT(blocks.size() == 2);
	if (blocks.size() != 2) return 1;
	const ParticleKeyBlock &spark = blocks[0];
	TEST_EXPECT(spark.kind == BlockKind::Particle && spark.id == "spark" && particle_block_title(spark) == "[particledef] spark" &&
	            spark.first_line == 1 && spark.last_line == 7);
	const ParticleKeyField *burst = field_of(spark, "emit_burst");
	TEST_EXPECT(burst && burst->present && burst->value == "2" && burst->span.line == 5 && burst->span.column == 15 &&
	            burst->row && burst->row->clamped && burst->row->min == 1);
	// The keys of its kind it lacks follow, the graphic layers' and the slots' left to be named.
	const ParticleKeyField *speed = field_of(spark, "speed");
	TEST_EXPECT(speed && !speed->present && speed->value.empty() && !field_of(spark, "g#_alpha"));
	TEST_EXPECT(blocks[1].kind == BlockKind::Effect && blocks[1].id == "Hit" && field_of(blocks[1], "pdefs")->present);

	// A value set: its span replaced, the rest as it stood.
	Edit edit;
	std::string why;
	TEST_EXPECT(particle_key_edit(text, spark, "emit_burst", "4", edit, why));
	Diagnostic error;
	TEST_EXPECT(document->apply(edit, error));
	std::string expected = kText;
	expected.replace(expected.find("emit_burst = 2"), 14, "emit_burst = 4");
	TEST_EXPECT(text.text() == expected);
	// A key the block lacks: its line before the closing brace, in the block's indent and line end.
	blocks = particle_key_blocks(text);
	TEST_EXPECT(particle_key_edit(text, blocks[0], "speed", "12.5", edit, why) && document->apply(edit, error));
	expected.replace(expected.find("}\r\n[effectdef]"), 0, "\tspeed = 12.5;\r\n");
	TEST_EXPECT(text.text() == expected);
	blocks = particle_key_blocks(text);
	const ParticleKeyField *added = field_of(blocks[0], "speed");
	TEST_EXPECT(added && added->present && added->value == "12.5");
	// A graphic layer's key named, not listed.
	TEST_EXPECT(particle_key_edit(text, blocks[0], "g1_alpha", "0.5", edit, why) && document->apply(edit, error));
	TEST_EXPECT(text.text().find("\tg1_alpha = 0.5;\r\n}\r\n[effectdef]") != std::string::npos);

	// What the reader would take otherwise, refused with why.
	blocks = particle_key_blocks(text);
	const ParticleKeyBlock &again = blocks[0];
	TEST_EXPECT(!particle_key_edit(text, again, "emit_burst", "0", edit, why) && why.find("as 1") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "color1", "1, 2", edit, why) && why.find("3 values") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "color1", "1, 2, 300", edit, why) && why.find("it reads 44") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "flags", "NEVERAGE BOGUS", edit, why) && why.find("BOGUS") != std::string::npos);
	TEST_EXPECT(particle_key_edit(text, again, "flags", "NEVERAGE HAZE", edit, why));
	TEST_EXPECT(!particle_key_edit(text, again, "graphic1", "spark.tga, glow", edit, why) && why.find("as blend") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "speed", "fast", edit, why) && why.find("atof reads it as 0") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "speed", "1.5x", edit, why) && why.find("atof reads it as 1.5") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "emit_burst", "12x", edit, why) && why.find("atol reads it as 12") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "id", "a; b", edit, why) && why.find("';'") != std::string::npos);
	TEST_EXPECT(!particle_key_edit(text, again, "g5_alpha", "1", edit, why) && why.find("no key") != std::string::npos);
	TEST_EXPECT(particle_key_words(*again.fields.front().row).size() > 0);
	// A text the reader stops in has no block.
	TEST_EXPECT(particle_key_blocks(*text_of(*loaded("[particledef]\r\n{\r\n\tid = a;\r\n"))).empty());
	return 0;
}

// The game's reader is line ordered [orig: CParticleDefEntry_ParseGraphicProperty @ 0x5E3550]: a graphic line copies
// the particle's colours, alpha, scale and curves into its layer as it is read, and a layer's key goes to the layer of
// the graphic line before it. In a block laid out as the shipped ones are (its graphic lines last), an added key a
// graphic line copies goes before the first graphic line, a layer's key after its own graphic line, a graphic line at
// the end where it opens the layer its number names; a layer key of no graphic line, and a graphic1 after others, are
// refused. Each line of a repeated key is its own row, and an edit of the first rewrites the first.
int test_line_order() {
	const std::string shipped = "[particledef]\r\n{\r\n\tid = a;\r\n\temit_rate = 5;\r\n\tgraphic1 = a.tga, additive;\r\n"
	                            "\tg1_flip_frames = 2;\r\n\tgraphic2 = b.tga, additive;\r\n}\r\n";
	const auto added = [&](const char *key, const char *value, std::string &why) {
		std::unique_ptr<DocumentBase> document = loaded(shipped);
		const TextDocument &text = *text_of(*document);
		const std::vector<ParticleKeyBlock> blocks = particle_key_blocks(text);
		Edit edit;
		Diagnostic error;
		if (blocks.empty() || !particle_key_edit(text, blocks[0], key, value, edit, why) || !document->apply(edit, error))
			return std::string();
		return text.text();
	};
	std::string why;
	TEST_EXPECT(added("alpha_func", "fade", why).find("\talpha_func = fade;\r\n\tgraphic1 =") != std::string::npos);
	TEST_EXPECT(added("g1_alpha", "0.5", why).find("\tg1_flip_frames = 2;\r\n\tg1_alpha = 0.5;\r\n\tgraphic2 =") !=
	            std::string::npos);
	TEST_EXPECT(added("g2_alpha", "0.5", why).find("\tgraphic2 = b.tga, additive;\r\n\tg2_alpha = 0.5;\r\n}") !=
	            std::string::npos);
	TEST_EXPECT(added("speed", "3", why).find("\tgraphic2 = b.tga, additive;\r\n\tspeed = 3;\r\n}") != std::string::npos);
	TEST_EXPECT(added("graphic3", "c.tga, additive", why).find("\tgraphic3 = c.tga, additive;\r\n}") != std::string::npos);
	TEST_EXPECT(added("g3_alpha", "0.5", why).empty() && why.find("no graphic3 line") != std::string::npos);
	// graphic1 after other graphic lines (a block whose first is graphic2): the game would read it into the first
	// layer again.
	{
		std::unique_ptr<DocumentBase> document = loaded("[particledef]\r\n{\r\n\tid = a;\r\n\tgraphic2 = b.tga, additive;\r\n}\r\n");
		const TextDocument &text = *text_of(*document);
		const std::vector<ParticleKeyBlock> blocks = particle_key_blocks(text);
		Edit edit;
		TEST_EXPECT(!blocks.empty() && !particle_key_edit(text, blocks[0], "graphic1", "c.tga, additive", edit, why) &&
		            why.find("first layer again") != std::string::npos);
	}
	// A repeated key: each line its own row, the first's edit its own line.
	std::unique_ptr<DocumentBase> document = loaded("[particledef]\r\n{\r\n\tid = a;\r\n\temit_rate = 5;\r\n\temit_rate = 6;\r\n}\r\n");
	const TextDocument &text = *text_of(*document);
	const std::vector<ParticleKeyBlock> blocks = particle_key_blocks(text);
	TEST_EXPECT(blocks.size() == 1);
	if (blocks.empty()) return 1;
	const ParticleKeyField *first = nullptr;
	for (const ParticleKeyField &field : blocks[0].fields)
		if (field.present && field.key == "emit_rate" && !first) first = &field;
	TEST_EXPECT(first && first->value == "5");
	Edit edit;
	Diagnostic error;
	TEST_EXPECT(first && particle_key_edit(text, blocks[0], "emit_rate", "7", edit, why, first) && document->apply(edit, error));
	TEST_EXPECT(text.text().find("emit_rate = 7;\r\n\temit_rate = 6;") != std::string::npos);
	// A text of comments alone reads, of no block.
	bool read = false;
	TEST_EXPECT(particle_key_blocks(*text_of(*loaded("// nothing here\r\n")), &read).empty() && read);
	return 0;
}

int test_query() {
	editor_test::TempProjectDir dir("opennova_particle_keys");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Particles"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/t.ptl", kText));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("t.ptl"));
	JsonValue args;
	std::string error;
	opennova::io::json_parse("{\"path\": \"t.ptl\"}", args, error);
	const JsonValue answer = session.query("particle_keys", args, error);
	TEST_EXPECT(error.empty());
	const JsonValue *blocks = answer.get("blocks");
	TEST_EXPECT(blocks && blocks->is_array() && blocks->array.size() == 2);
	if (!blocks || blocks->array.size() != 2) return 1;
	const JsonValue &spark = blocks->array[0];
	TEST_EXPECT(spark.get_string("title", "") == "[particledef] spark" && spark.get_string("kind", "") == "particle");
	const JsonValue *keys = spark.get("keys");
	bool burst = false;
	for (const JsonValue &key : keys ? keys->array : std::vector<JsonValue>())
		if (key.get_string("key", "") == "emit_burst")
			burst = key.get_bool("present", false) && key.get_string("value", "") == "2" && key.get_number("line", 0) == 5 &&
			        key.get_string("words", "").find("at least 1") != std::string::npos;
	TEST_EXPECT(burst);
	// Another kind of document is refused.
	opennova::io::json_parse("{\"path\": \"nothing.ptl\"}", args, error);
	error.clear();
	session.query("particle_keys", args, error);
	TEST_EXPECT(!error.empty());
	return 0;
}

// The install's particle files: each read by the game's reader into blocks, each key's text at its span its value.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's particle files)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView view;
	std::string view_error;
	TEST_EXPECT(view.open(install_spec(install, project), view_error));
	size_t files = 0, blocks = 0, keys = 0;
	for (const opennova::VfsFileLocation &location : view.vfs().list_files()) {
		if (classify_asset(location.logical_name, nullptr) != AssetKind::Particles) continue;
		std::vector<uint8_t> bytes;
		if (!view.vfs().read_file_raw(location.logical_name, bytes)) continue;
		std::unique_ptr<DocumentBase> document = document_type_for(AssetKind::Particles)->make();
		Diagnostic error;
		if (!document->load_bytes(bytes, location.logical_name, AssetKind::Particles, "jo", error)) continue;
		const TextDocument &text = *text_of(*document);
		const std::vector<ParticleKeyBlock> read = particle_key_blocks(text);
		++files;
		blocks += read.size();
		for (const ParticleKeyBlock &block : read)
			for (const ParticleKeyField &field : block.fields) {
				if (!field.present) continue;
				++keys;
				std::string spelled;
				TEST_EXPECT(text.span_text(field.span, spelled) && spelled == field.value);
			}
	}
	std::printf("retail: %zu particle files, %zu blocks, %zu keys at their spans\n", files, blocks, keys);
	// The install's counts, pinned (Joint Operations: Combined Arms, its base's particle files of every extension).
	TEST_EXPECT(files == 85 && blocks == 2895 && keys == 123369);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = test_blocks_and_sets();
	failures += test_line_order();
	failures += test_query();
	failures += test_retail();
	if (failures == 0) std::printf("editor_particle_keys: all passed\n");
	return failures == 0 ? 0 : 1;
}
