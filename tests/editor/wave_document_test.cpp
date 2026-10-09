// Waves (ADR 0046, round S23 lane A): a .wav as a document held as its bytes and read as the game's loader reads it
// (lwf::wave_facts, lwf::wave_retail_check): its facts and picture on the wire; the one finding a wave the loader
// refuses makes (asset.wave_unplayable), on the wave's own file; a trim and a normalise, each one undo step written
// in the form the game takes; the wave_operation request through a session and its refusals. Minted waves alone (the
// engine's writer); the retail leg (OPENNOVA_JO_ASSETS): every shipped wave through the document byte for byte, every
// one but DSkid.wav (D-SND-33) found playable.
#include <editor/documents/document_types.h>
#include <editor/documents/wave_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/lwf/wav_source.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

// A mono 16-bit wave of `frames` frames of a 441 Hz sine at `level`, through the engine's writer.
std::vector<uint8_t> mono_wave(size_t frames, double level = 0.25, uint32_t rate = 22050) {
	std::vector<uint8_t> data;
	for (size_t f = 0; f < frames; ++f)
		io::append_u16_le(data, uint16_t(int16_t(std::lround(level * 32767.0 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(f) / rate)))));
	std::vector<uint8_t> out;
	std::string error;
	lwf::wav_write_pcm_mono(data.data(), data.size(), rate, 16, out, error);
	return out;
}

// The same as a 16-bit stereo RIFF, which the game's loader refuses (its channel test).
std::vector<uint8_t> stereo_wave(size_t frames) {
	std::vector<uint8_t> out;
	const auto text = [&](const char *t) { out.insert(out.end(), t, t + 4); };
	text("RIFF");
	io::append_u32_le(out, uint32_t(36 + frames * 4));
	text("WAVE");
	text("fmt ");
	io::append_u32_le(out, 16);
	io::append_u16_le(out, 1);
	io::append_u16_le(out, 2);
	io::append_u32_le(out, 22050);
	io::append_u32_le(out, 22050 * 4);
	io::append_u16_le(out, 4);
	io::append_u16_le(out, 16);
	text("data");
	io::append_u32_le(out, uint32_t(frames * 4));
	for (size_t f = 0; f < frames * 2; ++f) io::append_u16_le(out, uint16_t(int16_t(f % 200) * 50));
	return out;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

bool load(WaveDocument &wave, const std::vector<uint8_t> &bytes, Diagnostic &error) {
	return wave.load_bytes(bytes, "sounds/tone.wav", AssetKind::Wave, "jo", error);
}

int test_document() {
	const std::vector<uint8_t> bytes = mono_wave(22050);
	WaveDocument wave;
	Diagnostic error;
	TEST_EXPECT(load(wave, bytes, error));
	const lwf::WaveFacts &facts = wave.facts();
	TEST_EXPECT(facts.read && facts.retail.plays && facts.frames == 22050 && std::fabs(facts.seconds - 1.0) < 1e-9 &&
	            std::fabs(facts.peak - 0.25f) < 0.01f && facts.envelope.size() == kWavePictureBins);
	TEST_EXPECT(validate_wave_file(wave).empty());
	const SerializeResult same = wave.serialize();
	TEST_EXPECT(same.ok() && std::vector<uint8_t>(same.text.begin(), same.text.end()) == bytes);
	const JsonValue content = wave_content_json(wave);
	TEST_EXPECT(content.get_bool("plays", false) && content.get_number("frames", 0) == 22050 &&
	            content.get("picture") && content.get("picture")->array.size() == kWavePictureBins);
	// The stereo wave: the one finding, on its file, in the loader's words.
	WaveDocument stereo;
	TEST_EXPECT(load(stereo, stereo_wave(1000), error) && !stereo.facts().retail.plays);
	const std::vector<Diagnostic> findings = validate_wave_file(stereo);
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "asset.wave_unplayable" && findings[0].asset == "sounds/tone.wav");
	TEST_EXPECT(document_type_for(AssetKind::Wave) && document_type_for(AssetKind::Wave)->id == DocumentTypeId::Wave);
	return 0;
}

