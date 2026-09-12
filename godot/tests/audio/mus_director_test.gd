extends GutTest

const SCRIPT_FIXTURE := "res://../fixtures/mus/synth_gamemus.bin"
const BANK_FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"


func test_director_starts_and_stops() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(BANK_FIXTURE)
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	assert_not_null(script, "music script fixture loads")
	if script == null:
		return
	var dir := MusicDirector.new()
	add_child_autofree(dir)
	dir.bank = bank
	dir.load_mus_script(script)
	dir.auto_start = false
	dir.start()
	assert_eq(dir.vm_state(), 1, "RUNNING (=MUS_VM_RUNNING) after start")
	dir.stop()
	assert_eq(dir.vm_state(), 0, "STOPPED (=MUS_VM_STOPPED) after stop")


func test_director_emits_section_entered_on_jump() -> void:
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	assert_not_null(script, "music script fixture loads")
	if script == null:
		return
	var dir := MusicDirector.new()
	add_child_autofree(dir)
	dir.load_mus_script(script)
	dir.auto_start = false
	var got_signal := [false]
	dir.section_entered.connect(func(_n): got_signal[0] = true)
	dir.start()
	var sections := script.get_section_names(StringName(script.get_default_script_name()))
	if sections.size() > 1:
		dir.jump_to_section(StringName(sections[1]))
		assert_true(got_signal[0], "section_entered signal fires after jump_to_section")


func test_exit_tree_stops_the_music_vm() -> void:
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	var dir := MusicDirector.new()
	dir.auto_start = false
	add_child_autofree(dir)
	dir.load_mus_script(script)
	dir.start()
	assert_eq(dir.vm_state(), 1)
	remove_child(dir)
	assert_eq(dir.vm_state(), 0, "tree exit stops the VM before its players leave")


func test_service_playback_drain_is_bounded_and_reports_the_timeout() -> void:
	# The quit path's drain (MusicService.await_playback_stopped) must give up
	# after its bound instead of waiting on a mixer that never advances.
	var bank := SbfBank.new()
	bank.load_from_path(BANK_FIXTURE)
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	var dir: MusicDirector = MusicService.director()
	assert_not_null(dir, "the autoload owns the one director")
	if dir == null:
		return
	dir.bank = bank
	dir.load_mus_script(script)
	dir.start()
	await wait_until(dir.has_pending_playback, 2.0)
	assert_true(dir.has_pending_playback(), "the fixture reached real streaming playback")
	var drained: bool = await MusicService.await_playback_stopped(0)
	assert_false(drained, "an exhausted bound reports the timeout while playback is pending")
	assert_true(dir.has_pending_playback(), "the timeout leaves the playback to the mixer")
	MusicService.stop_context()
	drained = await MusicService.await_playback_stopped()
	assert_true(drained, "a live mixer releases the stopped playback inside the bound")
	assert_false(dir.has_pending_playback(), "the shutdown barrier clears after the drain")


func test_stopped_playback_releases_its_bank_after_the_mixer_drains() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(BANK_FIXTURE)
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	var dir := MusicDirector.new()
	dir.auto_start = false
	add_child_autofree(dir)
	dir.bank = bank
	dir.load_mus_script(script)
	dir.start()
	await wait_until(dir.has_pending_playback, 2.0)
	assert_true(dir.has_pending_playback(), "the fixture reached real streaming playback")
	var bank_ref: WeakRef = weakref(bank)
	dir.stop()
	dir.bank = null
	dir.load_mus_script(null)
	bank = null
	await wait_until(func(): return bank_ref.get_ref() == null, 2.0)
	assert_null(bank_ref.get_ref(), "a stopped stream releases the bank on the mixer cleanup pass")
	assert_false(dir.has_pending_playback(), "the shutdown barrier clears after playback destruction")


# The SP round-end tail's end track reaches the loaded script through the
# director's signal_end_track (the engine's mus_vm_signal: the MessageHandler
# restart frame MusicCtx_SelectEndTrack steps). The seam reports the engine's
# codes: -1 with no script loaded, -2 when the loaded script carries no handler
# (the compiled synth fixture: mus_compile emits none; retail gamemus.bin
# dispatches 1 -> Missionwin, 2 -> Missionlose, pinned by the mus_vm ctest),
# and the frame runs on a stopped VM as well as a running one.
func test_signal_end_track_reports_the_restart_frame_codes() -> void:
	var dir := MusicDirector.new()
	dir.auto_start = false
	add_child_autofree(dir)
	assert_eq(dir.signal_end_track(1), -1, "no script loaded: the frame is refused")
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	dir.load_mus_script(script)
	dir.start()
	assert_eq(dir.vm_state(), 1, "RUNNING after start")
	var sections: Array = []
	dir.section_entered.connect(func(name): sections.append(String(name)))
	assert_eq(dir.signal_end_track(2), -2, "the handler-less fixture refuses the frame")
	assert_eq(sections.size(), 0, "no handler, no section transition")
	assert_eq(dir.vm_state(), 1, "the refused frame leaves the VM running")
	dir.stop()
	assert_eq(dir.signal_end_track(1), -2,
			"a stopped VM with a loaded script still takes the frame (the handler decides)")
	assert_eq(dir.vm_state(), 0, "the frame never restarts the embedder state")
