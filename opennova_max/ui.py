"""3ds Max UI hooks for OpenNova export commands."""
from __future__ import annotations


def export_ase():
    # type: () -> bool
    from .ase_scene_exporter import export_scene_with_dialog

    return export_scene_with_dialog()


def build_menu_script():
    # type: () -> str
    return r'''
try (
    local mainMenu = menuMan.getMainMenuBar()
    local exportMenu = undefined
    for i = 1 to mainMenu.numItems do (
        local item = mainMenu.getItem i
        if item != undefined and item.getTitle() == "Export" do exportMenu = item.getSubMenu()
    )
    if exportMenu == undefined do (
        exportMenu = menuMan.createMenu "Export"
        local exportMenuItem = menuMan.createSubMenuItem "Export" exportMenu
        mainMenu.addItem exportMenuItem -1
    )

    local openNovaMenu = menuMan.createMenu "OpenNovaExportMenu"
    local aseItem = menuMan.createActionItem "OpenNovaExportAse" "OpenNova"
    aseItem.setTitle "Novalogic ASE (.ase)"
    openNovaMenu.addItem aseItem -1

    local openNovaItem = menuMan.createSubMenuItem "OpenNova" openNovaMenu
    exportMenu.addItem openNovaItem -1
    menuMan.updateMenuBar()
) catch (
    format "OpenNova export menu registration failed: %\n" (getCurrentException())
)
'''.strip()


def install_menu():
    # type: () -> None
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - only available inside Max
        raise RuntimeError("pymxs is not available; menu install only runs inside 3ds Max.") from exc
    pymxs.runtime.execute(build_menu_script())
