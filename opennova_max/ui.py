"""Shared helpers for the 3ds Max Qt importer UI.

The user-facing importer is a Qt dialog. This module keeps the Max menu
launcher and the import callbacks small so GUI, scripted single imports, and
batch imports all share the same scene/output implementation.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


_STATUS_LISTENERS = []


@dataclass(frozen=True)
class DefinitionScanItem:
    name: str
    type: str
    source_model: str = ""
    output_stem: str = ""


def show_importer() -> bool:
    """Create the OpenNova Qt importer dialog inside 3ds Max."""
    from .qt_ui import show_importer_dialog

    return show_importer_dialog()


def close_importer() -> bool:
    """Close the Qt importer dialog if it is currently open."""
    from .qt_ui import close_importer_dialog

    return close_importer_dialog()


def register_menu() -> bool:
    """Register the OpenNova launcher action and best-effort menu item."""
    _rt().execute(build_menu_script())
    return True


def build_menu_script() -> str:
    """Return the MaxScript launcher/menu registration script."""
    return r'''
try
(
    if menuMan.registerMenuContext 0x5cb72810 then
    (
        local mainMenuBar = menuMan.getMainMenuBar()
        local subMenu = menuMan.createMenu "OpenNova"
        local importItem = menuMan.createActionItem "OpenNovaImporter" "OpenNova"
        if importItem != undefined do
        (
            try
            (
                importItem.setTitle "Importer..."
                importItem.setUseCustomTitle true
            )
            catch()
            subMenu.addItem importItem -1
            local subMenuItem = menuMan.createSubMenuItem "OpenNova" subMenu
            local insertIndex = mainMenuBar.numItems() - 1
            if insertIndex < 1 do insertIndex = -1
            mainMenuBar.addItem subMenuItem insertIndex
            menuMan.updateMenuBar()
        )
    )
)
catch
(
    print ("OpenNova menu registration failed: " + getCurrentException())
)
'''


def unregister_menu() -> bool:
    """Best-effort placeholder for symmetry with ``register_menu``."""
    return False


def _scan_definitions(game_dir: str) -> tuple[bool, list[DefinitionScanItem], str]:
    if not game_dir:
        return False, [], "Game directory is required."
    if not Path(game_dir).is_dir():
        return False, [], "Game directory does not exist."

    try:
        from pyopennova import definitions as defs
        from pyopennova.asset_resolver import AssetResolver

        items: list[DefinitionScanItem] = []
        with AssetResolver(game_dir) as resolver:
            weapons, item_defs = defs.process_def_files(resolver)
            for weapon in weapons:
                source_model = defs.ensure_extension(weapon.graphic1.main, ".3di")
                items.append(DefinitionScanItem(
                    name=weapon.name,
                    type="weapon",
                    source_model=source_model,
                    output_stem=Path(source_model).stem,
                ))
            for item in item_defs:
                source_model = defs.ensure_extension(item.graphic_us, ".3di")
                items.append(DefinitionScanItem(
                    name=item.name,
                    type="item",
                    source_model=source_model,
                    output_stem=Path(source_model).stem,
                ))
    except Exception as exc:
        return False, [], str(exc)

    return True, items, ""


def _run_loose_from_ui(
    threedi_path,
    asset_dir: str,
    output_dir: str,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool = True,
    copy_textures: bool = True,
) -> bool:
    paths = _loose_paths(threedi_path)
    output_root = output_dir.strip()
    if not paths or not output_root:
        _set_status("Loose import requires a .3di path and output root.")
        return False
    if not write_ase and not write_3dp and not write_max and not copy_textures:
        _set_status("Select ASE, 3DP/3DA, MAX, and/or Textures output.")
        return False
    invalid = [str(path) for path in paths if path.suffix.casefold() != ".3di"]
    if invalid:
        _set_status(f"Loose imports require .3di files: {', '.join(invalid)}")
        return False
    duplicate_stems = _duplicate_stems(paths)
    if duplicate_stems:
        _set_status(f"Loose import has duplicate output names: {', '.join(duplicate_stems)}")
        return False

    root = Path(output_root)
    asset_base_dir = asset_dir.strip() or None
    if len(paths) == 1:
        from .import_runner import run_loose_import

        path = paths[0]
        model_output_dir = root / path.stem
        ok = run_loose_import(
            str(path),
            str(model_output_dir),
            output_stem=path.stem,
            asset_base_dir=asset_base_dir,
            import_collisions=bool(import_collisions),
            import_occlusion=bool(import_occlusion),
            import_lights=bool(import_lights),
            write_ase=bool(write_ase),
            write_3dp=bool(write_3dp),
            write_max=bool(write_max),
            copy_textures=bool(copy_textures),
            reset_scene=True,
        )
        _set_status(f"Loose import {'complete' if ok else 'produced no geometry'} -> {model_output_dir}")
        return bool(ok)

    from .import_runner import MaxImportRequest, run_batch

    requests = [
        MaxImportRequest(
            resource=str(path),
            output_dir=str(root / path.stem),
            base_dir=asset_base_dir or "",
            output_stem=path.stem,
            import_collisions=bool(import_collisions),
            import_occlusion=bool(import_occlusion),
            import_lights=bool(import_lights),
            write_ase=bool(write_ase),
            write_3dp=bool(write_3dp),
            write_max=bool(write_max),
            copy_textures=bool(copy_textures),
        )
        for path in paths
    ]
    results = run_batch(requests)
    completed = sum(1 for result in results if getattr(result, "ok", False))
    _set_status(f"Loose batch complete: {completed}/{len(paths)} files -> {root}")
    return completed == len(paths)


def _run_definition_item(
    item,
    game_dir: str,
    output_root: str,
    *,
    import_arms: bool,
    import_animations: bool,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool = True,
    copy_textures: bool = True,
) -> bool:
    from .import_runner import run_import

    if not game_dir.strip():
        _set_status("Definition import requires a game directory.")
        return False
    if not output_root.strip():
        _set_status("Definition import requires an output root.")
        return False
    if not write_ase and not write_3dp and not write_max and not copy_textures:
        _set_status("Select ASE, 3DP/3DA, MAX, and/or Textures output.")
        return False

    _set_status(f"Importing {item.type} {item.name}...")
    ok = run_import(
        game_dir.strip(),
        item.name,
        item.type,
        output_root.strip(),
        import_arms=bool(import_arms),
        import_animations=bool(import_animations),
        import_collisions=bool(import_collisions),
        import_occlusion=bool(import_occlusion),
        import_lights=bool(import_lights),
        output_stem=item.output_stem,
        write_ase=bool(write_ase),
        write_3dp=bool(write_3dp),
        write_max=bool(write_max),
        copy_textures=bool(copy_textures),
        reset_scene=True,
    )
    output_name = item.output_stem or item.name
    output_path = Path(output_root.strip()) / output_name
    _set_status(f"{item.type} {item.name} {'complete' if ok else 'produced no geometry'} -> {output_path}")
    return bool(ok)


def _filtered_items(items: Iterable[object], type_filter: str, search_text: str = "") -> list[object]:
    normalized = _normalize_type_filter(type_filter)
    terms = _search_terms(search_text)
    filtered = [
        item for item in items
        if normalized == "all" or getattr(item, "type", "") == normalized
    ]
    if terms:
        filtered = [item for item in filtered if _matches_search(item, terms)]
    return sorted(
        filtered,
        key=lambda item: (
            getattr(item, "type", "").casefold(),
            getattr(item, "name", "").casefold(),
        ),
    )


def _format_scan_item(item) -> str:
    source = getattr(item, "source_model", "") or "(definition lookup)"
    stem = getattr(item, "output_stem", "") or Path(source).stem
    return f"[{item.type}] {item.name} -> {stem}"


def _loose_paths(value) -> list[Path]:
    if isinstance(value, (list, tuple, set)):
        raw_paths = value
    else:
        raw_paths = [value]
    return [Path(str(path).strip()) for path in raw_paths if str(path).strip()]


def _duplicate_stems(paths: Iterable[Path]) -> list[str]:
    seen: set[str] = set()
    duplicates: set[str] = set()
    for path in paths:
        key = path.stem.casefold()
        if key in seen:
            duplicates.add(path.stem)
        seen.add(key)
    return sorted(duplicates, key=str.casefold)


def _matches_search(item, terms: list[str]) -> bool:
    fields = [str(field).casefold() for field in _search_fields(item)]
    normalized_fields = [_normalize_search_text(field) for field in fields]
    return all(
        any(
            term in field or _is_ordered_subsequence(term, normalized_field)
            for field, normalized_field in zip(fields, normalized_fields)
        )
        for term in terms
    )


def _search_fields(item) -> list[str]:
    source = getattr(item, "source_model", "")
    stem = getattr(item, "output_stem", "") or Path(source).stem
    return [
        getattr(item, "type", ""),
        getattr(item, "name", ""),
        source,
        stem,
        _format_scan_item(item),
    ]


def _search_terms(search_text: str) -> list[str]:
    return [
        normalized
        for term in search_text.strip().split()
        for normalized in [_normalize_search_text(term)]
        if normalized
    ]


def _normalize_search_text(value: object) -> str:
    return "".join(ch for ch in str(value).casefold() if ch.isalnum())


def _is_ordered_subsequence(needle: str, haystack: str) -> bool:
    if not needle:
        return True
    if len(needle) > len(haystack):
        return False
    if len(needle) == 1:
        return needle in haystack
    max_span = max(len(needle) + 3, int(len(needle) * 1.6))
    for start, ch in enumerate(haystack):
        if ch != needle[0]:
            continue
        pos = 1
        end = start
        for end in range(start + 1, len(haystack)):
            if haystack[end] == needle[pos]:
                pos += 1
                if pos == len(needle):
                    if end - start + 1 <= max_span:
                        return True
                    break
    return False


def _normalize_type_filter(type_filter: str) -> str:
    value = (type_filter or "all").strip().casefold()
    return value if value in {"all", "weapon", "item"} else "all"


def _set_status(message: str) -> None:
    print(f"OpenNova: {message}")
    for callback in list(_STATUS_LISTENERS):
        try:
            callback(message)
        except Exception:
            pass


def add_status_listener(callback) -> None:
    if callback not in _STATUS_LISTENERS:
        _STATUS_LISTENERS.append(callback)


def remove_status_listener(callback) -> None:
    try:
        _STATUS_LISTENERS.remove(callback)
    except ValueError:
        pass


def _rt():
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; opennova_max UI only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime
