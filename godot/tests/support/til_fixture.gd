class_name TilFixture
extends RefCounted

## A minimal one-entry .til overlay the game-shell tests stage beside their
## fixture terrain: the "til0" header, one entry whose 16.16 x lands in the
## given cell, flag byte set.


static func bytes_for_cell(cell_x: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(28)
	bytes.encode_u32(0, 0x74696c30)
	bytes.encode_u32(4, 1)
	bytes.encode_u32(16, cell_x * (16 << 16))
	bytes.encode_u32(20, 0)
	bytes[24] = 1
	return bytes
