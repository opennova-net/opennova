// The blanks DI-33 adds (ADR 0046 DI-33, "Create a missing file with the engine's own writers"): every file kind a
// reference or the boot manifest names that the engine has a writer for gets a blank, the smallest file the game's
// loader takes, made from scratch by the engine's writer. Each blank is read back here through the engine's own
// reader of its format (the one the game's loader is ported to), and through the editor's document type where the
// editor opens the kind, which finds nothing wrong with it; the companions (an animation map's reset clip, a music
// bank's script) are the files the blank names or the game reads with it.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/project/project_document.h>
#include <formats/adm/adm.h>
#include <formats/aip/aip.h>
#include <formats/avatars/avatars.h>
#include <formats/bad/bad.h>
#include <formats/dbf/dbf.h>
#include <formats/dds/dds.h>
#include <formats/def/def.h>
#include <formats/def/def_hudpos_write.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/lwf/wav_source.h>
#include <formats/mus/mus.h>
#include <formats/particle/parser.h>
#include <formats/sbf/sbf.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/menu/menu_credits.h>

#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// The blank a file of `kind` named `name` gets (its role's where the name is a role's file), made.
std::vector<uint8_t> blank_of(const std::string &name, AssetKind kind, const std::string &role = std::string()) {
	BlankRequest request;
	request.logical_name = name;
	request.role = role;
	request.project_title = "Writers Test";
	std::vector<uint8_t> out;
	Diagnostic error;
	if (!make_blank(request, kind, out, error)) std::fprintf(stderr, "%s: %s\n", name.c_str(), error.message.c_str());
	return out;
}

std::string text_of(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

// Every line ends CR LF (the game's ASCII walk ends a line there and nowhere else).
bool crlf(const std::string &text) {
	for (size_t i = 0; i < text.size(); ++i)
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) return false;
	return !text.empty() && text.back() == '\n';
}

// The file opened through its kind's document type, its findings none above `worst`.
bool opens_clean(const std::string &name, AssetKind kind, const std::vector<uint8_t> &bytes,
                 DiagnosticSeverity worst = DiagnosticSeverity::Info) {
	const DocumentType *type = document_type_for(kind);
	if (!type) return false;
	std::unique_ptr<DocumentBase> document = type->make();
	Diagnostic error;
	if (!document->load_bytes(bytes, name, kind, "jo", error)) {
		std::fprintf(stderr, "%s does not open: %s\n", name.c_str(), error.message.c_str());
		return false;
	}
	for (const Diagnostic &d : type->validate_file(*document))
		if (d.severity > worst) {
			std::fprintf(stderr, "%s: %s %s\n", name.c_str(), d.code().c_str(), d.message.c_str());
			return false;
		}
	return true;
}

} // namespace

