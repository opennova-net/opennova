"""Shared helpers for the 3ds Max Qt importer UI.

The user-facing importer is a Qt dialog. This module keeps the Max menu
launcher small. Filtering, scanning, and execution go through the backend
Protocol defined in opennova_qt_ui.backend.
"""
from __future__ import annotations

from dataclasses import dataclass

from opennova_qt_ui.filtering import (  # noqa: F401 - kept for backwards compat
    _format_scan_item,
    filtered_items as _filtered_items,
)


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


def _rt():
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError(
            "pymxs is not available; opennova_max UI only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime
