extends GutTest


func test_runtime_does_not_ship_resource_roots() -> void:
	var forbidden_dirs := [
		"res://" + "assets",
	]
	for dir_path in forbidden_dirs:
		assert_false(
			DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(dir_path)),
			"%s must not exist; game data belongs in the CLI resource root." % dir_path
		)


func test_resource_root_resolves_only_top_level_files() -> void:
	var root := _make_flat_root("flat_resolve")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "trn")
	DirAccess.make_dir_recursive_absolute(root.path_join("terrains"))
	TestFs.write_text(self, root.path_join("terrains/Dvxi5.trn"), "nested")

	var resources := ResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)

	assert_eq(_norm(resources.resolve_file("alpha.trn")), _norm(root.path_join("Alpha.TRN")))
	assert_eq(resources.resolve_file("terrains/alpha.trn"), "", "Resource names must be flat basenames, not nested paths.")
	assert_string_contains(resources.get_last_error(), "flat filename", "Pathful lookups should explain the flat resource-root contract.")
	assert_eq(resources.resolve_file("Dvxi5.trn"), "", "Nested files are not part of the flat resource root.")

	var trns := resources.list_files(".trn")
	assert_eq(trns.size(), 1)
	assert_eq(String(trns[0]).get_file(), "Alpha.TRN")


func test_editor_set_root_dir_is_loose_only() -> void:
	var root := _make_flat_root("loose_only")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "loose trn")
	WorldFixture.write_pff(self, root.path_join("aa_base.pff"), [
		{"name": "Alpha.TRN", "bytes": "archived trn"},
		{"name": "Bravo.env", "bytes": "archived env"},
	])

	var resources := ResourceRoot.new()
	# The editor authors loose files; set_root_dir mounts loose only, never the PFFs.
	assert_eq(resources.set_root_dir(root), OK)
	assert_true(resources.has_file("alpha.trn"), "Loose files mount in the editor.")
	assert_false(resources.has_file("bravo.env"), "PFF-only entries must not mount in the editor (loose-only).")
	assert_eq(resources.read_file("Alpha.trn").get_string_from_utf8(), "loose trn")


func test_runtime_mount_is_packed_with_optional_loose_override() -> void:
	var root := _make_flat_root("packed_runtime")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "loose trn")
	# The runtime mounts only the witnessed boot archive table (language/localres/
	# resource.pff) [orig: PFF_OpenAllArchives @ 0x4a4310]; an arbitrary-named .pff
	# never mounts at runtime (D-VFS-2).
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "Alpha.TRN", "bytes": "archived trn"},
		{"name": "Bravo.env", "bytes": "archived env"},
	])
	WorldFixture.write_pff(self, root.path_join("zz_extra.pff"), [
		{"name": "Extra.env", "bytes": "never mounts"},
	])

	var resources := ResourceRoot.new()
	# Packed runtime (no /d): the archives are the source; loose files do NOT shadow them.
	assert_eq(resources.mount_runtime(root), OK)
	assert_true(resources.has_file("bravo.env"), "PFF entries mount at runtime.")
	assert_eq(resources.read_file("Alpha.trn").get_string_from_utf8(), "archived trn", "Without /d the runtime ignores loose overrides.")
	assert_eq(resources.read_file("Bravo.env").get_string_from_utf8(), "archived env")
	assert_false(resources.has_file("extra.env"), "An archive outside the boot table never mounts at runtime.")

	var entries := resources.list_file_entries(".env")
	assert_eq(entries.size(), 1)
	assert_eq(String(entries[0].logical_name), "Bravo.env")
	assert_eq(String(entries[0].source_type), "pff")
	assert_eq(String(entries[0].archive_path).get_file(), "resource.pff")

	# Packed + loose override (/d): loose files shadow the archives.
	assert_eq(resources.mount_runtime(root, "", true), OK)
	assert_eq(resources.read_file("Alpha.trn").get_string_from_utf8(), "loose trn", "With /d a loose file overrides the archived entry.")


