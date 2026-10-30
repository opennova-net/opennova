#include <runtime/audio/music_stream_pump.h>

namespace opennova::audio {

namespace {

// The service frame's tries [orig: AudioStream_ServiceFrame @ 0x671dd0 - `for (i = 0; i < 7;
// ++i) if (AudioStream_Update()) break;` @ 0x671ddc..0x671de7].
constexpr int kServiceUpdates = 7;

bool vm_runs(const mus::MusVM *vm) {
	return vm != nullptr && mus::mus_vm_state(vm) == mus::MUS_VM_RUNNING;
}

} // namespace

void MusicStreamPump::install(mus::MusVM *vm, MusicStreamDevice *device) {
	vm_ = vm;
	device_ = device;
	if (vm_ == nullptr) return;
	mus::MusVMHooks hooks = {};
	hooks.user = this;
	hooks.on_play_sound = on_play_sound;
	hooks.on_flush_buffer = on_flush_buffer;
	hooks.on_section_entered = on_section_entered;
	hooks.on_echo = on_echo;
	mus::mus_vm_set_hooks(vm_, &hooks);
}

void MusicStreamPump::begin() {
	running_ = vm_runs(vm_);
	streaming_ = false;
}

void MusicStreamPump::resume() {
	running_ = vm_runs(vm_);
}

void MusicStreamPump::end() {
	running_ = false;
	streaming_ = false;
}

MusicPumpOutcome MusicStreamPump::service_frame(uint32_t dt_ms) {
	if (!running_ || vm_ == nullptr) return MusicPumpOutcome::Idle;
	for (int update = 0; update < kServiceUpdates; ++update) {
		if (streaming_) {
			// The track streams on while it sounds: the stream's queue is full, the frame
			// done [orig: AudioStream_Update @ 0x671c95 -> return 1].
			if (device_ != nullptr && device_->track_sounding()) break;
			// It has streamed out: its bytes left reach 0 [orig: @ 0x671d61].
			streaming_ = false;
		}
		// The step [orig: AudioStream_Update @ 0x671c7f -> AudioVM_RunInstance @ 0x672ee0].
		mus::mus_vm_tick(vm_, dt_ms);
		const mus::MusVMState state = mus::mus_vm_state(vm_);
		// PAUSED is the embedder's pause and resume; only a VM stopped, halted or in error
		// ends the context.
		if (state != mus::MUS_VM_RUNNING && state != mus::MUS_VM_PAUSED) {
			running_ = false;
			return state == mus::MUS_VM_ERROR ? MusicPumpOutcome::Failed : MusicPumpOutcome::Ended;
		}
		// The step selected a track: it streams through the rest of the frame.
		if (streaming_) break;
	}
	return MusicPumpOutcome::Running;
}

void MusicStreamPump::on_play_sound(void *user, uint32_t entry, int wait) {
	(void)wait;  // play and playw both start the entry (D-MUS-11)
	MusicStreamPump *self = static_cast<MusicStreamPump *>(user);
	if (self == nullptr) return;
	// The entry's bytes seed the stream [orig: AudioVM_SelectStream @ 0x671bf0].
	self->streaming_ = true;
	if (self->device_ != nullptr) self->device_->start_track(entry);
}

void MusicStreamPump::on_flush_buffer(void *user) {
	MusicStreamPump *self = static_cast<MusicStreamPump *>(user);
	// [orig: AudioVM_Intrinsic_GFB @ 0x672150 - g_AudioStreamBytesLeft = 0]: the next update
	// steps the script, whatever the track still sounding.
	if (self != nullptr) self->streaming_ = false;
}

void MusicStreamPump::on_section_entered(void *user, const char *name) {
	MusicStreamPump *self = static_cast<MusicStreamPump *>(user);
	if (self != nullptr && self->device_ != nullptr) self->device_->section_entered(name);
}

void MusicStreamPump::on_echo(void *user, int32_t value) {
	MusicStreamPump *self = static_cast<MusicStreamPump *>(user);
	if (self != nullptr && self->device_ != nullptr) self->device_->echo(value);
}

} // namespace opennova::audio
