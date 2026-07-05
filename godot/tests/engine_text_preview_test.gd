extends GutTest

# F2 gate (docs/oned/workspace-maturity-program.md): the EngineTextPreview
# widget renders sample text through the game's own font path — NovaFntResource
# -> FontFile plus HudText's draw semantics (fixed pixel size, alignment
# anchors, half-bright). Public API only (ADR 0018); rendering itself is
# validated visually in the driver run, here we lock the contract.

const EngineTextPreviewScript = preload("res://modtools/framework/engine_text_preview.gd")
const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"

var _temp_roots: Array = []


func after_each() -> void:
	for root in _temp_roots:
		var dir := DirAccess.open(root)
		if dir != null:
			for file in dir.get_files():
				dir.remove(file)
			DirAccess.remove_absolute(root)
	_temp_roots.clear()


func _make_preview() -> Control:
	var preview: Control = EngineTextPreviewScript.new()
	add_child_autofree(preview)
	preview.size = Vector2(240, 60)
	return preview


func _load_fixture_fnt() -> NovaFntResource:
	var res := ResourceLoader.load(FNT_PATH, "NovaFntResource", ResourceLoader.CACHE_MODE_IGNORE) as NovaFntResource
	assert_not_null(res, "Serpen24.fnt fixture should load as NovaFntResource.")
	return res


func test_defaults_have_no_font_and_a_visible_sample() -> void:
	var preview := _make_preview()
	assert_false(preview.has_font(), "A fresh panel carries no font.")
	assert_null(preview.get_font_file(), "No FontFile before one is loaded.")
	assert_false(preview.get_sample_text().is_empty(), "A default sample line ships so the panel is never blank.")
	preview.set_sample_text("HELLO")
	assert_eq(preview.get_sample_text(), "HELLO", "Sample text round-trips.")


func test_adopts_the_edited_documents_font_through_to_font_file() -> void:
	var preview := _make_preview()
	var res := _load_fixture_fnt()
	if res == null:
		return
	assert_true(preview.set_font_from_fnt(res), "A loaded .fnt adopts through the runtime FontFile view.")
	assert_true(preview.has_font(), "The panel holds the font.")
	assert_gt(preview.get_font_file().get_fixed_size(), 0, "The FontFile carries the .fnt's fixed pixel size.")
	assert_eq(preview.font_pixel_size(), preview.get_font_file().get_fixed_size(),
		"The draw uses the .fnt's own fixed size — glyphs do not scale, positions do.")

	assert_false(preview.set_font_from_fnt(null), "A null document reports failure.")
	assert_false(preview.has_font(), "A null document clears the panel.")


func test_resolves_a_font_from_the_resource_root_like_the_runtime() -> void:
	var preview := _make_preview()
	var root_dir := OS.get_cache_dir().path_join("opennova_etp_root_%d" % Time.get_ticks_usec())
	_temp_roots.append(root_dir)
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FNT_PATH))
	assert_gt(bytes.size(), 0, "Fixture bytes should read.")
	var out := FileAccess.open(root_dir.path_join("Serpen24.fnt"), FileAccess.WRITE)
	out.store_buffer(bytes)
	out.close()

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK, "Temp dir mounts as a loose resource root.")
	assert_true(preview.set_font_from_root(root, "Serpen24.fnt"),
		"The runtime resolution path (HudText.load_font over the VFS) finds the font.")
	assert_true(preview.has_font(), "The resolved font lands in the panel.")

	var before: FontFile = preview.get_font_file()
	assert_false(preview.set_font_from_root(root, "missing.fnt"), "An unresolvable name reports failure.")
	assert_eq(preview.get_font_file(), before, "A failed lookup keeps the current font — a typo must not blank the panel.")


func test_half_bright_matches_the_original_color_mode() -> void:
	var preview := _make_preview()
	preview.set_text_color(Color(1, 1, 1, 1))
	assert_eq(preview.effective_color(), Color(1, 1, 1, 1), "Full-bright hands the color through untouched.")
	preview.set_half_bright(true)
	assert_true(preview.is_half_bright(), "Half-bright mode is inspectable.")
	assert_eq(preview.effective_color(), HudText.half_bright(Color(1, 1, 1, 1)),
		"Half-bright is the original's halved-RGB opaque mode, via the shared helper.")


func test_alignment_moves_the_design_space_anchor() -> void:
	var preview := _make_preview()
	preview.set_alignment(HudText.Align.LEFT)
	assert_almost_eq(preview.draw_anchor_design().x, 16.0, 0.001, "Left samples anchor at the left margin.")
	preview.set_alignment(HudText.Align.CENTER)
	assert_almost_eq(preview.draw_anchor_design().x, 512.0, 0.001, "Centered samples anchor mid-design-space.")
	preview.set_alignment(HudText.Align.RIGHT)
	assert_almost_eq(preview.draw_anchor_design().x, 1008.0, 0.001, "Right samples anchor at the right margin.")
	assert_eq(preview.get_alignment(), int(HudText.Align.RIGHT), "Alignment round-trips.")

	# The vertical anchor centers the glyph cell for the panel's current height.
	var fs := float(preview.font_pixel_size())
	var expected_y := 768.0 * (preview.size.y - fs) / (2.0 * preview.size.y)
	assert_almost_eq(preview.draw_anchor_design().y, expected_y, 0.001,
		"The vertical anchor keeps the glyph cell mid-panel after HudLayout scaling.")


func test_draws_without_error_in_tree() -> void:
	var preview := _make_preview()
	var res := _load_fixture_fnt()
	if res == null:
		return
	preview.set_font_from_fnt(res)
	preview.set_sample_text("AMMO 30 / 90")
	preview.queue_redraw()
	await get_tree().process_frame
	assert_true(is_instance_valid(preview), "A font-bearing panel survives a draw pass.")
