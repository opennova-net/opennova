from __future__ import annotations

from pathlib import Path

from pyopennova import materials
from pyopennova.texture_outputs import copy_model_textures


class FakeTexture:
    def __init__(self, name: str, slot: int = materials.THREEDI_IR_TEX_SLOT_DIFFUSE):
        self.name = name.encode("utf-8")
        self.slot = slot
        self.type = 0
        self.flags = 0
        self.frame = 0


class FakeMaterial:
    def __init__(self, index: int, texture_names: list[str]):
        self.index = index
        self.shader_name = b"FF_MT_OP"
        self.flags = 0
        self.blend_mode = 0
        self.luminosity = 0
        self.texture_count = len(texture_names)
        self.textures = [FakeTexture(name) for name in texture_names]


class FakeIR:
    source_format = 0

    def __init__(self, materials_):
        self.materials = materials_
        self.material_count = len(materials_)


class FakeResolver:
    def __init__(self, paths: dict[str, Path]):
        self.paths = {name.casefold(): str(path) for name, path in paths.items()}

    def resolve_texture(self, name: str, **_kwargs):
        return self.paths.get(name.casefold())


def test_copy_model_textures_uses_authored_texture_name(tmp_path: Path) -> None:
    source = tmp_path / "Source" / "Wall.TGA"
    source.parent.mkdir()
    source.write_bytes(b"\x00\x00\x02" + bytes(29))

    written = copy_model_textures(
        FakeIR([FakeMaterial(1, ["Wall.tga"])]),
        str(tmp_path / "out"),
        resolver=FakeResolver({"Wall.tga": source}),
    )

    copied = tmp_path / "out" / "textures" / "Wall.tga"
    assert written == [str(copied)]
    assert copied.read_bytes() == source.read_bytes()
    assert not copied.with_suffix(".dds").exists()


def test_copy_model_textures_adds_dds_for_tga_with_dds_payload(tmp_path: Path) -> None:
    source = tmp_path / "Panel.DDS"
    payload = b"DDS " + bytes(range(32))
    source.write_bytes(payload)

    written = copy_model_textures(
        FakeIR([FakeMaterial(1, ["Panel.tga"])]),
        str(tmp_path / "out"),
        resolver=FakeResolver({"Panel.tga": source}),
    )

    texture_dir = tmp_path / "out" / "textures"
    assert {Path(path).name for path in written} == {"Panel.tga", "Panel.dds"}
    assert (texture_dir / "Panel.tga").read_bytes() == payload
    assert (texture_dir / "Panel.dds").read_bytes() == payload


def test_copy_model_textures_skips_unresolved_textures(tmp_path: Path) -> None:
    written = copy_model_textures(
        FakeIR([FakeMaterial(1, ["Missing.tga"])]),
        str(tmp_path / "out"),
        resolver=FakeResolver({}),
    )

    assert written == []
    assert not (tmp_path / "out" / "textures").exists()


def test_copy_model_textures_dedupes_repeated_destinations(tmp_path: Path) -> None:
    source = tmp_path / "Shared.tga"
    source.write_bytes(b"\x00\x00\x02" + bytes(29))

    written = copy_model_textures(
        FakeIR([
            FakeMaterial(1, ["Shared.tga"]),
            FakeMaterial(2, ["Shared.tga"]),
        ]),
        str(tmp_path / "out"),
        resolver=FakeResolver({"Shared.tga": source}),
    )

    assert written == [str(tmp_path / "out" / "textures" / "Shared.tga")]
