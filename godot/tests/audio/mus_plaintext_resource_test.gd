extends GutTest

# MUS load smoke (fixture-only). Stages the committed plaintext jo_gamemus.bin
# (byte-identical to JO_CLIENT/localres.pff's canonical entry) into user:// and
# loads it via ResourceLoader, asserting the MUS loader returns a NovaMusicScript.

const FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const USER_COPY := "user://test_jo_gamemus.bin"


func _stage() -> bool:
	var src := FileAccess.get_file_as_bytes(FIXTURE)
	if src.is_empty():
		return false
	var fa := FileAccess.open(USER_COPY, FileAccess.WRITE)
	if fa == null:
		return false
	fa.store_buffer(src)
	fa.close()
	return true


func _cleanup() -> void:
	if FileAccess.file_exists(USER_COPY):
		DirAccess.remove_absolute(USER_COPY)


func test_plaintext_gamemus_loads_as_mus() -> void:
	assert_true(_stage(), "stage committed jo_gamemus.bin into user://")

	var res := ResourceLoader.load(USER_COPY, "NovaMusicScript", ResourceLoader.CACHE_MODE_IGNORE)
	_cleanup()

	assert_not_null(res, "loader returns non-null for plaintext SCR0 gamemus.bin")
	if res == null:
		return
	assert_true(res is NovaMusicScript, "loader returns NovaMusicScript")
	if not (res is NovaMusicScript):
		return
	var ms := res as NovaMusicScript
	assert_gt(ms.get_script_count(), 0, "plaintext MUS yields at least one chunk")
	assert_eq(
		ms.get_default_script_name(),
		"gamescript",
		"jo_gamemus.bin default script should be gamescript"
	)
