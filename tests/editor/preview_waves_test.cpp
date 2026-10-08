// The preview's waves and the clip sounds' voices waiting on them (DI-04's player; editor/preview/preview_waves):
// each wave decoded once on a worker as the game decodes it and kept until its file changes; a voice waits for its
// wave and survives the project's files moving (a rescan, a save) unless its wave's file changed or went. Over
// waves minted here (RIFF PCM16) in a scratch folder.
//
// Pinned: a voice fired in the frame a rescan lands plays (the voices' old forget dropped it); an unchanged wave
// kept through a rescan, not decoded again; a wave whose file changed dropped with the voices waiting on it and
// decoded anew from the new bytes when next asked; one whose file went dropped likewise; a wave that failed (its
// file missing) decoded once its file comes to be; a voice whose wave fails dropped; clear drops the voices and
// keeps the waves.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <editor/preview/preview_waves.h>
#include <editor/project/project_files.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// A mono 22,050 Hz RIFF wave of `frames` PCM16 frames.
std::vector<uint8_t> wave_of(uint32_t frames) {
	std::vector<uint8_t> out;
	auto u32 = [&out](uint32_t v) {
		for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
	};
	auto u16 = [&out](uint16_t v) {
		out.push_back(uint8_t(v));
		out.push_back(uint8_t(v >> 8));
	};
	const uint32_t data = frames * 2;
	out.insert(out.end(), {'R', 'I', 'F', 'F'});
	u32(36 + data);
	out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	u32(16);
	u16(1);     // PCM
	u16(1);     // mono
	u32(22050); // rate
	u32(44100); // bytes a second
	u16(2);     // block
	u16(16);    // bits
	out.insert(out.end(), {'d', 'a', 't', 'a'});
	u32(data);
	for (uint32_t i = 0; i < frames; ++i) u16(uint16_t(int16_t((i % 64) * 256 - 8192)));
	return out;
}

// The wave once its decode is done (a few seconds at most).
PreviewWaves::Wave settled(PreviewWaves &waves, const std::string &file) {
	PreviewWaves::Wave wave = waves.wave(file);
	for (int i = 0; i < 5000 && wave.state == PreviewWaves::State::Decoding; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		wave = waves.wave(file);
	}
	return wave;
}

// The voices the queue hands over once nothing waits (a few seconds at most).
std::vector<PreviewVoiceQueue::Ready> taken(PreviewVoiceQueue &queue) {
	std::vector<PreviewVoiceQueue::Ready> out;
	for (int i = 0; i < 5000; ++i) {
		for (PreviewVoiceQueue::Ready &ready : queue.take_ready()) out.push_back(std::move(ready));
		if (queue.waiting() == 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return out;
}

int64_t frames_of(const PreviewWaves::Wave &wave) {
	return wave.pcm && wave.pcm->channels ? int64_t(wave.pcm->pcm16.size() / (2u * wave.pcm->channels)) : -1;
}

WorkspaceView::Voice voice_of(const char *path) {
	WorkspaceView::Voice voice;
	voice.path = path;
	return voice;
}

// A voice fired in the frame the project's files move (a save's rescan) plays: an unchanged wave is kept, its
// decode in flight or done, and never decoded again.
int test_a_voice_survives_a_rescan() {
	editor_test::TempProjectDir dir("opennova_preview_waves_survive");
	TEST_EXPECT(editor_test::write_bytes(dir.file("sounds/shot.wav"), wave_of(100)));
	PreviewWaves waves;
	PreviewVoiceQueue queue(waves);
	queue.add(dir.root(), {voice_of("sounds/shot.wav")});
	TEST_EXPECT(queue.waiting() == 1);
	// The rescan lands before the Shell's pump takes the voice: nothing it reads moved, so it waits on.
	TEST_EXPECT(queue.refresh().empty());
	TEST_EXPECT(queue.waiting() == 1);
	const std::vector<PreviewVoiceQueue::Ready> ready = taken(queue);
	TEST_EXPECT(ready.size() == 1 && ready[0].file == join_path(dir.root(), "sounds/shot.wav") &&
	            ready[0].voice.path == "sounds/shot.wav");
	// The wave kept through another rescan: the same decode, not another.
	const PreviewWaves::Wave before = settled(waves, ready.empty() ? std::string() : ready[0].file);
	TEST_EXPECT(before.state == PreviewWaves::State::Decoded && frames_of(before) == 100);
	TEST_EXPECT(waves.refresh().empty());
	const PreviewWaves::Wave after = waves.wave(ready.empty() ? std::string() : ready[0].file);
	TEST_EXPECT(after.state == PreviewWaves::State::Decoded && after.serial == before.serial && waves.decoding() == 0);
	// The project closing drops the voices waiting and keeps the waves.
	queue.add(dir.root(), {voice_of("sounds/shot.wav")});
	queue.clear();
	TEST_EXPECT(queue.waiting() == 0 && waves.wave(ready.empty() ? std::string() : ready[0].file).serial == before.serial);
	return 0;
}

// A wave whose file changed is dropped with the voices waiting on it, and decoded anew from its new bytes when next
// asked; one whose file went is dropped likewise.
int test_a_changed_or_removed_wave() {
	editor_test::TempProjectDir dir("opennova_preview_waves_changed");
	const std::string shot = join_path(dir.root(), "sounds/shot.wav");
	const std::string step = join_path(dir.root(), "sounds/step.wav");
	TEST_EXPECT(editor_test::write_bytes(shot, wave_of(100)));
	TEST_EXPECT(editor_test::write_bytes(step, wave_of(50)));
	PreviewWaves waves;
	PreviewVoiceQueue queue(waves);
	queue.add(dir.root(), {voice_of("sounds/shot.wav"), voice_of("sounds/step.wav")});
	const uint64_t serial = settled(waves, shot).serial;
	// Both decodes done first: a decode reading a file holds it open, and Windows refuses to delete an open file.
	TEST_EXPECT(settled(waves, step).state == PreviewWaves::State::Decoded);
	// The shot rewritten and the step deleted before the pump: both voices dropped with their waves.
	TEST_EXPECT(editor_test::write_bytes(shot, wave_of(200)));
	std::error_code ec;
	TEST_EXPECT(std::filesystem::remove(system_path(step), ec));
	const std::vector<std::string> dropped = queue.refresh();
	TEST_EXPECT(dropped.size() == 2 && queue.waiting() == 0 && taken(queue).empty());
	// Asked again: the new bytes, another decode; the step gone, a failure.
	const PreviewWaves::Wave again = settled(waves, shot);
	TEST_EXPECT(again.state == PreviewWaves::State::Decoded && frames_of(again) == 200 && again.serial != serial);
	TEST_EXPECT(settled(waves, step).state == PreviewWaves::State::Failed);
	// A voice whose wave does not decode is dropped, not kept waiting.
	queue.add(dir.root(), {voice_of("sounds/step.wav")});
	TEST_EXPECT(taken(queue).empty() && queue.waiting() == 0);
	// The step written again: the failure dropped at the rescan, the wave decoded.
	TEST_EXPECT(editor_test::write_bytes(step, wave_of(75)));
	TEST_EXPECT(waves.refresh() == std::vector<std::string>({step}));
	const PreviewWaves::Wave back = settled(waves, step);
	TEST_EXPECT(back.state == PreviewWaves::State::Decoded && frames_of(back) == 75);
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_a_voice_survives_a_rescan() == 0);
	TEST_EXPECT(test_a_changed_or_removed_wave() == 0);
	std::printf("preview_waves_test passed\n");
	return 0;
}