func test_runtime_mount_rejects_loose_only_root_even_with_dev_override() -> void:
	var root := _make_flat_root("runtime_requires_archive")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "loose trn")

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root, "", true), ERR_FILE_NOT_FOUND)
	assert_eq(resources.get_root_dir(), "", "A failed runtime mount must clear the partial loose state.")
	assert_eq(resources.get_expansion(), "")
	assert_eq(resources.get_last_error(), "No game data archives could be opened")
	assert_false(resources.has_file("Alpha.TRN", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST))


func test_packed_runtime_caller_can_force_loose_first() -> void:
	var root := _make_flat_root("packed_force_loose")
	TestFs.write_text(self, root.path_join("Shared.dat"), "loose")
	TestFs.write_text(self, root.path_join("LooseOnly.dat"), "loose only")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "Shared.dat", "bytes": "archive"},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	assert_eq(resources.read_file("Shared.dat").get_string_from_utf8(), "archive")
	assert_true(resources.has_file("LooseOnly.dat", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST))
	assert_eq(
		resources.read_file("LooseOnly.dat", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST).get_string_from_utf8(),
		"loose only",
		"Policy-aware has_file and read_file must agree on a loose-only winner."
	)
	assert_eq(
		resources.read_file("Shared.dat", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST).get_string_from_utf8(),
		"loose",
		"A packed session remains archive-first by default, but a retail caller can force loose-first."
	)


func test_dev_runtime_caller_can_force_archive_only() -> void:
	var root := _make_flat_root("dev_force_archive")
	TestFs.write_text(self, root.path_join("Shared.dat"), "loose")
	TestFs.write_text(self, root.path_join("LooseOnly.dat"), "loose only")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "Shared.dat", "bytes": "archive"},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root, "", true), OK)
	assert_eq(resources.read_file("Shared.dat").get_string_from_utf8(), "loose")
	assert_true(resources.has_file("Shared.dat", ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY))
	assert_eq(
		resources.read_file("Shared.dat", ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY).get_string_from_utf8(),
		"archive",
		"An archive-only retail caller bypasses the /d session loose override."
	)
	assert_false(resources.has_file("LooseOnly.dat", ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY))
	assert_true(
		resources.read_file("LooseOnly.dat", ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY).is_empty(),
		"Archive-only has_file and read_file must agree that a loose-only file is absent."
	)


func test_runtime_qualified_query_reaches_loose_file_without_aliasing_flat_archive() -> void:
	var root := _make_flat_root("qualified_runtime")
	DirAccess.make_dir_recursive_absolute(root.path_join("Nested"))
	TestFs.write_text(self, root.path_join("Nested/MixedCase.dat"), "nested loose")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "MixedCase.dat", "bytes": "flat archive"},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	assert_false(resources.has_file("nested/mixedcase.dat"))
	assert_true(
		resources.read_file("nested/mixedcase.dat").is_empty(),
		"A qualified packed-default query must not alias a flat archive entry."
	)
	assert_true(resources.has_file(
		"NESTED\\MIXEDCASE.DAT",
		ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
	))
	assert_eq(
		resources.read_file(
			"nested/mixedcase.dat",
			ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
		).get_string_from_utf8(),
		"nested loose"
	)

	assert_eq(resources.set_root_dir(root), OK)
	assert_false(resources.has_file(
		"nested/mixedcase.dat",
		ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
	), "Loose roots keep the flat-name contract for every policy value.")
	assert_true(resources.read_file(
		"nested/mixedcase.dat",
		ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
	).is_empty())


