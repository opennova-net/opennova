#pragma once

// The music stream pump: the per-frame service that steps the music context's AudioVM only once
// the track its last `play` selected has streamed out, and otherwise keeps that track streaming.
// The script is never stepped per frame; one `play` runs per track, and section changes land on
// track boundaries [orig: AudioStream_Update @ 0x671c60; AudioStream_ServiceFrame @ 0x671dd0].
// The VM is formats/mus; the pump owns the context's run state, whether the selected track still
// has data to stream (retail's bytes left, g_AudioStreamBytesLeft @ 0x31C37EC) and the service
// loop. The embedder's device (MusicStreamDevice) plays the one music stream: it starts the
// track each `play` selects and reports whether that track is still sounding.
// Witness record: docs/audio/mus-sbf-re.md, "AudioVM playback pacing".

#include <formats/mus/mus.h>

#include <cstdint>

namespace opennova::audio {

// The device the pump drives: the embedder's one music stream.
class MusicStreamDevice {
public:
	virtual ~MusicStreamDevice() = default;
	// `play` / `playw` selected SBF entry `entry`: the one stream switches to it from its start,
	// so a track still sounding stops streaming [orig: AudioVM_Op_Play @ 0x672CB0 ->
	// AudioVM_StartSound @ 0x671ff0 -> AudioVM_SelectStream @ 0x671bc0 - the entry's bytes
	// @ 0x671bf0, the seek to its data @ 0x671c4a].
	virtual void start_track(uint32_t entry) = 0;
	// Whether the track the last start_track began is still sounding.
	virtual bool track_sounding() const = 0;
	// The VM's section and GEcho notices (mus.h's on_section_entered / on_echo), passed on.
	virtual void section_entered(const char *name) { (void)name; }
	virtual void echo(int32_t value) { (void)value; }
};

// What one service frame did.
enum class MusicPumpOutcome {
	Idle,     // no context running: nothing stepped
	Running,  // the context runs on
	Ended,    // a step left the VM stopped or halted: the context ends
	Failed,   // a step stopped the VM in error (mus_vm_last_error)
};

class MusicStreamPump {
public:
	MusicStreamPump() = default;
	// The VM's hooks point at the pump, so it stays where it was installed.
	MusicStreamPump(const MusicStreamPump &) = delete;
	MusicStreamPump &operator=(const MusicStreamPump &) = delete;

	// Bind the VM and the device (neither owned): the VM's hooks become the pump's, `play` /
	// `playw` selecting the stream, GFB emptying it, the section and echo notices going to the
	// device.
	void install(mus::MusVM *vm, MusicStreamDevice *device);

	// A context opened or started: it runs while the VM runs, no track selected yet, so the
	// first service frame steps the script to its first `play` [orig: AudioVM_OpenMusicContext
	// @ 0x6722a0 - g_AudioVMContextActive = 1 @ 0x6722db, g_AudioStreamBytesLeft = 0
	// @ 0x6722e5].
	void begin();
	// A paused VM resumed (the embedder's pause, no retail counterpart): the context runs again
	// if the VM does; the selected track is kept.
	void resume();
	// The context stopped: nothing steps until the next begin [orig: AudioVM_StopMusicContext
	// @ 0x671e00 - g_AudioVMContextActive = 0 @ 0x671e14].
	void end();

	bool running() const { return running_; }
	// Whether the selected track still has data to stream: retail's bytes left above 0.
	bool streaming() const { return streaming_; }

	// One service frame [orig: AudioStream_ServiceFrame @ 0x671dd0 - the update up to seven
	// times, stopping at the first that finds the stream's queue full @ 0x671ddc..0x671de7]:
	// each update with the context running steps the script when the selected track has
	// streamed out, or with none selected [orig: AudioStream_Update @ 0x671c60 -
	// g_AudioVMContextActive @ 0x671c66, g_AudioStreamBytesLeft <= 0 @ 0x671c76 ->
	// AudioVM_RunInstance @ 0x671c7f], and otherwise streams it, the queue full while the
	// device sounds it [orig: Audio_GetPendingBufferCount >= 7 @ 0x671c95]. A step that selects
	// a track ends the frame, the track streaming through it. `dt_ms` goes to mus_vm_tick.
	MusicPumpOutcome service_frame(uint32_t dt_ms);

private:
	static void on_play_sound(void *user, uint32_t entry, int wait);
	static void on_flush_buffer(void *user);
	static void on_section_entered(void *user, const char *name);
	static void on_echo(void *user, int32_t value);

	mus::MusVM *vm_ = nullptr;
	MusicStreamDevice *device_ = nullptr;
	bool running_ = false;    // g_AudioVMContextActive
	bool streaming_ = false;  // g_AudioStreamBytesLeft > 0
};

} // namespace opennova::audio
