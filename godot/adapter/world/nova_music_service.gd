extends Node

## The one interactive-music context (autoload "NovaMusicService"). The original
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

# gamescript var indices the mission-start seed and the per-frame pump write
# (full per-index witness map: docs/audio/mus-sbf-re.md §Game music driving).
const VAR_HEALTH_PCT := 7  # health % [orig: seed @ 0x5255f0; per-frame @ 0x4b6324]
const VAR_TEAM := 10       # local-player team [orig: @ 0x4b62fc]
const SEEDED_VARS_FIRST := 1   # Var1..Var12 seeded at mission start
const SEEDED_VARS_LAST := 12   # [orig: @ 0x5255b3-0x52561b]

# The service owns the one director (and thereby the AudioStreamPlayer pool).
var _director: NovaMusicDirector = null
# "", "menu" or "game" — which context is loaded (ADR 0018 read seam for tests).
var _context := ""
# The loaded script of the current context (ADR 0018 read seam).
var _script: NovaMusicScript = null


func _ready() -> void:
	_director = NovaMusicDirector.new()
	_director.name = "MusicDirector"
	_director.auto_start = false
	add_child(_director)


func director() -> NovaMusicDirector:
	return _director


func current_context() -> String:
	return _context


func current_script() -> NovaMusicScript:
	return _script


## Resolve one interactive-music pair by its witnessed hardcoded stem against a
## mounted root. The engine names the base pairs MENUMUS.SBF/.BIN and
## GAMEMUS.SBF/.BIN; when expansion <n> is active they become
## expansion\<n>\M<n>.sbf + M<n>.bin (menu) and expansion\<n>\G<n>.sbf +
## G<n>.bin (game) [orig: Expansion_LoadAssets @ 0x4a4798 (base names) /
## @ 0x4a4906-0x4a494a (expansion forms)]. Retail's ONLY reselect is the
## expansion .pff existence check — a missing expansion\<n>\<n>.pff clears the
## expansion and reselects the base names [orig: File_CheckExists @ 0x4a4767,
## clear @ 0x4a4775]; once the .pff exists the expansion names are set
## unconditionally, and the context open then bails when the loose .sbf is
## absent (the CreateFileA gate precedes the script load [orig:
## AudioVM_OpenContextFile @ 0x672160]), so an expansion that ships partial or
## no music is SILENT in retail. Bank and script always come from the SAME stem
## (the script's play ops index that bank's entries), so halves are never mixed
## — the MusicPair typed record carries the two halves (ADR 0017).
static func resolve_music_pair(root, prefix: String, base_stem: String) -> MusicPair:
	var pair := MusicPair.new()
	if root == null:
		return pair
	var exp_name: String = root.get_expansion()
	if not exp_name.is_empty():
		var stem := prefix + exp_name
		# The expansion bank lives inside the expansion folder, streamed loose
		# [orig: "expansion\\%s\\M%s.sbf" @ 0x4a4906 / "expansion\\%s\\G%s.sbf" @ 0x4a4936].
		# Resolve its spelling case-insensitively, as retail did on Windows. Keep
		# the actual on-disk spelling for case-sensitive filesystems, and poison an
		# ambiguous duplicate instead of choosing by enumeration order.
		var expansion_dir: String = root.get_root_dir().path_join("expansion").path_join(exp_name)
		var bank_path := _resolve_loose_file(expansion_dir, stem + ".sbf")
		# Retail writes both expansion names immediately after the expansion PFF
		# exists-check. Preserve the actual spelling when the bank exists, but keep
		# the canonical missing path otherwise so the bank-first open fails silent.
		pair.bank = bank_path if not bank_path.is_empty() else expansion_dir.path_join(stem + ".sbf")
		pair.script_name = stem + ".bin"
		return pair
	pair.bank = String(root.resolve_file(base_stem + ".sbf"))
	pair.script_name = base_stem + ".bin" if root.has_file(base_stem + ".bin") else ""
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
func open_menu_context(root, script_override := "", bank_override := "") -> bool:
	var pair := resolve_music_pair(root, "M", "menumus")
	var bank_path := pair.bank
	if not bank_override.is_empty() and root != null:
		bank_path = String(root.resolve_file(bank_override))  # explicit override wins
	return _open_context("menu", root, bank_path, pair.script_name, script_override)


## Open the GAME music context at mission start. The original opens it only for
## MP session peers and STOPS the context in single-player [orig:
## Game_StartMission @ 0x525581: is_mp_session_peer -> AudioVM_OpenMusicContext
## (g_path_game_sbf/bin), else AudioVM_StopMusicContext @ 0x671e00]; ours opens
## it in ALL sessions — D-MUS-SPGATE, maintainer decision 2026-07-09 (our SP
## runs as a listen server, ADR 0009/0011/0012). The witnessed var seeding runs
## after the open exactly as retail's mission start does on both branches
## [orig: @ 0x5255b3-0x52561b]: Var1 = g_music_mission_state_seed (never
## written -> always 0; gamemus loops its Multiplayerstart P0 track),
## Var2..Var6 = 0, Var7 = 100 (health %), Var8..Var12 = 0.
func open_game_context(root) -> bool:
	var pair := resolve_music_pair(root, "G", "gamemus")
	var opened := _open_context("game", root, pair.bank, pair.script_name, "")
	if opened:
		for idx in range(SEEDED_VARS_FIRST, SEEDED_VARS_LAST + 1):
			_director.set_var(idx, 100 if idx == VAR_HEALTH_PCT else 0)
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
func _load_bank(path: String) -> NovaSbfBank:
	if path.is_empty():
		return null
	var b := NovaSbfBank.new()
	b.load_from_path(path)
	return b if b.get_entry_count() > 0 else null


# The .bin script loads as bytes through the VFS so it resolves from PFF
# archives [orig: AudioVM_LoadScriptFile @ 0x672d20]. root.read_file already
# applies the shared SCR/BFC1 payload decode, so the bytes arrive in the
# decrypted SCR0 form load_from_decrypted_bytes expects. An explicit override
# keeps the loose-path ResourceLoader route (the registered loader strips the
# SCR layer).
func _load_music_script(root, explicit: String, name: String) -> NovaMusicScript:
	if not explicit.is_empty() and root != null:
		var path := String(root.resolve_file(explicit))
		if path.is_empty():
			return null
		var res = ResourceLoader.load(path, "NovaMusicScript")
		return res as NovaMusicScript
	if root == null or name.is_empty():
		return null
	var bytes: PackedByteArray = root.read_file(name)
	if bytes.is_empty():
		return null
	var script := NovaMusicScript.new()
	script.load_from_decrypted_bytes(bytes, name)
	return script if script.get_script_count() > 0 else null
