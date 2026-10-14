// A music bank stream's audio from a wave (ADR 0046, round S23 lane A's bank): a stream's audio replaced and a
// stream added through the music_stream_audio request over a session, each one undo step; the bank saved and read
// back through the engine's reader (sbf_read_bank), each stream decoded as the preview player sounds it
// (music_stream_pcm) against the wave it was made from (a mono 44100 Hz wave of the project, a stereo 22050 Hz one
// on disk), within the codec's precision (a byte a sample at its chunk's shift) and at the stream's length; undo
// giving the stream its old chunks back; the same audio again no step; the refusals. Minted waves and banks alone.
#include <editor/documents/music_bank_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/lwf/wav_source.h>
#include <formats/sbf/sbf.h>

#include <base/io/file_io.h>
#include <base/io/le.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr double kPi = 3.14159265358979;

// A sine of `hz` at `level` of full scale, `frames` of it at `rate`, as 16-bit samples.
std::vector<int16_t> sine(size_t frames, double hz, double level, uint32_t rate) {
	std::vector<int16_t> out(frames);
	for (size_t f = 0; f < frames; ++f)
		out[f] = int16_t(std::lround(level * 32767.0 * std::sin(2.0 * kPi * hz * double(f) / rate)));
	return out;
}

// A 16-bit RIFF WAVE of `channels` interleaved channels at `rate`.
std::vector<uint8_t> riff(const std::vector<int16_t> &samples, uint16_t channels, uint32_t rate) {
	std::vector<uint8_t> out;
	const auto text = [&](const char *t) { out.insert(out.end(), t, t + 4); };
	const uint32_t data = uint32_t(samples.size() * 2);
	text("RIFF");
	io::append_u32_le(out, 36 + data);
	text("WAVE");
	text("fmt ");
	io::append_u32_le(out, 16);
	io::append_u16_le(out, 1);
	io::append_u16_le(out, channels);
	io::append_u32_le(out, rate);
	io::append_u32_le(out, rate * channels * 2);
	io::append_u16_le(out, uint16_t(channels * 2));
	io::append_u16_le(out, 16);
	text("data");
	io::append_u32_le(out, data);
	for (const int16_t s : samples) io::append_u16_le(out, uint16_t(s));
	return out;
}

// A sample of a wave as the stream holds it before the codec: its full scale the stream's (+-127 * 128).
long streamed(int16_t sample) { return std::lround(double(sample) / 32768.0 * 16256.0); }

// The stream at `place` of the bank `bytes` hold, decoded as the preview player sounds it, against `expected`
// (interleaved left and right): its length, and each sample within its chunk's step (the shift of its side) and
// the rounding of the conversion.
bool sounds_as(const std::vector<uint8_t> &bytes, int place, const std::vector<long> &expected) {
	lwf::WavPcm pcm;
	std::string error;
	sbf::SbfFile bank;
	if (!music_stream_pcm(bytes, place, pcm, error) || !sbf::sbf_read_bank(bytes.data(), bytes.size(), bank, error)) {
		std::fprintf(stderr, "stream %d: %s\n", place, error.c_str());
		return false;
	}
	if (pcm.channels != 2 || pcm.sample_rate != 22050 || pcm.pcm16.size() != expected.size() * 2 ||
	    pcm.loader_samples != expected.size() / 2) {
		std::fprintf(stderr, "stream %d: %zu samples, %zu expected\n", place, pcm.pcm16.size() / 2, expected.size());
		return false;
	}
	size_t at = 0;
	for (const sbf::SbfChunk &chunk : bank.streams[size_t(place)].chunks) {
		for (size_t i = 0; i < chunk.audio.size(); ++i, ++at) {
			const long step = long(128u >> (i % 2 ? chunk.scale_b : chunk.scale_a));
			const long got = long(int16_t(io::read_u16_le(pcm.pcm16.data() + at * 2)));
			const long off = std::labs(got - expected[at]);
			if (off > step + 2) {
				std::fprintf(stderr, "stream %d, sample %zu: %ld, %ld expected (step %ld)\n", place, at, got, expected[at], step);
				return false;
			}
		}
	}
	return at == expected.size();
}

