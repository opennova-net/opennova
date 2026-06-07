"""3ds Max UI hooks for OpenNova export commands."""
from __future__ import annotations


def export_ase():
    # type: () -> bool
    from .ase_scene_exporter import export_scene_with_dialog

    return export_scene_with_dialog()


def export_anims():
    # type: () -> bool
    from .anim_scene_exporter import export_anims_with_dialog

    return export_anims_with_dialog()


def build_menu_script():
    # type: () -> str
    return r'''
global openNovaRegisterModernMenus

global OPENNOVA_FILE_ASE_ACTION_GUID = "5CB72814-7B1D-4E71-9E31-0F5C4F4E4D01"
global OPENNOVA_FILE_ANIMS_ACTION_GUID = "5CB72814-7B1D-4E71-9E31-0F5C4F4E4D02"

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

fn openNovaRegisterModernMenus =
(
    local menuMgr = callbacks.notificationParam()
    if menuMgr == undefined do return false
    local mainMenuBar = menuMgr.mainMenuBar
    if mainMenuBar == undefined do return false

    local fileMenu = undefined
    try (fileMenu = menuMgr.GetMenuById "eed3eaef-ea24-4342-aacc-9dfd87f9a4f4") catch()
    if fileMenu == undefined do fileMenu = openNovaFindModernMenuByTitle mainMenuBar #("&File", "File")

    local exportMenu = undefined
    if fileMenu != undefined do
    (
        exportMenu = openNovaFindModernMenuByTitle fileMenu #("&Export", "Export", "&Export...", "Export...")
        if exportMenu == undefined do exportMenu = fileMenu
    )
    if exportMenu != undefined do
    (
        openNovaCreateModernAction exportMenu OPENNOVA_FILE_ASE_ACTION_GUID "OpenNovaExportAse`OpenNova" "Novalogic ASE (.ase)"
        openNovaCreateModernAction exportMenu OPENNOVA_FILE_ANIMS_ACTION_GUID "OpenNovaExportAnims`OpenNova" "Novalogic Anims (.adm + .bad)"
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
    local exportContextRegistered = false
    try (exportContextRegistered = menuMan.registerMenuContext 0x5cb72811) catch()
    if not exportContextRegistered do
    (
        print "OpenNova legacy File > Export registration context already exists; rebuilding menu entries."
    )

    local mainMenuBar = menuMan.getMainMenuBar()
    openNovaRemoveLegacyMenuItemByTitle mainMenuBar "OpenNova"

    local fileMenu = menuMan.findMenu "&File"
    if fileMenu == undefined do fileMenu = menuMan.findMenu "File"
    local exportMenu = undefined
    if fileMenu != undefined do
    (
        exportMenu = openNovaFindSubMenu fileMenu #("&Export", "Export", "&Export...", "Export...")
        if exportMenu == undefined do exportMenu = fileMenu
    )
    if exportMenu != undefined do
    (
        openNovaRemoveLegacyMenuItemByTitle exportMenu "Novalogic ASE (.ase)"
        openNovaRemoveLegacyMenuItemByTitle exportMenu "Novalogic Anims (.adm + .bad)"

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
            exportMenu.addItem aseItem -1
        )

        local animsItem = menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"
        openNovaLogLegacyAction "OpenNovaExportAnims" animsItem
        if animsItem != undefined then
        (
            try
            (
                animsItem.setTitle "Novalogic Anims (.adm + .bad)"
                animsItem.setUseCustomTitle true
            )
            catch()
            exportMenu.addItem animsItem -1
        )
        menuMan.updateMenuBar()
    )
)
catch
(
    print ("OpenNova File > Export menu registration failed: " + getCurrentException())
)
'''.strip()


def install_menu():
    # type: () -> None
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - only available inside Max
        raise RuntimeError("pymxs is not available; menu install only runs inside 3ds Max.") from exc
    pymxs.runtime.execute(build_menu_script())
