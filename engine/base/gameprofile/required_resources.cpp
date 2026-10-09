/* The witnessed boot-required resource manifest (ENG-6). Every row is
   transcribed from the R8 record (docs/required-resources.md) — that file is
   the witness source; edits land there first and here in the same change. */
#include <base/gameprofile/required_resources.h>

#include <base/io/strutil.h>
#include <formats/pff/pff.h>

#include <stddef.h>
#include <string.h>

namespace opennova::gameprofile {

namespace {

const RequiredResource k_required_resources[] = {
    /* --- Boot (Game_Run -> Game_InitSubsystems), witnessed order --- */
    { "game.cfg", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "missing -> silent Config_SetDefaults; version key != 0x1D re-defaults",
      "[orig: Game_LoadConfig @ 0x551480 via File_ParseASCIIFileWithCallback @ 0x53d980]", "game_cfg" },
    { "fgn2.bin", BOOT_PHASE_BOOT, RES_OPTIONAL, 0,
      "existence flag only, never parsed",
      "[orig: Game_LoadConfig @ 0x5514e8]", "fgn2_bin" },
    { "assets.cd", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "missing -> empty string, silent",
      "[orig: Game_ReadAssetsCDFile @ 0x4a5800]", "assets_cd" },
    { "CC.BIN", BOOT_PHASE_BOOT, RES_OPTIONAL, 0,
      "missing -> empty country code, silent",
      "[orig: Game_ReadCCBinFile @ 0x4a5860; value @ 0x4a5950]", "cc_bin" },
    { "filter.txt", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip",
      "[orig: ChatFilter_LoadFromFile @ 0x4fd640]", "filter_txt" },
    { "expansion\\<n>\\<n>.pff", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PATTERN,
      "absent -> expansion unregistered, base game proceeds (<n>L.pff sibling; <n>.bin names it)",
      "[orig: Expansion_ScanAndRegister @ 0x4a43d0; Expansion_LoadAssets @ 0x4a4730]", "expansion_archive" },
    /* An expansion's own boot files (RES_F_EXPANSION, <n> its name): its text table, read from the
       loose file alone (docs/vfs/vfs-pff-mount-re.md § Expansions), and its version text. */
    { "expansion\\<n>\\<n>.bin", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "missing -> no text override table, every string the base game's; the Mods list then reads <n>L.pff's copy, "
      "else names it \"Unnamed Expansion\"",
      "[orig: TextResource_LoadOverrideTable @ 0x4a49de through File_LoadResource @ 0x75b540; "
      "Expansion_ScanAndRegister @ 0x4a455b]", "expansion_table" },
    { "expansion\\<n>\\version.txt", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "missing -> checksum 0; a host with an expansion refuses a joiner whose checksum differs",
      "[orig: Expansion_LoadAssets @ 0x4a4858..0x4a4885; Server_ValidatePlayerJoinRequest @ 0x51232f]",
      "expansion_version" },
    { "resource.pff", BOOT_PHASE_BOOT, RES_FATAL, RES_F_PFF_TABLE_ANY,
      "zero boot-table archives opened -> earlyerr.txt line-3 dialog + exit; any single archive may be absent",
      "[orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @ 0x829f90; fatal check @ 0x4a6f44]", "resource_pff" },
    { "localres.pff", BOOT_PHASE_BOOT, RES_FATAL, RES_F_PFF_TABLE_ANY,
      "zero boot-table archives opened -> earlyerr.txt line-3 dialog + exit; any single archive may be absent",
      "[orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @ 0x829f90; fatal check @ 0x4a6f44]", "localres_pff" },
    { "language.pff", BOOT_PHASE_BOOT, RES_FATAL, RES_F_PFF_TABLE_ANY,
      "zero boot-table archives opened -> earlyerr.txt line-3 dialog + exit; any single archive may be absent",
      "[orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @ 0x829f90; fatal check @ 0x4a6f44]", "language_pff" },
    { "gameerr.bin", BOOT_PHASE_BOOT, RES_DIALOG, 0,
      "missing -> earlyerr.txt line-4 dialog, then boot continues",
      "[orig: Game_InitSubsystems @ 0x4a6fc8]", "gameerr" },
    { "gametext.bin", BOOT_PHASE_BOOT, RES_FATAL, 0,
      "\"Unable to load game strings\" MessageBox + exit",
      "[orig: Game_InitSubsystems @ 0x4a6fed]", "gametext" },
    { "vmacros.bin", BOOT_PHASE_BOOT, RES_FATAL, 0,
      "\"Unable to load voice macro strings\" MessageBox + exit",
      "[orig: Game_InitSubsystems @ 0x4a702f]", "vmacros" },
    { "keyhelp.bin", BOOT_PHASE_BOOT, RES_FATAL, 0,
      "\"Unable to load keyboard map strings\" MessageBox + exit",
      "[orig: Game_InitSubsystems @ 0x4a7072]", "keyhelp" },
    { "weapon.def", BOOT_PHASE_BOOT, RES_REQUIRED, 0,
      "missing -> silent; weapon table left with a single \"None\" entry (SCR-encrypted supported, key 0x2A5A8EAD)",
      "[orig: WeaponDef_LoadAll @ 0x54dd10; mission slots Game_StartMission @ 0x5254b8]", "weapon_def" },
    { "Avatars.def", BOOT_PHASE_BOOT, RES_OPTIONAL, 0,
      "silent skip",
      "[orig: CAvatarDefs_Init @ 0x57b180]", "avatars_def" },
    { "SndProf.def", BOOT_PHASE_BOOT, RES_REQUIRED, 0,
      "missing -> the boot goes on, but the 128-slot profile table it allocated is never cleared and every item definition binds its first slot (the \"default\" lookup's miss): a mission's items then take their sound sets from that uncleared memory, and the game can hang or crash once one is registered or played (a genx or envs item's loop, the player's footsteps)",
      "[orig: Game_InitSubsystems @ 0x4a7141 -> SoundProfile_LoadAll @ 0x527490 (the table AudioMem_AllocWithLabel @ 0x5274ce -> FastMem_Alloc @ 0x7697b0, no clear; the miss File_ParseASCIIFile @ 0x53d823); ItemDef_AllocateWithDefaults @ 0x49e3ea -> SoundProfile_FindSlotByName @ 0x526e30 (no profile: the table @ 0x526e6a); ItemDef_ResolveAllResources @ 0x49e62d..0x49e687; genx Entity_UpdateParentTransform @ 0x4a8d4f -> Entity_UpdateEnvSoundEmitter @ 0x4a81de -> SoundEmitter_RegisterSetLayers @ 0x528340]", "sndprof_def" },
    { "gt.ssc", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip (gate-tag override, key \"jop:2:oyez\")",
      "[orig: Mission_LoadEncryptedConfig @ 0x4cdcd0]", "gt_ssc" },
    { "items.def", BOOT_PHASE_BOOT, RES_REQUIRED, 0,
      "missing -> the boot goes on with no item definitions: the loader returns 1 whatever its parse did, so the \"Unable to load items.def\" fatal wired at the callsite is never reached",
      "[orig: Game_InitSubsystems @ 0x4a71a3 -> ItemDefs_LoadAndValidate @ 0x4a1da0 (File_ParseASCIIFile's result unread @ 0x4a1e12, returns 1 @ 0x4a20a2); the unreachable fatal @ 0x4a71af -> Game_FatalErrorWithMessageBox @ 0x4a5160]", "items_def" },
    { "charattr.def", BOOT_PHASE_BOOT, RES_SOFT, 0,
      "missing -> _errlog.txt \"Server ERROR! Could not load charattr definitions.\", continues",
      "[orig: Game_Run @ 0x4a7fe3 -> CharAttr_LoadFromDef @ 0x412140]", "charattr_def" },
    { "loading.pcx", BOOT_PHASE_BOOT, RES_OPTIONAL, 0,
      "graceful texture miss",
      "[orig: Game_ShowLoadingScreen @ 0x4a544f]", "loading_pcx" },
    { "*.npj/*.npz", BOOT_PHASE_BOOT, RES_REQUIRED, RES_F_PATTERN,
      "none found -> empty mission list (the only wildcard scan at boot)",
      "[orig: MissionList_ScanAndBuildFromFiles @ 0x563170]", "mission_scan" },
    { "hiscore.txt", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip",
      "[orig: HUD_LoadHighScoreText @ 0x5630e5]", "hiscore_txt" },
    { "_ffp.fx", BOOT_PHASE_BOOT, RES_REQUIRED, 0,
      "missing -> the boot goes on, but the renderer registers no fixed-function shader (its twelve FF_ tags and their #UV twins); with no other effect (.fx) in the archives its registry is empty, every model material resolves to an entry holding no pass, and no model draws (the terrain, the sky and the HUD do)",
      "[orig: Game_InitSubsystems @ 0x4a729c -> Render_InitAllSubsystems @ 0x586439 -> HLSLEffect_InitAndLoadAll @ 0x5b0080 -> HLSLEffect_InitFixedFunctionShaders @ 0x5af790 (the name @ 0x5afa3e, the failed load @ 0x5afd25); the missing tag clamped to entry 0, Material_ResolveEffectSubobjectsAndShader @ 0x5b18cd; no pass drawn, CRenderBatchQueue_FlushBatches @ 0x5da228]", "ffp_shader" },
    { "admin.cfg", BOOT_PHASE_BOOT, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip",
      "[orig: CAdminServer_LoadConfig @ 0x406d80; callsite @ 0x4a72c2]", "admin_cfg" },

    /* --- Main menu (Menu_InitShellResources enter), witnessed order --- */
    { "game.bin", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "load unchecked; menu strings absent if missing",
      "[orig: Menu_InitShellResources @ 0x552510, lazy resource @ 0x25510f8]", "game_bin" },
    { "player.sav", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "missing/bad magic -> profile defaults + first-run flag",
      "[orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 (@ 0x54f586)]", "player_sav" },
    { "weapon.sav", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "missing/bad magic -> profile defaults; resolved expansion\\<n>\\ first",
      "[orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 (@ 0x54f6c7)]", "weapon_sav" },
    { "prolog.BIK", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "exists-checked, skipped (loose-only via Win32 OpenFile, never in PFFs)",
      "[orig: Game_PlayIntroVideos @ 0x5637a0]", "prolog_bik" },
    { "intro.BIK", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "exists-checked, skipped (loose-only via Win32 OpenFile, never in PFFs)",
      "[orig: Game_PlayIntroVideos @ 0x5637a0]", "intro_bik" },
    { "MENUMUS.SBF", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful (silent menu music); expansion form M<n>.sbf; bank streams loose by path",
      "[orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60; names set Expansion_LoadAssets @ 0x4a4798]", "menumus_sbf" },
    { "MENUMUS.BIN", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful (silent menu music); expansion form M<n>.bin",
      "[orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60; names set Expansion_LoadAssets @ 0x4a4798]", "menumus_bin" },
    { "expansion\\<n>\\M<n>.sbf", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "in place of MENUMUS.SBF under /exp, set whether it exists or not: missing -> no menu music; streams loose by path",
      "[orig: Expansion_LoadAssets @ 0x4a4906; AudioVM_InitMenuMusicStreaming @ 0x56aa60]", "expansion_menumus_sbf", "MENUMUS.SBF" },
    { "M<n>.bin", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "in place of MENUMUS.BIN under /exp: missing -> no menu music",
      "[orig: Expansion_LoadAssets @ 0x4a491d; AudioVM_InitMenuMusicStreaming @ 0x56aa60]", "expansion_menumus_bin", "MENUMUS.BIN" },
    { "menu_style.mns", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "silent skip -> unstyled UI (menu fonts/colors come from its KEY set)",
      "[orig: Menu_InitShellResources @ 0x552604 via NapiConfigMap_LoadIncludeFile @ 0x63b970]", "menu_style" },
    { "brand.mns", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "silent skip (retail ships none; appended after menu_style.mns)",
      "[orig: Menu_InitShellResources @ 0x552616 via NapiConfigMap_LoadIncludeFile @ 0x63b970]", "brand_style" },
    { "main.bik", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "expansion -> default path fallback; missing -> no menu video",
      "[orig: UI_CreateMenuBinkVideos @ 0x54b590; strings @ 0x7d2a00-0x7d2a54]", "main_bik" },
    { "header.bik", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "expansion -> default path fallback; missing -> no menu video",
      "[orig: UI_CreateMenuBinkVideos @ 0x54b590; strings @ 0x7d2a00-0x7d2a54]", "header_bik" },
    { "footer.bik", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "expansion -> default path fallback; missing -> no menu video",
      "[orig: UI_CreateMenuBinkVideos @ 0x54b590; strings @ 0x7d2a00-0x7d2a54]", "footer_bik" },
    { "nw_cdata.coo", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "HRESULT ignored",
      "[orig: Menu_InitShellResources @ 0x55262b -> CUIStringTable_OpenAndLoad @ 0x63a500]", "nw_cdata" },
    { "main.mnu", BOOT_PHASE_MENU, RES_FATAL, 0,
      "menu never appears — boot dead-ends silently (no dialog); the entry node is \"Startup\"",
      "[orig: Menu_InitShellResources @ 0x552651 -> UIScene_LoadAndParseContent @ 0x63c830 -> CUIScene_SelectNodeByName @ 0x63b6b0]", "main_menu" },
    { "menutxt.bin", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "fallback literals used (nw_error.mnx hardcoded @ 0x558449)",
      "[orig: UIStringTable_LookupAndDup @ 0x63b290; e.g. @ 0x55840e, @ 0x5561b7]", "menutxt" },
    { "Arial12b.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash (text renders nothing; width breakpoints 640/800/1024)",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_arial12b" },
    { "Arial14n.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_arial14n" },
    { "Arial14b.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_arial14b" },
    { "Arial16n.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_arial16n" },
    { "Arial16b.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_arial16b" },
    { "Impac22b.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash (Impact pair loads at every width)",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_impac22b" },
    { "Impac38b.fnt", BOOT_PHASE_MENU, RES_REQUIRED, 0,
      "missing -> null font slot, scale 1.0, no crash (Impact pair loads at every width)",
      "[orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot]", "font_impac38b" },
    { "menu.lwf", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful",
      "[orig: PlayerInfo_InitProfileSelector @ 0x5613ba]", "menu_lwf" },
    { "PI_Idle.BAD", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful (player-preview idle; PI_actv/PI_LookR/PI_lookL table @ 0x83c830)",
      "[orig: PlayerInfo_InitPreviewModel @ 0x5600d0 (@ 0x560107)]", "pi_idle_bad" },
    { "Dt1rst.bad", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful (player-preview rest pose)",
      "[orig: PlayerInfo_InitPreviewModel @ 0x5600d0 (@ 0x560138)]", "dt1rst_bad" },
    { "HwmCube.dds", BOOT_PHASE_MENU, RES_OPTIONAL, 0,
      "graceful (player-preview environment cube)",
      "[orig: PlayerInfo_InitPreviewModel @ 0x5600d0]", "hwmcube_dds" },
    { "epass.bin", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip (stored credentials)",
      "[orig: EPass_LoadStoredCredentials @ 0x450b20]", "epass_bin" },
    { "passgen.bin", BOOT_PHASE_MENU, RES_OPTIONAL, RES_F_PLAYER_FILE,
      "silent skip (stored credentials)",
      "[orig: EPass_LoadCredentials @ 0x450eb0]", "passgen_bin" },

    /* --- Mission start (Game_StartMission @ 0x524360), witnessed order --- */
    { "failsafe.bad", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "silent: the fallback for a clip that does not load; without it such a clip registers nothing "
      "(the slot plays its reset clip). Retail JO ships none",
      "[orig: AnimMap_Init @ 0x40be96, the load kept null or not @ 0x40bea2..0x40bead; "
      "AnimMap_FindOrLoadBoneFile @ 0x40c030, the failsafe entry @ 0x40c285, no failsafe @ 0x40c260]",
      "failsafe_bad" },
    { "<n>L.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "the mission banks' first slot under /exp, exists-checked, silent skip",
      "[orig: Expansion_LoadAssets @ 0x4a4989 -> @ 0x4a499d; the bank loop @ 0x525443 over the table @ 0x82a5b0]",
      "expansion_locl_lwf" },
    { "<n>.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "the mission banks' second slot under /exp, exists-checked, silent skip",
      "[orig: Expansion_LoadAssets @ 0x4a495e -> @ 0x4a4972; the bank loop @ 0x525443 over the table @ 0x82a5b0]",
      "expansion_lwf" },
    { "gamelocl.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked per slot, silent skip (expansion slots <exp>L.lwf/<exp>.lwf load first)",
      "[orig: Game_StartMission @ 0x525443 loop over the 260-byte-stride table @ 0x82a5b0 via SoundBank_LoadIfExists]", "gamelocl_lwf" },
    { "game.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked per slot, silent skip",
      "[orig: Game_StartMission @ 0x525443 loop over the 260-byte-stride table @ 0x82a5b0 via SoundBank_LoadIfExists]", "game_lwf" },
    { "game3.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked per slot, silent skip",
      "[orig: Game_StartMission @ 0x525443 loop over the 260-byte-stride table @ 0x82a5b0 via SoundBank_LoadIfExists]", "game3_lwf" },
    { "game2.lwf", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked per slot, silent skip",
      "[orig: Game_StartMission @ 0x525443 loop over the 260-byte-stride table @ 0x82a5b0 via SoundBank_LoadIfExists]", "game2_lwf" },
    { "ammo.def", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "silent (empty ammo table); SCR key 0x2A5A8EAD",
      "[orig: Game_StartMission @ 0x52548a -> AmmoDef_LoadAll @ 0x40b0b0]", "ammo_def" },
    { "powerup.def", BOOT_PHASE_MISSION, RES_SOFT, 0,
      "_errlog.txt \"Unable to load powerup.def\", continues",
      "[orig: Game_StartMission @ 0x5256cd -> PowerUpDef_LoadFromFile @ 0x443350]", "powerup_def" },
    { "medmssn.bin", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "the literal fallback for mission text (<missionbase>.bin exists-checked first)",
      "[orig: TextResource_LoadMissionTextBin @ 0x51ed90]", "medmssn_bin" },
    { "game.wac", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked, silent skip (compiled game -> server -> mission into one buffer)",
      "[orig: WacScript_InitAndLoad @ 0x4f91f0 (game @ 0x4f9454)]", "game_wac" },
    { "server.wac", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "exists-checked, silent skip",
      "[orig: WacScript_InitAndLoad @ 0x4f91f0 (server @ 0x4f94bc)]", "server_wac" },
    { "GAMEMUS.SBF", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (MP mission music; SP stops the music context); expansion form G<n>.sbf",
      "[orig: Game_StartMission @ 0x525581-0x525598 -> AudioVM_OpenMusicContext @ 0x6722a0; names @ 0x4a47da]", "gamemus_sbf" },
    { "GAMEMUS.BIN", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (MP mission music); expansion form G<n>.bin",
      "[orig: Game_StartMission @ 0x525581-0x525598 -> AudioVM_OpenMusicContext @ 0x6722a0; names @ 0x4a47da]", "gamemus_bin" },
    { "expansion\\<n>\\G<n>.sbf", BOOT_PHASE_MISSION, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "in place of GAMEMUS.SBF under /exp: missing -> no mission music; streams loose by path",
      "[orig: Expansion_LoadAssets @ 0x4a4936; @ 0x525581-0x525598]", "expansion_gamemus_sbf", "GAMEMUS.SBF" },
    { "G<n>.bin", BOOT_PHASE_MISSION, RES_OPTIONAL, RES_F_PATTERN | RES_F_EXPANSION,
      "in place of GAMEMUS.BIN under /exp: missing -> no mission music",
      "[orig: Expansion_LoadAssets @ 0x4a494a; @ 0x525581-0x525598]", "expansion_gamemus_bin", "GAMEMUS.BIN" },
    { "loadscrn.pcx", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission-load screen art)",
      "[orig: Render_LoadingScreen @ 0x521d10 (@ 0x521dd9)]", "loadscrn_pcx" },
    { "Arials18.fnt", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission-load screen font)",
      "[orig: Render_LoadingScreen @ 0x521d10]", "font_arials18" },
    { "Arial22.fnt", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission-load screen font)",
      "[orig: Render_LoadingScreen @ 0x521d10]", "font_arial22" },
    /* The seven menus a mission opens by name (UI_OpenMenuScreen @ 0x54e520): missing, the screen
       does not open (the load's E_FAIL skips the select @ 0x54e584); present without the named
       screen, the menu-open flag rises over no screen (@ 0x54e59e) and gates the in-mission keys
       off. Each must hold its screen and the controls the player leaves it by
       (docs/required-resources.md, "What a mission's menus must hold"). */
    { "cmap.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> the commander key opens no command map; without its CMAP screen -> a menu is up over nothing",
      "[orig: Game_StartMission @ 0x526316/@ 0x526332; Input_HandleActionBinding @ 0x49b91b]", "cmap_menu" },
    { "game.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> Esc opens no in-mission menu; without its INGAME screen -> a menu is up over nothing",
      "[orig: Input_HandleActionBinding @ 0x49b3b1]", "game_menu" },
    { "weapon.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> the armory opens no WEAPON screen; without it -> a menu is up over nothing",
      "[orig: Input_HandleActionBinding @ 0x49b8de; Input_HandleActionBinding_0 @ 0x4e0b44]", "weapon_menu" },
    { "vehicle.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> a vehicle's loadout opens no VEHICLE screen; without it -> a menu is up over nothing",
      "[orig: Input_HandleActionBinding @ 0x49b892; Input_HandleActionBinding_0 @ 0x4e0af8]", "vehicle_menu" },
    { "stat.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> the round's end opens no STAT screen; without it -> a menu is up over nothing",
      "[orig: UI_ProcessEndRoundScreenTransition @ 0x5b8636]", "stat_menu" },
    { "death.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> a death opens no DEATH deploy screen (no spawn to pick); without it -> a menu is up over nothing",
      "[orig: Render_ProcessMainSceneFrame @ 0x5cab7e]", "death_menu" },
    { "mp.mnu", BOOT_PHASE_MISSION, RES_REQUIRED, 0,
      "missing -> the menu coming back from a NovaWorld session selects no NW_MULTI_PLAYER screen",
      "[orig: UI_EnterNovaWorldMenu @ 0x5588fa -> CUIScene_SelectNodeByName @ 0x558953]", "mp_menu" },
    { "hudfx.def", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "silent skip",
      "[orig: HUD_InitOverlaySystem @ 0x5a4620 (hudfx @ 0x5a462e)]", "hudfx_def" },
    { "hudpos.def", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "silent skip (default HUD positions)",
      "[orig: HUD_InitOverlaySystem @ 0x5a4620 (hudpos @ 0x5a4931)]", "hudpos_def" },
    { "monogram.tga", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission UI texture)",
      "[orig: Game_StartMission @ 0x525aa3-0x525aad]", "monogram_tga" },
    { "boxtile.tga", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission UI texture)",
      "[orig: Game_StartMission @ 0x525aa3-0x525aad]", "boxtile_tga" },
    { "border.tga", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful (mission UI texture)",
      "[orig: Game_StartMission @ 0x525aa3-0x525aad]", "border_tga" },
    { "upl.3di", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "lazy, null on miss (celestial models are data-driven from the mission .env)",
      "[orig: EffectWorld_LoadCelestialModels @ 0x5add25]", "upl_3di" },
    { "couri20b.fnt", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "graceful",
      "[orig: @ 0x572a67 / @ 0x572f24]", "font_couri20b" },
    /* The single-player end screens' backdrops, loaded as the epilog builds its screen at the
       round's end: the .tga, else the .dds of its name; with neither the image node holds no
       texture and draws nothing, so the screen's lines stand over the live world. */
    { "jo_Epil.tga", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "missing (and no jo_Epil.dds) -> the single-player win screen draws over the live world, no backdrop",
      "[orig: Cine_EpilogStateMachineUpdate @ 0x576240, the name @ 0x57658e -> CinematicFadeEvent_LoadTexture "
      "@ 0x570d00 (the .tga @ 0x570da3, else the .dds @ 0x570de3; a miss leaves the texture null @ 0x570ded); "
      "sub_5710B0 @ 0x5710b0 draws none @ 0x5710e1]", "jo_epil_tga" },
    { "jo_Epil2.tga", BOOT_PHASE_MISSION, RES_OPTIONAL, 0,
      "missing (and no jo_Epil2.dds) -> the single-player lose screen (MISSION FAILED) draws over the live world, "
      "no backdrop",
      "[orig: Cinematic_EpilogUpdate @ 0x574491, the name @ 0x5745aa -> CinematicFadeEvent_LoadTexture "
      "@ 0x570d00 (the .tga @ 0x570da3, else the .dds @ 0x570de3; a miss leaves the texture null @ 0x570ded); "
      "sub_5710B0 @ 0x5710b0 draws none @ 0x5710e1]", "jo_epil2_tga" },
};

