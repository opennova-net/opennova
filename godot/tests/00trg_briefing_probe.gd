extends SceneTree

# Asset-backed oracle for the retail S2C 0x7E briefing payload on 00TRg.
# The retail serializer writes [info]/briefing3 followed by [info]/briefing2
# (falling back to [info]/briefing), both as NUL-terminated cp1252 strings.
#
# Run:
#   NOVA_RESOURCE_DIR=<retail-jo-directory> \
#     Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
#     -s res://tests/00trg_briefing_probe.gd

const EXPANSION := "revx02"
const MISSION_TEXT := "00TRg.bin"
const ORACLE_BODY_SHA256 := "99ffe3380dd8fe7d06621c848532241d5426d83ac6832d12189984c315d94c07"


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set NOVA_RESOURCE_DIR to the retail JO directory")
		return

	var root := ResourceRoot.new()
	var mount_err := int(root.mount_runtime(resource_dir, EXPANSION, false, "jo"))
	if mount_err != OK:
		_fail("resource mount failed (%d): %s" % [mount_err, root.get_last_error()])
		return

	var mission_text_bytes := root.read_file(MISSION_TEXT)
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(mission_text_bytes) != OK:
		_fail("cannot parse %s from the %s mount" % [MISSION_TEXT, EXPANSION])
		return
	# Exercise the actual engine-facing native boundary as well as the independent
	# oracle construction below. The portable burst regression pins its output.
	var sim := Simulation.new()
	sim.set_mission_text_data(mission_text_bytes)
	sim.free()

	var briefing3 := table.get_string_in_section("info", "briefing3")
	var briefing2 := table.get_string_in_section("info", "briefing2")
	if briefing2.is_empty():
		briefing2 = table.get_string_in_section("info", "briefing")

	# 00TRg's witnessed text is ASCII, so to_ascii_buffer is also the exact
	# cp1252 byte image the retail serializer copies onto the wire.
	var body := PackedByteArray()
	body.append_array(briefing3.to_ascii_buffer())
	body.append(0)
	body.append_array(briefing2.to_ascii_buffer())
	body.append(0)
	var hasher := HashingContext.new()
	hasher.start(HashingContext.HASH_SHA256)
	hasher.update(body)
	var digest := hasher.finish().hex_encode()
	print("[briefing] first_len=%d second_len=%d body_len=%d sha256=%s" % [
		briefing3.length(), briefing2.length(), body.size(), digest])
	if briefing3.length() != 0 or briefing2.length() != 810:
		_fail("00TRg briefing lengths differ from the retail capture")
		return
	if digest != ORACLE_BODY_SHA256:
		_fail("00TRg briefing body differs from retail S2C 0x7E")
		return
	quit(0)


func _fail(message: String) -> void:
	push_error("00trg_briefing_probe: %s" % message)
	quit(1)
