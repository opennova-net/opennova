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
