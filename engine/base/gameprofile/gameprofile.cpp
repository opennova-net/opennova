#include <base/gameprofile/gameprofile.h>

#include <base/io/strutil.h>

#include <iterator>

namespace opennova::gameprofile {

/* The PFF container key is universal across every NovaLogic title we have reversed (the ROL7
   keystream seeded 0x0312A4CE, verified vs Jointops.exe PFF_LoadFileToMemory @ 0x768920). The
   per-game row still carries the key so a future un-reversed game only needs a new table entry,
   not a code change.

   SCR keying is per-game. Retail JO/DFX2 version-1 payloads use the JO_DFX2 key, which the SCR
   version byte selects, so those rows use VERSION_DETECT. The JO Demo stamps the SAME version byte
   (1) on every SCR file but keys them with the DEFAULT key (0xABEEFACE) — verified by decrypting
   demores.pff: all 214 version-1 payloads (.def/.adm/.aip/.ptg/.ptl/.ptu/.txt) decode cleanly under
   DEFAULT and to garbage under JO_DFX2, and the demo ships no version-2 files. So the demo forces
   the DEFAULT key. */
#define PFF_CONTAINER_KEY_DEFAULT GAMEPROFILE_PFF_CONTAINER_KEY_DEFAULT

namespace {

/* The version texts, each the binary's own sprintf(buffer, "V%i.%i.%i.%i", ...) at startup, the
   four numbers pushed as immediates or as a register the function set (the build's packed
   version dword, stored beside it, agrees):
   - JO (JO:CA Jointops.exe): 1, 7, 5, 7 into the menu's buffer and the session's, ebx = 1
     [orig: Game_ParseCommandLineAndInit @ 0x4a7310 — ebx @ 0x4a737b, the menu's
     @ 0x4a7d5c..0x4a7d6d (read by UI_OnStartupScreenActivate @ 0x555852), the session's
     @ 0x4a7d75..0x4a7d90 (the ClientAuth copy @ 0x569b56), the dword 0x01070507 @ 0x4a7d86].
   - JO demo (jodemo.exe, the demo install's): 1, 0, 0, 9, ebx = 1 and ebp = 0 [orig: jodemo.exe
     @ 0x48ad10 — ebx @ 0x48ad52, ebp @ 0x48b33e, the sprintf @ 0x48b440..0x48b459, the dword
     0x01000009 @ 0x48b44f].
   - DFX (DFX.EXE, the project's IDB's): 1, 7, 1, 9, ebx = 1 [orig: DFX.EXE @ 0x4a6180 — ebx
     @ 0x4a61eb, the sprintf @ 0x4a6b93..0x4a6bad, the dword 0x01070109 @ 0x4a6ba3].
   - DFX2 (dfx2.exe, the DFX2 install's): 1, 7, 5, 7, ebx = 1, both buffers as JO [orig: dfx2.exe
     @ 0x4a7360 — ebx @ 0x4a73cb, the sprintfs @ 0x4a7d8c..0x4a7d9d and @ 0x4a7da5..0x4a7dc0,
     the dword 0x01070507 @ 0x4a7db6].
   - BHD: no binary read; no version. */
const GameProfile k_profiles[] = {
    { GAME_JO,      "jo",     "Joint Operations",             PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, "V1.7.5.7" },
    { GAME_JO_DEMO, "jodemo", "Joint Operations (Demo)",      PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_FORCE_DEFAULT,  "V1.0.0.9" },
    { GAME_DFX,     "dfx",    "Delta Force: Xtreme",          PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, "V1.7.1.9" },
    { GAME_DFX2,    "dfx2",   "Delta Force: Xtreme 2",        PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, "V1.7.5.7" },
    { GAME_BHD,     "bhd",    "Delta Force: Black Hawk Down", PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, nullptr },
};

} // namespace

int gameprofile_count(void) {
    return static_cast<int>(std::size(k_profiles));
}

const GameProfile *gameprofile_at(int index) {
    if (index < 0 || index >= gameprofile_count()) {
        return nullptr;
    }
    return &k_profiles[index];
}

const GameProfile *gameprofile_by_id(int game_id) {
    for (const GameProfile &profile : k_profiles) {
        if (profile.id == game_id) {
            return &profile;
        }
    }
    return nullptr;
}

const GameProfile *gameprofile_by_code(const char *code) {
    if (code == nullptr) {
        return nullptr;
    }
    for (const GameProfile &profile : k_profiles) {
        if (strutil::iequals(profile.code, code)) {
            return &profile;
        }
    }
    return nullptr;
}

int gameprofile_scr_policy_for_code(const char *code) {
    const GameProfile *p = gameprofile_by_code(code);
    return p ? p->scr_policy : SCR_POLICY_VERSION_DETECT;
}

const char *gameprofile_version_text_for_code(const char *code) {
    const GameProfile *p = gameprofile_by_code(code);
    return (p ? p : gameprofile_by_id(GAME_JO))->version_text;
}

} // namespace opennova::gameprofile
