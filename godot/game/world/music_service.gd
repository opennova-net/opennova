extends Node

## The one interactive-music context (autoload "MusicService"). The original
## engine streams exactly ONE music context at a time — a (bank .sbf, script
## .bin) pair loaded into the AudioVM; switching music is a full context reload
## [orig: AudioVM_OpenMusicContext @ 0x6722a0 tears down the prior VM;
## AudioVM_OpenContextFile @ 0x672160]. The menu context is opened once at boot
## by the front end [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60] and the
## game context at mission start [orig: Game_StartMission @ 0x525581-0x52561b] —
## so the menu shell and the game world both open THEIR context through this one
## service.
##
## Full driving witness: docs/audio/mus-sbf-re.md §Game music driving.

# The gamescript var slots (mission-start seed + per-frame pump) are the bound
# MusicDirector.GAME_* constants — the witnesses live at the engine home,
# engine/runtime/audio audio/music_policy.h (full per-index witness map:
# docs/audio/mus-sbf-re.md §Game music driving).

# The longest a quit waits for the mixer to release stopped playbacks; the
# WASAPI driver retries a lost endpoint on about this cadence, and a live
# mixer drains within a few frames.
const PLAYBACK_DRAIN_TIMEOUT_MSEC := 1000

# The service owns the one director (and thereby the AudioStreamPlayer pool).
var _director: MusicDirector = null
# "", "menu" or "game" — which context is loaded (ADR 0018 read seam for tests).
var _context := ""
# The loaded script of the current context (ADR 0018 read seam).
var _script: MusicScript = null


func _ready() -> void:
	_director = MusicDirector.new()
	_director.name = "MusicDirector"
	_director.auto_start = false
	add_child(_director)


func _exit_tree() -> void:
	stop_context()


func director() -> MusicDirector:
	return _director


func current_context() -> String:
	return _context


func current_script() -> MusicScript:
	return _script


## Resolve one interactive-music pair against a mounted root. The witnessed
## name derivation (base MENUMUS/GAMEMUS stems, expansion M<n>/G<n> forms and
## their expansion\<n> subdir) is the engine's —
## MusicDirector.resolve_menu_music_pair / resolve_game_music_pair; the
## witness lives at the engine home, engine/runtime/audio
## audio/music_policy.h. This seam keeps only the filesystem orchestration:
## retail's ONLY reselect is the expansion .pff existence check — a missing
## expansion\<n>\<n>.pff clears the expansion and reselects the base names
## [orig: File_CheckExists @ 0x4a4767, clear @ 0x4a4775]; once the .pff
## exists the expansion names are set unconditionally, and the context open
## then bails when the loose .sbf is absent (the CreateFileA gate precedes
## the script load [orig: AudioVM_OpenContextFile @ 0x672160]), so an
## expansion that ships partial or no music is SILENT in retail. Bank and
## script always come from the SAME stem (the script's play ops index that
## bank's entries), so halves are never mixed — the MusicPair typed record
## carries the two halves (ADR 0017).
# `root` stays untyped on this seam: menu_shell_test's _MissingBankMusicRoot double
# observes the script-read count behind the bank gate, which a real
# ResourceRoot cannot report.
static func resolve_menu_music_pair(root) -> MusicPair:
	if root == null:
		return MusicPair.new()
	return _pair_from_names(root,
			MusicDirector.resolve_menu_music_pair(String(root.get_expansion())))


static func resolve_game_music_pair(root) -> MusicPair:
	if root == null:
		return MusicPair.new()
	return _pair_from_names(root,
			MusicDirector.resolve_game_music_pair(String(root.get_expansion())))


static func _pair_from_names(root, names: MusicPairNames) -> MusicPair:
	var pair := MusicPair.new()
	var bank_file := names.bank_file
	var script_file := names.script_file
	var subdir := names.subdir
	if not subdir.is_empty():
		# The expansion bank lives inside the expansion folder, streamed loose
		# (the subdir witness lives at the engine home, audio/music_policy.h).
		# Resolve its spelling case-insensitively, as retail did on Windows. Keep
		# the actual on-disk spelling for case-sensitive filesystems, and poison an
		# ambiguous duplicate instead of choosing by enumeration order.
		var expansion_dir: String = root.get_root_dir().path_join(subdir)
		var bank_path := _resolve_loose_file(expansion_dir, bank_file)
		# Retail writes both expansion names immediately after the expansion PFF
		# exists-check. Preserve the actual spelling when the bank exists, but keep
		# the canonical missing path otherwise so the bank-first open fails silent.
		pair.bank = bank_path if not bank_path.is_empty() else expansion_dir.path_join(bank_file)
		pair.script_name = script_file
		return pair
	pair.bank = String(root.resolve_file(bank_file))
	pair.script_name = script_file if root.has_file(script_file) else ""
	return pair


static func _resolve_loose_file(dir_path: String, filename: String) -> String:
	if dir_path.is_empty() or filename.is_empty():
		return ""
	var resolved_path := ""
	var wanted := filename.to_lower()
	for entry in DirAccess.get_files_at(dir_path):
		var actual := String(entry)
		if actual.to_lower() != wanted:
			continue
		if not resolved_path.is_empty():
			return ""  # duplicate case variants are ambiguous
		resolved_path = dir_path.path_join(actual)
	return resolved_path


## Open the MENU music context [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60:
## open the M pair + set volume, NO initial var — the shown screen's MUSICVAR
## selects the section afterwards]. Explicit overrides keep the shell's dev
## seams (an explicit script loads by loose path via ResourceLoader).
## Returns true when the context is loaded and the VM has been freshly started.
# `root` stays untyped on this chain (resolve_*_pair / _open_context /
# _load_music_script): menu_shell_test drives it with a RefCounted double whose
# has_file/read_file cannot override the native ResourceRoot's.
func open_menu_context(root, script_override := "", bank_override := "") -> bool:
	var pair := resolve_menu_music_pair(root)
	var bank_path := pair.bank
	if not bank_override.is_empty() and root != null:
		bank_path = String(root.resolve_file(bank_override))  # explicit override wins
	return _open_context("menu", root, bank_path, pair.script_name, script_override)


