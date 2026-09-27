class_name TestPcx
extends RefCounted

## Minimal 8-bit palettized 2x2 PCX files (RLE, every pixel a literal) that the
## texture tests hand ResourceRoot.load_texture.


## Pixels take palette indices 0..3 over a grey-ramp palette (entry i = i,i,i).
static func ramp_2x2() -> PackedByteArray:
	var bytes := _header_2x2()
	for p in [0, 1, 2, 3]:  # 4 literal pixels (values < 0xC0 pass through RLE)
		bytes.append(p)
	bytes.append(0x0C)  # palette marker
	for i in range(256):
		bytes.append(i)  # r
		bytes.append(i)  # g
		bytes.append(i)  # b
	return bytes


## Every pixel takes palette index 1, which holds `color`; the rest of the
## palette is black.
static func solid_2x2(color: Color) -> PackedByteArray:
	var bytes := _header_2x2()
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


static func _header_2x2() -> PackedByteArray:
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
	return bytes
