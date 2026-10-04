class_name TestPff
extends RefCounted

## The minimal PFF3 fixture writer the GUT suite shares: a 20-byte header,
## 36-byte entries with 16-byte names, payloads packed after the table.
## `entries` rows are {name: String, bytes: PackedByteArray | String}; a
## String payload is stored as UTF-8. Names longer than 16 bytes are
## truncated and reported as ERR_INVALID_PARAMETER after the file is written
## (the fixture stays on disk so the failing test can still inspect it).
## Each entry's +12 word is stamped NEW_ENTRY_TIMESTAMP, as every retail entry
## carries a Unix time and the game's effect loaders skip an entry stamped 0
## (pff.h PFF_NEW_ENTRY_TIMESTAMP, D-VFS-12); a row's optional `timestamp`
## overrides it (0 is a third-party packer's unstamped entry, D-VFS-13).

## pff::PFF_NEW_ENTRY_TIMESTAMP: 2004-06-15 00:00 UTC.
const NEW_ENTRY_TIMESTAMP := 1087257600


static func write(path: String, entries: Array) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ERR_CANT_OPEN
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size
	var names_fit := true
	file.store_32(header_size)
	file.store_32(0x33464650)  # "PFF3"
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes := entry_bytes(entry)
		var name_bytes := String(entry.name).to_utf8_buffer()
		if name_bytes.size() > 16:
			names_fit = false
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(int(entry.get("timestamp", NEW_ENTRY_TIMESTAMP)))
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry_bytes(entry))
	file.close()
	return OK if names_fit else ERR_INVALID_PARAMETER


## The payload of one fixture row: raw bytes as given, a String as UTF-8.
static func entry_bytes(entry: Dictionary) -> PackedByteArray:
	return entry.bytes if entry.bytes is PackedByteArray else String(entry.bytes).to_utf8_buffer()
