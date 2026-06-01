from pathlib import Path

import pytest

from pyopennova.pcx import PcxDecodeError, decode_pcx_rgb


FIXTURE = Path("fixtures/lw/dflw/badguy/ADstCamo.pcx")


def test_decodes_indexed_pcx_fixture_to_rgb_pixels():
    image = decode_pcx_rgb(FIXTURE.read_bytes())

    assert image.width == 32
    assert image.height == 32
    assert len(image.rgb) == 32 * 32 * 3
    assert image.rgb[:6] == bytes([142, 140, 137, 131, 128, 125])


def test_rejects_non_pcx_data():
    with pytest.raises(PcxDecodeError, match="manufacturer"):
        decode_pcx_rgb(b"\x00" * 128)
