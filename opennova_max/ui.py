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


def export_ase() -> bool:
    """Prompt for an ASE path and export the current 3ds Max scene."""
    from .ase_scene_exporter import export_scene_with_dialog

    return export_scene_with_dialog()


def export_anims() -> bool:
    """Prompt for an ADM path and export the current 3ds Max timeline."""
    from .anim_exporter import export_anims_with_dialog

    return export_anims_with_dialog()


def register_menu() -> bool:
    """Register the OpenNova launcher action and best-effort menu item."""
    _rt().execute(build_menu_script())
    return True


def build_menu_script() -> str:
    """Return the MaxScript launcher/menu registration script."""
    return r'''
global openNovaRegisterModernMenus

global OPENNOVA_MENU_GUID = "5CB72810-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_IMPORT_ACTION_GUID = "5CB72811-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_ASE_ACTION_GUID = "5CB72812-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_ANIM_ACTION_GUID = "5CB72813-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_FILE_ASE_ACTION_GUID = "5CB72814-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_FILE_ANIM_ACTION_GUID = "5CB72815-7B1D-4E71-9E31-0F5C4F4E4D01"

fn openNovaMenuItemTitle item =
(
    try (item.getTitle()) catch("")
)

fn openNovaFindSubMenu menu titles =
(
    if menu == undefined do return undefined
    for i = 1 to menu.numItems() do
    (
        local item = menu.getItem i
        if item != undefined do
        (
            local title = openNovaMenuItemTitle item
            for wanted in titles do
            (
                if title == wanted do return item.getSubMenu()
            )
            local childMenu = undefined
            try (childMenu = item.getSubMenu()) catch()
            if childMenu != undefined do
            (
                local found = openNovaFindSubMenu childMenu titles
                if found != undefined do return found
            )
        )
    )
    undefined
)

fn openNovaModernTitleMatches title titles =
(
    if title == undefined do return false
    local normalizedTitle = substituteString title "&" ""
    for wanted in titles do
    (
        if title == wanted do return true
        if normalizedTitle == (substituteString wanted "&" "") do return true
    )
    false
)

fn openNovaFindModernMenuByTitle menu titles =
(
    if menu == undefined do return undefined
    try
    (
        for item in menu.menuItems do
        (
            local childMenu = undefined
            try
            (
                if item.isSubMenu do childMenu = item.subMenu
            )
            catch()
            if childMenu != undefined do
            (
                if openNovaModernTitleMatches childMenu.title titles do return childMenu
                local found = openNovaFindModernMenuByTitle childMenu titles
                if found != undefined do return found
            )
        )
    )
    catch()
    undefined
)

fn openNovaCreateModernAction menu itemGuid actionId title =
(
    if menu == undefined do return undefined
    try (menu.DeleteItem itemGuid) catch()
    try
    (
        menu.CreateAction itemGuid 647394 actionId title:title
    )
    catch
    (
        print ("OpenNova modern menu action registration failed for " + actionId + ": " + getCurrentException())
        undefined
    )
)

fn openNovaRemoveLegacyMenuItemByTitle menu title =
(
    if menu == undefined do return false
    local removed = false
    for i = menu.numItems() to 1 by -1 do
    (
        local item = menu.getItem i
        if item != undefined do
        (
            local itemTitle = openNovaMenuItemTitle item
            if itemTitle == title do
            (
                try
                (
                    menu.removeItemByPosition i
                    removed = true
                )
                catch
                (
                    try
                    (
                        menu.removeItem item
                        removed = true
                    )
                    catch()
                )
            )
        )
    )
    removed
)

fn openNovaRemoveLegacyMenu menu title =
(
    openNovaRemoveLegacyMenuItemByTitle menu title
)

fn openNovaLogLegacyAction macroName item =
(
    if item != undefined then
    (
        print ("OpenNova menu action available: " + macroName + "`OpenNova")
    )
    else
    (
        print ("OpenNova menu action missing: " + macroName + "`OpenNova")
    )
)

fn openNovaBuildLegacyOpenNovaMenu mainMenuBar =
(
    if mainMenuBar == undefined do return false
    openNovaRemoveLegacyMenu mainMenuBar "OpenNova"

    local subMenu = menuMan.createMenu "OpenNova"
    local importItem = menuMan.createActionItem "OpenNovaImporter" "OpenNova"
    openNovaLogLegacyAction "OpenNovaImporter" importItem
    if importItem != undefined do
    (
        try
        (
            importItem.setTitle "Importer..."
            importItem.setUseCustomTitle true
        )
        catch()
        subMenu.addItem importItem -1
    )

    local aseOpenNovaItem = menuMan.createActionItem "OpenNovaExportAse" "OpenNova"
    openNovaLogLegacyAction "OpenNovaExportAse" aseOpenNovaItem
    if aseOpenNovaItem != undefined do
    (
        try
        (
            aseOpenNovaItem.setTitle "Novalogic ASE (.ase)"
            aseOpenNovaItem.setUseCustomTitle true
        )
        catch()
        subMenu.addItem aseOpenNovaItem -1
    )

    local animOpenNovaItem = menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"
    openNovaLogLegacyAction "OpenNovaExportAnims" animOpenNovaItem
    if animOpenNovaItem != undefined do
    (
        try
        (
            animOpenNovaItem.setTitle "Novalogic Anims (.adm + .bad)"
            animOpenNovaItem.setUseCustomTitle true
        )
        catch()
        subMenu.addItem animOpenNovaItem -1
    )

    local subMenuItem = menuMan.createSubMenuItem "OpenNova" subMenu
    local insertIndex = mainMenuBar.numItems() - 1
    if insertIndex < 1 do insertIndex = -1
    mainMenuBar.addItem subMenuItem insertIndex
    menuMan.updateMenuBar()
    true
)

fn openNovaRegisterModernMenus =
(
    local menuMgr = callbacks.notificationParam()
    if menuMgr == undefined do return false
    local mainMenuBar = menuMgr.mainMenuBar
    if mainMenuBar == undefined do return false

    local openNovaMenu = undefined
    try (openNovaMenu = menuMgr.GetMenuById OPENNOVA_MENU_GUID) catch()
    if openNovaMenu == undefined do
    (
        try
        (
            local helpMenuId = "cee8f758-2199-411b-81e7-d3ff4a80d143"
            openNovaMenu = mainMenuBar.CreateSubMenu OPENNOVA_MENU_GUID "OpenNova" beforeId:helpMenuId
        )
        catch
        (
            try (openNovaMenu = mainMenuBar.CreateSubMenu OPENNOVA_MENU_GUID "OpenNova") catch()
        )
    )
    if openNovaMenu != undefined do
    (
        openNovaCreateModernAction openNovaMenu OPENNOVA_IMPORT_ACTION_GUID "OpenNovaImporter`OpenNova" "Importer..."
        openNovaCreateModernAction openNovaMenu OPENNOVA_ASE_ACTION_GUID "OpenNovaExportAse`OpenNova" "Novalogic ASE (.ase)"
        openNovaCreateModernAction openNovaMenu OPENNOVA_ANIM_ACTION_GUID "OpenNovaExportAnims`OpenNova" "Novalogic Anims (.adm + .bad)"
    )

    local fileMenu = undefined
    try (fileMenu = menuMgr.GetMenuById "eed3eaef-ea24-4342-aacc-9dfd87f9a4f4") catch()
    if fileMenu == undefined do fileMenu = openNovaFindModernMenuByTitle mainMenuBar #("&File", "File")

    local openNovaExportMenu = undefined
    if fileMenu != undefined do
    (
        openNovaExportMenu = openNovaFindModernMenuByTitle fileMenu #("&Export", "Export", "&Export...", "Export...")
        if openNovaExportMenu == undefined do openNovaExportMenu = fileMenu
    )
    if openNovaExportMenu != undefined do
    (
        openNovaCreateModernAction openNovaExportMenu OPENNOVA_FILE_ASE_ACTION_GUID "OpenNovaExportAse`OpenNova" "Novalogic ASE (.ase)"
        openNovaCreateModernAction openNovaExportMenu OPENNOVA_FILE_ANIM_ACTION_GUID "OpenNovaExportAnims`OpenNova" "Novalogic Anims (.adm + .bad)"
    )
    true
)

try
(
    local iCuiMenuMgr = maxOps.GetICuiMenuMgr()
    if iCuiMenuMgr != undefined do
    (
        callbacks.removeScripts id:#OpenNovaMaxMenus
        callbacks.addScript #cuiRegisterMenus openNovaRegisterModernMenus id:#OpenNovaMaxMenus
        try
        (
            iCuiMenuMgr.LoadConfiguration (iCuiMenuMgr.GetCurrentConfiguration())
        )
        catch
        (
            print ("OpenNova modern menu refresh failed: " + getCurrentException())
        )
    )
)
catch
(
    print ("OpenNova modern menu registration failed: " + getCurrentException())
)

try
(
    local legacyContextRegistered = false
    try (legacyContextRegistered = menuMan.registerMenuContext 0x5cb72810) catch()
    if not legacyContextRegistered do
    (
        print "OpenNova legacy menu registration context already exists; rebuilding menu."
    )
    local mainMenuBar = menuMan.getMainMenuBar()
    openNovaBuildLegacyOpenNovaMenu mainMenuBar
)
catch
(
    print ("OpenNova menu registration failed: " + getCurrentException())
)

try
(
    local exportContextRegistered = false
    try (exportContextRegistered = menuMan.registerMenuContext 0x5cb72811) catch()
    if not exportContextRegistered do
    (
        print "OpenNova legacy File > Export registration context already exists; rebuilding menu entries."
    )
    local fileMenu = menuMan.findMenu "&File"
    if fileMenu == undefined do fileMenu = menuMan.findMenu "File"
    local openNovaExportMenu = undefined
    if fileMenu != undefined do
    (
        openNovaExportMenu = openNovaFindSubMenu fileMenu #("&Export", "Export", "&Export...", "Export...")
        if openNovaExportMenu == undefined do openNovaExportMenu = fileMenu
    )
    if openNovaExportMenu != undefined do
    (
        openNovaRemoveLegacyMenuItemByTitle openNovaExportMenu "Novalogic ASE (.ase)"
        openNovaRemoveLegacyMenuItemByTitle openNovaExportMenu "Novalogic Anims (.adm + .bad)"

        local aseItem = menuMan.createActionItem "OpenNovaExportAse" "OpenNova"
        openNovaLogLegacyAction "OpenNovaExportAse" aseItem
        if aseItem != undefined then
        (
            try
            (
                aseItem.setTitle "Novalogic ASE (.ase)"
                aseItem.setUseCustomTitle true
            )
            catch()
            openNovaExportMenu.addItem aseItem -1
        )
        local animItem = menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"
        openNovaLogLegacyAction "OpenNovaExportAnims" animItem
        if animItem != undefined then
        (
            try
            (
                animItem.setTitle "Novalogic Anims (.adm + .bad)"
                animItem.setUseCustomTitle true
            )
            catch()
            openNovaExportMenu.addItem animItem -1
        )
        menuMan.updateMenuBar()
    )
)
catch
(
    print ("OpenNova File > Export menu registration failed: " + getCurrentException())
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
