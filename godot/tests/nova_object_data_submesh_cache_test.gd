extends GutTest

# NovaObjectData's memoized submesh builds: build_lod_submeshes hands every
# caller SHARED ArrayMesh refs (one mesh set per (lod, skeletal, bones) key per
# data instance), with fresh entry dictionaries so callers can't taint the
# cache. The mission placer shares one NovaObjectData per graphic, so N
# animated entities stop paying N mesh builds - the recorded baseline's
# dominant cost (docs/perf/mission-load-baseline.md).

const SHED := "res://../fixtures/threedi/3di3/Shed.3di"
const CHARMODEL := "res://../fixtures/threedi/3di3/CharModel.3di"
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")


func _open(path: String) -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK,
		"fixture should open: %s" % path)
	return data


func _mesh_rids(submeshes: Array) -> Array:
	var rids: Array = []
	for entry in submeshes:
		rids.append(((entry as Dictionary).get("mesh") as Mesh).get_rid())
	return rids


# All MeshInstance3D descendants in tree order (rebuild adds them under the
# per-part Robj container nodes in submesh order, so two models built from the
# same data list pairwise-comparable instances).
func _mesh_instances(node: Node) -> Array:
	var out: Array = []
	for child in node.get_children():
		if child is MeshInstance3D:
			out.append(child)
		out.append_array(_mesh_instances(child))
	return out


func test_repeat_builds_share_the_same_meshes() -> void:
	var data := _open(SHED)
	var first := data.build_lod_submeshes(0)
	var second := data.build_lod_submeshes(0)
	assert_false(first.is_empty())
	assert_eq(_mesh_rids(first), _mesh_rids(second),
		"two builds of the same key return the SAME ArrayMesh instances")


func test_two_models_from_one_data_share_meshes_but_not_materials() -> void:
	var data := _open(SHED)
	var a: Node3D = add_child_autofree(NovaObjectModelScript.new())
	var b: Node3D = add_child_autofree(NovaObjectModelScript.new())
	a.set_object_data(data)
	b.set_object_data(data)

	var a_meshes := _mesh_instances(a)
	var b_meshes := _mesh_instances(b)
	assert_false(a_meshes.is_empty(), "the model built render meshes")
	assert_eq(a_meshes.size(), b_meshes.size(), "both models built the same parts")
	for i in range(mini(a_meshes.size(), b_meshes.size())):
		var mi_a := a_meshes[i] as MeshInstance3D
		var mi_b := b_meshes[i] as MeshInstance3D
		assert_eq(mi_a.mesh.get_rid(), mi_b.mesh.get_rid(),
			"both models render the SAME mesh (the cache's point)")
		if mi_a.material_override != null or mi_b.material_override != null:
			assert_ne(mi_a.material_override, mi_b.material_override,
				"...while materials stay PER-INSTANCE (runtime params diverge)")


func test_cache_keys_separate_static_and_fake_skin_builds() -> void:
	var data := _open(SHED)
	var static_rids := _mesh_rids(data.build_lod_submeshes(0))
	var skinned := data.build_lod_submeshes(0, true, 8)
	var skinned_rids := _mesh_rids(skinned)
	assert_false(static_rids.is_empty())
	for rid in skinned_rids:
		assert_false(static_rids.has(rid),
			"fake-skin builds carry different vertex arrays - never the static meshes")
	assert_eq(_mesh_rids(data.build_lod_submeshes(0, true, 8)), skinned_rids,
		"...and repeat skeletal builds share within their own key")


func test_document_edits_invalidate_the_cache() -> void:
	var data := _open(SHED)
	var before := _mesh_rids(data.build_lod_submeshes(0))
	assert_true(data.set_material_field(0, "rgb_gen_rate", 1.0),
		"a material edit goes through the OED setter funnel")
	var after := _mesh_rids(data.build_lod_submeshes(0))
	assert_false(before.is_empty())
	for rid in after:
		assert_false(before.has(rid), "any document edit rebuilds fresh meshes")


func test_reload_invalidates_the_cache() -> void:
	var data := _open(SHED)
	var before := _mesh_rids(data.build_lod_submeshes(0))
	assert_eq(data.open_file(ProjectSettings.globalize_path(SHED)), OK)
	var after := _mesh_rids(data.build_lod_submeshes(0))
	for rid in after:
		assert_false(before.has(rid), "reopening rebuilds fresh meshes")


func test_callers_cannot_taint_the_cache() -> void:
	var data := _open(SHED)
	var first := data.build_lod_submeshes(0)
	# Vandalize everything a caller can reach EXCEPT the shared meshes.
	(first[0] as Dictionary)["robj_index"] = 999
	(first[0] as Dictionary).erase("abs")
	first.clear()
	var second := data.build_lod_submeshes(0)
	assert_false(second.is_empty(), "the cached entry list survives a caller clearing its copy")
	assert_ne(int((second[0] as Dictionary).get("robj_index", -1)), 999,
		"...and entry edits never reach the cache (fresh dictionaries per call)")


func test_lod_round_trip_returns_the_cached_meshes() -> void:
	# The object editor switches LODs freely; coming back must not pay a
	# rebuild (and must return the SAME meshes - the cache held them).
	var data := _open(CHARMODEL)
	var lod0 := _mesh_rids(data.build_lod_submeshes(0))
	data.build_lod_submeshes(1)  # may be empty on single-LOD fixtures; harmless
	assert_eq(_mesh_rids(data.build_lod_submeshes(0)), lod0,
		"returning to LOD 0 reuses the cached meshes")


func test_material_edit_on_a_shared_data_does_not_leak_across_models() -> void:
	var data := _open(SHED)
	var a: Node3D = add_child_autofree(NovaObjectModelScript.new())
	var b: Node3D = add_child_autofree(NovaObjectModelScript.new())
	a.set_object_data(data)
	b.set_object_data(data)
	# Snapshot the pre-edit meshes: the post-edit assertions below would all hold
	# on this initial shared state too, so PROVING the deferred rebuild + cache
	# invalidation happened requires the post-edit RIDs to be disjoint from these.
	var pre_edit_rids: Array = []
	for mi in _mesh_instances(a):
		pre_edit_rids.append((mi as MeshInstance3D).mesh.get_rid())
	assert_false(pre_edit_rids.is_empty(), "the models built render meshes before the edit")
	# An edit notifies object_changed (deferred) -> both models rebuild from the
	# fresh cache; their material overrides must remain distinct objects.
	data.set_material_field(0, "rgb_gen_rate", 2.0)
	await get_tree().process_frame
	var a_meshes := _mesh_instances(a)
	var b_meshes := _mesh_instances(b)
	assert_false(a_meshes.is_empty(), "models rebuilt after the edit")
	for i in range(mini(a_meshes.size(), b_meshes.size())):
		var mi_a := a_meshes[i] as MeshInstance3D
		var mi_b := b_meshes[i] as MeshInstance3D
		assert_false(pre_edit_rids.has(mi_a.mesh.get_rid()),
			"the deferred rebuild really happened: post-edit meshes are FRESH, not the pre-edit set")
		assert_eq(mi_a.mesh.get_rid(), mi_b.mesh.get_rid(),
			"post-edit rebuilds still share the (fresh) meshes")
		if mi_a.material_override != null:
			assert_ne(mi_a.material_override, mi_b.material_override,
				"...with materials still per-instance")
