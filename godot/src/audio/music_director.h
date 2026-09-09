#pragma once

// MusicDirector embeds a engine/formats/mus VM and routes its hooks to Godot
// signals. Owns a small AudioStreamPlayer pool for marker playback driven
// by the play / playw opcodes.
//
// Witnessed wire-level paths (engine/formats/mus): Jointops.exe!AudioVM_LoadScriptFile @
// 0x00672D20 (script load), Jointops.exe!AudioVM_Op_Play @ 0x672CB0 + AudioVM_Op_PlayWait @
// 0x672C90 (sound triggers), Jointops.exe!Intrinsic_GSV / GSDV (volume).

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <runtime/audio/music_policy.h>

#include <formats/mus/mus.h>

namespace godot {

class MusicPairNames;

class MusicScript;
class SbfBank;

class MusicDirector : public Node {
	GDCLASS(MusicDirector, Node)

public:
	// Witnessed music-driving policy re-exports (engine audio/music_policy.h
	// is the value authority and carries the [orig] witnesses). Bound as
	// class constants for the shell/service GDScript.
	enum {
		// menuscript: the shown screen's MUSICVAR writes this var slot.
		MENU_MUSIC_VAR_SLOT = opennova::audio::kMenuMusicVarSlot,
		// gamescript: mission-start seeds Var<FIRST>..Var<LAST>; Var1 is the
		// (never-written) mission-state seed, Var7 health % (seeded
		// GAME_HEALTH_SEED), Var10 the local-player team.
		GAME_SEEDED_VAR_FIRST = opennova::audio::kGameMusicSeededVarFirst,
		GAME_SEEDED_VAR_LAST = opennova::audio::kGameMusicSeededVarLast,
		GAME_VAR_MISSION_STATE = opennova::audio::kGameMusicMissionStateVarSlot,
		GAME_VAR_HEALTH_PCT = opennova::audio::kGameMusicHealthPctVarSlot,
		GAME_VAR_TEAM = opennova::audio::kGameMusicTeamVarSlot,
		GAME_HEALTH_SEED = opennova::audio::kGameMusicHealthSeed,
	};

	// Pure name resolution for the witnessed music pairs (no filesystem), as
	// a MusicPairNames record (audio/music_pair_names.h). The service keeps
	// the on-disk case probing / VFS resolution; these only name the pair
	// (engine music_policy.h owns the scheme + witnesses).
	static Ref<MusicPairNames> resolve_menu_music_pair(const String &p_expansion_name);
	static Ref<MusicPairNames> resolve_game_music_pair(const String &p_expansion_name);

	MusicDirector();
	~MusicDirector();

	// Properties / methods. We use load_mus_script (not set_script / set_mus_script)
	// to escape Object.set_script collision: GDScript dispatches set_script to the
	// builtin Object method when our override exists, dropping the assignment on
	// the floor. The plain `script` property is removed entirely; the smoke scene
	// and the editor call load_mus_script() directly.
	void load_mus_script(const Ref<MusicScript> &p_script);
	Ref<MusicScript> get_mus_script() const;
	void set_bank(const Ref<SbfBank> &p_bank);
	Ref<SbfBank> get_bank() const;
	void set_script_name(const StringName &p_name);
	StringName get_script_name() const;
	void set_auto_start(bool p_auto);
	bool get_auto_start() const;

	// Methods
	void start();
	void stop();
	// stop() requests the mixer's fade-out. This stays true until all started
	// playbacks (including retired pool entries) have actually been released.
	bool has_pending_playback() const;
	void pause();
	void resume();
	void jump_to_section(const StringName &p_section_name);
	int get_var(int p_var_index) const;
	void set_var(int p_var_index, int p_value);
	int vm_state() const;
	String last_error() const;

	// Engine callbacks (Node virtuals via godot-cpp)
	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods();

private:
	// Hook trampolines (per Phase A spec): translate raw VM args into
	// signal emissions. Always called with `user` == MusicDirector*.
	static void _on_play_sound(void *user, uint32_t sbf_entry_index, int wait);
	static void _on_section_entered(void *user, const char *name);
	static void _on_echo(void *user, int32_t arg);

	Ref<MusicScript> _script;
	Ref<SbfBank> _bank;
	StringName _script_name;
	bool _auto_start = true;
	int _player_pool_size = 2;
	StringName _audio_bus = StringName("Music");

	opennova::mus::MusVM *_vm = nullptr;
	Vector<AudioStreamPlayer *> _players;
	// IDs observe AudioServer ownership without keeping a stopped stream alive.
	Vector<ObjectID> _playbacks;
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

