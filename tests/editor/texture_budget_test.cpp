// ADR 0046 S18, the texture budget (documents/texture_budget over runtime/renderer/device_texture): what a
// model row's texture costs the game at each object texture detail level and as its .dds, from the file's
// header; over a minted project the texture_uses query's budgets, the use check's texture.memory on a texture
// past 16 MB with its chain and texture.normal_slot_loader on a normal-map slot's row the diffuse loader reads,
// its fix giving an .mdt's row type 4. The retail leg (OPENNOVA_JO_DIR): every texture row of every model the
// install ships costed through the file its loader opens, the most a model texture holds, and no normal-map
// slot's row of a type the diffuse loader reads.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/install_view.h>
#include <editor/documents/texture_budget.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/texture_uses.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/material_texture.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
using opennova::renderer::DeviceTextureFormat;

namespace {

constexpr uint64_t kMB = 1024 * 1024;

std::vector<uint8_t> tga(uint32_t w, uint32_t h, bool alpha = true) {
	const std::vector<uint8_t> rgba(size_t(w) * h * 4, 200);
	std::vector<uint8_t> out;
	std::string error;
	if (alpha) opennova::tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	else opennova::tga::tga_write_rgb24(rgba.data(), w, h, out, error);
	return out;
}

// A DXT5 of `side` with its full chain, every block's alpha endpoints `alpha` (255 opaque).
std::vector<uint8_t> dxt5(uint32_t side, uint8_t alpha) {
	std::vector<std::vector<uint8_t>> levels;
	for (uint32_t s = side; s >= 1; s /= 2) {
		const size_t blocks = size_t(std::max(1u, s / 4)) * std::max(1u, s / 4);
		std::vector<uint8_t> level(blocks * 16, 0);
		for (size_t b = 0; b < blocks; ++b) level[b * 16] = level[b * 16 + 1] = alpha;
		levels.push_back(std::move(level));
		if (s == 1) break;
	}
	std::vector<uint8_t> out;
	std::string error;
	opennova::dds::dds_write_dxt(opennova::dds::dds_fourcc('D', 'X', 'T', '5'), side, side, levels, out, error);
	return out;
}

int test_budget() {
	// The base game's crate diffuse: a 2048 x 2048 32-bit TGA in slot 1.
	const TextureHeader crate = texture_header("oncrate1_0.tga", tga(2048, 2048));
	const TextureBudget budget = texture_budget(crate, "oncrate1_0.tga", TextureBudgetLoader::Stage, 1);
	TEST_EXPECT(budget.known && budget.full().format == DeviceTextureFormat::A8R8G8B8 && budget.full().width == 2048 &&
	            budget.full().levels == 10);
	TEST_EXPECT(budget.full().bytes > 21 * kMB && budget.full().bytes < 22 * kMB);
	// Halved twice at the lowest detail, once at 1, kept at 2 (slot 1).
	TEST_EXPECT(budget.detail[0].width == 512 && budget.detail[1].width == 1024 && budget.detail[2].width == 2048);
	// Its .dds: DXT5 (it holds an alpha), a quarter of the bytes.
	TEST_EXPECT(budget.offers_dds && budget.as_dds.format == DeviceTextureFormat::Dxt5 && budget.as_dds.levels == 12 &&
	            budget.as_dds.bytes > 5 * kMB && budget.as_dds.bytes < 6 * kMB);
	TEST_EXPECT(texture_budget_words(budget) ==
	            "21.3 MB in the game (2048 x 2048, A8R8G8B8 (uncompressed), 10 levels: 21.3 MB); 5.3 MB as a DXT5 .dds");
	// A 24-bit one's .dds is DXT1.
	const TextureBudget opaque =
			texture_budget(texture_header("wall.tga", tga(256, 256, false)), "wall.tga", TextureBudgetLoader::Stage, 1);
	TEST_EXPECT(opaque.as_dds.format == DeviceTextureFormat::Dxt1);
	// A slot-3 normal map read by the normal-map loader: capped at 512, the detail never halving it.
	const TextureHeader arm = texture_header("arm.mdt", tga(4096, 64));
	const TextureBudget normal = texture_budget(arm, "arm.mdt", TextureBudgetLoader::Normal, 3);
	TEST_EXPECT(normal.full().width == 512 && normal.full().height == 8 && normal.detail[0].width == 512 && !normal.offers_dds);
	// The same row read by the diffuse loader in slot 3: whole, its .dds offered.
	const TextureBudget as_diffuse = texture_budget(arm, "arm.mdt", TextureBudgetLoader::Stage, 3);
	TEST_EXPECT(as_diffuse.full().width == 4096 && as_diffuse.detail[0].width == 4096 && as_diffuse.offers_dds);
	// A DDS: an opaque DXT5 counted as the DXT1 the game stores it as; its levels skipped at the lowest detail.
	const TextureHeader solid = texture_header("solid.dds", dxt5(256, 255));
	TEST_EXPECT(solid.read && solid.dds_dxt5_opaque && solid.dds_levels == 9);
	const TextureBudget dds = texture_budget(solid, "solid.dds", TextureBudgetLoader::Stage, 1);
	TEST_EXPECT(dds.full().format == DeviceTextureFormat::Dxt1 && dds.full().dxt5_as_dxt1 && !dds.offers_dds);
	TEST_EXPECT(dds.detail[0].width == 64 && dds.detail[0].levels == 7);
	const TextureHeader clear = texture_header("clear.dds", dxt5(256, 0));
	TEST_EXPECT(clear.read && !clear.dds_dxt5_opaque);
	TEST_EXPECT(texture_budget(clear, "clear.dds", TextureBudgetLoader::Stage, 1).full().format == DeviceTextureFormat::Dxt5);
	// The words of bytes.
	TEST_EXPECT(texture_bytes_words(96) == "96 bytes" && texture_bytes_words(340 * 1024) == "340 KB" &&
	            texture_bytes_words(uint64_t(21.3 * kMB)) == "21.3 MB");
	// The roles costed: a model row's.
	TextureBudgetLoader loader = TextureBudgetLoader::Stage;
	TEST_EXPECT(texture_role_budget_loader(TextureRoleId::ModelHeightNormal, loader) && loader == TextureBudgetLoader::Normal);
	TEST_EXPECT(texture_role_budget_loader(TextureRoleId::ModelPlain, loader) && loader == TextureBudgetLoader::Plain);
	TEST_EXPECT(!texture_role_budget_loader(TextureRoleId::TerrainColourMap, loader));
	std::printf("budget: the device texture of each loader, its .dds, an opaque DXT5 as DXT1\n");
	return 0;
}

const Diagnostic *finding(const SessionView &view, const std::string &code, const std::string &mentions) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == code && d.message.find(mentions) != std::string::npos) return &d;
	return nullptr;
}