func test_load_texture_obeys_runtime_vfs_precedence() -> void:
	var root := _make_flat_root("texture_precedence")
	TestFs.write_bytes(self, root.path_join("mission.pcx"), _solid_test_pcx(Color.BLUE))
	WorldFixture.write_pff(self, root.path_join("language.pff"), [
		{"name": "mission.pcx", "bytes": _solid_test_pcx(Color.RED)},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var packed_texture: Texture2D = resources.load_texture("mission.pcx")
	assert_not_null(packed_texture, "The packed PCX should decode through load_texture().")
	if packed_texture != null:
		assert_true(
			packed_texture.get_image().get_pixel(0, 0).is_equal_approx(Color.RED),
			"Packed mode must return the language.pff PCX, not the same-named loose file."
		)

	assert_eq(resources.mount_runtime(root, "", true), OK)
	var override_texture: Texture2D = resources.load_texture("mission.pcx")
	assert_not_null(override_texture, "The /d loose-override PCX should decode through load_texture().")
	if override_texture != null:
		assert_true(
			override_texture.get_image().get_pixel(0, 0).is_equal_approx(Color.BLUE),
			"PackedWithLooseOverride mode must return the same-named loose PCX."
		)


func test_texture_cache_separates_policy_and_full_query() -> void:
	var root := _make_flat_root("texture_policy_cache")
	DirAccess.make_dir_recursive_absolute(root.path_join("Nested"))
	TestFs.write_bytes(self, root.path_join("swatch.pcx"), _solid_test_pcx(Color.BLUE))
	TestFs.write_bytes(self, root.path_join("Nested/swatch.pcx"), _solid_test_pcx(Color.GREEN))
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "swatch.pcx", "bytes": _solid_test_pcx(Color.RED)},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var packed: Texture2D = resources.load_texture("swatch.pcx")
	var forced_loose: Texture2D = resources.load_texture(
		"swatch.pcx",
		ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
	)
	var nested_loose: Texture2D = resources.load_texture(
		"nested/swatch.pcx",
		ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST
	)
	assert_not_null(packed)
	assert_not_null(forced_loose)
	assert_not_null(nested_loose)
	if packed != null and forced_loose != null and nested_loose != null:
		assert_true(packed.get_image().get_pixel(0, 0).is_equal_approx(Color.RED))
		assert_true(forced_loose.get_image().get_pixel(0, 0).is_equal_approx(Color.BLUE))
		assert_true(
			nested_loose.get_image().get_pixel(0, 0).is_equal_approx(Color.GREEN),
			"The cache key includes the complete qualified query, not only its basename."
		)


func test_editor_load_texture_retains_loose_png_support() -> void:
	var image := Image.create(3, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color.GREEN)
	var root := _make_flat_root("loose_png")
	TestFs.write_bytes(self, root.path_join("swatch.png"), image.save_png_to_buffer())

	var resources := ResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)
	var texture: Texture2D = resources.load_texture("swatch.png")
	assert_not_null(texture, "A loose editor PNG should still decode through the VFS-backed texture interface.")
	if texture != null:
		assert_eq(texture.get_width(), 3)
		assert_eq(texture.get_height(), 2)
		assert_true(texture.get_image().get_pixel(0, 0).is_equal_approx(Color.GREEN))


func test_runtime_expansion_override_chain() -> void:
	var root := _make_flat_root("expansion")
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "shared.env", "bytes": "base env"},
		{"name": "baseonly.trn", "bytes": "base trn"},
	])
	WorldFixture.write_pff(self, root.path_join("expansion/jox01/jox01.pff"), [
		{"name": "shared.env", "bytes": "main env"},
		{"name": "exponly.3di", "bytes": "exp model"},
	])
	WorldFixture.write_pff(self, root.path_join("expansion/jox01/jox01L.pff"), [
		{"name": "shared.env", "bytes": "local env"},
	])
	TestFs.write_text(self, root.path_join("expansion/jox01/shared.env"), "loose env")

	var resources := ResourceRoot.new()
	# Packed runtime (no /d): archive chain {name}L.pff > {name}.pff > base; loose ignored.
	assert_eq(resources.mount_runtime(root, "jox01"), OK)
	assert_eq(resources.get_expansion(), "jox01", "A real expansion mount reports itself.")
	assert_eq(resources.read_file("shared.env").get_string_from_utf8(), "local env", "{name}L.pff wins among archives.")
	assert_eq(resources.read_file("exponly.3di").get_string_from_utf8(), "exp model", "Expansion archive beats base.")
	assert_eq(resources.read_file("baseonly.trn").get_string_from_utf8(), "base trn", "Base archive still reachable.")
	# With /d the loose expansion file overrides every archive.
	assert_eq(resources.mount_runtime(root, "jox01", true), OK)
	assert_eq(resources.read_file("shared.env").get_string_from_utf8(), "loose env", "Loose expansion file wins under /d.")
	# A missing expansion falls back to base-game mounting (no error). get_expansion() must
	# report the FALLBACK, not echo the request: it is the only evidence a caller whose data
	# set has to match a peer's has that the mount did not take (the LAN joiner, D-NET-178).
	assert_eq(resources.mount_runtime(root, "doesnotexist"), OK)
	assert_eq(resources.get_expansion(), "",
		"A silently-unmounted expansion must never be reported as mounted.")
	assert_eq(resources.read_file("baseonly.trn").get_string_from_utf8(), "base trn")
	assert_false(resources.has_file("exponly.3di"), "and the expansion archive really is gone")


