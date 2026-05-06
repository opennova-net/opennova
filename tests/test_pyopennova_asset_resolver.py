from __future__ import annotations

from pathlib import Path

from pyopennova.asset_resolver import AssetResolver


def test_resolve_texture_materializes_misnamed_dds_payload(tmp_path: Path) -> None:
    source = tmp_path / "Example.TGA"
    payload = b"DDS " + bytes(range(32))
    source.write_bytes(payload)

    with AssetResolver(str(tmp_path)) as resolver:
        first = resolver.resolve_texture("Example.tga")
        second = resolver.resolve_texture("Example.tga")

    assert first is not None
    assert second == first
    resolved = Path(first)
    assert resolved.name == "Example.dds"
    assert resolved.read_bytes() == payload
    assert resolved != source


def test_resolve_texture_keeps_regular_tga_path(tmp_path: Path) -> None:
    source = tmp_path / "Regular.TGA"
    source.write_bytes(b"\x00\x00\x02" + bytes(29))

    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve_texture("Regular.tga")

    assert resolved == str(source)


def test_resolve_texture_3di3_prefers_dds_over_authored_tga(tmp_path: Path) -> None:
    authored = tmp_path / "Panel.TGA"
    dds = tmp_path / "Panel.DDS"
    authored.write_bytes(b"\x00\x00\x02" + bytes(29))
    dds.write_bytes(b"DDS " + bytes(range(32)))

    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve_texture("Panel.tga", source_format=1)

    assert resolved == str(dds)


def test_resolve_texture_3di3_uses_tga_when_dds_missing(tmp_path: Path) -> None:
    authored = tmp_path / "Panel.TGA"
    authored.write_bytes(b"\x00\x00\x02" + bytes(29))

    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve_texture("Panel.tga", strategy="3di3_df4oed")

    assert resolved == str(authored)


def test_resolve_texture_3di3_does_not_apply_dds_fallback_to_mdt(tmp_path: Path) -> None:
    mdt = tmp_path / "Panel.MDT"
    dds = tmp_path / "Panel.DDS"
    mdt.write_bytes(b"mdt payload")
    dds.write_bytes(b"DDS " + bytes(range(32)))

    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve_texture("Panel.mdt", source_format=1)

    assert resolved == str(mdt)
