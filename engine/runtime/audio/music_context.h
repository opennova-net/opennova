#pragma once

// THE MUSIC CONTEXT: the one interactive-music context a process streams. Retail streams exactly
// one context at a time, a (bank .sbf, script .bin) pair loaded into the AudioVM, and switching
// music is a full context reload: the open tears the prior context down first
// [orig: AudioVM_OpenMusicContext @0x6722a0 -> AudioVM_OpenContextFile @0x672160, the teardown
// @0x672168..0x6721a3]. The front end opens the menu context once at its start
// [orig: AudioVM_InitMenuMusicStreaming @0x56aa60] and a mission start the game context
// [orig: Game_StartMission @0x525581..0x52561b], so the menu and the game world both open theirs
// here. The names are music_policy.h's; the stream is music_stream_pump.h's.
//
// The open reads the pair's two files in retail's order: the bank's loose file first, and no
// file there is no script read [orig: AudioVM_OpenContextFile @0x672160 - CreateFileA @0x6721da
// bails @0x6721df ahead of AudioVM_LoadScriptFile @0x6721f7], then the script through the
// resource system [orig: AudioVM_LoadScriptFile @0x672d20 - FileSystem_FileExists @0x672d44,
// File_LoadResource @0x672d5c], then the bank's SBF0 header (mus_vm_open_context carries that
// gate and the script's). A missing half is silence, never a mixed pair.
//
// Full driving witness: docs/audio/mus-sbf-re.md, "Game music driving".

#include <runtime/audio/music_policy.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::audio {

// Which context is open.
enum class MusicContextKind : uint8_t {
	None,
	Menu,
	Game,
};

// The context's name as the shells spell it ("", "menu", "game").
const char *music_context_name(MusicContextKind kind);

// One resolved pair: the bank's loose .sbf path and the script's resource name, always from the
// SAME stem (the script's play ops index that bank's entries). An empty half is unresolved; the
// open then bails to silence.
struct MusicPair {
	std::string bank;        // the loose .sbf path ("" = unresolved)
	std::string script_name; // the .bin's resource name ("" = unresolved)
};

// What the resolve and the open read of the mounted game (the embedder's resource root).
class MusicFiles {
public:
	virtual ~MusicFiles() = default;
	// The expansion the root actually mounted ("" for the base game). Retail's only reselect is
	// the expansion .pff's existence check, which the mount ran: a missing expansion\<n>\<n>.pff
	// cleared the expansion, so the base names are selected [orig: Expansion_LoadAssets -
	// File_CheckExists @0x4a4767, the clear @0x4a4775].
	virtual std::string expansion() const = 0;
	// The root's directory on disk, every loose path's base.
	virtual std::string root_dir() const = 0;
	// The loose file `name` in directory `dir` matched without case, as retail opens it on
	// Windows: its on-disk path, or "" for none.
	virtual std::string find_loose(const std::string &dir, const std::string &name) const = 0;
	// The root's loose file `name`: its path on disk, or "" for none.
	virtual std::string resolve_file(const std::string &name) const = 0;
	// Whether the resource system finds `name` (the archives, then the loose files).
	virtual bool has_file(const std::string &name) const = 0;
	// `name`'s bytes through the resource system, decoded (the SCR / BFC1 payload decode, so a
	// script arrives in its SCR0 form); false for none.
	virtual bool read_file(const std::string &name, std::vector<uint8_t> &out) const = 0;
};

// The pair a context of `kind` opens over `files` (Menu or Game; None resolves nothing). With an
// expansion mounted both names are its own unconditionally: the bank streams loose from the
// expansion's folder (its on-disk spelling when the file is there, the canonical path otherwise,
// so the bank-first open fails silent) and the script is the expansion stem's name whether or not
// the file exists, so an expansion that ships partial or no music is SILENT [orig:
// Expansion_LoadAssets @0x4a4906..0x4a494a, no probe of either file]. The base pair's bank is the
// root's loose file and its script the resource name when the resource system finds it.
MusicPair resolve_music_pair(MusicContextKind kind, const MusicFiles &files);

// The embedder's player of the one context: its director, which holds the halves it loaded and
// opens the VM over them.
class MusicContextPlayer {
public:
	virtual ~MusicContextPlayer() = default;
	// The context's teardown: the VM stopped, the stream emptied, both halves dropped
	// [orig: AudioVM_StopMusicContext @0x671e00].
	virtual void stop_context() = 0;
	// The bank's loose file at `path`, held for the open: false when no file is there (the open's
	// file leg: retail's CreateFileA). A file the reader refuses stays an unopened bank, which the
	// VM's open finds without a header.
	virtual bool load_bank(const std::string &path) = 0;
	// The script read from the loose file at `path` (a dev override): false when it does not load.
	virtual bool load_script_file(const std::string &path) = 0;
	// The script read from its decoded bytes: the scripts the file holds (0: none).
	virtual int load_script_bytes(const std::vector<uint8_t> &bytes, const std::string &name) = 0;
	// Drop the script a load above held (the open then has none).
	virtual void drop_script() = 0;
	// The VM's open over the held halves (mus_vm_open_context: the bank, the script, the SBF0
	// header; a bank of no entry opens): true when the context runs.
	virtual bool open_vm() = 0;
	// One gamescript variable's write (mus_vm_set_var: dropped with no context open).
	virtual void set_var(int index, int32_t value) = 0;
};

// The variable writes a mission start makes after its game context open, in order: Var1 the
// mission-state seed (g_MusicMissionStateSeed, never written by retail, so 0), Var2..Var6 0, Var7
// the health percent seed (100), Var8..Var12 0 [orig: Game_StartMission @0x5255b3..0x52561b].
// They run whether or not the open succeeded; the VM drops a write with no context open.
struct MusicVarSeed {
	int index;
	int32_t value;
};
std::vector<MusicVarSeed> game_music_seed();

class MusicContext {
public:
	MusicContextKind kind() const { return kind_; }

	// Open the context of `kind` over the pair `files` resolves (files null: no pair, so the open
	// fails to silence): the prior context's teardown, the bank's file, the script (an explicit
	// `script_override` by the root's loose path, else the pair's resource bytes, kept when they
	// hold a script), the VM's open; then, for the game context, the mission start's variable
	// seed. `bank_override` names a loose bank the root resolves in place of the pair's (a dev
	// seam). True when the context runs.
	bool open(MusicContextKind kind, const MusicFiles *files, MusicContextPlayer &player,
			const std::string &script_override = std::string(),
			const std::string &bank_override = std::string());
	// The teardown: no context until the next open.
	void stop(MusicContextPlayer &player);

private:
	MusicContextKind kind_ = MusicContextKind::None;
};

} // namespace opennova::audio
