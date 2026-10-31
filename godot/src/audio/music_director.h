#pragma once

// MusicDirector embeds a engine/formats/mus VM and the engine's music stream pump
// (runtime/audio/music_stream_pump.h), which paces the VM, over the engine's music stream
// (runtime/audio/music_stream.h), which streams the track each play / playw selects onto the
// mixer's music channels; the section and echo notices are Godot signals.
//
// The engine port (engine/formats/mus mus_vm.cpp, which carries the witnesses) loads the
// script and fires the play / playw opcodes' sound triggers; the pump steps it.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <runtime/audio/music_context.h>
#include <runtime/audio/music_policy.h>
#include <runtime/audio/music_stream.h>
#include <runtime/audio/music_stream_pump.h>

#include <formats/mus/mus.h>

namespace godot {

class MusicPair;
class MusicScript;
class ResourceRoot;
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

	// The pair a menu / game context opens over the mounted `p_root` (engine
	// runtime/audio/music_context.h resolve_music_pair, which carries the rule and its
	// witnesses); an empty pair with no root.
	static Ref<MusicPair> resolve_menu_music_pair(const Ref<ResourceRoot> &p_root);
	static Ref<MusicPair> resolve_game_music_pair(const Ref<ResourceRoot> &p_root);

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

	// The one music context (engine runtime/audio/music_context.h MusicContext: the prior
	// context's teardown, the bank's file before the script, the VM's open, the game context's
	// seed): the menu context over `p_root` (a non-empty override names a loose script or bank
	// the root resolves, the dev seams), the game context at a mission start, the teardown,
	// and which context runs ("", "menu", "game") with the script it loaded.
	bool open_menu_context(const Ref<ResourceRoot> &p_root, const String &p_script_override = String(),
			const String &p_bank_override = String());
	bool open_game_context(const Ref<ResourceRoot> &p_root);
	void stop_context();
	String get_context() const;
	Ref<MusicScript> get_context_script() const;

	// Methods
	// The music context open over the pair the shell read (engine mus_vm_open_context
	// carries the gate and its witness): false, no context and neither half kept, at
	// its bails; a pass keeps the pair and runs the script.
	bool open_context(const Ref<SbfBank> &p_bank, const Ref<MusicScript> &p_script);
	void start();
	void stop();
	// Whether a track streams: stop() ends it at once, the music channels flushed.
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
	// The pump's device: the engine's music stream, the section and echo notices as signals.
	class StreamDevice final : public opennova::audio::MusicStreamDevice {
	public:
		explicit StreamDevice(MusicDirector &p_owner) : _owner(p_owner) {}
		void start_track(uint32_t p_entry) override { _owner._stream.start_track(p_entry); }
		void flush() override { _owner._stream.flush(); }
		bool streaming() const override { return _owner._stream.streaming(); }
		bool queue_full() const override { return _owner._stream.queue_full(); }
		void stream_block() override { _owner._stream.stream_block(); }
		void volume_changed(int32_t p_current, int32_t p_target) override {
			_owner._stream.volume_changed(p_current, p_target);
		}
		void section_entered(const char *p_name) override;
		void echo(int32_t p_value) override;

	private:
		MusicDirector &_owner;
	};
	// The stream bound to `_bank`'s bytes, at the music option's global volume
	// (engine audio::MusicStream::open).
	void _open_stream();

	// The context's player: the director's teardown, the two halves it loads for the open, the
	// VM's open and the seed's writes.
	class ContextPlayer final : public opennova::audio::MusicContextPlayer {
	public:
		explicit ContextPlayer(MusicDirector &p_owner) : _owner(p_owner) {}
		void stop_context() override;
		bool load_bank(const std::string &p_path) override;
		bool load_script_file(const std::string &p_path) override;
		int load_script_bytes(const std::vector<uint8_t> &p_bytes, const std::string &p_name) override;
		void drop_script() override;
		bool open_vm() override;
		void set_var(int p_index, int32_t p_value) override;

	private:
		MusicDirector &_owner;
		Ref<SbfBank> _bank;
		Ref<MusicScript> _script;
	};

	bool _open(opennova::audio::MusicContextKind p_kind, const Ref<ResourceRoot> &p_root,
			const String &p_script_override, const String &p_bank_override);

	// The script start() and open_context() bind: the named one, else the file's default.
	const opennova::mus::MusScript *_raw_script(const Ref<MusicScript> &p_script) const;

	Ref<MusicScript> _script;
	Ref<SbfBank> _bank;
	StringName _script_name;
	bool _auto_start = true;

	opennova::mus::MusVM *_vm = nullptr;
	opennova::audio::MusicStream _stream;
	StreamDevice _device{ *this };
	opennova::audio::MusicStreamPump _pump;
	ContextPlayer _context_player{ *this };
	opennova::audio::MusicContext _context;
};

} // namespace godot

