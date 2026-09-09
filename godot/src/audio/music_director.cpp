// MusicDirector. Embeds a engine/formats/mus VM and forwards hook callbacks to
// godot::Object signals. Witnessed wire-level paths in engine/formats/mus:
//   Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20   -> mus_vm_load_script
//   Jointops.exe!AudioVM_Op_Play @ 0x672CB0 / AudioVM_Op_PlayWait @ 0x672C90
//                                                  -> _on_play_sound
//   Jointops.exe!AudioVM_Op_SetState @ 0x672C70              -> _on_section_entered
//   Jointops.exe!Intrinsic_GSV @ 0x6720E0 / GSDV @ 0x672120
//                                                  -> _on_volume_changed
//   Jointops.exe!Intrinsic_GEcho @ 0x6720C0            -> _on_echo

#include "audio/music_director.h"
#include "audio/music_pair_names.h"

#include "audio/music_script.h"
#include "audio/sbf_audio_stream.h"
#include "audio/sbf_bank.h"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace opennova::mus;

MusicDirector::MusicDirector() {
	_vm = mus_vm_create();
}

MusicDirector::~MusicDirector() {
	if (_vm) {
		mus_vm_destroy(_vm);
		_vm = nullptr;
	}
}

void MusicDirector::_bind_methods() {
	// Property accessors
	ClassDB::bind_method(D_METHOD("load_mus_script", "script"), &MusicDirector::load_mus_script);
	ClassDB::bind_method(D_METHOD("get_mus_script"), &MusicDirector::get_mus_script);
	ClassDB::bind_method(D_METHOD("set_bank", "bank"), &MusicDirector::set_bank);
	ClassDB::bind_method(D_METHOD("get_bank"), &MusicDirector::get_bank);
	ClassDB::bind_method(D_METHOD("set_script_name", "name"), &MusicDirector::set_script_name);
	ClassDB::bind_method(D_METHOD("get_script_name"), &MusicDirector::get_script_name);
	ClassDB::bind_method(D_METHOD("set_auto_start", "auto"), &MusicDirector::set_auto_start);
	ClassDB::bind_method(D_METHOD("get_auto_start"), &MusicDirector::get_auto_start);

	// Methods
	ClassDB::bind_method(D_METHOD("start"), &MusicDirector::start);
	ClassDB::bind_method(D_METHOD("stop"), &MusicDirector::stop);
	ClassDB::bind_method(D_METHOD("has_pending_playback"), &MusicDirector::has_pending_playback);
	ClassDB::bind_method(D_METHOD("pause"), &MusicDirector::pause);
	ClassDB::bind_method(D_METHOD("resume"), &MusicDirector::resume);
	ClassDB::bind_method(D_METHOD("jump_to_section", "section_name"), &MusicDirector::jump_to_section);
	ClassDB::bind_method(D_METHOD("get_var", "var_index"), &MusicDirector::get_var);
	ClassDB::bind_method(D_METHOD("set_var", "var_index", "value"), &MusicDirector::set_var);
	ClassDB::bind_method(D_METHOD("vm_state"), &MusicDirector::vm_state);
	ClassDB::bind_method(D_METHOD("last_error"), &MusicDirector::last_error);

	// Witnessed music-driving policy (engine audio/music_policy.h re-exports).
	BIND_CONSTANT(MENU_MUSIC_VAR_SLOT);
	BIND_CONSTANT(GAME_SEEDED_VAR_FIRST);
	BIND_CONSTANT(GAME_SEEDED_VAR_LAST);
	BIND_CONSTANT(GAME_VAR_MISSION_STATE);
	BIND_CONSTANT(GAME_VAR_HEALTH_PCT);
	BIND_CONSTANT(GAME_VAR_TEAM);
	BIND_CONSTANT(GAME_HEALTH_SEED);
	ClassDB::bind_static_method("MusicDirector",
			D_METHOD("resolve_menu_music_pair", "expansion_name"),
			&MusicDirector::resolve_menu_music_pair);
	ClassDB::bind_static_method("MusicDirector",
			D_METHOD("resolve_game_music_pair", "expansion_name"),
			&MusicDirector::resolve_game_music_pair);

	// Properties (inspector). We deliberately do NOT expose a script property
	// because Godot scans for set_script/get_script pairs as the built-in script
	// slot, and even renamed pairs (e.g. set_mus_script) risk surprising the
	// engine. The smoke scene and the editor call load_mus_script() directly.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bank", PROPERTY_HINT_RESOURCE_TYPE,
						 "SbfBank"),
			"set_bank", "get_bank");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "script_name"),
			"set_script_name", "get_script_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_start"),
			"set_auto_start", "get_auto_start");

	// Signals
	ADD_SIGNAL(MethodInfo("section_entered",
			PropertyInfo(Variant::STRING_NAME, "name")));
	ADD_SIGNAL(MethodInfo("echo", PropertyInfo(Variant::INT, "arg")));
}

