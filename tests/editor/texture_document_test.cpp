// ADR 0046 S18, the texture viewer: what the editor reads of a texture file (documents/texture_image,
// over the formats' decoders), the texture document, and the texture viewport through a session. Over
// files minted here by our writers: a TGA read as the game's reader reads it (its facts in words, its
// alpha graded, on or off, or none; a top-left one upside down), a PCX's palette and indices, a DXT5
// chain decoded level by level exactly as the ported D3DX codec decodes its blocks, an A8R8G8B8 DDS, a
// DDS cut short (the game cannot load it, and says why), a PNG under a .dds name (D3DX reads it as a
// PNG) and a name no reader takes. The document: it holds an image, serializes the bytes it read,
// refuses an edit (document.payload), says nothing changed since its load, snapshots. Through a session:
// no texture file is a "holds no records" finding; Files' selection of a texture (select_file) shows it
// in the Preview window before it is opened, read from its file, its device given the picture; the
// viewport query's state and hit over it, a SetViewport of its camera and options, a refused one; the
// texture opened (the Preview stepping aside for the tab that shows it), the document query's
// `texture`, the graph's use of it by an item; another file selected and the project closed letting go.
// The retail leg (OPENNOVA_JO_DIR): a sample of the install's textures of each extension, each read by
// its reader and loaded as a document.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <variant>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/texture_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_dxt.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/png_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

namespace dds = opennova::dds;
namespace tga = opennova::tga;
namespace renderer = opennova::renderer;

// A w x h image: texel i is R = i, G = 255 - i, B = 3i, A = alpha(i).
std::vector<uint8_t> pattern(uint32_t w, uint32_t h, uint8_t (*alpha)(size_t)) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i < size_t(w) * h; ++i) {
		out.push_back(uint8_t(i));
		out.push_back(uint8_t(255 - i));
		out.push_back(uint8_t(3 * i));
		out.push_back(alpha(i));
	}
	return out;
}
uint8_t graded(size_t i) { return uint8_t(i * 16); }
uint8_t cutout(size_t i) { return i % 3 ? 255 : 0; }
uint8_t opaque(size_t) { return 255; }

std::vector<uint8_t> tga_of(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
	std::vector<uint8_t> out;
	std::string error;
	tga::tga_write_rgba32(rgba.data(), w, h, out, error);
	return out;
}

std::vector<uint8_t> pcx_of(int w, int h) {
	opennova::IndexedImage8 image;
	image.width = w;
	image.height = h;
	for (int i = 0; i < w * h; ++i) image.indices.push_back(uint8_t(i * 7));
	for (int i = 0; i < 256; ++i) {
		image.palette[i][0] = uint8_t(i);
		image.palette[i][1] = uint8_t(i / 2);
		image.palette[i][2] = uint8_t(255 - i);
	}
	std::vector<uint8_t> out;
	std::string error;
	opennova::encode_pcx_indexed(image, out, error);
	return out;
}

// A DXT5 chain of an 8 x 8 image through the ported D3DX encoder (8, 4, 2, 1), and the levels' blocks.
std::vector<renderer::DxtSurface> dxt5_levels(const std::vector<uint8_t> &rgba) {
	return renderer::build_dxt_texture_levels(rgba.data(), 8, 8, renderer::TextureDxtFormat::Dxt5, 4);
}
std::vector<uint8_t> dds_of(const std::vector<renderer::DxtSurface> &levels) {
	std::vector<std::vector<uint8_t>> blocks;
	for (const renderer::DxtSurface &level : levels) blocks.push_back(level.blocks);
	std::vector<uint8_t> out;
	std::string error;
	dds::dds_write_dxt(dds::dds_fourcc('D', 'X', 'T', '5'), levels.front().width, levels.front().height, blocks, out, error);
	return out;
}

std::string fact(const TextureImage &image, const char *key) {
	const TextureFact *found = image.fact(key);
	return found ? found->words : std::string("<none>");
}