func test_runtime_remount_in_place_switches_expansion() -> void:
	# Re-mounting a LIVE runtime root replaces its data set rather than layering onto it:
	# the LAN joiner reconciles an already-mounted root against the host's authoritative
	# expansion (D-NET-178), so nothing the previous mount indexed or decoded may survive.
	var root := _make_flat_root("remount_expansion")
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox01"))
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "shared.env", "bytes": "base env"},
		{"name": "baseonly.trn", "bytes": "base trn"},
		{"name": "briefing.pcx", "bytes": _solid_test_pcx(Color.RED)},
	])
	WorldFixture.write_pff(self, root.path_join("expansion/jox01/jox01.pff"), [
		{"name": "shared.env", "bytes": "exp env"},
		{"name": "exponly.3di", "bytes": "exp model"},
		{"name": "briefing.pcx", "bytes": _solid_test_pcx(Color.BLUE)},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	assert_true(resources.is_runtime_mount())
	assert_eq(resources.get_expansion(), "")
	assert_eq(resources.read_file("shared.env").get_string_from_utf8(), "base env")
	assert_false(resources.has_file("exponly.3di"))
	var base_texture: Texture2D = resources.load_texture("briefing.pcx")
	assert_not_null(base_texture)
	if base_texture != null:
		assert_true(base_texture.get_image().get_pixel(0, 0).is_equal_approx(Color.RED))

	# Same object, no clear() in between.
	assert_eq(resources.mount_runtime(root, "jox01"), OK)
	assert_true(resources.is_runtime_mount(), "A remount stays a runtime mount.")
	assert_eq(resources.get_expansion(), "jox01", "get_expansion() reports the data set the remount landed on.")
	assert_eq(resources.read_file("shared.env").get_string_from_utf8(), "exp env",
		"The expansion archive wins after the remount; the previous mount's entry is gone.")
	assert_eq(resources.read_file("baseonly.trn").get_string_from_utf8(), "base trn", "Base archives stay mounted.")
	assert_true(resources.has_file("exponly.3di"), "Expansion-only entries appear after the remount.")
	var expansion_texture: Texture2D = resources.load_texture("briefing.pcx")
	assert_not_null(expansion_texture)
	if expansion_texture != null:
		assert_true(
			expansion_texture.get_image().get_pixel(0, 0).is_equal_approx(Color.BLUE),
			"The decoded-texture cache must not serve the previous mount's image."
		)


func test_is_runtime_mount_discriminates_runtime_from_editor_mounts() -> void:
	# The discriminator for a caller re-mounting a root it did not create: only a live
	# mount_runtime() mount may be re-mounted with mount_runtime.
	var root := _make_flat_root("mount_kind")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "loose trn")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [{"name": "Bravo.env", "bytes": "archived env"}])

	var resources := ResourceRoot.new()
	assert_false(resources.is_runtime_mount(), "A never-mounted root is not a runtime mount.")
	assert_eq(resources.set_root_dir(root), OK)
	assert_false(resources.is_runtime_mount(), "An editor loose mount is not a runtime mount.")
	assert_eq(resources.mount_runtime(root), OK)
	assert_true(resources.is_runtime_mount())

	var loose_only := _make_flat_root("mount_kind_loose_only")
	TestFs.write_text(self, loose_only.path_join("Alpha.TRN"), "loose trn")
	assert_eq(resources.mount_runtime(loose_only), ERR_FILE_NOT_FOUND)
	assert_false(resources.is_runtime_mount(), "A failed runtime mount leaves no runtime mount behind.")

	assert_eq(resources.mount_runtime(root), OK)
	resources.clear()
	assert_false(resources.is_runtime_mount(), "clear() drops the runtime mount.")


