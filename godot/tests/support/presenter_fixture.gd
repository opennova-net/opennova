class_name PresenterFixture
extends RefCounted

## The rig the in-world presenter tests share (the armory, the deploy screen,
## the host-punt surfacing): reference fixtures staged under a scratch res://
## directory, a ResourceRoot over that directory, the synthetic anim root, and
## the WorldView fake the deploy-screen presenter reads. Every helper that
## asserts takes the GutTest so the check lands in the calling test's report.


## The screen's narrow world view, faked over a sim and the staged menu root
## (rule 11: a fake of the WorldView interface through its virtual hooks).
class FakeWorldView:
	extends WorldView
	var root: ResourceRoot
	var sim_value: Simulation

	func _sim() -> Simulation:
		return sim_value

	func _resource_root() -> ResourceRoot:
		return root


## Copy each reference fixture of `staged` (`<rel under fixtures/>` -> staged
## file name) into `res_dir`, creating the directory first.
static func stage(test: GutTest, res_dir: String, staged: Dictionary) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	if not DirAccess.dir_exists_absolute(dir):
		test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	for rel in staged:
		var target := dir.path_join(String(staged[rel]))
		var output := FileAccess.open(target, FileAccess.WRITE)
		test.assert_not_null(output, "temporary presenter fixture opens for write: %s" % target)
		if output != null:
			output.store_buffer(FileAccess.get_file_as_bytes(RetailData.fixture(String(rel))))
			output.close()


## Remove the staged files named in `targets` (what stage() or an authored
## fixture's stage() wrote), then the directory itself.
static func unstage(res_dir: String, targets: PackedStringArray) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	for staged_name in targets:
		var path := dir.path_join(String(staged_name))
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)


## A ResourceRoot over the staged directory.
static func root_over(test: GutTest, res_dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	test.assert_eq(root.set_root_dir(ProjectSettings.globalize_path(res_dir)), OK)
	return root


## A ResourceRoot over the synthetic anim fixtures (fixtures/anim).
static func anim_root(test: GutTest) -> ResourceRoot:
	return root_over(test, "res://../fixtures/anim")