enum { k_required_resource_count =
           (int)(sizeof(k_required_resources) / sizeof(k_required_resources[0])) };

} // namespace

int gameprofile_required_resource_count(void) {
    return k_required_resource_count;
}

const RequiredResource *gameprofile_required_resource_at(int index) {
    if (index < 0 || index >= k_required_resource_count) {
        return NULL;
    }
    return &k_required_resources[index];
}

const RequiredResource *gameprofile_required_resource_find(const char *name) {
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < k_required_resource_count; ++i) {
        if (strutil::iequals(k_required_resources[i].name, name)) {
            return &k_required_resources[i];
        }
    }
    return NULL;
}

const RequiredResource *gameprofile_required_resource_by_role(const char *role) {
    if (!role) {
        return NULL;
    }
    for (int i = 0; i < k_required_resource_count; ++i) {
        if (strcmp(k_required_resources[i].role, role) == 0) {
            return &k_required_resources[i];
        }
    }
    return NULL;
}

namespace {

constexpr const char *kExpansionNamePlaceholder = "<n>";

// The last component of a row's name: what follows its last '\' (the folder the game reads it in).
std::string file_component(const RequiredResource &row) {
    const std::string name = row.name;
    const size_t slash = name.find_last_of('\\');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

} // namespace

std::string gameprofile_expansion_file_name(const RequiredResource *row, const std::string &expansion) {
    if (!row) {
        return std::string();
    }
    std::string out = file_component(*row);
    const size_t placeholder = strlen(kExpansionNamePlaceholder);
    for (size_t at = out.find(kExpansionNamePlaceholder); at != std::string::npos;
         at = out.find(kExpansionNamePlaceholder, at + expansion.size())) {
        out.replace(at, placeholder, expansion);
    }
    return out;
}

bool gameprofile_expansion_file_formed(const RequiredResource *row) {
    return row && file_component(*row).find(kExpansionNamePlaceholder) != std::string::npos;
}

bool gameprofile_expansion_names_fit_archive(const std::string &expansion) {
    // [orig: Expansion_LoadAssets: "M%s.bin" @ 0x4a491d (and "G%s.bin" @ 0x4a494a), "%sL.lwf" @ 0x4a4989]
    for (const char *role : {"expansion_menumus_bin", "expansion_locl_lwf"}) {
        const std::string file = gameprofile_expansion_file_name(gameprofile_required_resource_by_role(role), expansion);
        if (!pff::logical_name_fits_archive(file)) {
            return false;
        }
    }
    return true;
}

} // namespace opennova::gameprofile
