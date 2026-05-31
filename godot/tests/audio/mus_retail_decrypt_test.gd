extends GutTest

# Retail-disk MUS decryption smoke. Drops the encrypted gamemus.bin from the
# user's JO install into user://, points ResourceLoader at it, and asserts
# the loader picks the MUS-style SCR fallback (no "SCR\xVV" magic on disk;
# scr_decrypt_mus prepends "SCR0").
#
# Skipped without failure when the source asset is absent so CI without the
# JO assets folder still passes. Local devs with the asset get full coverage.
#
# See libs/scr/src/scr.cpp::scr_decrypt_mus for the cipher details.

const RETAIL_SRC := "C:/Users/taylor/Desktop/JO_ASSETS_t/gamemus.bin"
const USER_COPY := "user://test_retail_gamemus.bin"


func _retail_available() -> bool:
	return FileAccess.file_exists(RETAIL_SRC)


func _stage_retail_copy() -> bool:
	# user:// paths in headless test runs are writable; copy the encrypted
	# bin into user:// so ResourceLoader can find it via a virtual path.
	var src_bytes := FileAccess.get_file_as_bytes(RETAIL_SRC)
	if src_bytes.is_empty():
		return false
	var fa := FileAccess.open(USER_COPY, FileAccess.WRITE)
	if fa == null:
		return false
	fa.store_buffer(src_bytes)
	fa.close()
	return true


func _cleanup_retail_copy() -> void:
	if FileAccess.file_exists(USER_COPY):
		DirAccess.remove_absolute(USER_COPY)


func test_retail_gamemus_loads_via_mus_scr_fallback() -> void:
	if not _retail_available():
		# Asset isn't present (typical CI). Note in the GUT log and pass.
		gut.p("SKIP: retail gamemus.bin not at " + RETAIL_SRC)
		assert_true(true, "skip-if-absent")
		return

	assert_true(_stage_retail_copy(), "stage retail gamemus.bin into user://")

	var res := ResourceLoader.load(USER_COPY, "NovaMusicScript", ResourceLoader.CACHE_MODE_IGNORE)
	_cleanup_retail_copy()

	assert_not_null(res, "loader returns non-null for retail-disk gamemus.bin")
	if res == null:
		return
	assert_true(res is NovaMusicScript, "loader returns NovaMusicScript for SCR-encrypted retail file")
	if not (res is NovaMusicScript):
		return
	var ms := res as NovaMusicScript
	assert_gt(ms.get_script_count(), 0, "decrypted retail MUS yields at least one chunk")
	assert_eq(
		ms.get_default_script_name(),
		"gamescript",
		"retail gamemus.bin default script should be gamescript"
	)