const MusicBankDocument *bank_of(ProjectSession &session, const std::string &path) {
	return dynamic_cast<const MusicBankDocument *>(session.document_base_for(path));
}

bool made_audio(const std::vector<sbf::SbfChunk> &a, const std::vector<sbf::SbfChunk> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].scale_a != b[i].scale_a || a[i].scale_b != b[i].scale_b || a[i].audio != b[i].audio) return false;
	return true;
}

// The conversion alone: a mono wave both sides at 22050 a second, another rate resampled to its length, its words;
// a wave of three channels, of no sample or of no form refused.
int test_conversion() {
	const std::vector<int16_t> mono = sine(4410, 441.0, 0.5, 44100);
	MusicStreamAudioEdit made;
	std::string why;
	std::vector<uint8_t> wave;
	lwf::wav_write_pcm_mono(reinterpret_cast<const uint8_t *>(mono.data()), mono.size() * 2, 44100, 16, wave, why);
	TEST_EXPECT(music_stream_audio_from_wave("sounds/tone.wav", wave, made, why));
	size_t bytes = 0;
	for (const sbf::SbfChunk &chunk : *made.chunks) bytes += chunk.audio.size();
	TEST_EXPECT(bytes == 2205 * 2 && made.words.find("tone.wav (16-bit PCM, mono, 44100 Hz: the channel on both sides, "
	                                                 "resampled to 22050 Hz), 0.100 s") == 0);
	// A sting of 0.050 s says so (three places under a second), never rounded to 0.1 s.
	{
		const std::vector<int16_t> sting = sine(1103, 441.0, 0.5, 22050);
		std::vector<uint8_t> short_wave;
		MusicStreamAudioEdit short_made;
		lwf::wav_write_pcm_mono(reinterpret_cast<const uint8_t *>(sting.data()), sting.size() * 2, 22050, 16, short_wave, why);
		TEST_EXPECT(music_stream_audio_from_wave("sting.wav", short_wave, short_made, why) &&
		            short_made.words.size() > 9 && short_made.words.compare(short_made.words.size() - 9, 9, ", 0.050 s") == 0);
	}
	std::vector<int16_t> pcm;
	TEST_EXPECT(lwf::wave_stereo_pcm16(wave, 22050, 1.0f, pcm, why) && pcm.size() == 2205 * 2 && pcm[200] == pcm[201] &&
	            std::labs(long(pcm[200]) - long(mono[200])) <= 1);
	TEST_EXPECT(!music_stream_audio_from_wave("five.wav", riff(std::vector<int16_t>(300, 0), 3, 22050), made, why) &&
	            why.find("3 channels") != std::string::npos);
	TEST_EXPECT(!music_stream_audio_from_wave("empty.wav", riff({}, 2, 22050), made, why) &&
	            why.find("no sample") != std::string::npos);
	TEST_EXPECT(!music_stream_audio_from_wave("text.wav", std::vector<uint8_t>(64, 'x'), made, why));
	TEST_EXPECT(music_stream_name_of("C:/music/My theme (final) mix.wav") == "MY THEME (FINAL)" &&
	            music_stream_name_of("sounds/caf\xC3\xA9.wav") == "CAF__");
	return 0;
}