// A model of one triangle, two faces back to back: one LOD, one part, six vertices in one strip, read back by the
// 3DI3 reader; the model document
// finds nothing. The celestial model's role (upl.3di) is the same blank.
static int test_model() {
	const std::vector<uint8_t> bytes = blank_of("crate.3di", AssetKind::Model);
	threedi::Threedi3di3 model{};
	TEST_EXPECT(!bytes.empty() && threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
	TEST_EXPECT(model.lod_count == 1 && model.lods[0].render_object_count == 1 && model.lods[0].vertices.count == 6 &&
	            model.lods[0].strip_count == 1);
	threedi::threedi_3di3_free(&model);
	TEST_EXPECT(opens_clean("crate.3di", AssetKind::Model, bytes));
	TEST_EXPECT(find_blank_factory_for_role("upl_3di") && find_blank_factory_for_role("upl_3di")->kind == AssetKind::Model);
	TEST_EXPECT(!blank_of("upl.3di", AssetKind::Model, "upl_3di").empty());
	return 0;
}

// A clip of one bone at rest, one looping frame, its two events written; an animation map of its anim_reset row
// naming <table>_rst.bad, which is made with it (blank_companion), the stem cut to fit a packed name.
static int test_animation() {
	const std::vector<uint8_t> clip = blank_of("walk.bad", AssetKind::Animation);
	bad::BadFile file{};
	TEST_EXPECT(!clip.empty() && bad::bad_parse_buffer(clip.data(), clip.size(), &file) == 0);
	TEST_EXPECT(file.num_bones == 1 && file.frame_count == 1 && file.fps == 30 && file.num_events == 2 && (file.flags & 1u));
	bad::bad_free(&file);
	TEST_EXPECT(opens_clean("walk.bad", AssetKind::Animation, clip));

	const std::vector<uint8_t> map = blank_of("tank.adm", AssetKind::AnimationMap);
	adm::AdmFile table{};
	TEST_EXPECT(!map.empty() && adm::adm_parse_buffer(reinterpret_cast<const char *>(map.data()), map.size(), &table) == 0);
	TEST_EXPECT(table.count == 1 && std::string(table.entries[0].key) == "anim_reset" && table.entries[0].variant_count == 1 &&
	            std::string(table.entries[0].variants[0]) == "tank_rst.bad");
	adm::adm_free(&table);
	TEST_EXPECT(opens_clean("tank.adm", AssetKind::AnimationMap, map));
	TEST_EXPECT(blank_reset_clip_name("Soldier_Walks.adm") == "soldier_rst.bad");
	ProjectDocument project;
	std::string companion;
	const BlankFactory *clip_factory =
	        blank_companion(*find_blank_factory_for_kind(AssetKind::AnimationMap), "tank.adm", project, companion);
	TEST_EXPECT(clip_factory == find_blank_factory_for_kind(AssetKind::Animation) && companion == "tank_rst.bad");
	return 0;
}

// The sound lane: a wave of one sample of silence the game's loader plays (46 bytes, the editor's check of that
// loader's rules passing it); a dialog bank of no group; the shell's sound bank.
static int test_sound() {
	const std::vector<uint8_t> wave = blank_of("door.wav", AssetKind::Wave);
	lwf::WavPcm pcm;
	std::string error;
	TEST_EXPECT(wave.size() == 46 && lwf::wav_decode_pcm16(wave.data(), wave.size(), pcm, error));
	TEST_EXPECT(pcm.channels == 1 && pcm.sample_rate == 22050 && pcm.pcm16.size() == 2);
	TEST_EXPECT(lwf::wave_retail_check(wave).plays);
	std::vector<uint8_t> none;
	const uint8_t sample[2] = {0, 0};
	TEST_EXPECT(!lwf::wav_write_pcm_mono(sample, 0, 22050, 16, none, error) && !lwf::wav_write_pcm_mono(sample, 1, 22050, 16, none, error) &&
	            !lwf::wav_write_pcm_mono(sample, 2, 0, 16, none, error) && !lwf::wav_write_pcm_mono(sample, 2, 22050, 24, none, error));

	const std::vector<uint8_t> dialogs = blank_of("m01.dbf", AssetKind::DialogBank);
	dbf::File bank;
	TEST_EXPECT(dialogs.size() == 28 && dbf::parse_dbf_memory(dialogs.data(), dialogs.size(), bank, error) && bank.groups.empty());
	// Made with the sound bank of its name, which the game opens beside it.
	ProjectDocument project;
	std::string companion;
	TEST_EXPECT(blank_companion(*find_blank_factory_for_kind(AssetKind::DialogBank), "m01.dbf", project, companion) ==
	                    find_blank_factory_for_kind(AssetKind::SoundBank) &&
	            companion == "m01.lwf");
	TEST_EXPECT(find_blank_factory_for_role("menu_lwf") && !blank_of("menu.lwf", AssetKind::SoundBank, "menu_lwf").empty());
	return 0;
}

// The music pairs: a bank of no stream (the 24-byte header), made with its script, whose one section idles; the
// mission's script carries the handler the round's end runs, the shell's none; the shell's opens in its document
// clean, the mission's held read only as retail's own (its MUS text has no handler).
static int test_music() {
	const std::vector<uint8_t> bank = blank_of("MENUMUS.SBF", AssetKind::MusicBank, "menumus_sbf");
	sbf::SbfArchive archive{};
	TEST_EXPECT(bank.size() == 24 && sbf::sbf_open_memory(&archive, bank.data(), bank.size()) == 0 &&
	            archive.header.entry_count == 0);
	sbf::sbf_close(&archive);
	ProjectDocument project;
	std::string companion;
	TEST_EXPECT(blank_companion(*find_blank_factory_for_role("menumus_sbf"), "MENUMUS.SBF", project, companion) ==
	                    find_blank_factory_for_role("menumus_bin") &&
	            companion == "MENUMUS.BIN");
	TEST_EXPECT(blank_companion(*find_blank_factory_for_role("gamemus_sbf"), "GAMEMUS.SBF", project, companion) ==
	                    find_blank_factory_for_role("gamemus_bin") &&
	            companion == "GAMEMUS.BIN");
	for (const char *role : {"menumus_bin", "gamemus_bin"}) {
		const bool mission = std::string(role) == "gamemus_bin";
		const std::vector<uint8_t> script = blank_of(mission ? "GAMEMUS.BIN" : "MENUMUS.BIN", AssetKind::MusicScript, role);
		mus::MusFile file{};
		TEST_EXPECT(!script.empty() && mus::mus_open_memory(&file, script.data(), script.size()) == 0);
		TEST_EXPECT(file.header.chunk_count == 1 && file.scripts[0].section_count == 1 && file.scripts[0].code_size >= 1 &&
		            file.scripts[0].code[0] == 0x3F && file.scripts[0].has_message_handler == (mission ? 1 : 0));
		mus::mus_close(&file);
		if (!mission) TEST_EXPECT(opens_clean("MENUMUS.BIN", AssetKind::MusicScript, script));
		else TEST_EXPECT(opens_clean("GAMEMUS.BIN", AssetKind::MusicScript, script, DiagnosticSeverity::Warning));
	}
	return 0;
}

// The text families: a particle file of no effect, a credits roll of one line, an AI profile of no type, the HUD
// layout with nothing moved, an avatars table of nothing; each CR LF, read by the engine's reader as the game
// reads one, and opened clean.
static int test_text() {
	const std::vector<uint8_t> particles = blank_of("neweffects.ptl", AssetKind::Particles);
	particle::ParticleFile effects;
	particle::ParseError parse_error;
	TEST_EXPECT(crlf(text_of(particles)) &&
	            particle::load_particles_from_buffer(reinterpret_cast<const char *>(particles.data()), particles.size(), effects,
	                                                 parse_error) &&
	            effects.effects.empty() && effects.particles.empty());
	TEST_EXPECT(opens_clean("neweffects.ptl", AssetKind::Particles, particles));

	const std::vector<uint8_t> credits = blank_of("newcredits.kda", AssetKind::Credits);
	menu::MarqueeCredits roll;
	TEST_EXPECT(crlf(text_of(credits)) && menu::marquee_load_credits(credits.data(), credits.size(), roll, nullptr));
	TEST_EXPECT(roll.scroll_rate == 1.0f && roll.center_x == 400 && roll.nodes.size() == 1 &&
	            roll.nodes[0].line == "Writers_Test" && roll.nodes[0].font == "Arial14b");
	TEST_EXPECT(opens_clean("newcredits.kda", AssetKind::Credits, credits));

	const std::vector<uint8_t> profile = blank_of("tank.aip", AssetKind::AiProfile);
	TEST_EXPECT(crlf(text_of(profile)) && aip::parse_profile(profile.data(), profile.size()).type == 0);
	TEST_EXPECT(opens_clean("tank.aip", AssetKind::AiProfile, profile));

	const std::vector<uint8_t> hud = blank_of("hudpos.def", AssetKind::HudPosDefs, "hudpos_def");
	def::DefHudPosFile layout{};
	TEST_EXPECT(crlf(text_of(hud)) && def::def_parse_hudpos_memory(hud.data(), hud.size(), &layout) == 0);
	TEST_EXPECT(opens_clean("hudpos.def", AssetKind::HudPosDefs, hud));

	const std::vector<uint8_t> avatars = blank_of("Avatars.def", AssetKind::AvatarDefs, "avatars_def");
	avatars::AvatarsFile table{};
	TEST_EXPECT(crlf(text_of(avatars)) && avatars::avatars_parse_memory(avatars.data(), avatars.size(), &table) == 0 &&
	            table.parts_count == 0 && table.nationalities_count == 0);
	avatars::avatars_free(&table);
	TEST_EXPECT(opens_clean("Avatars.def", AssetKind::AvatarDefs, avatars));
	return 0;
}

// The player preview's environment cube: a DDS cube map D3DX loads, six faces of 128 a side; the textures the game
// reads by name at the boot and the round's end, the checkerboard at the boot screen's size and the texture's own.
static int test_textures() {
	const std::vector<uint8_t> cube = blank_of("HwmCube.dds", AssetKind::Texture, "hwmcube_dds");
	dds::DdsImage image;
	std::string error;
	TEST_EXPECT(dds::dds_read(cube.data(), cube.size(), image, error) && image.loads && image.faces == 6 &&
	            image.header.width == 128 && image.header.height == 128);
	uint32_t width = 0, height = 0;
	const std::vector<uint8_t> loading = blank_of("loading.pcx", AssetKind::Texture, "loading_pcx");
	TEST_EXPECT(!loading.empty());
	const std::vector<uint8_t> backdrop = blank_of("jo_Epil.tga", AssetKind::Texture, "jo_epil_tga");
	TEST_EXPECT(!backdrop.empty() && !blank_of("jo_Epil2.tga", AssetKind::Texture, "jo_epil2_tga").empty());
	TEST_EXPECT(!dds::dds_header_size(backdrop.data(), backdrop.size(), width, height)); // a TGA, as its name says
	TEST_EXPECT(!blank_of("medmssn.bin", AssetKind::Strings, "medmssn_bin").empty());
	return 0;
}

// What stays without a blank, each for its reason: fgn2.bin (its presence alone switches the game to its foreign
// effects), failsafe.bad (with it a clip that does not load plays it in place of the slot's reset), hudfx.def (no
// reader or writer of it in the engine), the country code, a video, the terrain (an import of its images, S20).
static int test_left_out() {
	for (const char *role : {"fgn2_bin", "failsafe_bad", "hudfx_def", "cc_bin", "intro_bik"})
		TEST_EXPECT(find_blank_factory_for_role(role) == nullptr);
	for (AssetKind kind : {AssetKind::Video, AssetKind::Terrain, AssetKind::TerrainPolyData, AssetKind::FaceAnimation,
	                       AssetKind::CountryCode, AssetKind::RawBin, AssetKind::HudFxDefs})
		TEST_EXPECT(find_blank_factory_for_kind(kind) == nullptr);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_model();
	failures += test_animation();
	failures += test_sound();
	failures += test_music();
	failures += test_text();
	failures += test_textures();
	failures += test_left_out();
	if (failures == 0) std::printf("editor_blank_writers: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
