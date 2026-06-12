extends GutTest

# McpLogHub: cursor paging, ring overflow accounting, the status mirror seam,
# and the engine log tail (classification, continuation merge, partial lines).

const FAKE_LOG := "user://mcp_hub_test.log"

var hub: McpLogHub


func before_each() -> void:
	hub = McpLogHub.new()
	McpLogHub.instance = null


func after_each() -> void:
	McpLogHub.instance = null
	if FileAccess.file_exists(FAKE_LOG):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(FAKE_LOG))


func _write_fake_log(text: String) -> void:
	var file := FileAccess.open(FAKE_LOG, FileAccess.WRITE)
	file.store_string(text)
	file.close()


func _append_fake_log(text: String) -> void:
	var file := FileAccess.open(FAKE_LOG, FileAccess.READ_WRITE)
	file.seek_end()
	file.store_string(text)
	file.close()


func test_note_assigns_increasing_seq() -> void:
	hub.note("server", "info", "one")
	hub.note("server", "info", "two")
	var page := hub.get_entries()
	assert_eq(page["entries"].size(), 2)
	assert_eq(int(page["entries"][0]["seq"]), 1)
	assert_eq(int(page["entries"][1]["seq"]), 2)
	assert_eq(int(page["next_cursor"]), 2)


func test_cursor_pages_incrementally() -> void:
	for i in range(5):
		hub.note("server", "info", "entry %d" % i)
	var first := hub.get_entries(0, 3)
	var rest := hub.get_entries(int(first["next_cursor"]))
	assert_eq(rest["entries"].size(), 0, "Tail page already saw the newest entries.")
	var from_two := hub.get_entries(2)
	assert_eq(from_two["entries"].size(), 3, "Entries after seq 2.")
	assert_eq(String(from_two["entries"][0]["text"]), "entry 2")


func test_cursor_zero_returns_tail() -> void:
	for i in range(10):
		hub.note("server", "info", "entry %d" % i)
	var page := hub.get_entries(0, 4)
	assert_eq(page["entries"].size(), 4)
	assert_eq(String(page["entries"][0]["text"]), "entry 6", "Tail of the last `limit` entries.")
	assert_eq(int(page["dropped"]), 0)


func test_ring_overflow_counts_dropped() -> void:
	for i in range(McpLogHub.RING_CAP + 10):
		hub.note("server", "info", "entry %d" % i)
	var page := hub.get_entries(5)
	assert_eq(int(page["dropped"]), 5, "Client at cursor 5 lost seq 6..10 to the ring.")
	var contiguous := hub.get_entries(10)
	assert_eq(int(contiguous["dropped"]), 0, "Cursor 10 is contiguous with first retained seq 11.")


func test_sources_filter() -> void:
	hub.note("server", "info", "from server")
	hub.note("status", "info", "from status")
	hub.note("script", "info", "from script")
	var page := hub.get_entries(0, 200, PackedStringArray(["status"]))
	assert_eq(page["entries"].size(), 1)
	assert_eq(String(page["entries"][0]["text"]), "from status")


func test_note_status_static_seam() -> void:
	McpLogHub.note_status("ignored — no instance")
	McpLogHub.instance = hub
	McpLogHub.note_status("mirrored")
	var page := hub.get_entries()
	assert_eq(page["entries"].size(), 1)
	assert_eq(String(page["entries"][0]["source"]), "status")
	assert_eq(String(page["entries"][0]["text"]), "mirrored")


func test_engine_ingest_classifies_and_merges() -> void:
	_write_fake_log("")
	hub.set_engine_log_path(ProjectSettings.globalize_path(FAKE_LOG))
	_append_fake_log("Mission loaded in 2.4s\n")
	_append_fake_log("SCRIPT ERROR: Invalid call on a Nil value.\n")
	_append_fake_log("   at: run (gdscript://123.gd:4)\n")
	_append_fake_log("WARNING: something minor\n")
	hub.ingest_engine()
	var page := hub.get_entries(0, 10, PackedStringArray(["engine"]))
	assert_eq(page["entries"].size(), 3, "Continuation folded into its error block.")
	assert_eq(String(page["entries"][0]["level"]), "info")
	assert_eq(String(page["entries"][1]["level"]), "error")
	assert_true(String(page["entries"][1]["text"]).contains("at: run"), "at: frame merged into the error entry.")
	assert_eq(String(page["entries"][2]["level"]), "warn")


func test_engine_ingest_holds_partial_line() -> void:
	_write_fake_log("")
	hub.set_engine_log_path(ProjectSettings.globalize_path(FAKE_LOG))
	_append_fake_log("complete line\nincomplete frag")
	hub.ingest_engine()
	var page := hub.get_entries(0, 10, PackedStringArray(["engine"]))
	assert_eq(page["entries"].size(), 1, "Half-written line is not ingested.")
	_append_fake_log("ment finished\n")
	hub.ingest_engine()
	page = hub.get_entries(0, 10, PackedStringArray(["engine"]))
	assert_eq(page["entries"].size(), 2)
	assert_eq(String(page["entries"][1]["text"]), "incomplete fragment finished")


func test_engine_delta_is_stateless() -> void:
	_write_fake_log("")
	hub.set_engine_log_path(ProjectSettings.globalize_path(FAKE_LOG))
	var mark := hub.engine_mark()
	_append_fake_log("ERROR: boom\n   at: somewhere\n")
	var blocks := hub.engine_delta(mark)
	assert_eq(blocks.size(), 1)
	assert_eq(String(blocks[0]["level"]), "error")
	assert_true(String(blocks[0]["text"]).contains("boom"))
	assert_eq(hub.engine_delta(mark).size(), 1, "Delta does not consume; same answer twice.")


func test_unavailable_engine_log_degrades_explicitly() -> void:
	hub.set_engine_log_path("")
	assert_false(hub.engine_available())
	assert_eq(hub.engine_mark(), -1)
	assert_eq(hub.engine_delta(0).size(), 0)
	var page := hub.get_entries()
	assert_eq(String(page["engine_log"]), "unavailable")