int test_decode() {
	// A TGA of graded alpha: as the game reads it, its facts in words.
	const std::vector<uint8_t> rgba = pattern(4, 4, graded);
	std::shared_ptr<const TextureImage> image = decode_texture("brick.tga", tga_of(rgba, 4, 4));
	TEST_EXPECT(image->reader == TextureReader::Tga && image->loads && image->decoded && !image->blank);
	TEST_EXPECT(image->levels.size() == 1 && image->width() == 4 && image->height() == 4 && image->levels[0].rgba == rgba);
	TEST_EXPECT(image->alpha == TextureAlpha::Graded && !image->upside_down && image->palette.empty());
	TEST_EXPECT(fact(*image, "format") == "TGA image" && fact(*image, "size") == "4 x 4 (sides are powers of two)");
	TEST_EXPECT(fact(*image, "texels") == "32 bits: colour and 8 bits of alpha" && fact(*image, "compression") == "none");
	TEST_EXPECT(fact(*image, "alpha") == "graded: 16 values from 0 to 240" && fact(*image, "loads") == "loads it");
	TEST_EXPECT(image->game_levels == renderer::pixel_texture_mip_levels(4, 4));
	TEST_EXPECT(fact(*image, "rows") == "bottom row first, as the game reads it");
	uint8_t texel[4] = {};
	TEST_EXPECT(texture_texel(*image, 0, 1, 2, texel) && texel[0] == 9 && texel[3] == 144 && !texture_texel(*image, 0, 4, 0, texel));
	// An .mdt is a TGA; on or off alpha; none.
	image = decode_texture("normal.MDT", tga_of(pattern(4, 2, cutout), 4, 2));
	TEST_EXPECT(image->reader == TextureReader::Tga && image->alpha == TextureAlpha::Mask &&
	            fact(*image, "alpha") == "on or off: each texel 0 or 255 (a cut-out)");
	image = decode_texture("plain.tga", tga_of(pattern(3, 2, opaque), 3, 2));
	TEST_EXPECT(image->alpha == TextureAlpha::None && fact(*image, "size") == "3 x 2 (a side is no power of two)");
	// A top-left TGA: the game reads it bottom up all the same, so it shows upside down.
	std::vector<uint8_t> top = tga_of(rgba, 4, 4);
	top[17] |= 0x20;
	image = decode_texture("top.tga", top);
	TEST_EXPECT(image->upside_down && image->levels[0].rgba == decode_texture("x.tga", tga_of(rgba, 4, 4))->levels[0].rgba);
	TEST_EXPECT(fact(*image, "rows").find("upside down") != std::string::npos);

	// A PCX: its palette and indices, opaque.
	image = decode_texture("sky.pcx", pcx_of(8, 4));
	TEST_EXPECT(image->reader == TextureReader::Pcx && image->loads && image->decoded && image->palette_size() == 256 &&
	            image->indices.size() == 32 && image->indices[5] == 35 && image->alpha == TextureAlpha::None);
	TEST_EXPECT(texture_texel(*image, 0, 5, 0, texel) && texel[0] == 35 && texel[1] == 17 && texel[2] == 220 && texel[3] == 255);
	TEST_EXPECT(fact(*image, "texels") == "8-bit indices into a 256-colour palette" &&
	            fact(*image, "palette") == "256 colours, every one opaque");
	// Not 8 bits a plane: the game's reader refuses it.
	std::vector<uint8_t> four = pcx_of(8, 4);
	four[3] = 4;
	image = decode_texture("four.pcx", four);
	TEST_EXPECT(!image->loads && image->refusal.find("8 bits") != std::string::npos);

	// A DXT5 chain: every level decoded as the ported D3DX codec decodes its blocks.
	const std::vector<uint8_t> source = pattern(8, 8, graded);
	const std::vector<renderer::DxtSurface> levels = dxt5_levels(source);
	TEST_EXPECT(levels.size() == 4);
	image = decode_texture("metal.dds", dds_of(levels));
	TEST_EXPECT(image->reader == TextureReader::Dds && image->loads && image->decoded && image->levels.size() == 4 &&
	            image->game_levels == 4);
	for (size_t i = 0; i < levels.size(); ++i)
		TEST_EXPECT(image->levels[i].width == levels[i].width &&
		            image->levels[i].rgba == renderer::encode_rgba8(renderer::decode_dxt_surface(levels[i])));
	TEST_EXPECT(fact(*image, "texels") == "DXT5: 8 bits a texel, in 4 x 4 blocks" &&
	            fact(*image, "mips") == "4 in the file, down to 1 x 1" &&
	            fact(*image, "compression") == "DXT5 block compression (16 bytes a block)");
	// Cut short: the game cannot load it.
	std::vector<uint8_t> cut = dds_of(levels);
	cut.resize(cut.size() - 4);
	image = decode_texture("cut.dds", cut);
	TEST_EXPECT(!image->loads && image->levels.empty() && fact(*image, "loads").rfind("cannot load it: ", 0) == 0);
	// A8R8G8B8.
	std::vector<uint8_t> argb;
	std::string error;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(rgba.data(), 4, 4, argb, error));
	image = decode_texture("cube.dds", argb);
	TEST_EXPECT(image->loads && image->decoded && image->levels[0].rgba == rgba && image->alpha == TextureAlpha::Graded);
	// A PNG under a .dds name: D3DX reads it as a PNG.
	image = decode_texture("odd.dds", editor_test::gradient_png(5, 3));
	TEST_EXPECT(image->loads && image->decoded && image->width() == 5 && fact(*image, "reader").find("D3DX") == 0 &&
	            fact(*image, "format").find("PNG") == 0);
	// A name no reader takes.
	image = decode_texture("art.bmp", argb);
	TEST_EXPECT(image->reader == TextureReader::None && !image->loads);
	std::printf("decode: TGA, MDT, PCX, DXT5 and A8R8G8B8 DDS, a PNG under a .dds name, as the game's readers read them\n");
	return 0;
}

