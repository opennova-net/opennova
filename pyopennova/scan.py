"""Host-neutral scanner for definition (DEF) directories.

Both the Blender and Max consumers delegate to this module so the scan
logic isn't duplicated. Returns the same `ScanResult` shape both surfaces
already use.
"""
from __future__ import annotations

import logging
from pathlib import Path

from opennova_jobs import ScanItem, ScanResult


_log = logging.getLogger(__name__)


def scan_definitions(base_dir: str) -> ScanResult:
    """Scan a game directory and return the available weapons and items."""
    if not base_dir:
        return ScanResult(ok=False, error="Game directory is required.")
    if not Path(base_dir).is_dir():
        return ScanResult(ok=False, error="Game directory does not exist.")

    from pyopennova import asset_resolver as ar
    from pyopennova import definitions as defs

    items: list[ScanItem] = []
    try:
        with ar.AssetResolver(base_dir) as resolver:
            weapons, item_defs = defs.process_def_files(resolver)
            for weapon in weapons:
                source_model = defs.ensure_extension(weapon.graphic1.main, ".3di")
                items.append(
                    ScanItem(
                        name=weapon.name,
                        type="weapon",
                        source_model=source_model,
                        output_stem=Path(source_model).stem,
                    )
                )
            for item in item_defs:
                source_model = defs.ensure_extension(item.graphic_us, ".3di")
                items.append(
                    ScanItem(
                        name=item.name,
                        type="item",
                        source_model=source_model,
                        output_stem=Path(source_model).stem,
                    )
                )
    except Exception as exc:
        _log.error("scan_definitions failed: %s", exc, exc_info=True)
        return ScanResult(ok=False, error=str(exc))

    return ScanResult(ok=True, items=items)