## Open the GAME music context at mission start for a session with a local
## client. Retail SP is host+client (connection mode 3); dedicated hosts alone
## stop the context [orig: Game_StartMission @0x525581..0x5255AE;
## CGameSession_SetConnectionMode @0x4C49F0; D-MUS-SPGATE witness correction].
## GameWorld opens before the initial WAC compile, whose M# operands bind the
## context's globals. The witnessed variable seeding follows the open:
## GAME_SEEDED_VAR_FIRST..LAST zeroed except GAME_VAR_HEALTH_PCT =
## GAME_HEALTH_SEED (GAME_VAR_MISSION_STATE stays 0 — never written by retail,
## so gamemus loops its Multiplayerstart P0 track); the slot/seed witnesses
## live at the engine home, engine/runtime/audio audio/music_policy.h.
func open_game_context(root) -> bool:
	var pair := resolve_game_music_pair(root)
	var opened := _open_context("game", root, pair.bank, pair.script_name, "")
	if opened:
		for idx in range(MusicDirector.GAME_SEEDED_VAR_FIRST,
				MusicDirector.GAME_SEEDED_VAR_LAST + 1):
			_director.set_var(idx, MusicDirector.GAME_HEALTH_SEED
					if idx == MusicDirector.GAME_VAR_HEALTH_PCT else 0)
	return opened


## Tear down the streaming context [orig: AudioVM_StopMusicContext @ 0x671e00].
## The loaded script/bank refs drop so the next open is a full context reload.
func stop_context() -> void:
	if _director != null:
		_director.stop()
		_director.set_bank(null)
		_director.load_mus_script(null)
		_director.set_script_name(&"")
	_context = ""
	_script = null


## Godot releases stopped streaming playbacks on a later audio/main-thread pass.
## Orderly application shutdown must keep that pump alive until the extension's
## playback objects are gone; stopping the VM alone does not release them.
## The wait is bounded: AudioServer erases a stopped playback only inside its
## mix step, and a WASAPI driver that lost its endpoint (or ended its thread
## on a buffer error) never mixes again, so an unbounded wait would hold the
## quit forever. A healthy mixer drains in a few frames; after
## `timeout_msec` the caller quits with live playback (the forced-exit
## ordering tracked in docs/world/npc-mission-completion.md "Runtime
## shutdown"; docs/audio/mus-sbf-re.md "Godot playback shutdown"). Returns
## false on that timeout.
func await_playback_stopped(timeout_msec: int = PLAYBACK_DRAIN_TIMEOUT_MSEC) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_msec
	while is_instance_valid(_director) and _director.has_pending_playback():
		if Time.get_ticks_msec() >= deadline:
			push_warning("MusicService: playback drain timed out; quitting with live playback (forced-exit ordering, see mus-sbf-re.md)")
			return false
		await get_tree().process_frame
	return true


func set_var(idx: int, value: int) -> void:
	if _director != null:
		_director.set_var(idx, value)


func get_var(idx: int) -> int:
	return _director.get_var(idx) if _director != null else 0


# --- Internals ------------------------------------------------------------

# Bank + script swap together: the witnessed context open takes the pair and
# bails before loading the script when the bank is missing [orig:
# AudioVM_OpenContextFile @ 0x672160 — the .sbf CreateFileA gate precedes the
# script load], so a missing half means silence (non-fatal), never a mixed pair.
func _open_context(name: String, root, bank_path: String, script_name: String, script_override: String) -> bool:
	stop_context()
	if _director == null:
		return false
	var bank := _load_bank(bank_path)
	if bank == null:
		return false  # retail's CreateFileA gate precedes script loading
	var script := _load_music_script(root, script_override, script_name)
	if script == null:
		return false  # incomplete pair -> silence, matching the witnessed bail
	_context = name
	_script = script
	_director.set_bank(bank)
	_director.load_mus_script(script)
	# start() performs mus_vm_load_script(), which is the full-context reset.
	# The dummy audio driver supports the same lifecycle in headless tests.
	_director.start()
	return true


# The .sbf bank streams loose from disk by path [orig: AudioVM_OpenContextFile
# @ 0x672160 CreateFileA; Sbf_OpenFile_Gamemus @ 0x4ed6c0].
func _load_bank(path: String) -> SbfBank:
	if path.is_empty():
		return null
	var b := SbfBank.new()
	b.load_from_path(path)
	return b if b.get_entry_count() > 0 else null


# The .bin script loads as bytes through the VFS so it resolves from PFF
# archives [orig: AudioVM_LoadScriptFile @ 0x672d20]. root.read_file already
# applies the shared SCR/BFC1 payload decode, so the bytes arrive in the
# decrypted SCR0 form load_from_decrypted_bytes expects. An explicit override
# takes the loose-path route (load_from_path runs the payload decode itself).
func _load_music_script(root, explicit: String, name: String) -> MusicScript:
	if not explicit.is_empty() and root != null:
		var path := String(root.resolve_file(explicit))
		if path.is_empty():
			return null
		var res := MusicScript.new()
		if res.load_from_path(path) != OK:
			return null
		return res
	if root == null or name.is_empty():
		return null
	var bytes: PackedByteArray = root.read_file(name)
	if bytes.is_empty():
		return null
	var script := MusicScript.new()
	script.load_from_decrypted_bytes(bytes, name)
	return script if script.get_script_count() > 0 else null
