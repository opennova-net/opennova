// The music stream pump (runtime/audio/music_stream_pump.h) over scripts compiled from MUS text:
// the script steps only once the track its last `play` selected has stopped sounding, a service
// frame steps through section changes to the next `play` (at most seven updates), a signal's
// handler whose run reaches a `play` starts it at once, the shipped handler's GFB empties the
// stream so the next update steps over a track still sounding when its run ends short of the
// sting's `play`, a fresh context selects no track, and a context ends with its VM or with
// end().
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

// The shipped gamemus handler's shape: its `on` table's entry enters a section (Missionwin) whose
// first op is a `setstate` to the section that plays the sting, with the handler's GFB or without.
std::string shipped_sting_src(bool flush) {
	std::string src = "script t\n";
	src += "section Begin\n{\n  play sound_1\n  enter Begin\n}\n";
	src += "handler MessageHandler(msgtype, source)\n{\n";
	if (flush) src += "  FB()\n";
	src += "  on (msgtype) enter Begin Win\n}\n";
	src += "section Win\n{\n  enter WinPlay\n}\n";
	src += "section WinPlay\n{\n  play sound_5\n  enter Begin\n}\n";
	return src;
}

// The signal runs the step at once (MusicCtx_SelectEndTrack @0x672FD0 sets the restart byte and
// runs AudioVM_RunContext @0x672E50): a handler whose run reaches a `play` starts it in the
// signal's own run (the play's STC ends the run, AudioVM_Op_Play @0x672CBF), its
// AudioVM_SelectStream seeding the bytes left again (@0x671BF0), so a GFB ahead of it changes
// nothing. The shipped handler's run ends at Missionwin's `setstate` (its STC) short of the sting's
// `play`: its GFB has zeroed the bytes left (AudioVM_Intrinsic_GFB @0x672150), so the next update
// steps at once over the track still sounding (AudioStream_Update @0x671C76) and plays the sting;
// without the GFB the sting waits for that track to stream out.
void test_flush_steps_over_the_sounding_track() {
	for (const char *src : {kStingSrc, kStingNoFlushSrc}) {
		Context c(src);
		CHECK(c.ok);
		if (!c.ok) return;
		c.pump.service_frame(16);
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1}));
		CHECK(mus::mus_vm_signal(c.vm, 1) == 0);
		CHECK(c.device.tracks == std::vector<uint32_t>({1, 5}));
		CHECK(c.pump.streaming());
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1, 5}));
	}
	{
		const std::string src = shipped_sting_src(true);
		Context c(src.c_str());
		CHECK(c.ok);
		if (!c.ok) return;
		c.pump.service_frame(16);
		CHECK(mus::mus_vm_signal(c.vm, 1) == 0);
		CHECK(c.device.tracks == std::vector<uint32_t>({1}));
		CHECK(!c.pump.streaming());
		CHECK(c.device.sounding);
		c.pump.service_frame(16);
		CHECK(c.device.tracks == std::vector<uint32_t>({1, 5}));
	}
	{
		const std::string src = shipped_sting_src(false);
		Context c(src.c_str());
		CHECK(c.ok);
		if (!c.ok) return;
		c.pump.service_frame(16);
		CHECK(mus::mus_vm_signal(c.vm, 1) == 0);
		CHECK(c.device.tracks == std::vector<uint32_t>({1}));
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
