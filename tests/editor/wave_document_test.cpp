// Waves (ADR 0046, round S23 lane A): a .wav as a document held as its bytes and read as the game's loader reads it
// (lwf::wave_facts, lwf::wave_retail_check): its facts and picture on the wire; the one finding a wave the loader
// refuses makes (asset.wave_unplayable), on the wave's own file; a trim and a normalise, each one undo step written
// in the form the game takes; the wave_operation request through a session and its refusals. Every wave of the
// variant set through the document byte for byte (the loader's forms minted under fixtures/lwf/, IMA ADPCM among
// them, our base game's, the PCM forms at the rates the shipped waves take), its one finding exactly where the loader
// refuses the wave, as for DSkid.wav's 16-bit stereo alone of JO's (D-SND-33). Minted waves and our own alone.
#include <editor/documents/document_types.h>
#include <editor/documents/wave_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/lwf/wav_source.h>

#include <base/io/le.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/menus_audio_fixtures.h"
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

using editor_test::has_code;

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
	test_temp::TempDir dir("opennova_editor_wave_document");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Waves"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string path = "sounds/tone.wav";
	TEST_EXPECT(test_io::write_file(root + "/" + path, mono_wave(22050)));
	editor_test::handle_to_end(session, request::rescan());
	ActionOutcome done = editor_test::handle_to_end(session, request::wave_operation(path, "trim", {{"start", "0"}, {"end", "0.5"}}, true));
	TEST_EXPECT(done.done() && session.view().activity.status.find("Trimmed") != std::string::npos);
	const auto *wave = dynamic_cast<const WaveDocument *>(session.document_base_for(path));
	TEST_EXPECT(wave && wave->dirty() && wave->facts().frames == 11025);
	done = editor_test::handle_to_end(session, request::wave_operation(path, "normalise"));
	TEST_EXPECT(done.done() && std::fabs(wave->facts().peak - 1.0f) < 0.01f);
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(wave->facts().frames == 11025 && std::fabs(wave->facts().peak - 0.25f) < 0.01f);
	// The Shell plays the wave's file: a wave with unsaved edits is refused until it is saved.
	ActionOutcome played = editor_test::handle_to_end(session, request::play_sound(path));
	TEST_EXPECT(!played.done() && session.view().workspace.sound.path.empty());
	editor_test::handle_to_end(session, request::save(path));
	played = editor_test::handle_to_end(session, request::play_sound(path));
	TEST_EXPECT(played.done() && session.view().workspace.sound.path == path);
	std::vector<uint8_t> saved;
	std::string message;
	TEST_EXPECT(io::read_file_bytes(root + "/" + path, saved, message) && lwf::wave_facts(saved).frames == 11025);
	TEST_EXPECT(!editor_test::handle_to_end(session, request::wave_operation(path, "fade")).done());
	TEST_EXPECT(!editor_test::handle_to_end(session, request::wave_operation("menus/main.mnu", "trim", {}, true)).done());
	return 0;
}

// The IMA ADPCM waves the format's lane mints (tests/fixtures/minimal_lwf_gen.cpp, tests/fixtures/ima_adpcm_source.h):
// mono 4-bit IMA ADPCM, a 20-byte fmt chunk and a fact chunk, the form a third of JO's waves take (D-SND-45), in its
// two shipped shapes: 11025 Hz in 256-byte blocks of 505 samples, 22050 Hz in 512-byte blocks of 1017.
const char *const kAdpcmVariants[] = {"fixtures/lwf/ima_adpcm.wav", "fixtures/lwf/ima_adpcm_512.wav"};

// A mono 8-bit wave of `frames` frames of the 441 Hz sine at 0.25, through the engine's writer.
std::vector<uint8_t> mono_wave8(size_t frames, uint32_t rate) {
	std::vector<uint8_t> data;
	for (size_t f = 0; f < frames; ++f)
		data.push_back(uint8_t(128 + std::lround(0.25 * 127.0 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(f) / rate))));
	std::vector<uint8_t> out;
	std::string error;
	lwf::wav_write_pcm_mono(data.data(), data.size(), rate, 8, out, error);
	return out;
}

