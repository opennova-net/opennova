#pragma once

// MusicDirector embeds a engine/formats/mus VM and the engine's music stream pump
// (runtime/audio/music_stream_pump.h), which paces the VM, and is the pump's device: a
// small AudioStreamPlayer pool streaming the one music track each play / playw selects,
// the section and echo notices as Godot signals.
//
// The engine port (engine/formats/mus mus_vm.cpp, which carries the witnesses) loads the
// script and fires the play / playw opcodes' sound triggers; the pump steps it.

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <runtime/audio/music_policy.h>
#include <runtime/audio/music_stream_pump.h>

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
	// The music context open over the pair the shell read (engine mus_vm_open_context
	// carries the gate and its witness): false, no context and neither half kept, at
	// its bails; a pass keeps the pair and runs the script.
	bool open_context(const Ref<SbfBank> &p_bank, const Ref<MusicScript> &p_script);
	void start();
	void stop();
	// stop() requests the mixer's fade-out. This stays true until all started
	// playbacks (including retired pool entries) have actually been released.
	bool has_pending_playback() const;
	void pause();
	void resume();
	void jump_to_section(const StringName &p_section_name);
	std::shared_ptr<opennova::mus::MusGlobals> globals_for_wac() const {
        return opennova::mus::mus_vm_globals(_vm);
    }
	int get_var(int p_var_index) const;
	void set_var(int p_var_index, int p_value);
	int vm_state() const;
	// The loaded script's MessageHandler restart frame (engine mus_vm_signal:
	// the step MusicCtx_SelectEndTrack runs at once): the SP round-end tail
	// hands the end track here (1 win / 2 lose; retail gamemus.bin dispatches
	// them to Missionwin / Missionlose). Runs on any loaded script regardless
	// of the streaming state; the handler's setstate lands at once, its GFB
	// empties the stream, and the next frame steps to the sting's first entry,
	// which cuts the sounding track. Returns 0 when the
	// frame ran, -1 with no script, -2 when the script carries no handler (the
	// signal dropped, the plain step run).
	int signal_end_track(int p_value);

	// Engine callbacks (Node virtuals via godot-cpp)
	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods();

private:
	// The pump's device: the player pool streaming the track a play selects, and
	// the section and echo notices as signals.
	class StreamDevice final : public opennova::audio::MusicStreamDevice {
	public:
		explicit StreamDevice(MusicDirector &p_owner) : _owner(p_owner) {}
		void start_track(uint32_t p_entry) override;
		bool track_sounding() const override;
		void section_entered(const char *p_name) override;
		void echo(int32_t p_value) override;

	private:
		MusicDirector &_owner;
	};

	// The script start() and open_context() bind: the named one, else the file's default.
	const opennova::mus::MusScript *_raw_script(const Ref<MusicScript> &p_script) const;

	Ref<MusicScript> _script;
	Ref<SbfBank> _bank;
	StringName _script_name;
	bool _auto_start = true;
	int _player_pool_size = 2;
	StringName _audio_bus = StringName("Music");

	opennova::mus::MusVM *_vm = nullptr;
	StreamDevice _device{ *this };
	opennova::audio::MusicStreamPump _pump;
	Vector<AudioStreamPlayer *> _players;
	// IDs observe AudioServer ownership without keeping a stopped stream alive.
	Vector<ObjectID> _playbacks;
	int _next_player = 0;
	// The player streaming the track the VM's last `play` started: the one music
	// stream, whose sounding the pump reads (track_sounding).
	AudioStreamPlayer *_active_play = nullptr;
};

} // namespace godot

