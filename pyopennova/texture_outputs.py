"""Texture sidecar output helpers shared by DCC integrations."""
from __future__ import annotations

import logging
import os
import shutil
from pathlib import Path
from typing import Iterable

from pyopennova.materials import TextureDescriptor, describe_materials


log = logging.getLogger(__name__)


def copy_model_textures(ir, output_dir: str, resolver=None) -> list[str]:
    """Copy resolved material textures into ``output_dir/textures``.

    The authored texture basename is preserved for compatibility with existing
    references. If an authored ``.tga`` resolves to a DDS payload, a matching
    ``.dds`` file is written as a second sidecar.
    """
    if ir is None or not output_dir:
        return []

    written: list[str] = []
    copied: set[str] = set()
    texture_dir = Path(output_dir) / "textures"

    for texture in _resolved_textures(ir, resolver):
        authored_name = os.path.basename(texture.name)
        if not authored_name or not texture.path:
            continue

        source = Path(texture.path)
        if not source.is_file():
            log.warning("Skipping missing texture %r resolved to %s", texture.name, source)
            continue

        primary = texture_dir / authored_name
        if _copy_once(source, primary, copied):
            written.append(str(primary))

        if primary.suffix.casefold() == ".tga" and _is_dds_payload(source):
            dds_path = primary.with_suffix(".dds")
            if _copy_once(source, dds_path, copied):
                written.append(str(dds_path))

    return written


def _resolved_textures(ir, resolver) -> Iterable[TextureDescriptor]:
    try:
        descriptors = describe_materials(ir, resolver=resolver)
    except Exception as exc:
        log.warning("Could not inspect model textures: %s", exc)
        return ()
    textures: list[TextureDescriptor] = []
    for desc in descriptors:
        for texture in desc.textures:
            if texture.path:
                textures.append(texture)
            elif texture.name:
                log.warning("Skipping unresolved texture %r", texture.name)
    return textures


def _copy_once(source: Path, dest: Path, copied: set[str]) -> bool:
    key = os.path.normcase(os.path.abspath(str(dest)))
    if key in copied:
        return False
    copied.add(key)

    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not _same_path(source, dest):
            shutil.copyfile(source, dest)
        return True
    except OSError as exc:
        log.warning("Could not copy texture %s -> %s: %s", source, dest, exc)
        return False


def _is_dds_payload(path: Path) -> bool:
    try:
        with open(path, "rb") as handle:
            return handle.read(4) == b"DDS "
    except OSError:
        return False


def _same_path(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve()
    except OSError:
        return False
