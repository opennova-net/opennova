extends GutTest

const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
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
