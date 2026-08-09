class_name CreditsEditorDocument
extends EditorResourceDocument

# Credits (.kda) document: the shared EditorResourceDocument lifecycle over a
# CbinCreditsResource. Only the blank factory, the loader, and the file naming
# are domain code.


func _make_new_resource():
	return CbinCreditsResource.new()


func _default_basename() -> String:
	return "credits"


func _file_extension() -> String:
	return "kda"


# CbinCreditsResource reads/writes itself (ADR 0032 direct document I/O).
func _save_resource(path: String) -> Error:
	return (resource as CbinCreditsResource).save_to_path(path)


func open_kda(path: String) -> Error:
	var loaded := CbinCreditsResource.new()
	if loaded.load_from_path(path) != OK:
		return ERR_CANT_OPEN
	adopt_loaded(loaded, path)
	return OK


# --- Snapshot undo (B2) ---------------------------------------------------------
# Whole-document text snapshots via the CbinCreditsResource to_text/from_text
# pair (the same serialization the source view ships on). Undo/redo rebuilds
# the entry objects, so entry identity resets — identical to a source-view
# Apply; the block list reconciles by entry and rebuilds its cards.

func _snapshot() -> Variant:
	if resource == null:
		return null
	return (resource as CbinCreditsResource).to_text()


func _apply_snapshot(snap: Variant) -> void:
	if resource == null:
		return
	var res := resource as CbinCreditsResource
	var text := String(snap)
	# from_text emits the resource's changed signal, which already routes dirty
	# + the resource_changed/state_changed emits through _on_resource_changed.
	if res.from_text(text):
		return
	# from_text refuses zero-entry input BY DESIGN (a source-view Apply must
	# not wipe a document by accident) — but a zero-entry SNAPSHOT is a
	# legitimate undo target (undoing the first insert). Restore it by hand:
	# strip the entries, then re-apply the ENV fields the snapshot carries
	# (the same setters the env bar rides; top_y/bottom_y have no editor and
	# keep their live values).
	while res.get_entry_count() > 0:
		res.remove_entry(res.get_entry_count() - 1)
	for raw_line in text.split("\n"):
		var line := raw_line.strip_edges()
		if line.begins_with("scroll_rate="):
			res.set_scroll_rate(float(line.get_slice("=", 1)))
		elif line.begins_with("vertical_space="):
			res.set_vertical_space(int(line.get_slice("=", 1)))
		elif line.begins_with("center_x="):
			res.set_center_x(int(line.get_slice("=", 1)))
