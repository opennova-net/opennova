extends GutTest

## The menu backdrop device leg (MenuVideoUnderlay): the witnessed
## expansion-first source pick, direct BIKi decode, and the STARTUP/strips
## draw gate (policy pinned engine-side by the menu_video ctest; witness:
## docs/mnu/menu-re.md "The menu backdrop (Bink underlay)").

var _root_dir := ""


func before_each() -> void:
	_root_dir = "user://underlay_test_%d" % randi()
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(_root_dir))


func after_each() -> void:
	_remove_tree(ProjectSettings.globalize_path(_root_dir))


func _remove_tree(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for name in dir.get_files():
		DirAccess.remove_absolute(path.path_join(name))
	for name in dir.get_directories():
		_remove_tree(path.path_join(name))
	DirAccess.remove_absolute(path)


func _touch(rel: String) -> void:
	var absolute := ProjectSettings.globalize_path(_root_dir.path_join(rel))
	DirAccess.make_dir_recursive_absolute(absolute.get_base_dir())
	var f := FileAccess.open(absolute, FileAccess.WRITE)
	f.store_string("stub")
	f.close()


class BitWriter:
	var bytes := PackedByteArray()
	var bit_count := 0

	func write(value: int, count: int) -> void:
		for bit in count:
			if (bit_count & 7) == 0:
				bytes.append(0)
			bytes[bytes.size() - 1] |= ((value >> bit) & 1) << (bit_count & 7)
			bit_count += 1

	func align_32() -> void:
		while (bit_count & 31) != 0:
			write(0, 1)


func _write_fill_plane(writer: BitWriter, color: int) -> void:
	writer.write(0, 4) # block types
	writer.write(0, 4) # scaled sub-types
	for _tree in 16:
		writer.write(0, 4) # color high-nibble contexts
	writer.write(0, 4) # color low nibble
	writer.write(0, 4) # patterns
	writer.write(0, 4) # X motion
	writer.write(0, 4) # Y motion
	writer.write(0, 4) # runs
	writer.write(1, 10) # one block type
	writer.write(1, 1) # repeat encoding
	writer.write(6, 4) # fill block
	writer.write(0, 9) # no scaled sub-types
	writer.write(1, 10) # one color
	writer.write(1, 1) # repeat encoding
	writer.write(color >> 4, 4)
	writer.write(color & 0xf, 4)
	for _empty_bundle in 6:
		writer.write(0, 10)
	writer.align_32()


func _write_white_biki(rel: String) -> void:
	var packet := BitWriter.new()
	packet.write(0, 32)
	_write_fill_plane(packet, 235) # Y
	_write_fill_plane(packet, 128) # V
	_write_fill_plane(packet, 128) # U
	var absolute := ProjectSettings.globalize_path(_root_dir.path_join(rel))
	DirAccess.make_dir_recursive_absolute(absolute.get_base_dir())
	var file := FileAccess.open(absolute, FileAccess.WRITE)
	var first_frame_offset := 48
	var file_size := first_frame_offset + packet.bytes.size()
	file.store_32(0x694b4942) # BIKi
	file.store_32(file_size - 8)
	file.store_32(1) # frames
	file.store_32(packet.bytes.size())
	file.store_32(0) # reserved
	file.store_32(8)
	file.store_32(8)
	file.store_32(30)
	file.store_32(1)
	file.store_32(0) # flags
	file.store_32(0) # audio tracks
	file.store_32(first_frame_offset | 1)
	file.store_buffer(packet.bytes)
	file.close()


func _make_underlay() -> MenuVideoUnderlay:
	var underlay: MenuVideoUnderlay = add_child_autofree(MenuVideoUnderlay.new())
	return underlay


func test_expansion_movie_wins_and_decodes_directly() -> void:
	_write_white_biki("main.bik")
	_write_white_biki("expansion/revx02/main.bik")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "revx02")
	assert_eq(underlay.get_slot_source(0), "expansion/revx02/main.bik",
			"the expansion's movie is the witnessed pick")
	assert_eq(underlay.get_active_slot_count(), 1)
	assert_eq(underlay.get_failed_count(), 0)


func test_invalid_selected_movie_counts_failed() -> void:
	# The invalid expansion BIK is the witnessed pick; falling back to the
	# root BIK after that choice would show the wrong movie.
	_write_white_biki("main.bik")
	_touch("expansion/revx02/main.bik")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "revx02")
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_failed_count(), 1)


func test_missing_movies_skip_silently() -> void:
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "")
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_failed_count(), 0)


func test_screen_gate_follows_the_startup_rule() -> void:
	var underlay := _make_underlay()
	underlay.set_screen("STARTUP")
	assert_true(underlay.is_startup_layout(), "STARTUP is the front page")
	underlay.set_screen("SINGLE_PLAYER")
	assert_false(underlay.is_startup_layout(), "sub-screens take the strips")


func test_stop_clears_slots() -> void:
	_write_white_biki("header.bik")
	var underlay := _make_underlay()
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "")
	assert_eq(underlay.get_active_slot_count(), 1)
	underlay.stop()
	assert_eq(underlay.get_active_slot_count(), 0)
	assert_eq(underlay.get_slot_source(1), "")


func test_hidden_menu_suspends_and_resumes_movie_processing() -> void:
	_write_white_biki("main.bik")
	var menu := Control.new()
	add_child_autofree(menu)
	var underlay := MenuVideoUnderlay.new()
	menu.add_child(underlay)
	underlay.set_source(ProjectSettings.globalize_path(_root_dir), "")
	assert_true(underlay.is_processing(), "a visible menu advances its movie")
	menu.hide()
	assert_false(underlay.is_processing(),
			"an invisible menu cannot keep decoding and uploading Bink frames")
	menu.show()
	assert_true(underlay.is_processing(),
			"showing the menu resumes the existing movie slots")


func test_process_timing_is_opt_in_and_consumed_once() -> void:
	var underlay := _make_underlay()
	assert_false(underlay.is_runtime_profiling_enabled())
	assert_eq(underlay.consume_process_us(), 0)
	underlay.set_runtime_profiling_enabled(true)
	assert_true(underlay.is_runtime_profiling_enabled())
	assert_eq(underlay.consume_process_us(), 0,
			"opening capture starts with no stale video sample")
	underlay.set_runtime_profiling_enabled(false)
	assert_false(underlay.is_runtime_profiling_enabled())
	assert_eq(underlay.consume_process_us(), 0,
			"closing capture clears the pending sample")