int test_project() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_budget"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Budget"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::string t = root + "/textures/";
	// A 2048 crate diffuse past the warning; a small one under it; two arms' normal maps in slot 3 with type 0,
	// a .tga and an .mdt; a crate normal map of type 4; an opaque DXT5 beside a .tga its row names.
	TEST_EXPECT(editor_test::write_bytes(t + "crate.tga", tga(2048, 2048)) && editor_test::write_bytes(t + "small.tga", tga(64, 64)) &&
	            editor_test::write_bytes(t + "arm_n.tga", tga(64, 64)) && editor_test::write_bytes(t + "leg_n.mdt", tga(1024, 1024)) &&
	            editor_test::write_bytes(t + "crate_n.mdt", tga(512, 512)) && editor_test::write_bytes(t + "skin.tga", tga(8, 8)) &&
	            editor_test::write_bytes(t + "skin.dds", dxt5(256, 255)));
	const std::string scene = dir.file("scene");
	TEST_EXPECT(editor_test::write_text(scene + "/crate.o3d",
	                                    "o3d 2\nmodel CRATE\nmaterial VS_PHONGT\ntexture crate.tga 1 0\ntexture crate_n.mdt 3 4\n"
	                                    "material FF_ST_OP\ntexture small.tga 1 0\n"
	                                    "material VS_SKBUMPDIFFT\ntexture skin.tga 1 0\ntexture arm_n.tga 3 0\n"
	                                    "material VS_SKBUMPDIFFT\ntexture small.tga 1 0\ntexture leg_n.mdt 3 0\n"
	                                    "lod 0\npart 0 0 0 0\nmesh 0 0\n"
	                                    "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"));
	ImportChoice model;
	model.path = scene + "/crate.o3d";
	const ImportResult imported = import_assets({model}, ProjectPaths::for_root(root), *view.project.document, false);
	TEST_EXPECT(imported.imported == std::vector<std::string>({"models/crate.3di"}));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	// The memory: the crate's 2048 diffuse alone, once, on its row, with its .dds's cost and the lowest detail's.
	size_t memory = 0;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "texture.memory") {
			++memory;
			TEST_EXPECT(d.asset == "models/crate.3di" && d.severity == DiagnosticSeverity::Warning && !blocks_build(d));
			TEST_EXPECT(d.message.find("crate.tga") != std::string::npos && d.message.find("takes 21.3 MB") != std::string::npos &&
			            d.message.find("As a DXT5 .dds beside it") != std::string::npos &&
			            d.message.find("it would take 5.3 MB") != std::string::npos &&
			            d.message.find("halves it to 512 x 512 (1.3 MB)") != std::string::npos);
		}
	TEST_EXPECT(memory == 1);
	// The normal-map slot read as a diffuse: the .tga's words say how to store a finished one; the .mdt's fix
	// gives its row type 4.
	const Diagnostic *tga_row = finding(view, "texture.normal_slot_loader", "arm_n.tga");
	const Diagnostic *mdt_row = finding(view, "texture.normal_slot_loader", "leg_n.mdt");
	TEST_EXPECT(tga_row && mdt_row && !finding(view, "texture.normal_slot_loader", "crate_n.mdt"));
	if (!tga_row || !mdt_row) return 1;
	TEST_EXPECT(tga_row->message.find("sits in normal-map slot 3 with type 0") != std::string::npos &&
	            tga_row->message.find("Store a finished normal map as an .mdt with type 4") != std::string::npos);
	TEST_EXPECT(mdt_row->message.find("1024 x 1024, A8R8G8B8 (uncompressed), 9 levels") != std::string::npos &&
	            mdt_row->message.find("Give the row type 4 (a finished normal map), and it takes 1.3 MB") != std::string::npos);
	TEST_EXPECT(fixes_for(*tga_row, view).empty());
	const std::vector<ProblemFix> fixes = fixes_for(*mdt_row, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Give its row type 4 (normal map)" &&
	            fixes[0].request.kind == EditorRequestKind::EditRecord && fixes[0].request.open_first);
	if (fixes.empty()) return 1;
	editor_test::handle_to_end(session, fixes[0].request);
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	TEST_EXPECT(!finding(view, "texture.normal_slot_loader", "leg_n.mdt") && finding(view, "texture.normal_slot_loader", "arm_n.tga"));
	// The wire: each use's budget, by the file its loader opens (skin.tga's row loads skin.dds, an opaque DXT5
	// stored as DXT1).
	JsonValue args;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse("{\"path\":\"crate.tga\"}", args, error));
	JsonValue answer = session.query("texture_uses", args, error);
	const JsonValue *uses = answer.get("uses");
	TEST_EXPECT(error.empty() && uses && uses->array.size() == 1);
	if (uses && uses->array.size() == 1) {
		const JsonValue *budget = uses->array[0].get("budget");
		TEST_EXPECT(budget && budget->get_string("loader", "") == "stage" && budget->get_number("slot", 0) == 1 &&
		            budget->get("detail") && budget->get("detail")->array.size() == 4 && budget->get("as_dds") &&
		            budget->get("as_dds")->get_string("format", "") == "DXT5");
		if (budget && budget->get("detail") && budget->get("detail")->array.size() == 4) {
			const JsonValue &full = budget->get("detail")->array[3];
			TEST_EXPECT(full.get_number("width", 0) == 2048 && full.get_string("format", "") == "A8R8G8B8" &&
			            full.get_number("levels", 0) == 10);
		}
	}
	TEST_EXPECT(opennova::io::json_parse("{\"path\":\"skin.dds\"}", args, error));
	answer = session.query("texture_uses", args, error);
	uses = answer.get("uses");
	TEST_EXPECT(error.empty() && uses && uses->array.size() == 1);
	if (uses && uses->array.size() == 1) {
		const JsonValue *budget = uses->array[0].get("budget");
		TEST_EXPECT(budget && budget->get("as_dds") && budget->get("as_dds")->is_null() && budget->get("detail") &&
		            budget->get("detail")->array.size() == 4 &&
		            budget->get("detail")->array[3].get_string("format", "") == "DXT1" &&
		            budget->get("detail")->array[3].get_bool("dxt5_as_dxt1", false));
	}
	// A use the budget does not cost (a .tga beside the .dds its row loads reads no budget of its own).
	TEST_EXPECT(opennova::io::json_parse("{\"path\":\"skin.tga\"}", args, error));
	answer = session.query("texture_uses", args, error);
	uses = answer.get("uses");
	TEST_EXPECT(error.empty() && uses && uses->array.size() == 1 && !uses->array[0].get_bool("reads_file", true));
	std::printf("project: the memory past 16 MB said once; a normal slot read as a diffuse, its .mdt's fix; the budgets on the wire\n");
	return 0;
}