// --- Witnessed music-pair naming (pure re-export) -----------------------

static Ref<MusicPairNames> music_pair_record(const opennova::audio::MusicPairNames &names) {
	Ref<MusicPairNames> out;
	out.instantiate();
	out->assign(names);
	return out;
}

Ref<MusicPairNames> MusicDirector::resolve_menu_music_pair(const String &p_expansion_name) {
	return music_pair_record(opennova::audio::menu_music_pair_names(
			p_expansion_name.utf8().get_data()));
}

Ref<MusicPairNames> MusicDirector::resolve_game_music_pair(const String &p_expansion_name) {
	return music_pair_record(opennova::audio::game_music_pair_names(
			p_expansion_name.utf8().get_data()));
}

// --- Property setters / getters ----------------------------------------

void MusicDirector::load_mus_script(const Ref<MusicScript> &p_script) {
	_script = p_script;
}

Ref<MusicScript> MusicDirector::get_mus_script() const {
	return _script;
}

void MusicDirector::set_bank(const Ref<SbfBank> &p_bank) {
	_bank = p_bank;
}

Ref<SbfBank> MusicDirector::get_bank() const {
	return _bank;
}

void MusicDirector::set_script_name(const StringName &p_name) {
	_script_name = p_name;
}

StringName MusicDirector::get_script_name() const {
	return _script_name;
}

void MusicDirector::set_auto_start(bool p_auto) {
	_auto_start = p_auto;
}

bool MusicDirector::get_auto_start() const {
	return _auto_start;
}

// --- Lifecycle ---------------------------------------------------------

void MusicDirector::_ready() {
	for (int i = 0; i < _player_pool_size; ++i) {
		AudioStreamPlayer *p = memnew(AudioStreamPlayer);
		p->set_name(String("MusPlayer_") + String::num_int64(i));
		p->set_bus(_audio_bus);
		add_child(p);
		_players.push_back(p);
	}
	if (_auto_start && _script.is_valid()) {
		start();
	}
}

void MusicDirector::_process(double p_delta) {
	for (int i = _playbacks.size() - 1; i >= 0; --i) {
		if (ObjectDB::get_instance(_playbacks[i]) == nullptr) {
			_playbacks.remove_at(i);
		}
	}
	if (!_vm_running || _vm == nullptr) {
		return;
	}
	// Pace the VM the way the original engine does: AudioVM advances the script
	// only when the current track has finished streaming -- audio_stream_update
	// @0x671c60 steps the VM (sub_672EE0) solely on `remaining_bytes <= 0`, never
	// once per frame. While the track the last `play` started is still sounding,
	// leave the VM halted where that `play` stopped it. Without this gate the VM
	// races through `play`/`setstate` every frame, restarting the SBF stream
	// (AudioVM_StartSound @0x671ff0 seeks to the start) ~60x/sec -> a low buzz.
	if (_active_play != nullptr && _active_play->is_playing()) {
		return;
	}
	mus_vm_tick(_vm, (uint32_t)(p_delta * 1000.0));
	MusVMState state = mus_vm_state(_vm);
	// PAUSED is a non-terminal pause-and-resume condition; only RUNNING -> something
	// else (HALTED/STOPPED/ERROR) counts as the end of this play session.
	if (state != MUS_VM_RUNNING && state != MUS_VM_PAUSED) {
		_vm_running = false;
		if (state == MUS_VM_ERROR) {
			const char *msg = mus_vm_last_error(_vm);
			UtilityFunctions::push_warning("MusicDirector: VM error: ", String(msg ? msg : ""));
		}
	}
}

void MusicDirector::_exit_tree() {
	stop();
	// AudioStreamPlayer children are owned by the Node tree and freed
	// automatically; just drop our pointers so the next _ready can rebuild.
	_players.clear();
	_next_player = 0;
	_active_play = nullptr;
}

// --- Methods -----------------------------------------------------------

