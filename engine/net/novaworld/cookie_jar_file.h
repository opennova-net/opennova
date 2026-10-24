#pragma once

#include <net/novaworld/http_login.h> // CookieJar, CookieEntry
#include <net/novaworld/lobby_vars.h> // RetailMachineInputs (the volume's information)

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The menu browser's cookie jar across processes: the file nw_cdata.coo, which the menu's creation
// loads into the process's one jar and five sites save it to, and the process's menu browser state
// the NovaWorld web flow (LobbyHttpFlow) works on. The file lives where retail opens it by its bare
// name, the game's working directory (LaunchFlags.working_dir: where game.cfg and player.sav live).
namespace opennova {

inline constexpr char kCookieJarFileName[] = "nw_cdata.coo";

// The file: a word the reader skips (the save's express-login flag, 1 or 0), the magic 0x434F4F32
// ("2OOC" on disk), then one record per saved entry: the node's word (0: the one node every jar
// access uses), the entry's five strings (name, value, expires, path, domain), each a u32le length
// and that many bytes encrypted under the volume key (a null string and an empty one both a length
// of 0 and no bytes), then the entry's word. Everything is little-endian.
// [orig: CookieJar_SaveToFile @0x64EE60 - the magic stored @0x64EE8F, written @0x64EE97; the node
//  word written @0x64EF16, the strings from @0x64EF1B, the entry's word written @0x64F23B;
//  CUIStringTable_LoadFromFile @0x64F290 - the magic's test @0x64F30B]
inline constexpr uint32_t kCookieJarFileMagic = 0x434F4F32u;

// The 16 bytes retail makes of the current directory's volume (GetVolumeInformationA on a null
// root): the serial's "%08X" text XORed in twice (its characters 0..7 into bytes 0..7 and,
// mirrored, 15..8), the serial word into bytes 0..3 and 12..15, the serial XOR the maximum
// component length into 4..7, the serial XOR the file system flags into 8..11, then the volume
// name's and the file system name's first 16 characters into the leading bytes.
// [orig: CDKey_GenerateVolumeFingerprint @0x651ED0 - the 16 bytes cleared @0x651EF1..0x651EFE,
//  GetVolumeInformationA called @0x651F27, sprintf "%08X" called @0x651F3C, the text's XORs
//  @0x651F41..0x651FB1, the serial's words @0x651FB4..0x651FC9, the volume name @0x651FCC..0x652005,
//  the file system name @0x65200D..0x652046]
std::array<uint8_t, 16> volume_fingerprint(const RetailMachineInputs &volume);

// The jar file's cipher key: the fingerprint as the C string the cipher reads, up to its first
// zero byte (byte 16 is set to 0). So a file written on one volume does not decrypt on another,
// on another machine or after the volume's serial changes: its strings come back as other bytes.
// [orig: CookieJar_SaveToFile @0x64EE60 - CDKey_GenerateVolumeFingerprint called @0x64EE7B, the
//  key's byte 16 cleared @0x64EE8A; CUIStringTable_LoadFromFile @0x64F290 - called @0x64F2FB,
//  cleared @0x64F300; Crypto_EncryptBuffer @0x437510 - the key's strlen]
std::string cookie_jar_key(const RetailMachineInputs &volume);

// The file's bytes for `jar`: the leading word (1 unless `remember_login_unchecked`), the magic,
// then a record for each entry, in the jar's order, whose name begins "PERSISTENT"
// (case-insensitively), PERSISTENTEXPRESSLOGINDATA only when the leading word is 1. Each string is
// written to its first NUL and encrypted with Crypto_EncryptBuffer under `key` (our nwu_decrypt,
// the ADD chain).
// [orig: CUIScene_SaveCookieJarFile @0x63BA60 - the REMEMBER_LOGIN property read
//  (CUIScene_FindNamedValue called @0x63BA66), the flag 0 only for "UNCHECKED" (_stricmp
//  @0x63BA75), fopen "wb" called @0x63BA9A, the flag written @0x63BABD, CookieJar_SaveToFile called
//  @0x63BAC8; CookieJar_SaveToFile @0x64EE60 - the slot walk @0x64EEB0, "PERSISTENT" compared
//  (String_CompareNoCaseN, 10) @0x64EEDE, PERSISTENTEXPRESSLOGINDATA @0x64EEFA against the flag
//  @0x64EF06, a string's strlen @0x64EF30..0x64EF39, Crypto_EncryptBuffer called @0x64EF71, its
//  length and bytes written @0x64EF76..0x64EF8E, a null string's 0 @0x64EFAE]
std::vector<uint8_t> write_cookie_jar(const CookieJar &jar, bool remember_login_unchecked,
		std::string_view key);

// The file's records appended to `jar` as the loader appends them (no store, no match): each string
// decrypted with Crypto_DecryptBuffer under `key` (our nwu_encrypt, the SUB chain). A file shorter
// than its leading word and four bytes more, or one whose magic is not kCookieJarFileMagic, adds
// nothing. A record whose length word or bytes would run past the file ends the load, the partial
// record dropped (retail reads its buffer's slack there, or past it). A record under a node word
// other than 0 is skipped: retail keeps it in a node no jar access reads, and no retail save writes
// one.
// [orig: CUIStringTable_OpenAndLoad @0x63A500 - fopen "rb" called @0x63A50B, the leading word read
//  and dropped (fread called @0x63A52D), CUIStringTable_LoadFromFile called @0x63A533;
//  CUIStringTable_LoadFromFile @0x64F290 - fewer than 4 bytes past the word @0x64F2CD, the magic
//  @0x64F30B, the first record's test @0x64F323, each length's bound @0x64F33A.., the bytes copied
//  and Crypto_DecryptBuffer called @0x64F373 for the first string, the record's word @0x64F49F,
//  the node by the word's low byte @0x64F4AC and the word @0x64F4D0, the entries grown by one
//  @0x64F5A7..0x64F5FF]
void read_cookie_jar(const uint8_t *data, size_t size, std::string_view key, CookieJar &jar);

// The menu's browser state the NovaWorld web flow works on, held for the process: the menu's
// creation makes it (the scene's properties dropped, the jar emptied and loaded from the file) and
// it lives until the menu's teardown on the way out; the NovaWorld entries, sessions and matches
// in between all use it.
// [orig: Menu_InitShellResources @0x552500 - g_GameMenu made only while null (@0x5525A6,
//  CUIManager_Create called @0x5525D8, which empties the jar's table: CookieJar_ClearTable
//  @0x64E500 called @0x64AF6D), CUIStringTable_OpenAndLoad("nw_cdata.coo") called @0x552630;
//  CGameMenu_Destructor @0x63E100 - CUIScene_DestroyAllContent called @0x63E137]
struct MenuBrowserState {
	CookieJar jar;
	// The sign-in page's REMEMBER_LOGIN checkbox as the scene holds it (the checkbox is GLOBAL_VAR:
	// each state write sets the scene property of its name, "CHECKED" or "UNCHECKED"): unset until
	// a sign-in page's activation sets the checkbox, then the checkbox's state.
	// [orig: CCheckWnd_SetCheckedState @0x64AC70 - the property's write (CUIScene_SetNamedValue called
	//  @0x64AC9B) when the checkbox has its scene (+0xF8 @0x64AC77); CCheckWnd_GetStateString
	//  @0x64AD40]
	std::optional<bool> remember_login_checked;
	// A LOGIN control was pressed (g_LoginButtonPressed: set by the press, cleared when the home
	// page shows). A process global, not the scene's: the menu's re-creation keeps it.
	// [orig: UI_MarkLoginButtonPressed @0x557520; UI_SaveCookieJarOnHomeTabs @0x557530 - the clear
	//  @0x557540]
	bool login_button_pressed = false;
	// The file the saves write and the creation loads ("" when the state has none: a flow's own
	// state in tests and opennova-serve), and its cipher key (cookie_jar_key).
	std::string file_path;
	std::string key;

