extends GutTest

# Phase 5: the widget tree must reveal (expand) collapsed ancestors when a deep widget
# is selected programmatically (a canvas pick of a nested widget), so the selected row
# is actually visible instead of hidden under collapsed parents.

const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _collapse_all(item: TreeItem) -> void:
	if item == null:
		return
	item.collapsed = true
	var c := item.get_first_child()
	while c != null:
		_collapse_all(c)
		c = c.get_next()


func test_select_id_expands_collapsed_ancestors() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var win := doc.add_widget(root, NovaMnuDocument.TYPE_WINDOW, Rect2(100, 100, 200, 200))
	var child := doc.add_widget(win, NovaMnuDocument.TYPE_STATIC, Rect2(10, 10, 50, 30))

	var tree = MnuWidgetTreeScript.new()
	add_child_autofree(tree)
	tree.set_document(doc)
	await get_tree().process_frame

	# Collapse every row so the deep child would be hidden.
	_collapse_all(tree.get_root())

	tree.select_id(child)
	assert_eq(tree.get_selected_id(), child, "The deep widget is the active selection.")

	# Every ancestor of the selected row must now be expanded.
	var ancestor := tree.get_selected().get_parent()
	var checked := 0
	while ancestor != null:
		assert_false(ancestor.collapsed, "An ancestor of the selected row is expanded.")
		checked += 1
		ancestor = ancestor.get_parent()
	assert_gt(checked, 0, "The selected row has ancestors that were revealed.")
