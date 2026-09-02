class_name NativeModelFixture
extends RefCounted

## The synthetic-.3di staging helpers the two-sim net tests share: read a
## committed fixture model, rewrite one USRP record's name or authored
## position in place (threedi parse_usrp: 48-byte records, the 16-byte name
## at record offset +32, the 16.16 position ints 32 bytes before it), and
## write the result under a test-owned native model directory. Every
## function is pure over its bytes; an empty PackedByteArray is the "no
## match" result so a caller's assert names the miss.


static func repo_file_bytes(res_path: String) -> PackedByteArray:
	var file := FileAccess.open(
			ProjectSettings.globalize_path(res_path), FileAccess.READ)
	if file == null:
		return PackedByteArray()
	var bytes := file.get_buffer(file.get_length())
	file.close()
	return bytes


static func pattern_offset(data: PackedByteArray, pattern: String) -> int:
	var wanted := pattern.to_ascii_buffer()
	if wanted.is_empty() or data.size() < wanted.size():
		return -1
	var at := data.find(wanted[0], 0)
	while at >= 0 and at + wanted.size() <= data.size():
		if data.slice(at, at + wanted.size()) == wanted:
			return at
		at = data.find(wanted[0], at + 1)
	return -1


## Rewrite one USRP record's 16-byte name field in place. Empty result = no match.
static func with_renamed_user_point(data: PackedByteArray, old_name: String,
		new_name: String) -> PackedByteArray:
	var offset := pattern_offset(data, old_name)
	var replacement := new_name.to_ascii_buffer()
	if offset < 0 or replacement.size() > 16:
		return PackedByteArray()
	for i in range(16):
		data[offset + i] = replacement[i] if i < replacement.size() else 0
	return data


## Overwrite one USRP record's authored 16.16 position ints (record base sits
## 32 bytes before the name field).
static func with_user_point_position(data: PackedByteArray, name: String,
		raw_x: int, raw_y: int, raw_z: int) -> PackedByteArray:
	var offset := pattern_offset(data, name)
	if offset < 32:
		return PackedByteArray()
	data.encode_s32(offset - 32, raw_x)
	data.encode_s32(offset - 28, raw_y)
	data.encode_s32(offset - 24, raw_z)
	return data


static func write_native_model(dir: String, name: String, bytes: PackedByteArray) -> bool:
	if bytes.is_empty():
		return false
	var file := FileAccess.open(dir.path_join(name), FileAccess.WRITE)
	if file == null:
		return false
	file.store_buffer(bytes)
	file.close()
	return true


## Drain the local player's weapon-switch events and apply each one whose
## weapon the test's definition table knows (the emplaced .50 keeps its
## mounted flag).
static func apply_weapon_switch_events(sim: Simulation,
		defs_by_name: Dictionary) -> void:
	for value: PlayerWeaponEvent in sim.drain_local_player_weapon_events():
		var name := value.switch_to_weapon
		if name.is_empty() or not defs_by_name.has(name):
			continue
		sim.set_local_player_weapon(
				defs_by_name[name], {}, name == "WPN_EMPLCD50NA")
