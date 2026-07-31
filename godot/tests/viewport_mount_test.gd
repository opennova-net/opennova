extends GutTest

const ViewportMountScript = preload("res://modtools/framework/viewport_mount.gd")


func _make_mount(counter: Dictionary):
	return ViewportMountScript.new(&"TestViewport", func() -> Control:
		counter["count"] += 1
		return Control.new()
	)


func test_mount_creates_names_and_parents_node_once() -> void:
	var counter := {"count": 0}
	var mount = _make_mount(counter)
	var attach := Control.new()
	add_child_autofree(attach)
	var node := mount.mount(attach)
	assert_not_null(node, "mount() should build the viewport node.")
	assert_eq(String(node.name), "TestViewport", "mount() should name the node.")
	assert_eq(node.get_parent(), attach, "mount() should parent the node to the attach target.")
	assert_eq(node.size_flags_horizontal, Control.SIZE_EXPAND_FILL, "mount() should expand-fill the node.")
	mount.mount(attach)
	assert_eq(counter["count"], 1, "factory should run once across repeated mounts.")
	assert_eq(mount.get_viewport_node(), node, "re-mount should reuse the same node.")
	mount.release()


func test_is_mounted_tracks_parenting() -> void:
	var counter := {"count": 0}
	var mount = _make_mount(counter)
	var attach := Control.new()
	add_child_autofree(attach)
	assert_false(mount.is_mounted(), "Nothing is mounted before mount().")
	mount.mount(attach)
	assert_true(mount.is_mounted(), "After mount() the node is parented to the mount.")
	mount.unmount()
	assert_false(mount.is_mounted(), "After unmount() the node is detached.")
	assert_true(is_instance_valid(mount.get_viewport_node()), "unmount() keeps the node alive for re-mount.")
	mount.release()


func test_release_frees_node_and_next_mount_rebuilds() -> void:
	var counter := {"count": 0}
	var mount = _make_mount(counter)
	var attach := Control.new()
	add_child_autofree(attach)
	mount.mount(attach)
	mount.release()
	assert_null(mount.get_viewport_node(), "release() clears the node reference.")
	mount.mount(attach)
	assert_eq(counter["count"], 2, "after release(), mount() builds a fresh node.")
	mount.release()