// Every wave of the variant set through the document: fixtures/lwf/'s (the loader's forms, minted), our base game's
// (assets/: mono 8- and 16-bit PCM at 11025, 22050 and 44100 Hz) and, made here, the PCM forms at the other rates
// the shipped waves take (16-bit at 5000 and 16000 Hz, 8-bit at 8000 and 48000). Each reads and writes back byte for
// byte, and makes its one finding (asset.wave_unplayable) exactly where the game's loader refuses it; none of the
// forms the shipped waves take but DSkid.wav's 16-bit stereo, test_document's refused wave (D-SND-33), makes one, the
// IMA ADPCM variants decoded to their frames among them.
int test_every_wave() {
	std::vector<variant_fixtures::Input> inputs;
	const bool all = variant_fixtures::inputs({"fixtures/lwf", "assets"}, ".wav",
	                                          std::vector<std::string>(std::begin(kAdpcmVariants), std::end(kAdpcmVariants)), inputs);
	struct Wave {
		std::string label;
		std::vector<uint8_t> bytes;
		bool shipped_form; // a form a shipped wave the loader takes is written in: no finding
	};
	std::vector<Wave> waves;
	for (const variant_fixtures::Input &input : inputs) {
		const bool adpcm = std::find(std::begin(kAdpcmVariants), std::end(kAdpcmVariants), input.label) != std::end(kAdpcmVariants);
		waves.push_back({input.label, test_io::read_file(input.path), adpcm || input.label.rfind("assets/", 0) == 0});
	}
	waves.push_back({"16-bit PCM at 5000 Hz", mono_wave(500, 0.25, 5000), true});
	waves.push_back({"16-bit PCM at 16000 Hz", mono_wave(1600, 0.25, 16000), true});
	waves.push_back({"8-bit PCM at 8000 Hz", mono_wave8(800, 8000), true});
	waves.push_back({"8-bit PCM at 48000 Hz", mono_wave8(4800, 48000), true});
	size_t refused = 0, adpcm = 0;
	for (const Wave &w : waves) {
		WaveDocument wave;
		Diagnostic error;
		const bool loaded = !w.bytes.empty() && wave.load_bytes(w.bytes, "sounds/wave.wav", AssetKind::Wave, "jo", error);
		const SerializeResult same = wave.serialize();
		const bool equal = loaded && same.ok() && same.text == std::string(w.bytes.begin(), w.bytes.end());
		if (!equal) std::printf("  %s: %zu bytes, written again %zu\n", w.label.c_str(), w.bytes.size(), same.text.size());
		TEST_EXPECT(equal);
		const lwf::WaveRetailCheck check = lwf::wave_retail_check(w.bytes);
		const std::vector<Diagnostic> findings = validate_wave_file(wave);
		const bool said = check.plays ? findings.empty()
		                              : findings.size() == 1 && findings[0].code() == "asset.wave_unplayable";
		if (!said || (w.shipped_form && !check.plays))
			std::printf("  %s: %s, %zu finding(s)\n", w.label.c_str(), check.plays ? "plays" : check.why.c_str(), findings.size());
		TEST_EXPECT(said && (!w.shipped_form || check.plays));
		refused += check.plays ? 0 : 1;
		if (wave.facts().format.tag == lwf::kWaveTagImaAdpcm) {
			++adpcm;
			TEST_EXPECT(wave.facts().read && wave.facts().frames > 0);
		}
	}
	std::printf("waves: %zu through the document byte for byte, %zu of them IMA ADPCM, %zu the loader refuses\n",
	            waves.size(), adpcm, refused);
	TEST_EXPECT(all && adpcm >= std::size(kAdpcmVariants));
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_document();
	failed += test_operations();
	failed += test_session();
	failed += test_every_wave();
	if (failed == 0) std::printf("editor wave document: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