int test_document() {
	const std::vector<uint8_t> bytes = tga_of(pattern(4, 4, graded), 4, 4);
	const DocumentType *type = document_type_for(AssetKind::Texture);
	TEST_EXPECT(type && type->id == DocumentTypeId::Texture && document_content(*type) == DocumentContent::Image);
	std::unique_ptr<DocumentBase> made = type->make();
	TEST_EXPECT(made->holds_image() && !records_of(*made) && !text_of(*made));
	Diagnostic error;
	TEST_EXPECT(made->load_bytes(bytes, "textures/brick.tga", AssetKind::Texture, "jo", error) && !made->blocked());
	const auto *texture = dynamic_cast<const TextureDocument *>(made.get());
	TEST_EXPECT(texture && texture->image() && texture->image()->width() == 4);
	TEST_EXPECT(made->serialize().text == std::string(bytes.begin(), bytes.end()) &&
	            made->rewrite_need() == DocumentBase::RewriteNeed::None && !made->dirty());
	Edit set;
	set.field = "name";
	Diagnostic refused;
	TEST_EXPECT(!made->apply(set, refused) && refused.code() == "document.payload" && made->revision() == 0);
	ChangeSet since;
	TEST_EXPECT(made->changes_since(made->load_generation(), 0, since) && std::get_if<RasterChanges>(&since));
	TEST_EXPECT(!made->changes_since(made->load_generation() + 1, 0, since));
	const std::unique_ptr<DocumentBase> snapshot = made->snapshot();
	TEST_EXPECT(snapshot->is_snapshot() && snapshot->identity() == made->identity() &&
	            snapshot->serialize().text == made->serialize().text &&
	            dynamic_cast<const TextureDocument *>(snapshot.get())->image() == texture->image());
	TEST_EXPECT(type->validate_file(*made).empty() && type->findings().count == 0);
	const JsonValue content = type->content_json(*made);
	TEST_EXPECT(content.get_string("reader", "") == "tga" && content.get_number("width", 0) == 4 && content.get_bool("loads", false));
	// A file that does not decode still opens: the image says why the game cannot load it.
	std::unique_ptr<DocumentBase> broken = type->make();
	TEST_EXPECT(broken->load_bytes({1, 2, 3}, "bad.dds", AssetKind::Texture, "jo", error) &&
	            !dynamic_cast<const TextureDocument *>(broken.get())->image()->loads);
	std::printf("document: an image, its bytes as read, read only, nothing changed since its load, snapshotted\n");
	return 0;
}