int test_operations() {
	const std::vector<uint8_t> bytes = mono_wave(22050);
	std::vector<uint8_t> out;
	std::string words, why;
	// A trim: the frames from 0.25 s to 0.5 s.
	WaveOperation trim;
	trim.kind = WaveOperationKind::Trim;
	trim.params = {{"start", "0.25"}, {"end", "0.5"}};
	TEST_EXPECT(apply_wave_operation(bytes, trim, out, words, why));
	lwf::WaveFacts facts = lwf::wave_facts(out);
	TEST_EXPECT(facts.retail.plays && facts.frames == 5512 && words.rfind("Trimmed to 0.250 s", 0) == 0);
	// A normalise: the loudest sample at 0.9.
	WaveOperation normalise;
	normalise.kind = WaveOperationKind::Normalise;
	normalise.params = {{"peak", "0.9"}};
	TEST_EXPECT(apply_wave_operation(bytes, normalise, out, words, why));
	facts = lwf::wave_facts(out);
	TEST_EXPECT(facts.retail.plays && std::fabs(facts.peak - 0.9f) < 0.01f && facts.frames == 22050);
	// A stereo wave the game refuses: written as one it plays.
	normalise.params.clear();
	TEST_EXPECT(apply_wave_operation(stereo_wave(1000), normalise, out, words, why) && lwf::wave_facts(out).retail.plays);
	// Refused: a trim keeping nothing, a param of another name, a peak past 1, a silent wave normalised.
	trim.params = {{"start", "0.5"}, {"end", "0.25"}};
	TEST_EXPECT(!apply_wave_operation(bytes, trim, out, words, why) && why.find("keeps no sample") != std::string::npos);
	trim.params = {{"from", "0.1"}};
	TEST_EXPECT(!apply_wave_operation(bytes, trim, out, words, why));
	normalise.params = {{"peak", "1.5"}};
	TEST_EXPECT(!apply_wave_operation(bytes, normalise, out, words, why));
	normalise.params.clear();
	TEST_EXPECT(!apply_wave_operation(mono_wave(100, 0.0), normalise, out, words, why) && why.find("silent") != std::string::npos);
	WaveOperationKind kind;
	TEST_EXPECT(wave_operation_kind("normalize", kind) && kind == WaveOperationKind::Normalise && !wave_operation_kind("fade", kind));
	return 0;
}

// The wave_operation request over a session: one undo step each, the status saying what it did, Save writing it;
// refused for an operation the wave does not take and a document that is no wave.
int test_session() {
	editor_test::TempProjectDir dir("opennova_editor_wave_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Waves"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string path = "sounds/tone.wav";
	TEST_EXPECT(editor_test::write_bytes(root + "/" + path, mono_wave(22050)));
	editor_test::handle_to_end(session, request::rescan());
	ActionOutcome done = editor_test::handle_to_end(session, request::wave_operation(path, "trim", {{"start", "0"}, {"end", "0.5"}}, true));
	TEST_EXPECT(done.done() && session.view().activity.status.find("Trimmed") != std::string::npos);
	const auto *wave = dynamic_cast<const WaveDocument *>(session.document_base_for(path));
	TEST_EXPECT(wave && wave->dirty() && wave->facts().frames == 11025);
	done = editor_test::handle_to_end(session, request::wave_operation(path, "normalise"));
	TEST_EXPECT(done.done() && std::fabs(wave->facts().peak - 1.0f) < 0.01f);
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(wave->facts().frames == 11025 && std::fabs(wave->facts().peak - 0.25f) < 0.01f);
	editor_test::handle_to_end(session, request::save(path));
	std::vector<uint8_t> saved;
	std::string message;
	TEST_EXPECT(io::read_file_bytes(root + "/" + path, saved, message) && lwf::wave_facts(saved).frames == 11025);
	TEST_EXPECT(!editor_test::handle_to_end(session, request::wave_operation(path, "fade")).done());
	TEST_EXPECT(!editor_test::handle_to_end(session, request::wave_operation("menus/main.mnu", "trim", {}, true)).done());
	return 0;
}

int test_retail() {
	if (!retail::selected()) return 0;
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped waves)");
		return 0;
	}
	size_t waves = 0, unplayable = 0;
	std::string refused;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(assets), ec)) {
		std::string ext = entry.path().extension().u8string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".wav") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().u8string());
		WaveDocument wave;
		Diagnostic error;
		TEST_EXPECT(wave.load_bytes(bytes, entry.path().filename().u8string(), AssetKind::Wave, "jo", error));
		const SerializeResult same = wave.serialize();
		const bool equal = same.ok() && same.text == std::string(bytes.begin(), bytes.end());
		if (!equal)
			std::printf("  %s: %zu bytes, written again %zu\n", entry.path().filename().u8string().c_str(), bytes.size(),
			            same.text.size());
		TEST_EXPECT(equal);
		if (!validate_wave_file(wave).empty()) {
			++unplayable;
			refused = entry.path().filename().u8string();
		}
		++waves;
	}
	std::printf("retail: %zu waves through the document byte for byte, %zu the loader refuses (%s)\n", waves, unplayable,
	            refused.c_str());
	// The one shipped wave the loader refuses, DSkid.wav (its name as the tree spells it).
	TEST_EXPECT(waves > 1000 && unplayable == 1 && opennova::strutil::to_lower(refused) == "dskid.wav");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_document();
	failed += test_operations();
	failed += test_session();
	failed += test_retail();
	if (failed == 0) std::printf("editor wave document: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
