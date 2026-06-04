#include "gameprofile/gameprofile.h"

#include <stddef.h>

/* The PFF container key is universal across every NovaLogic title we have reversed (the ROL7
   keystream seeded 0x0312A4CE, verified vs Jointops.exe PFF_LoadFileToMemory @ 0x768920). The
   per-game row still carries the key so a future un-reversed game only needs a new table entry,
   not a code change. All shipping payloads decode by SCR version, and no BFC1 compressor exists,
   so every row uses VERSION_DETECT + bfc1_compress 0 and defaults new archives to PFF3. */
#define PFF_CONTAINER_KEY_DEFAULT 0x0312A4CEu
#define GAMEPROFILE_FORMAT_PFF3   0  /* mirrors PffFormat in pff/pff.h */

static const NovaGameProfile k_profiles[] = {
    { NOVA_GAME_JO,      "Joint Operations",             PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_JO_DEMO, "Joint Operations (Demo)",      PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_DFX,     "Delta Force: Xtreme",          PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_DFX2,    "Delta Force: Xtreme 2",        PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
    { NOVA_GAME_BHD,     "Delta Force: Black Hawk Down", PFF_CONTAINER_KEY_DEFAULT, SCR_POLICY_VERSION_DETECT, 0, GAMEPROFILE_FORMAT_PFF3 },
};

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