func test_boot_manifest_reports_missing_fatal_resources() -> void:
	# ENG-6: the witnessed boot manifest (engine/base/gameprofile required_resources,
	# docs/required-resources.md) probed against the mounted root. Only the
	# individually-fatal FILE rows are probed; the boot-archive-table trio is
	# mount_runtime's own gate [orig: fatal check @ 0x4a6f44].
	var root := _make_flat_root("boot_manifest")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "gametext.bin", "bytes": "strings"},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var missing := resources.list_missing_boot_resources()
	assert_false("gametext.bin" in missing, "A mounted fatal-set file is not reported missing.")
	assert_true("vmacros.bin" in missing, "Missing fatal-set files are named.")
	assert_true("keyhelp.bin" in missing, "Missing fatal-set files are named.")
	assert_true("items.def" in missing, "Missing fatal-set files are named.")
	assert_true("main.mnu" in missing, "The menu-phase fatal is probed too.")
	assert_false("resource.pff" in missing, "Archive-table rows are the mount gate's job, never probed per-file.")
	assert_true(resources.boot_resource_failure_text("gametext.bin").contains("Unable to load game strings"),
		"Failure text quotes the witnessed retail behavior.")
	assert_eq(resources.boot_resource_failure_text("nonsense.xyz"), "", "Unknown names have no failure text.")
	assert_eq(ResourceRoot.new().list_missing_boot_resources().size(), 0,
		"An unmounted root probes nothing.")


func test_resource_root_list_expansions() -> void:
	var root := _make_flat_root("list_expansions")
	# Two valid expansions (a subdir holding a matching <name>.pff) plus one incomplete
	# subdir (no matching pff) that must be excluded.
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox01"))
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox02"))
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/incomplete"))
	WorldFixture.write_pff(self, root.path_join("expansion/jox01/jox01.pff"), [{"name": "a.3di", "bytes": "x"}])
	WorldFixture.write_pff(self, root.path_join("expansion/jox02/jox02.pff"), [{"name": "b.3di", "bytes": "y"}])
	TestFs.write_text(self, root.path_join("expansion/incomplete/readme.txt"), "no pff here")

	# list_expansions does not require the root to be mounted (the UI lists before mounting).
	var expansions := ResourceRoot.new().list_expansions(root)
	assert_eq(expansions.size(), 2, "Only subdirs with a matching <name>.pff are expansions.")
	assert_true(expansions.has("jox01"))
	assert_true(expansions.has("jox02"))
	assert_false(expansions.has("incomplete"), "A subdir without <name>.pff is not an expansion.")

	# A root with no expansion/ dir yields an empty list (the UI hides the control).
	var base_only := _make_flat_root("list_expansions_base")
	assert_eq(ResourceRoot.new().list_expansions(base_only).size(), 0)


