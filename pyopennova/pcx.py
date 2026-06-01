from __future__ import annotations

from dataclasses import dataclass


class PcxDecodeError(ValueError):
    pass


@dataclass(frozen=True)
class PcxRgbImage:
    width: int
    height: int
    rgb: bytes


def decode_pcx_rgb(data: bytes) -> PcxRgbImage:
    width, height, planes, bytes_per_line = _parse_header(data)
    if planes == 1:
        return _decode_indexed(data, width, height, bytes_per_line)
    if planes == 3:
        return _decode_truecolor(data, width, height, bytes_per_line)
    raise PcxDecodeError("Unsupported PCX plane count")


def _parse_header(data: bytes) -> tuple[int, int, int, int]:
    if len(data) < 128:
        raise PcxDecodeError("PCX header truncated")
    if data[0] != 0x0A:
        raise PcxDecodeError("Bad PCX manufacturer byte")
    if data[2] != 1:
        raise PcxDecodeError("Unsupported PCX encoding")
    if data[3] != 8:
        raise PcxDecodeError("Unsupported PCX bit depth")

    xmin = data[4] | (data[5] << 8)
    ymin = data[6] | (data[7] << 8)
    xmax = data[8] | (data[9] << 8)
    ymax = data[10] | (data[11] << 8)
    width = xmax - xmin + 1
    height = ymax - ymin + 1
    if width <= 0 or height <= 0 or width > 4096 or height > 4096:
        raise PcxDecodeError("PCX dimensions out of range")

    planes = data[65]
    bytes_per_line = data[66] | (data[67] << 8)
    if bytes_per_line < width:
        bytes_per_line = width
    return width, height, planes, bytes_per_line


def _decode_indexed(data: bytes, width: int, height: int, bytes_per_line: int) -> PcxRgbImage:
    if len(data) < 128 + 769:
        raise PcxDecodeError("PCX palette missing")
    if data[-769] != 0x0C:
        raise PcxDecodeError("PCX 256-color palette marker missing")

    pos = 128
    indices = bytearray(width * height)
    for y in range(height):
        row, pos = _decode_scanline(data, pos, bytes_per_line)
        start = y * width
        indices[start:start + width] = row[:width]

    palette = data[-768:]
    rgb = bytearray(width * height * 3)
    for i, color_index in enumerate(indices):
        pal = color_index * 3
        dst = i * 3
        rgb[dst + 0] = palette[pal + 0]
        rgb[dst + 1] = palette[pal + 1]
        rgb[dst + 2] = palette[pal + 2]
    return PcxRgbImage(width=width, height=height, rgb=bytes(rgb))


def _decode_truecolor(data: bytes, width: int, height: int, bytes_per_line: int) -> PcxRgbImage:
    pos = 128
    rgb = bytearray(width * height * 3)
    for y in range(height):
        row, pos = _decode_scanline(data, pos, bytes_per_line * 3)
        for x in range(width):
            dst = (y * width + x) * 3
            rgb[dst + 0] = row[x]
            rgb[dst + 1] = row[bytes_per_line + x]
            rgb[dst + 2] = row[bytes_per_line * 2 + x]
    return PcxRgbImage(width=width, height=height, rgb=bytes(rgb))


def _decode_scanline(data: bytes, pos: int, count: int) -> tuple[bytes, int]:
    row = bytearray()
    while len(row) < count and pos < len(data):
        value = data[pos]
        pos += 1
        if (value & 0xC0) == 0xC0:
            run = value & 0x3F
            if pos >= len(data):
                raise PcxDecodeError("PCX RLE run truncated")
            row.extend([data[pos]] * run)
            pos += 1
        else:
            row.append(value)
    if len(row) < count:
        raise PcxDecodeError("PCX scanline truncated")
    return bytes(row[:count]), pos
