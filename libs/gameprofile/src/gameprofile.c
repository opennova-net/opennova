#include "gameprofile/gameprofile.h"

#include <stddef.h>
#include <ctype.h>

/* The PFF container key is universal across every NovaLogic title we have reversed (the ROL7
   keystream seeded 0x0312A4CE, verified vs Jointops.exe PFF_LoadFileToMemory @ 0x768920). The
   per-game row still carries the key so a future un-reversed game only needs a new table entry,
   not a code change. No BFC1 compressor exists, so every row uses bfc1_compress 0 and defaults new
   archives to PFF3.

   SCR keying is per-game. Retail JO/DFX2 version-1 payloads use the JO_DFX2 key, which the SCR
   version byte selects, so those rows use VERSION_DETECT. The JO Demo stamps the SAME version byte
   (1) on every SCR file but keys them with the DEFAULT key (0xABEEFACE) — verified by decrypting
   demores.pff: all 214 version-1 payloads (.def/.adm/.aip/.ptg/.ptl/.ptu/.txt) decode cleanly under
   DEFAULT and to garbage under JO_DFX2, and the demo ships no version-2 files. So the demo forces
   the DEFAULT key. */
#define PFF_CONTAINER_KEY_DEFAULT 0x0312A4CEu
#define GAMEPROFILE_FORMAT_PFF3   0  /* mirrors PffFormat in pff/pff.h */

static const NovaGameProfile k_profiles[] = {
    { NOVA_GAME_JO,      "jo",     "Joint Operations",             PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_JO_DEMO, "jodemo", "Joint Operations (Demo)",      PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_FORCE_DEFAULT,   0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_DFX,     "dfx",    "Delta Force: Xtreme",          PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_DFX2,    "dfx2",   "Delta Force: Xtreme 2",        PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_BHD,     "bhd",    "Delta Force: Black Hawk Down", PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
};

/* Case-insensitive ASCII compare of two NUL-terminated strings. */
static int code_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

int gameprofile_count(void) {
    return (int)(sizeof(k_profiles) / sizeof(k_profiles[0]));
}

const NovaGameProfile *gameprofile_at(int index) {
    if (index < 0 || index >= gameprofile_count()) {
        return NULL;
    }
    return &k_profiles[index];
}

const NovaGameProfile *gameprofile_by_id(int game_id) {
    int i;
    for (i = 0; i < gameprofile_count(); ++i) {
        if (k_profiles[i].id == game_id) {
            return &k_profiles[i];
        }
    }
    return NULL;
}

const NovaGameProfile *gameprofile_by_code(const char *code) {
    int i;
    if (code == NULL) {
        return NULL;
    }
    for (i = 0; i < gameprofile_count(); ++i) {
        if (code_ieq(k_profiles[i].code, code)) {
            return &k_profiles[i];
        }
    }
    return NULL;
}

int gameprofile_scr_policy_for_code(const char *code) {
    const NovaGameProfile *p = gameprofile_by_code(code);
    return p ? p->scr_policy : SCR_POLICY_VERSION_DETECT;
}