func test_resource_root_loads_dds_from_pff() -> void:
	# A DDS that exists only inside a .pff has no filesystem path, so it must decode from
	# the archived bytes (Godot 4.6 Image.load_dds_from_buffer). Round-trip a real image
	# through Godot's own DDS encoder for a genuinely decodable fixture.
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.2, 0.4, 0.8, 1.0))
	var dds := image.save_dds_to_buffer()
	assert_gt(dds.size(), 4, "save_dds_to_buffer should produce DDS bytes.")

	var root := _make_flat_root("dds_pff")
	# resource.pff: only the witnessed boot-table archives mount at runtime (D-VFS-2).
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [{"name": "swatch.dds", "bytes": dds}])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var tex: Texture2D = resources.load_texture("swatch.dds")
	assert_not_null(tex, "A DDS resident only inside a .pff should decode to a texture.")
	if tex != null:
		assert_eq(tex.get_width(), 4)
		assert_eq(tex.get_height(), 4)


func test_packed_texture_collapses_compound_authored_extensions() -> void:
	# Retail Wwall models author names such as Jbark_2.dds.tga while resource.pff
	# stores Jbark_2.dds. The shared candidate generator must retain that inner
	# recognized filename before probing alternate extensions.
	var inner_image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	inner_image.fill(Color.RED)
	var fallback_image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	fallback_image.fill(Color.BLUE)
	var root := _make_flat_root("compound_texture_extension")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "swatch.dds", "bytes": inner_image.save_dds_to_buffer()},
		{"name": "swatch.dds.dds", "bytes": fallback_image.save_dds_to_buffer()},
	])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var tex: Texture2D = resources.load_texture("swatch.dds.tga")
	assert_not_null(
		tex,
		"A compound authored .dds.tga reference should resolve the packaged .dds.",
	)
	if tex != null:
		assert_true(
			tex.get_image().get_pixel(0, 0).is_equal_approx(Color.RED),
			"The exposed .dds filename must win before generic extension fallbacks.",
		)


func test_resolve_file_snapshots_per_cache_epoch() -> void:
	# resolve_file reads a one-walk-per-epoch snapshot of the root directory, matching
	# the index-backed listings: on-disk edits surface via scan/mount or an explicit
	# bump_cache_epoch(), never mid-epoch.
	var root := _make_flat_root("resolve_epoch")
	TestFs.write_text(self, root.path_join("Alpha.TRN"), "trn")

	var resources := ResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)
	assert_eq(_norm(resources.resolve_file("alpha.trn")), _norm(root.path_join("Alpha.TRN")))

	TestFs.write_text(self, root.path_join("Bravo.TRN"), "trn")
	assert_eq(resources.resolve_file("bravo.trn"), "", "A file added after the mount stays invisible until the epoch moves.")

	ResourceRoot.bump_cache_epoch()
	assert_eq(_norm(resources.resolve_file("bravo.trn")), _norm(root.path_join("Bravo.TRN")), "bump_cache_epoch() re-reads the directory.")

	TestFs.write_text(self, root.path_join("Charlie.TRN"), "trn")
	assert_eq(resources.set_root_dir(root), OK)
	assert_eq(_norm(resources.resolve_file("charlie.trn")), _norm(root.path_join("Charlie.TRN")), "A remount/rescan re-reads the directory.")


func test_packed_texture_loads_share_one_decode_per_epoch() -> void:
	# PFF-resident textures decode once per epoch; repeat load_texture calls hand out
	# the same Texture2D instance (consumers never mutate textures — the loose path
	# has shared its decode cache the same way since the resolver caches landed).
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.6, 0.3, 0.1, 1.0))
	var root := _make_flat_root("dds_pff_cache")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [{"name": "swatch.dds", "bytes": image.save_dds_to_buffer()}])

	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var first: Texture2D = resources.load_texture("swatch.dds")
	var second: Texture2D = resources.load_texture("swatch.dds")
	assert_not_null(first)
	assert_true(first == second, "Repeat packed loads should return the cached texture, not a fresh decode.")
	assert_null(resources.load_texture("missing.dds"), "Misses stay misses when cached.")
	assert_null(resources.load_texture("missing.dds"))

	ResourceRoot.bump_cache_epoch()
	var after_bump: Texture2D = resources.load_texture("swatch.dds")
	assert_not_null(after_bump, "An epoch bump must not lose the texture, only the cache.")


