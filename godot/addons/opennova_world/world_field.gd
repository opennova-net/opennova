@tool
class_name WorldField
extends RefCounted
## One editable native field: the document that owns it, how the Inspector
## presents it, and the typed read and write of its value. The edit session,
## the Inspector plugin, the property widget and the undo action all consult
## this table instead of switching on a field ordinal. Adding a field is one
## row in [method _build] plus its read and write functions.

## The native documents a session opens, in the order it names, snapshots and
## saves them: the BMS, its TRN, its base ENV.
enum Document { MISSION, TERRAIN, ENVIRONMENT }
## Inspector presentation. The widget also fixes the value type the session
## validates: TIME_TEXT and INT_SPIN carry an int, FLOAT_SPIN a float, CHECK a
## bool, FILENAME_TEXT a bare filename present in the world's game data.
enum Widget { TIME_TEXT, FILENAME_TEXT, INT_SPIN, FLOAT_SPIN, CHECK }
## Every editable field. The table is indexed by this id.
enum Id { START_TIME, SKY_MAP_1, SKY_MAP_2, SKY_HEIGHT, FOLIAGE_GRAPHIC, FOLIAGE_MATCH, FOLIAGE_SHADOW }
## The BMS start time counts 1/256 hours; the widget converts it to HH:MM.
const START_TIME_UNITS_PER_HOUR := 256
const HOURS_PER_DAY := 24
const MINUTES_PER_HOUR := 60

var id: Id
var document: Document
## The Inspector section heading, shown once before the section's first field.
var group: String
var label: String
## A slotted field addresses one of the terrain's foliage definitions per edit;
## the Inspector repeats it per slot and undo actions name the slot.
var slotted := false
var widget: Widget
var min_value := 0.0
var max_value := 0.0
var step := 1.0
var placeholder := ""
## Appended to a FILENAME_TEXT value that carries no extension.
var default_extension := ""
## The message for a value outside the widget's type or range.
var range_message := ""
## read(mission: MissionData, terrain: TerrainData, environment: EnvFile, slot: int) -> Variant
var read: Callable
## write(mission: MissionData, terrain: TerrainData, environment: EnvFile, slot: int, value: Variant) -> void
var write: Callable

static var _table: Array[WorldField] = []


static func all() -> Array[WorldField]:
	if _table.is_empty():
		_table = _build()
	return _table


static func spec(field: Id) -> WorldField:
	return all()[field]


static func for_document(document: Document) -> Array[WorldField]:
	var fields: Array[WorldField] = []
	for field in all():
		if field.document == document:
			fields.append(field)
	return fields


static func _build() -> Array[WorldField]:
	var table: Array[WorldField] = [
		_row(Id.START_TIME, Document.MISSION, "Mission", "Start time", Widget.TIME_TEXT,
				_read_start_time, _write_start_time)
				.ranged(0, HOURS_PER_DAY * START_TIME_UNITS_PER_HOUR - 1,
						"Start time must be between 00:00 and 23:59.").text("HH:MM"),
		_row(Id.SKY_MAP_1, Document.ENVIRONMENT, "Environment", "Sky map 1", Widget.FILENAME_TEXT,
				_read_sky_map_1, _write_sky_map_1).text("Asset filename"),
		_row(Id.SKY_MAP_2, Document.ENVIRONMENT, "Environment", "Sky map 2", Widget.FILENAME_TEXT,
				_read_sky_map_2, _write_sky_map_2).text("Asset filename"),
		_row(Id.SKY_HEIGHT, Document.ENVIRONMENT, "Environment", "Sky height", Widget.FLOAT_SPIN,
				_read_sky_height, _write_sky_height)
				.ranged(10, 500, "Sky height must be between 10 and 500."),
		_row(Id.FOLIAGE_GRAPHIC, Document.TERRAIN, "Foliage", "Graphic", Widget.FILENAME_TEXT,
				_read_foliage_graphic, _write_foliage_graphic)
				.per_slot().text("Asset filename", ".3di"),
		_row(Id.FOLIAGE_MATCH, Document.TERRAIN, "Foliage", "Map match", Widget.INT_SPIN,
				_read_foliage_match, _write_foliage_match)
				.per_slot().ranged(-1, 255, "Map match must be between -1 and 255."),
		_row(Id.FOLIAGE_SHADOW, Document.TERRAIN, "Foliage", "Cast shadow", Widget.CHECK,
				_read_foliage_shadow, _write_foliage_shadow)
				.per_slot().ranged(0, 1, "Shadow must be enabled or disabled."),
	]
	assert(table.size() == Id.size(), "every WorldField.Id has one row")
	for i in range(table.size()):
		assert(table[i].id == i, "the WorldField table is indexed by Id")
	return table


