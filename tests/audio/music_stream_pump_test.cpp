// The music stream pump (runtime/audio/music_stream_pump.h) over scripts compiled from MUS text:
// the script steps only once the track its last `play` selected has stopped sounding, a service
// frame steps through section changes to the next `play` (at most seven updates), GFB empties
// the stream so the next frame steps over a track still sounding, a fresh context selects no
// track, and a context ends with its VM or with end().
#include <formats/mus/mus.h>
#include <runtime/audio/music_stream_pump.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova;
using opennova::audio::MusicPumpOutcome;
using opennova::audio::MusicStreamPump;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// The device: the tracks started, in order, and whether the last one still sounds.
struct Device final : audio::MusicStreamDevice {
	std::vector<uint32_t> tracks;
	std::vector<std::string> sections;
	bool sounding = false;
	void start_track(uint32_t entry) override {
		tracks.push_back(entry);
		sounding = true;
	}
	bool track_sounding() const override { return sounding; }
	void section_entered(const char *name) override { sections.push_back(name ? name : ""); }
};

// A compiled script on a started VM, the pump installed and begun over the device.
struct Context {
	mus::MusScript script{};
	mus::MusVM *vm = nullptr;
	MusicStreamPump pump;
	Device device;
	bool ok = false;

	explicit Context(const char *src) {
		std::memset(&script, 0, sizeof(script));
		int line = 0, col = 0;
		const char *err = nullptr;
		if (mus::mus_compile(src, &script, &line, &col, &err) != 0) {
			std::printf("compile failed @%d:%d: %s\n", line, col, err ? err : "?");
			return;
		}
		vm = mus::mus_vm_create();
		pump.install(vm, &device);
		if (mus::mus_vm_load_script(vm, &script) != 0) return;
		mus::mus_vm_start(vm);
		pump.begin();
		ok = pump.running();
	}
	~Context() {
		mus::mus_vm_destroy(vm);
		mus::mus_script_free(&script);
	}
};

const char *kChainSrc =
		"script t\n"
		"section Begin\n{\n  play sound_1\n  enter A\n}\n"
		"section A\n{\n  enter B\n}\n"
		"section B\n{\n  play sound_2\n  enter Begin\n}\n";

// One `play` per track: the script waits on the sounding track, then one frame steps through
// the section changes to the next `play`.
void test_steps_on_track_completion() {
	Context c(kChainSrc);
	CHECK(c.ok);
	if (!c.ok) return;
	CHECK(c.pump.service_frame(16) == MusicPumpOutcome::Running);
	CHECK(c.device.tracks == std::vector<uint32_t>({1}));
	CHECK(c.pump.streaming());
	for (int frame = 0; frame < 5; ++frame) c.pump.service_frame(16);
	CHECK(c.device.tracks == std::vector<uint32_t>({1}));
	CHECK(c.device.sections.empty());
	c.device.sounding = false;
	CHECK(c.pump.service_frame(16) == MusicPumpOutcome::Running);
	CHECK(c.device.tracks == std::vector<uint32_t>({1, 2}));
	CHECK(c.device.sections == std::vector<std::string>({"A", "B"}));
	CHECK(c.pump.streaming());
}

// A frame's updates stop at seven: a chain of eight section changes reaches its `play` on the
// second frame.
void test_frame_takes_seven_updates() {
	std::string src = "script t\nsection Begin\n{\n  enter S1\n}\n";
	for (int i = 1; i < 8; ++i)
		src += "section S" + std::to_string(i) + "\n{\n  enter S" + std::to_string(i + 1) + "\n}\n";
	src += "section S8\n{\n  play sound_4\n  enter Begin\n}\n";
	Context c(src.c_str());
	CHECK(c.ok);
	if (!c.ok) return;
	c.pump.service_frame(16);
	CHECK(c.device.tracks.empty());
	CHECK(c.device.sections.size() == 7);
	CHECK(!c.device.sections.empty() && c.device.sections.back() == "S7");
	c.pump.service_frame(16);
	CHECK(c.device.tracks == std::vector<uint32_t>({4}));
}

const char *kStingSrc =
		"script t\n"
		"section Begin\n{\n  play sound_1\n  enter Begin\n}\n"
		"handler MessageHandler(msgtype, source)\n{\n  FB()\n  on (msgtype) enter Begin Sting\n}\n"
		"section Sting\n{\n  play sound_5\n  enter Begin\n}\n";

const char *kStingNoFlushSrc =
		"script t\n"
		"section Begin\n{\n  play sound_1\n  enter Begin\n}\n"
		"handler MessageHandler(msgtype, source)\n{\n  on (msgtype) enter Begin Sting\n}\n"
		"section Sting\n{\n  play sound_5\n  enter Begin\n}\n";

// GFB empties the stream: the frame after the handler steps to the sting's `play` while the
// track it cuts still sounds. Without it the sting waits for that track.
void test_flush_steps_over_the_sounding_track() {
	{
		Context c(kStingSrc);
		CHECK(c.ok);
		if (!c.ok) return;
		c.pump.service_frame(16);
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1}));
		CHECK(mus::mus_vm_signal(c.vm, 1) == 0);
		CHECK(!c.pump.streaming());
		CHECK(c.device.sounding);
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1, 5}));
	}
	{
		Context c(kStingNoFlushSrc);
		CHECK(c.ok);
		if (!c.ok) return;
		c.pump.service_frame(16);
		CHECK(mus::mus_vm_signal(c.vm, 1) == 0);
		CHECK(c.pump.streaming());
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1}));
		c.device.sounding = false;
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1, 5}));
	}
}

// A fresh context selects no track; end() and a VM that stops end the pumping.
void test_context_bounds() {
	Context c(kChainSrc);
	CHECK(c.ok);
	if (!c.ok) return;
	c.pump.service_frame(16);
	CHECK(c.pump.streaming());
	c.pump.begin();
	CHECK(c.pump.running() && !c.pump.streaming());
	c.pump.end();
	CHECK(!c.pump.running());
	c.device.sounding = false;
	CHECK(c.pump.service_frame(16) == MusicPumpOutcome::Idle);
	CHECK(c.device.tracks == std::vector<uint32_t>({1}));
	c.pump.begin();
	mus::mus_vm_stop(c.vm);
	CHECK(c.pump.service_frame(16) == MusicPumpOutcome::Ended);
	CHECK(!c.pump.running());
	CHECK(c.pump.service_frame(16) == MusicPumpOutcome::Idle);
	CHECK(c.device.tracks == std::vector<uint32_t>({1}));
}

} // namespace

int main() {
	test_steps_on_track_completion();
	test_frame_takes_seven_updates();
	test_flush_steps_over_the_sounding_track();
	test_context_bounds();
	if (failures != 0) {
		std::printf("music_stream_pump: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("music_stream_pump: all cases passed\n");
	return 0;
}