void MusicDirector::start() {
	if (_vm == nullptr || _script.is_null()) {
		return;
	}
	String name = _script_name == StringName()
			? _script->get_default_script_name()
			: String(_script_name);
	const MusScript *raw = _script->raw_script(name);
	if (raw == nullptr) {
		return;
	}
	if (mus_vm_load_script(_vm, raw) != 0) {
		const char *msg = mus_vm_last_error(_vm);
		UtilityFunctions::push_warning("MusicDirector: ", String(msg ? msg : "mus_vm_load_script failed"));
		return;
	}

	MusVMHooks hooks = {};
	hooks.user = this;
	hooks.on_play_sound = _on_play_sound;
	hooks.on_section_entered = _on_section_entered;
	hooks.on_echo = _on_echo;
	mus_vm_set_hooks(_vm, &hooks);

	_active_play = nullptr; // fresh context: first _process advances the VM to the first play
	mus_vm_start(_vm);
	_vm_running = (mus_vm_state(_vm) == MUS_VM_RUNNING);
}

void MusicDirector::stop() {
	if (_vm) {
		mus_vm_stop(_vm);
	}
	_vm_running = false;
	for (int i = 0; i < _players.size(); ++i) {
		if (_players[i]) {
			_players[i]->stop();
			// A stopped player still owns its AudioStream (and therefore the old
			// SBF bank). Drop it so a context teardown actually releases the pair.
			_players[i]->set_stream(Ref<AudioStream>());
		}
	}
	_active_play = nullptr;
}

bool MusicDirector::has_pending_playback() const {
	for (int i = 0; i < _playbacks.size(); ++i) {
		if (ObjectDB::get_instance(_playbacks[i]) != nullptr) {
			return true;
		}
	}
	return false;
}

void MusicDirector::pause() {
	if (_vm) {
		mus_vm_pause(_vm);
	}
}

void MusicDirector::resume() {
	if (_vm) {
		mus_vm_resume(_vm);
		_vm_running = (mus_vm_state(_vm) == MUS_VM_RUNNING);
	}
}

void MusicDirector::jump_to_section(const StringName &p_section_name) {
	if (_vm == nullptr) {
		return;
	}
	mus_vm_jump_to_section(_vm, String(p_section_name).utf8().get_data());
}

int MusicDirector::get_var(int p_var_index) const {
	if (_vm == nullptr) {
		return 0;
	}
	return (int)mus_vm_get_var(_vm, (uint8_t)p_var_index);
}

void MusicDirector::set_var(int p_var_index, int p_value) {
	if (_vm == nullptr) {
		return;
	}
	mus_vm_set_var(_vm, (uint8_t)p_var_index, (int32_t)p_value);
}

int MusicDirector::vm_state() const {
	if (_vm == nullptr) {
		return (int)MUS_VM_STOPPED;
	}
	return (int)mus_vm_state(_vm);
}

String MusicDirector::last_error() const {
	if (_vm == nullptr) {
		return String();
	}
	const char *s = mus_vm_last_error(_vm);
	return String(s ? s : "");
}

// --- Hook trampolines --------------------------------------------------

void MusicDirector::_on_play_sound(void *user, uint32_t sbf_entry_index, int wait) {
	MusicDirector *self = (MusicDirector *)user;
	if (self == nullptr) {
		return;
	}

	if (self->_bank.is_valid()) {
		Ref<SbfAudioStream> stream = self->_bank->get_stream_at((int)sbf_entry_index);
		if (stream.is_valid() && !self->_players.is_empty()) {
			AudioStreamPlayer *p = self->_players[self->_next_player % self->_players.size()];
			self->_next_player++;
			if (p != nullptr) {
				p->set_stream(stream);
				p->play();
				Ref<AudioStreamPlayback> playback = p->get_stream_playback();
				if (playback.is_valid()) {
					self->_playbacks.push_back(ObjectID(playback->get_instance_id()));
				}
				// Gate the VM on this track finishing (see _process). The original
				// streams exactly one music context at a time (audio_stream_update),
				// so the most-recently started player is THE active track.
				self->_active_play = p;
			}
		}
	}
}

void MusicDirector::_on_section_entered(void *user, const char *name) {
	MusicDirector *self = (MusicDirector *)user;
	if (self == nullptr) {
		return;
	}
	self->emit_signal("section_entered", StringName(name ? name : ""));
}


void MusicDirector::_on_echo(void *user, int32_t arg) {
	MusicDirector *self = (MusicDirector *)user;
	if (self == nullptr) {
		return;
	}
	self->emit_signal("echo", (int)arg);
}
