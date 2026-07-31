#ifndef NOVA_MUSIC_DIRECTOR_H
#define NOVA_MUSIC_DIRECTOR_H

// NovaMusicDirector embeds a libs/mus VM and routes its hooks to Godot
// signals. Owns a small AudioStreamPlayer pool for marker playback driven
// by the play / playw opcodes.
//
// Witnessed wire-level paths (libs/mus): Jointops.exe!AudioVM_LoadScriptFile @
// 0x00672D20 (script load), Jointops.exe!VmOp_Play @ 0x672CB0 + VmOp_PlayWait @
// 0x672C90 (sound triggers), Jointops.exe!Intrinsic_GSV / GSDV (volume).

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "mus/mus.h"

namespace godot {

class NovaMusicScript;
class NovaSbfBank;

class NovaMusicDirector : public Node {
	GDCLASS(NovaMusicDirector, Node)

public:
	NovaMusicDirector();
	~NovaMusicDirector();

	// Properties / methods. We use load_mus_script (not set_script / set_mus_script)
	// to escape Object.set_script collision: GDScript dispatches set_script to the
	// builtin Object method when our override exists, dropping the assignment on
	// the floor. The plain `script` property is removed entirely; the smoke scene
	// and the editor call load_mus_script() directly.
	void load_mus_script(const Ref<NovaMusicScript> &p_script);
	Ref<NovaMusicScript> get_mus_script() const;
	void set_bank(const Ref<NovaSbfBank> &p_bank);
	Ref<NovaSbfBank> get_bank() const;
	void set_script_name(const StringName &p_name);
	StringName get_script_name() const;
	void set_auto_start(bool p_auto);
	bool get_auto_start() const;
	void set_player_pool_size(int p_size);
	int get_player_pool_size() const;
	void set_audio_bus(const StringName &p_bus);
	StringName get_audio_bus() const;

	// Methods
	void start();
	void stop();
	void pause();
	void resume();
	void jump_to_section(const StringName &p_section_name);
	int get_var(int p_var_index) const;
	void set_var(int p_var_index, int p_value);
	StringName current_section() const;
	int vm_state() const;
	String last_error() const;
	// Current VM program counter (bytecode offset of the next opcode), or -1 when
	// no VM/script is live. Drives the editor's live statement highlight: a
	// statement whose [code_offset, code_offset+byte_size) contains this pc is the
	// one about to execute. Witnessed: Jointops.exe!AudioVM_DispatchLoop esi=IP.
	int current_pc() const;

	// Engine callbacks (Node virtuals via godot-cpp)
	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods();

private:
	// Hook trampolines (per Phase A spec): translate raw VM args into
	// signal emissions. Always called with `user` == NovaMusicDirector*.
	static void _on_play_sound(void *user, uint32_t sbf_entry_index, int wait);
	static void _on_section_entered(void *user, const char *name);
	static void _on_var_changed(void *user, uint8_t idx, int32_t v);
	static void _on_volume_changed(void *user, int32_t left_16_16, int32_t right_16_16);
	static void _on_echo(void *user, int32_t arg);

	Ref<NovaMusicScript> _script;
	Ref<NovaSbfBank> _bank;
	StringName _script_name;
	bool _auto_start = true;
	int _player_pool_size = 2;
	StringName _audio_bus = StringName("Music");

	MusVM *_vm = nullptr;
	Vector<AudioStreamPlayer *> _players;
	int _next_player = 0;
	bool _vm_running = false;
	// The player streaming the track the VM's last `play` started. The VM is only
	// advanced once this track finishes (matching the original's pacing in
	// audio_stream_update @0x671c60, which steps the script only when the stream's
	// remaining bytes reach 0). Without this the VM re-fires `play` every frame and
	// restarts the SBF stream ~60x/sec -> a low buzz.
	AudioStreamPlayer *_active_play = nullptr;
};

} // namespace godot

#endif // NOVA_MUSIC_DIRECTOR_H
