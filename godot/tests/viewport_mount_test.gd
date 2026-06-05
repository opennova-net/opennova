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
	var host := Control.new()
	add_child_autofree(host)
	var node := mount.mount(host)
	assert_not_null(node, "mount() should build the viewport node.")
	assert_eq(String(node.name), "TestViewport", "mount() should name the node.")
	assert_eq(node.get_parent(), host, "mount() should parent the node to the host.")
	assert_eq(node.size_flags_horizontal, Control.SIZE_EXPAND_FILL, "mount() should expand-fill the node.")
	mount.mount(host)
	assert_eq(counter["count"], 1, "factory should run once across repeated mounts.")
	assert_eq(mount.get_viewport_node(), node, "re-mount should reuse the same node.")
	mount.release()


func test_is_mounted_tracks_parenting() -> void:
	var counter := {"count": 0}
	var mount = _make_mount(counter)
	var host := Control.new()
	add_child_autofree(host)
	assert_false(mount.is_mounted(), "Nothing is mounted before mount().")
	mount.mount(host)
	assert_true(mount.is_mounted(), "After mount() the node is parented to the host.")
	mount.unmount()
	assert_false(mount.is_mounted(), "After unmount() the node is detached.")
	assert_true(is_instance_valid(mount.get_viewport_node()), "unmount() keeps the node alive for re-mount.")
	mount.release()


func test_release_frees_node_and_next_mount_rebuilds() -> void:
	var counter := {"count": 0}
	var mount = _make_mount(counter)
	var host := Control.new()
	add_child_autofree(host)
	mount.mount(host)
	mount.release()
	assert_null(mount.get_viewport_node(), "release() clears the node reference.")
	mount.mount(host)
	assert_eq(counter["count"], 2, "after release(), mount() builds a fresh node.")
	mount.release()