int test_session() {
	test_temp::TempDir dir("opennova_editor_music_stream_audio");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Music from waves"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(!root.empty());
	if (root.empty()) return 1;

	// The bank: two streams of their own audio.
	sbf::SbfFile original;
	const std::vector<int16_t> first = sine(3000, 300.0, 0.2, 22050), second = sine(6000, 600.0, 0.3, 22050);
	original.streams.push_back(sbf::sbf_encode_stream("FIRST", first.data(), first.size()));
	original.streams.push_back(sbf::sbf_encode_stream("SECOND", second.data(), second.size()));
	std::vector<uint8_t> written;
	std::string why;
	TEST_EXPECT(sbf::sbf_write_bank(original, written, why));
	const std::string path = "music/theme.sbf";
	TEST_EXPECT(test_io::write_file(root + "/" + path, written));
	// A mono wave of the project at 44100 Hz, and a stereo one on disk at 22050 Hz (left and right apart).
	const std::vector<int16_t> mono = sine(10000, 441.0, 0.9, 44100);
	std::vector<uint8_t> mono_wave;
	lwf::wav_write_pcm_mono(reinterpret_cast<const uint8_t *>(mono.data()), mono.size() * 2, 44100, 16, mono_wave, why);
	TEST_EXPECT(test_io::write_file(root + "/sounds/mono44.wav", mono_wave));
	const std::vector<int16_t> left = sine(7000, 441.0, 0.6, 22050), right = sine(7000, 882.0, 1.0, 22050);
	std::vector<int16_t> both(14000);
	for (size_t f = 0; f < 7000; ++f) both[f * 2] = left[f], both[f * 2 + 1] = right[f];
	const std::string stereo = dir.file("stereo22.wav");
	TEST_EXPECT(test_io::write_file(stereo, riff(both, 2, 22050)));
	editor_test::handle_to_end(session, request::rescan());

	// Stream 1's audio from the project's wave, the bank opened first: one step, said.
	ActionOutcome done = editor_test::handle_to_end(session, request::music_stream_audio(path, "sounds/mono44.wav", "1", "", true));
	const MusicBankDocument *bank = bank_of(session, path);
	TEST_EXPECT(done.done() && bank && bank->dirty());
	if (!bank) return 1;
	TEST_EXPECT(session.view().activity.status.find("Stream 1's audio made from mono44.wav (16-bit PCM, mono, 44100 Hz: the "
	                                                "channel on both sides, resampled to 22050 Hz)") == 0);
	TEST_EXPECT(bank->bank_row()->streams.size() == 2 && bank->bank_row()->streams[1].name == "SECOND" &&
	            std::fabs(music_stream_seconds(bank->bank_row()->streams[1]) - 5000.0 / 22050.0) < 1e-9);
	const uint64_t replaced = bank->revision();
	// The same wave again changes nothing: no step.
	TEST_EXPECT(editor_test::handle_to_end(session, request::music_stream_audio(path, "sounds/mono44.wav", "1")).done() &&
	            bank->revision() == replaced);
	// A new stream from the wave on disk: at the end, named after it, selected.
	done = editor_test::handle_to_end(session, request::music_stream_audio(path, stereo));
	TEST_EXPECT(done.done() && bank->bank_row()->streams.size() == 3 && bank->bank_row()->streams[2].name == "STEREO22" &&
	            session.view().activity.status.find("Stream 2 added from stereo22.wav") == 0);
	TEST_EXPECT(session.view().documents.selection.primary.child == bank->bank_row()->ids.lists[0][2].id);

	// The Shell streams the bank's file: refused until saved, then it plays the new stream.
	TEST_EXPECT(!editor_test::handle_to_end(session, request::play_stream(path, 2)).done());
	editor_test::handle_to_end(session, request::save(path));
	TEST_EXPECT(!bank->dirty() && editor_test::handle_to_end(session, request::play_stream(path, 2)).done() &&
	            session.view().workspace.sound.voices.size() == 1 && session.view().workspace.sound.voices[0].stream == 2);

	// Read back through the engine's reader: stream 0 as it was; stream 1 the mono wave on both sides, every other
	// sample of it (44100 to 22050); stream 2 the stereo wave, left and right apart.
	std::vector<uint8_t> saved;
	TEST_EXPECT(io::read_file_bytes(root + "/" + path, saved, why));
	sbf::SbfFile read;
	TEST_EXPECT(sbf::sbf_read_bank(saved.data(), saved.size(), read, why) && read.streams.size() == 3 &&
	            read.streams[0].name == "FIRST" && read.streams[1].name == "SECOND" && read.streams[2].name == "STEREO22" &&
	            made_audio(read.streams[0].chunks, original.streams[0].chunks));
	std::vector<long> expected;
	for (size_t f = 0; f < 5000; ++f) expected.push_back(streamed(mono[f * 2])), expected.push_back(streamed(mono[f * 2]));
	TEST_EXPECT(sounds_as(saved, 1, expected));
	expected.clear();
	for (size_t f = 0; f < 7000; ++f) expected.push_back(streamed(left[f])), expected.push_back(streamed(right[f]));
	TEST_EXPECT(sounds_as(saved, 2, expected));
	// The wave's full scale is the stream's: the right side's peaks reach the codec's last step, unclipped.
	lwf::WavPcm pcm;
	TEST_EXPECT(music_stream_pcm(saved, 2, pcm, why));
	long peak = 0;
	for (size_t i = 1; i < pcm.pcm16.size() / 2; i += 2)
		peak = std::max(peak, std::labs(long(int16_t(io::read_u16_le(pcm.pcm16.data() + i * 2)))));
	TEST_EXPECT(peak >= 16000 && peak <= 16256);

	// Undo: the new stream gone; again, stream 1 its old chunks; redo, the wave's again.
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(bank->bank_row()->streams.size() == 2);
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(made_audio(bank->bank().streams[1].chunks, original.streams[1].chunks) &&
	            made_audio(bank->bank().streams[0].chunks, original.streams[0].chunks));
	editor_test::handle_to_end(session, request::redo(path));
	TEST_EXPECT(made_audio(bank->bank().streams[1].chunks, read.streams[1].chunks));

	// Refused, nothing changed: a stream the bank has not, a name for a place, no wave, a wave that does not read or
	// has three channels, a value of no key it takes, a name the field refuses, a document that is no bank.
	TEST_EXPECT(test_io::write_file(root + "/sounds/three.wav", riff(std::vector<int16_t>(900, 100), 3, 22050)));
	editor_test::handle_to_end(session, request::rescan());
	const uint64_t before = bank->revision();
	const auto refused = [&](const EditorRequest &request, const char *words) {
		const ActionOutcome outcome = editor_test::handle_to_end(session, request);
		bool said = false;
		for (const Diagnostic &d : outcome.findings) said = said || d.message.find(words) != std::string::npos;
		if (!said) std::fprintf(stderr, "no '%s' in the refusal: %s\n", words, session.view().activity.status.c_str());
		return !outcome.done() && said && bank->revision() == before;
	};
	TEST_EXPECT(refused(request::music_stream_audio(path, "sounds/mono44.wav", "2"), "no stream at '2'"));
	TEST_EXPECT(refused(request::music_stream_audio(path, "sounds/mono44.wav", "SECOND"), "by its place"));
	EditorRequest no_wave = request::music_stream_audio(path, "");
	no_wave.values.clear();
	TEST_EXPECT(refused(no_wave, "names the wave"));
	TEST_EXPECT(refused(request::music_stream_audio(path, "sounds/missing.wav", "0"), "could not be read"));
	TEST_EXPECT(refused(request::music_stream_audio(path, "sounds/three.wav", "0"), "3 channels"));
	EditorRequest other = request::music_stream_audio(path, "sounds/mono44.wav", "0");
	other.values.emplace_back("gain", "2");
	TEST_EXPECT(refused(other, "'gain' is none of them"));
	TEST_EXPECT(!editor_test::handle_to_end(session, request::music_stream_audio(path, "sounds/mono44.wav", "0", std::string(17, 'N'))).done() &&
	            bank->revision() == before);
	TEST_EXPECT(!editor_test::handle_to_end(session, request::music_stream_audio("sounds/mono44.wav", "sounds/mono44.wav", "0", "", true)).done() &&
	            bank->revision() == before);
	// A name given with the audio: one step, both.
	TEST_EXPECT(editor_test::handle_to_end(session, request::music_stream_audio(path, "sounds/mono44.wav", "0", "OPENING")).done() &&
	            bank->bank_row()->streams[0].name == "OPENING" &&
	            made_audio(bank->bank().streams[0].chunks, bank->bank().streams[1].chunks));
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(bank->bank_row()->streams[0].name == "FIRST" && made_audio(bank->bank().streams[0].chunks, original.streams[0].chunks));
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_conversion();
	failed += test_session();
	if (failed == 0) std::printf("editor music stream audio: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