struct Rig {
	editor_test::TempProjectDir dir{"opennova_editor_texture_document"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string brick = "textures/brick.tga";
	const std::string sky = "textures/sky.pcx";
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const TextureViewport *viewport(const std::string &path) {
		return static_cast<const TextureViewport *>(session.viewports().find(path, ViewportKind::Texture));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue query(const std::string &name, const std::string &args_json) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(args_json, args, error);
		return session.query(name, args, error);
	}
};

int test_session() {
	Rig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Textures"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_bytes(rig.root() + "/" + rig.brick, tga_of(pattern(4, 4, graded), 4, 4)) &&
	            editor_test::write_bytes(rig.root() + "/" + rig.sky, pcx_of(8, 4)) &&
	            editor_test::write_text(rig.root() + "/defs/items.def",
	                                    "begin \"Brick\"\nid 100300\ntype building\nhud_image brick.tga\nend\n"));
	editor_test::handle_to_end(rig.session, request::rescan());
	rig.pump();
	// No texture is a finding of a document that holds no records.
	for (const Diagnostic &d : rig.view().findings.diagnostics) TEST_EXPECT(d.code() != "document.no_records");
	// Files says the editor opens it.
	bool editable = false, listed = false;
	for (int offset = 0; offset < 2000 && !listed; offset += 100) {
		const JsonValue files = rig.query("files", "{\"offset\":" + std::to_string(offset) + ",\"limit\":100}");
		const JsonValue *rows = files.get("files");
		if (!rows || rows->array.empty()) break;
		for (const JsonValue &row : rows->array)
			if (row.get_string("path", "") == rig.brick) {
				listed = true;
				editable = row.get_bool("editable", false);
			}
	}
	TEST_EXPECT(listed && editable);

	// Selected in Files: the Preview window shows it, read from its file, before it is opened.
	editor_test::handle_to_end(rig.session, request::select_file(rig.brick));
	TEST_EXPECT(rig.session.outcome().done());
	TEST_EXPECT(rig.view().documents.file_selected.path == rig.brick && rig.view().documents.files_lead &&
	            rig.view().documents.preview_shown == ViewportKind::Texture &&
	            rig.view().documents.previews[ViewportKind::Texture].path == rig.brick);
	rig.pump();
	const TextureViewport *viewport = rig.viewport(rig.brick);
	TEST_EXPECT(viewport && viewport->from_file() && viewport->status() == ViewportStatus::Ready && viewport->reads() == 1 &&
	            viewport->image()->width() == 4);
	editor_test::FakeDevice *device = rig.devices.held(rig.brick, ViewportKind::Texture);
	TEST_EXPECT(device && device->since(0) == std::vector<ViewportAction>{ViewportAction::Rebuild});
	rig.pump();
	TEST_EXPECT(viewport->reads() == 1 && device->last() == ViewportAction::Keep);
	// The viewport query over it: its state, a texel hit.
	JsonValue state = rig.query("viewport", "{\"path\":\"" + rig.brick + "\",\"op\":\"state\"}");
	TEST_EXPECT(state.get_string("kind", "") == "texture" && state.get_string("status", "") == "ready");
	const JsonValue *body = state.get("body");
	TEST_EXPECT(body && body->get_number("width", 0) == 4 && body->get_bool("from_file", false) &&
	            body->get_string("alpha", "") == "graded");
	// The headless picture is 800 x 600, the texture fitted (150 pixels a texel) about its middle.
	const JsonValue hit = rig.query("viewport", "{\"path\":\"" + rig.brick + "\",\"op\":\"hit\",\"x\":400,\"y\":300}");
	TEST_EXPECT(hit.get_string("kind", "") == "texel" && hit.get_string("name", "").rfind("2, 2: ", 0) == 0);
	// Its camera and options set; a scale out of range refused, nothing changed.
	TextureCamera camera;
	camera.fit = false;
	camera.scale = 4.0f;
	camera.x = 1.0f;
	camera.y = 1.0f;
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.brick, texture_camera_change(camera)));
	TEST_EXPECT(rig.session.outcome().done() && viewport->camera() == camera);
	TextureViewportOptions options;
	options.channels = TextureChannels::Alpha;
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.brick, texture_options_change(options)));
	TEST_EXPECT(rig.session.outcome().done() && viewport->options() == options);
	editor_test::handle_to_end(rig.session,
	                           request::set_viewport(rig.brick, "{\"kind\":\"texture\",\"camera\":{\"scale\":1000}}"));
	TEST_EXPECT(!rig.session.outcome().done() && viewport->camera() == camera);

	// Opened: the tab shows it; the Preview window steps aside rather than show it twice.
	editor_test::handle_to_end(rig.session, request::open_document(rig.brick));
	TEST_EXPECT(rig.session.outcome().done() && rig.view().documents.active == rig.brick);
	TEST_EXPECT(!rig.view().documents.files_lead && rig.view().documents.preview_shown == ViewportKind::kCount);
	rig.pump();
	TEST_EXPECT(rig.viewport(rig.brick) == viewport && !viewport->from_file() && viewport->camera() == camera);
	const JsonValue document = rig.query("document", "{\"path\":\"" + rig.brick + "\"}");
	const JsonValue *texture = document.get("texture");
	TEST_EXPECT(texture && texture->get_number("height", 0) == 4 && texture->get_string("reader", "") == "tga");
	// An edit is refused: read only.
	Edit set;
	set.field = "name";
	rig.session.handle(request::edit_record(rig.brick, set));
	TEST_EXPECT(!rig.session.outcome().done());
	// The item that names it uses it (the graph's edge, by the item's record).
	TEST_EXPECT(rig.view().findings.graph);
	const std::vector<const GraphEdge *> users = rig.view().findings.graph->usages_of(rig.brick);
	TEST_EXPECT(users.size() == 1 && users[0]->source == "defs/items.def" && users[0]->field == "hud_image");

	// Another texture selected leads the Preview again; a file of another kind does not.
	editor_test::handle_to_end(rig.session, request::select_file(rig.sky));
	TEST_EXPECT(rig.view().documents.files_lead && rig.view().documents.preview_shown == ViewportKind::Texture &&
	            rig.view().documents.previews[ViewportKind::Texture].path == rig.sky);
	rig.pump();
	TEST_EXPECT(rig.viewport(rig.sky) && rig.viewport(rig.sky)->image()->palette_size() == 256);
	editor_test::handle_to_end(rig.session, request::select_file("defs/items.def"));
	TEST_EXPECT(!rig.view().documents.files_lead && rig.view().documents.previews[ViewportKind::Texture].path.empty());
	rig.pump();
	TEST_EXPECT(!rig.viewport(rig.sky) && rig.viewport(rig.brick));
	// A file the project lacks is refused.
	rig.session.handle(request::select_file("textures/none.tga"));
	TEST_EXPECT(!rig.session.outcome().done());
	// Closed: its viewport goes with its document, the selection with the project.
	editor_test::handle_to_end(rig.session, request::select_file(rig.sky));
	editor_test::handle_to_end(rig.session, request::close_document(rig.brick));
	rig.pump();
	TEST_EXPECT(!rig.viewport(rig.brick) && rig.viewport(rig.sky));
	editor_test::handle_to_end(rig.session, request::close_project());
	TEST_EXPECT(rig.view().documents.file_selected.path.empty() && !rig.view().documents.files_lead);
	std::printf("session: Files' selection previewed from its file, the viewport query and SetViewport over it, opened, "
	            "used by an item\n");
	return 0;
}