// Every texture row of every model the install ships, through the file its loader opens: the most a model
// texture holds, the rows past the warning, and no normal-map slot's row of a type the diffuse loader reads.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every shipped model's textures costed)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView install_view;
	std::string why;
	TEST_EXPECT(install_view.open(install_spec(install, project), why));
	const opennova::Vfs &mount = install_view.vfs();
	const auto exists = [&mount](const std::string &name) { return mount.has_file(name); };
	size_t models = 0, rows = 0, costed = 0, past = 0, normal_slot = 0;
	uint64_t largest = 0;
	std::string largest_row;
	std::map<std::string, size_t> by_format;
	std::map<std::string, std::pair<TextureHeader, bool>> headers;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (classify_asset(name, nullptr) != AssetKind::Model) continue;
		std::vector<uint8_t> bytes;
		opennova::threedi::Threedi3di3 model{};
		if (!mount.read_file(name, bytes) ||
		    opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) != 0)
			continue;
		++models;
		for (uint32_t m = 0; m < model.material_count; ++m) {
			const opennova::threedi::ThreediMaterial &material = model.materials[m];
			for (uint32_t r = 0; r < material.texture_count && r < 24; ++r) {
				const opennova::threedi::ThreediMaterialTexture &row = material.textures[r];
				if (row.name[0] == 0) continue;
				++rows;
				TextureRoleId role = TextureRoleId::kCount;
				const uint8_t runtime = opennova::renderer::material_texture_runtime_type(row.type);
				if (runtime == 4 || runtime == 5) role = TextureRoleId::ModelNormalMap;
				else if (runtime == 1) role = TextureRoleId::ModelPlain;
				else if (runtime == 0 || runtime == 2 || runtime == 8) role = TextureRoleId::ModelDiffuse;
				TextureBudgetLoader loader = TextureBudgetLoader::Stage;
				if (!texture_role_budget_loader(role, loader)) continue;
				if ((row.slot == 3 || row.slot == 4) && loader != TextureBudgetLoader::Normal) {
					++normal_slot;
					std::fprintf(stderr, "retail: %s row %s slot %u type %u\n", name.c_str(), row.name, unsigned(row.slot),
					             unsigned(row.type));
				}
				const opennova::renderer::MaterialTextureSource source =
						opennova::renderer::material_texture_source(row.name, row.type, exists);
				if (source.file.empty()) continue;
				auto &[header, read] = headers[source.file];
				if (!read) {
					read = true;
					std::vector<uint8_t> file;
					if (mount.read_file(source.file, file)) header = texture_header(source.file, file);
				}
				const TextureBudget budget = texture_budget(header, source.file, loader, row.slot);
				if (!budget.known) continue;
				++costed;
				const uint64_t bytes_held = budget.full().bytes;
				++by_format[opennova::renderer::device_texture_format_name(budget.full().format)];
				if (bytes_held > largest) {
					largest = bytes_held;
					largest_row = name + ": " + source.file + " (" + device_texture_words(budget.full()) + ")";
				}
				past += bytes_held > kTextureMemoryWarnBytes ? 1 : 0;
			}
		}
		opennova::threedi::threedi_3di3_free(&model);
	}
	for (const auto &[format, count] : by_format) std::printf("retail: %-12s %zu rows\n", format.c_str(), count);
	std::printf("retail: %zu models, %zu texture rows, %zu costed; the largest %s; %zu past %s; %zu normal-map slots read "
	            "as a diffuse\n",
	            models, rows, costed, largest_row.c_str(), past,
	            texture_bytes_words(kTextureMemoryWarnBytes).c_str(), normal_slot);
	// JO:CA: 649 models, 3,105 of 3,111 rows costed (six name a file the install lacks), 1,842 of them as the DXT1 an
	// opaque DXT5 loads as, 872 as DXT5, 391 built from pixels; the largest 1.3 MB.
	TEST_EXPECT(models > 500 && costed > 3000 && largest <= 2 * 1024 * 1024 && past == 0 && normal_slot == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_budget();
	failures += test_project();
	failures += test_retail();
	if (failures == 0) std::printf("editor_texture_budget: all passed\n");
	return failures == 0 ? 0 : 1;
}
