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

const GameProfile k_profiles[] = {
    { GAME_JO,      "jo",     "Joint Operations",             PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT },
    { GAME_JO_DEMO, "jodemo", "Joint Operations (Demo)",      PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_FORCE_DEFAULT },
    { GAME_DFX,     "dfx",    "Delta Force: Xtreme",          PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT },
    { GAME_DFX2,    "dfx2",   "Delta Force: Xtreme 2",        PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT },
    { GAME_BHD,     "bhd",    "Delta Force: Black Hawk Down", PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT },
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

} // namespace opennova::gameprofile