func test_clear_releases_cached_texture_before_render_server_shutdown() -> void:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.6, 0.3, 0.1, 1.0))
	var root := _make_flat_root('dds_clear_cache')
	WorldFixture.write_pff(self, root.path_join('resource.pff'), [{
		'name': 'swatch.dds',
		'bytes': image.save_dds_to_buffer(),
	}])
	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root), OK)
	var texture: Texture2D = resources.load_texture('swatch.dds')
	assert_not_null(texture)
	var weak_texture: WeakRef = weakref(texture)
	texture = null
	assert_not_null(weak_texture.get_ref(),
		'The live resource root owns its decoded texture cache.')

	resources.clear()

	assert_null(weak_texture.get_ref(),
		'clear() must release cached ImageTextures while RenderingServer is alive.')


func after_each() -> void:
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join("opennova_resource_root_contract"))


func _make_flat_root(name: String) -> String:
	var root := OS.get_cache_dir().path_join("opennova_resource_root_contract").path_join("%s_%d" % [name, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	return root


func _solid_test_pcx(color: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(128)
	bytes[0] = 0x0A  # manufacturer
	bytes[1] = 5     # version
	bytes[2] = 1     # RLE
	bytes[3] = 8     # bits per pixel
	# xmin/ymin = 0, xmax/ymax = 1 (little-endian u16 pairs at 4..11)
	bytes[8] = 1
	bytes[10] = 1
	bytes[65] = 1    # planes
	bytes[66] = 2    # bytes per line
	for _pixel in range(4):
		bytes.append(1)  # palette index 1; values below 0xC0 are RLE literals
	bytes.append(0x0C)  # palette marker
	for index in range(256):
		if index == 1:
			bytes.append(int(color.r * 255.0))
			bytes.append(int(color.g * 255.0))
			bytes.append(int(color.b * 255.0))
		else:
			bytes.append(0)
			bytes.append(0)
			bytes.append(0)
	return bytes


func _norm(path: String) -> String:
	return path.replace("\\", "/").to_lower()


func _normal_test_tga(blue: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + 64)
	bytes[2] = 2
	bytes.encode_u16(12, 4)
	bytes.encode_u16(14, 4)
	bytes[16] = 32
	bytes[17] = 40
	for i in range(16):
		bytes[18 + i * 4] = blue
		bytes[19 + i * 4] = 70
		bytes[20 + i * 4] = 90
		bytes[21 + i * 4] = 128
	return bytes


func test_material_normals_choose_exact_sources_and_preserve_blue_as_alpha() -> void:
	var root := _make_flat_root("normal_sources")
	var source := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	source.fill(Color8(50, 70, 121, 128))
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "brick.dds", "bytes": source.save_dds_to_buffer()},
		{"name": "brick.tga", "bytes": _normal_test_tga(61)},
		{"name": "ready.mdt", "bytes": source.save_dds_to_buffer()},
	])
	TestFs.write_bytes(self, root.path_join("brick.tga"), _normal_test_tga(233))
	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root, "", false), OK)
	var packed: Texture2D = resources.load_material_texture("brick.tga", 4)
	assert_not_null(packed)
	assert_eq(packed.get_image().get_pixel(0, 0), Color8(127, 127, 255, 121))
	assert_eq(resources.load_material_texture("ready.mdt", 4).get_image().get_pixel(0, 0), Color8(50, 70, 121, 128))
	# A missing MDT is the port's BOUNDED fallback, not the witnessed retail
	# result: retail walks the null-data kernel for the absent file (jz
	# @0x58c586), D3DX corrects the 0x0 request to 1x1 (@0x690a2b / @0x690a37)
	# and the nonzero handle skips the checkerboard (@0x5b17f2), binding a
	# never-filled 1x1 texture that ADR 0003 does not reproduce.
	var missing: Texture2D = resources.load_material_texture("brick.mdt", 4)
	assert_eq(missing.get_width(), 128,
			"A missing MDT never aliases the existing TGA or DDS; the port binds its bounded checkerboard fallback.")
	assert_eq(missing.get_image().get_pixel(0, 0), Color8(48, 48, 48),
			"the missing-MDT fallback is the port's checkerboard, not retail's unfilled 1x1 texture")
	assert_eq(resources.mount_runtime(root, "", true), OK)
	var loose: Texture2D = resources.load_material_texture("brick.tga", 4)
	assert_eq(loose.get_image().get_pixel(0, 0), Color8(127, 127, 255, 233))
	resources.clear()


