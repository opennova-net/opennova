class_name ItemDbFixture
extends RefCounted

## An ItemDatabase over the committed compact items table
## (fixtures/def/items.def) with a test's own rows appended, staged under
## res://.godot because ItemDatabase loads by path. A type id resolves to its
## first row, so appended rows only add ids the table lacks; a test that must
## change an existing row edits the fixture text before staging it.

const FIXTURE_ITEMS := "res://../fixtures/def/items.def"

## One deploy-selectable pool-1 spawn zone: the mission stores id 1359 and the
## promotion applies ITEM_ID_OFFSET, so the ItemDef identity is 101359.
const SPAWN_ZONE_ROW := """begin "Spawn Zone Fixture"
  id 101359
  type object
  graphic MrkAlpha
  sid spawn_zone_fixture
  hp 100
  attrib: SpawnPoint
end
"""


## The committed table's text, CRLF-normalised; "" (with a failed assert) when
## it cannot be read.
static func fixture_text(test: GutTest) -> String:
	var file := FileAccess.open(ProjectSettings.globalize_path(FIXTURE_ITEMS), FileAccess.READ)
	test.assert_not_null(file, "fixtures/def/items.def opens")
	if file == null:
		return ""
	var text := file.get_as_text().replace("\r\n", "\n")
	file.close()
	return text


## The committed table plus `rows`, staged as res://.godot/<file_name>.
static func with_rows(test: GutTest, file_name: String, rows: String) -> ItemDatabase:
	var base := fixture_text(test)
	if base.is_empty():
		return null
	return load_with(test, file_name, base, rows)


## Stage `base` (the fixture text, edited by the caller) followed by `rows` as
## res://.godot/<file_name> and load it; null when the file cannot be written.
static func load_with(test: GutTest, file_name: String, base: String,
		rows: String) -> ItemDatabase:
	var path := stage_path(file_name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	test.assert_not_null(file, "the staged items table opens for writing")
	if file == null:
		return null
	file.store_string(TestFs.crlf(base if base.ends_with("\n") else base + "\n"))
	file.store_string(TestFs.crlf(rows))
	file.close()
	var db := ItemDatabase.new()
	test.assert_eq(db.load(path), OK, "the staged items table loads")
	return db


## The absolute path a staged table lives at.
static func stage_path(file_name: String) -> String:
	return ProjectSettings.globalize_path("res://.godot/" + file_name)


## Remove a staged table (a no-op when it was never staged).
static func release(file_name: String) -> void:
	var path := stage_path(file_name)
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)