	// The menu's creation: the scene's REMEMBER_LOGIN property dropped, the jar emptied, then the
	// file at `path` loaded into it (none, or one that fails to open, loads nothing).
	void create(std::string path, std::string jar_key);
	// The save: the file written whole (write_cookie_jar under the REMEMBER_LOGIN property), or
	// nothing with no file. False, `error` set, when it cannot be written (retail's HRESULT, which
	// every caller drops). Retail saves at five sites, ours at their equivalents:
	//  - the menu's teardown that destroys it (the way out, and a mods reload ahead of the menu's
	//    re-creation): MainGame.request_quit, MenuShell._mount_expansion;
	//  - the NovaWorld screen's deactivation: the NovaWorld panel's close
	//    (NetSessionController._on_novaworld_closed);
	//  - the home page's showing: LobbyHttpFlow::show_home_page;
	//  - a page whose title is a connect string, ahead of its parse:
	//    LobbyHttpFlow::save_at_connect_page;
	//  - an external browser's open: MenuShell._on_url_requested.
	// [orig: Menu_TeardownShellAndCloseBinkVideos @0x54E430 - under g_MenuDestroyOnTeardown (read
	//  @0x54E491, its jz @0x54E4A3), CUIScene_SaveCookieJarFile called @0x54E4AA (the flag set by
	//  Game_ReloadExpansionAndMods @0x552756 and by the quit confirmations @0x55578F / @0x5571DF);
	//  UI_DispatchScreenEvent @0x54E6A0 - case 6 (a screen's deactivation) on NW_MULTI_PLAYER
	//  (_stricmp @0x54F04B) runs UI_SaveCookieFileIfDirty (called @0x54F057; its save called
	//  @0x556B9F), case 7's save called @0x54F2C5; UI_SaveCookieJarOnHomeTabs @0x557530 - called
	//  @0x55753B; UIScene_OpenUrl @0x63E2F0 - the external open's save called @0x63E3CB]
	bool save(std::string &error) const;
};

// The process's one menu browser state.
MenuBrowserState &process_menu_browser();

} // namespace opennova