func _solid_test_tga(r: int, g: int, b: int) -> PackedByteArray:
	var bytes := _normal_test_tga(b)
	for i in range(16):
		bytes[19 + i * 4] = g
		bytes[20 + i * 4] = r
		bytes[21 + i * 4] = 255
	return bytes


func _mip_coloured_dds() -> PackedByteArray:
	# 4x4 red level 0 with an authored green 2x2 and blue 1x1 below it.
	var data := PackedByteArray()
	for side_colour in [[4, Color.RED], [2, Color.GREEN], [1, Color.BLUE]]:
		var colour: Color = side_colour[1]
		for _texel in side_colour[0] * side_colour[0]:
			data.append_array(PackedByteArray([colour.r8, colour.g8, colour.b8, 255]))
	return Image.create_from_data(4, 4, true, Image.FORMAT_RGBA8, data).save_dds_to_buffer()


func test_material_diffuse_resolves_exactly_the_file_retail_loads() -> void:
	# Texture_LoadByNameWithChannel (retail): an existing DDS sibling wins over
	# the named TGA (unless a loose file wins under loose-first), keeps its
	# authored mip levels, and nothing else is probed: no "_O" suffix, no
	# alternate extension. A compound "x.dds.tga" name queries "x.dds".
	var root := _make_flat_root("material_diffuse_sources")
	WorldFixture.write_pff(self, root.path_join("resource.pff"), [
		{"name": "wall.dds", "bytes": _mip_coloured_dds()},
		{"name": "wall.tga", "bytes": _solid_test_tga(0, 0, 255)},
		{"name": "trim_O.tga", "bytes": _solid_test_tga(0, 255, 0)},
		{"name": "bark.dds", "bytes": _mip_coloured_dds()},
	])
	TestFs.write_bytes(self, root.path_join("wall.tga"), _solid_test_tga(255, 255, 0))
	var resources := ResourceRoot.new()
	assert_eq(resources.mount_runtime(root, "", false), OK)
	var wall: Texture2D = resources.load_material_texture("wall.tga", 0)
	assert_not_null(wall)
	var image := wall.get_image()
	assert_eq(image.get_pixel(0, 0), Color8(255, 0, 0), "the DDS sibling wins over wall.tga")
	assert_true(image.has_mipmaps(), "the DDS keeps a mip chain")
	var level1 := image.get_mipmap_offset(1)
	var data := image.get_data()
	assert_eq(Color8(data[level1], data[level1 + 1], data[level1 + 2]), Color8(0, 255, 0),
			"the authored level 1 survives instead of a regenerated red one")
	var trim: Texture2D = resources.load_material_texture("trim.tga", 0)
	assert_eq(trim.get_width(), 128, "a missing trim.tga binds the checkerboard, never trim_O.tga")
	assert_eq(trim.get_image().get_pixel(0, 0), Color8(48, 48, 48))
	var bark: Texture2D = resources.load_material_texture("bark.dds.tga", 0)
	assert_eq(bark.get_image().get_pixel(0, 0), Color8(255, 0, 0),
			"bark.dds.tga queries bark.dds")
	assert_eq(resources.mount_runtime(root, "", true), OK)
	var loose: Texture2D = resources.load_material_texture("wall.tga", 0)
	assert_eq(loose.get_image().get_pixel(0, 0), Color8(255, 255, 0),
			"under loose-first an existing loose wall.tga takes the plain TGA path")
	resources.clear()