// --- the retail leg ----------------------------------------------------------------------------------

// A sample of the install's textures of each extension: each read by its reader and loaded as a
// document, the game loading it; their counts said.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's textures)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	opennova::Vfs mount;
	TEST_EXPECT(mount_retail(mount, install, project));
	constexpr size_t kSample = 40;
	std::map<std::string, size_t> listed, opened;
	size_t texels = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (classify_asset(name, nullptr) != AssetKind::Texture) continue;
		std::string extension = name.substr(name.find_last_of('.') + 1);
		for (char &c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (++listed[extension] > kSample) continue;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(mount.read_file_raw(name, bytes));
		std::unique_ptr<DocumentBase> document = make_texture_document();
		Diagnostic error;
		TEST_EXPECT(document->load_bytes(bytes, name, AssetKind::Texture, "jo", error));
		const TextureImage &image = *dynamic_cast<const TextureDocument &>(*document).image();
		if (!image.loads || !image.decoded || image.blank)
			std::fprintf(stderr, "retail: %s: %s%s\n", name.c_str(), image.refusal.c_str(), image.undecoded.c_str());
		TEST_EXPECT(image.loads && image.decoded && !image.blank && image.width() > 0 && !image.upside_down);
		texels += size_t(image.width()) * image.height();
		++opened[extension];
	}
	for (const auto &[extension, count] : listed)
		std::printf("retail: %zu .%s files, %zu read\n", count, extension.c_str(), opened[extension]);
	// Every extension the game's texture readers take is in the install, each sample read whole.
	for (const char *extension : {"dds", "mdt", "tga", "pcx", "png"})
		TEST_EXPECT(listed[extension] > 0 && opened[extension] == std::min(listed[extension], kSample));
	std::printf("retail: %zu texels read\n", texels);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_decode();
	failures += test_document();
	failures += test_session();
	failures += test_retail();
	if (failures == 0) std::printf("editor_texture_document: all passed\n");
	return failures == 0 ? 0 : 1;
}