static func _row(field: Id, owner: Document, section: String, caption: String, kind: Widget,
		reader: Callable, writer: Callable) -> WorldField:
	var spec := WorldField.new()
	spec.id = field
	spec.document = owner
	spec.group = section
	spec.label = caption
	spec.widget = kind
	spec.read = reader
	spec.write = writer
	return spec


func per_slot() -> WorldField:
	slotted = true
	return self


func ranged(minimum: float, maximum: float, message: String) -> WorldField:
	min_value = minimum
	max_value = maximum
	range_message = message
	return self


func text(hint: String, extension: String = "") -> WorldField:
	placeholder = hint
	default_extension = extension
	return self


static func _read_start_time(mission: MissionData, _terrain: TerrainData, _environment: EnvFile, _slot: int) -> Variant:
	return mission.get_info().start_time


static func _write_start_time(mission: MissionData, _terrain: TerrainData, _environment: EnvFile, _slot: int, value: Variant) -> void:
	mission.set_header_int("start_time", int(value))


static func _read_sky_map_1(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int) -> Variant:
	return environment.sky_map1


static func _write_sky_map_1(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int, value: Variant) -> void:
	environment.sky_map1 = str(value)


static func _read_sky_map_2(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int) -> Variant:
	return environment.sky_map2


static func _write_sky_map_2(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int, value: Variant) -> void:
	environment.sky_map2 = str(value)


static func _read_sky_height(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int) -> Variant:
	return environment.sky_height


static func _write_sky_height(_mission: MissionData, _terrain: TerrainData, environment: EnvFile, _slot: int, value: Variant) -> void:
	environment.sky_height = float(value)


static func _read_foliage_graphic(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int) -> Variant:
	return _foliage(terrain, slot).graphic


static func _write_foliage_graphic(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int, value: Variant) -> void:
	var defs := terrain.get_foliage_defs()
	(defs[slot] as TerrainFoliageDef).graphic = str(value)
	_write_foliage(terrain, defs)


static func _read_foliage_match(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int) -> Variant:
	return _foliage(terrain, slot).match


static func _write_foliage_match(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int, value: Variant) -> void:
	var defs := terrain.get_foliage_defs()
	(defs[slot] as TerrainFoliageDef).match = int(value)
	_write_foliage(terrain, defs)


static func _read_foliage_shadow(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int) -> Variant:
	return _foliage(terrain, slot).shadow


static func _write_foliage_shadow(_mission: MissionData, terrain: TerrainData, _environment: EnvFile, slot: int, value: Variant) -> void:
	var defs := terrain.get_foliage_defs()
	(defs[slot] as TerrainFoliageDef).shadow = bool(value)
	_write_foliage(terrain, defs)


static func _foliage(terrain: TerrainData, slot: int) -> TerrainFoliageDef:
	return terrain.get_foliage_defs()[slot] as TerrainFoliageDef


## Fresh native records preserve the unexposed color and attribute fields.
static func _write_foliage(terrain: TerrainData, defs: Array) -> void:
	terrain.set_foliage_defs(defs)
