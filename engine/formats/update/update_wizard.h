#pragma once

// UPDATE.WIZ, the product's update wizard file; cc.bin's territory; update.inf.
// docs/update/updater-re.md §2. Read by apps/update (the client) and, through the game folder's
// product (base/resource_index/boot_policy.h folder_product), by the game's launch.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opennova::update {

// The wizard's [HEADER] values, each read into a 260-byte field (DRIVE one character).
// [orig: Cupdate2Dlg_ReadWizard @ 0x404c90 (UPDATE.EXE)]
inline constexpr size_t kWizardValueBytes = 260;

struct UpdateWizard {
	std::string cmd; // CMD: the command file's name, e.g. JOPTR.CMD
	std::string url; // URL, URL2, URL3: the gateways (retail overwrites them, §3)
	std::string url2;
	std::string url3;
	std::string dlg; // DLG
	std::string product_login; // PRODUCT_LOGIN: the login, e.g. JOTRqq before the territory
	std::string drive; // DRIVE: one character
	std::string dir; // DIR
	std::string local; // LOCAL
	std::string territory; // TERRITORY
};

// The wizard read as UPDATE.EXE reads it: every key from [HEADER], an absent key (or section)
// empty. Retail fails only when the file cannot be opened. [orig: Cupdate2Dlg_ReadWizard
// @ 0x404c90 (UPDATE.EXE)]
UpdateWizard read_update_wizard(std::string_view text);

// The wizard written from the model: [HEADER] and each non-empty key in UPDATE.EXE's read
// order, CR LF lines. Empty when a value cannot be carried (a CR, an LF or a ';', or more than
// the field's 259 characters).
std::string write_update_wizard(const UpdateWizard &wizard);

// cc.bin's two pairs: the territory (bytes 0-1) and the placeholder it replaces (bytes 2-3),
// "US" and "QQ" when the file is absent or short. JO:CA ships "usqq0409us".
// [orig: Cupdate2Dlg_ReadWizard @ 0x404c90 (UPDATE.EXE)]
struct Territory {
	char code[2] = { 'U', 'S' };
	char placeholder[2] = { 'Q', 'Q' };
};

// The pairs from cc.bin's bytes: each pair a two-byte read over the default, so a short file
// replaces only what it holds.
Territory territory_from_cc_bin(const uint8_t *data, size_t size);

// The wizard value with its FIRST placeholder replaced by the territory: the lower-case
// placeholder is searched first and replaced in lower case, else the upper-case one in upper
// case. "JOTRqq" -> "JOTRus". [orig: Cupdate2Dlg_SubstituteTerritory @ 0x404be0 (UPDATE.EXE)]
std::string substitute_territory(std::string_view value, const Territory &territory);

// Every wizard value but DRIVE with the territory substituted, as the reader leaves them.
// [orig: Cupdate2Dlg_ReadWizard @ 0x404c90 (UPDATE.EXE)]
UpdateWizard with_territory(UpdateWizard wizard, const Territory &territory);

// The command files of the products a game folder's stock wizard names (ADR 0067 d2): JO's, as
// JO:CA's UPDATE.WIZ ships it (§2), DFX2's, as the Steam DFX2 install's UPDATE.WIZ ships it
// (CMD=DFX2.CMD, PRODUCT_LOGIN=DFX2), and OP: Acid Gambit's own (the wizard the release build
// writes into its package, products/release_build.h). A CMD names no territory placeholder, so
// the territory never changes it.
inline constexpr std::string_view kJoCommandFile = "JOPTR.CMD";
inline constexpr std::string_view kDfx2CommandFile = "DFX2.CMD";
inline constexpr std::string_view kAcidGambitCommandFile = "ACIDGAMBIT.CMD";

// The product a wizard's CMD names: "jo", "dfx2" or "acidgambit" (ASCII case-blind, as the
// updater opens the file); "" for any other (BHD's DFBHD.CMD, an expansion's JOEPTR.CMD).
std::string wizard_product(std::string_view cmd);

// update.inf as REVUPDAT writes it before relaunching the updater.
// [orig: RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE)]
inline constexpr std::string_view kUpdateInfText = "[UPDATE]\r\nDAT=0\r\n";

} // namespace opennova::update
