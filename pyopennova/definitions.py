"""
Common definitions parsing logic for Novalogic files.
Adapted from the pre-repo import prototype with imports updated to use
the local opennova package (FFI + pure-Python parsers).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

from .adm_ffi import parse_adm, free_adm
from .bad_ffi import parse_bad, free_bad
from .def_ffi import parse_weapons_def, free_weapons_def, parse_items_def, free_items_def
from .model import AnimationContext, AnimationMeta


log = logging.getLogger(__name__)


def ensure_extension(filename: str, ext: str) -> str:
    return filename if filename.lower().endswith(ext.lower()) else f"{filename}{ext}"


@dataclass
class Graphic1:
    main: str
    arms: str | None = None


@dataclass
class WeaponContext:
    name: str
    graphic1: Graphic1
    anim_adm: str | None = None


@dataclass
class ItemContext:
    name: str
    graphic_us: str
    anim_def: str | None = None


def parse_weapon_context(entry) -> WeaponContext | None:
    """Build a WeaponContext from a DefWeaponDef C struct."""
    gfx1 = entry.gfx1.decode("utf-8", errors="replace")
    if not gfx1:
        return None
    animadm = entry.animadm.decode("utf-8", errors="replace")
    gfx1a = entry.gfx1a.decode("utf-8", errors="replace")
    return WeaponContext(
        name=entry.weapon_name.decode("utf-8", errors="replace"),
        anim_adm=ensure_extension(animadm, ".adm") if animadm else None,
        graphic1=Graphic1(
            main=ensure_extension(gfx1, ".3di"),
            arms=ensure_extension(gfx1a, ".3di") if gfx1a else None,
        ),
    )


def parse_item_context(entry) -> ItemContext | None:
    """Build an ItemContext from a DefItemDef C struct."""
    graphic = entry.graphic.decode("utf-8", errors="replace")
    if not graphic:
        return None
    anim_def = entry.anim_def.decode("utf-8", errors="replace")
    return ItemContext(
        name=entry.display_name.decode("utf-8", errors="replace"),
        graphic_us=graphic,
        anim_def=anim_def if anim_def else None,
    )


def process_def_files(resolver) -> tuple[list[WeaponContext], list[ItemContext]]:
    weapons, items = [], []

    weapons_path = resolver.resolve("weapon.def")
    items_path = resolver.resolve("items.def")

    if weapons_path:
        log.debug("Processing weapon.def")
        wf = parse_weapons_def(str(weapons_path))
        try:
            for i in range(wf.count):
                context = parse_weapon_context(wf.entries[i])
                if context:
                    weapons.append(context)
        finally:
            free_weapons_def(wf)

    if items_path:
        log.debug("Processing items.def")
        ifl = parse_items_def(str(items_path))
        try:
            for i in range(ifl.count):
                context = parse_item_context(ifl.entries[i])
                if context:
                    items.append(context)
        finally:
            free_items_def(ifl)

    log.debug("Found %d weapons and %d items", len(weapons), len(items))
    return weapons, items


def build_animation_context(
    adm_field: str | None, resolver=None,
) -> AnimationContext | None:
    """
    Builds an AnimationContext by parsing an .adm file and then reading
    the corresponding BAD files via FFI to extract metadata (fps, frame count).
    """
    entries = _adm_entries(adm_field, resolver=resolver)

    if not entries:
        return None

    reset_entry = next(
        ((k, v) for k, v in entries if k == "anim_reset"),
        None,
    )
    if not reset_entry:
        return None

    # Use FFI to read BAD header info
    reset_bad_name = ensure_extension(reset_entry[1], ".bad")
    reset_bad_path = resolver.resolve(reset_bad_name)
    if not reset_bad_path:
        return None
    reset_bad_stem = reset_bad_name.rsplit(".", 1)[0] if "." in reset_bad_name else reset_bad_name
    reset_bad = parse_bad(str(reset_bad_path))
    try:
        reset_meta = AnimationMeta(
            animation_name=reset_entry[0],
            bad_filepath=str(reset_bad_path),
            fps=reset_bad.fps,
            frame_count=reset_bad.frame_count,
            bad_name=reset_bad_stem,
            flags=reset_bad.flags,
        )
    finally:
        free_bad(reset_bad)

    anim_metas = []
    for key, value in entries:
        if key == "anim_reset":
            continue
        bad_name = ensure_extension(value, ".bad")
        bad_stem = bad_name.rsplit(".", 1)[0] if "." in bad_name else bad_name
        bad_path = resolver.resolve(bad_name)
        if not bad_path:
            continue
        bf = parse_bad(str(bad_path))
        try:
            meta = AnimationMeta(
                animation_name=key,
                bad_filepath=str(bad_path),
                fps=bf.fps,
                frame_count=bf.frame_count,
                bad_name=bad_stem,
                flags=bf.flags,
            )
            anim_metas.append(meta)
        finally:
            free_bad(bf)

    return AnimationContext(reset_animation=reset_meta, animations=anim_metas)


def resolve_reset_bad_path(adm_field: str | None, resolver=None) -> str | None:
    """Resolve only the reset BAD path from an ADM field."""
    reset_entry = next(
        ((k, v) for k, v in _adm_entries(adm_field, resolver=resolver) if k == "anim_reset"),
        None,
    )
    if not reset_entry:
        return None
    reset_bad_name = ensure_extension(reset_entry[1], ".bad")
    reset_bad_path = resolver.resolve(reset_bad_name)
    return str(reset_bad_path) if reset_bad_path else None


def _adm_entries(adm_field: str | None, resolver=None) -> list[tuple[str, str]]:
    if not adm_field:
        return []

    adm_name = ensure_extension(adm_field, ".adm")
    adm_path = resolver.resolve(adm_name)
    if not adm_path:
        return []

    adm = parse_adm(str(adm_path))
    try:
        entries = []
        for i in range(adm.count):
            e = adm.entries[i]
            entries.append((e.key.decode("utf-8"), e.value.decode("utf-8")))
        return entries
    finally:
        free_adm(adm)
