// powerup.def: the pickup rows the Powerup items bind. The inline block is the
// shipped JO:CA table verbatim (four rows) plus the parser's edge arms; the
// reference-fixture copy, when OPENNOVA_JO_ASSETS carries one, is the SKIP-LEG
// retail leg. [orig: PowerUpDef_LoadFromFile @0x443350; PowerUpDef_ParseProperty
// @0x442EE0; PowerUpDef_RegisterNewEntry @0x442C00]
#include <formats/def/def.h>
#include <base/vfs/vfs.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/retail_paths.h"

using namespace opennova::def;

static int failures = 0;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static const char kShipped[] =
        "\r\n\r\n\r\n"
        "powerup \"FULLHP\"  \r\n"
        "\thp   -1\r\n"
        "\trespawn_time 30\r\n"
        "\tmax_respawns 1\r\n"
        "\taction \"respawn\"\r\n"
        "\t\tfunction powerup_respawn\r\n"
        "\tend\r\n"
        "\taction \"pickup\"\r\n"
        "\t\tsoundset HEALTH_UP\r\n"
        "\t\tfunction powerup_pickup\r\n"
        "\tend\r\n"
        "end\r\n"
        "\r\n"
        "powerup \"FULLHP_INF\"  \r\n"
        "\thp   -1\r\n"
        "\trespawn_time 30\r\n"
        "\taction \"respawn\"\r\n"
        "\t\tfunction powerup_respawn\r\n"
        "\tend\r\n"
        "\taction \"pickup\"\r\n"
        "\t\tsoundset HEALTH_UP\r\n"
        "\t\tfunction powerup_pickup\r\n"
        "\tend\r\n"
        "end\r\n"
        "\r\n"
        "powerup \"AmmoFull\"\r\n"
        "\trespawn_time 100\r\n"
        "\tmax_respawns 1\r\n"
        "\r\n"
        "\tallammo\r\n"
        "\r\n"
        "\taction \"respawn\"\r\n"
        "\t\tfunction powerup_respawn\r\n"
        "\tend\r\n"
        "\taction \"pickup\"\r\n"
        "\t\tsoundset PU_AMMO\r\n"
        "\t\tfunction powerup_pickup\r\n"
        "\tend\r\n"
        "end\r\n"
        "\r\n"
        "powerup \"AmmoFull_INF\"\r\n"
        "\trespawn_time 100\r\n"
        "\r\n"
        "\tallammo\r\n"
        "\r\n"
        "\taction \"respawn\"\r\n"
        "\t\tfunction powerup_respawn\r\n"
        "\tend\r\n"
        "\taction \"pickup\"\r\n"
        "\t\tsoundset PU_AMMO\r\n"
        "\t\tfunction powerup_pickup\r\n"
        "\tend\r\n"
        "end\r\n"
        " \r\n";

static const DefPowerupDef *row_named(const DefPowerupFile &f, const char *name) {
    for (size_t i = 0; i < f.count; ++i)
        if (std::strcmp(f.entries[i].name, name) == 0) return &f.entries[i];
    return nullptr;
}

// The four JO:CA rows by name: the JOTAC mod table carries them too, ahead of
// its twelve PU_* weapon rows, so a shipped install of either kind pins them.
static void check_shipped_rows(const DefPowerupFile &f) {
    CHECK(f.count >= 4);
    const DefPowerupDef *rows[4] = {row_named(f, "FULLHP"), row_named(f, "FULLHP_INF"),
                                    row_named(f, "AmmoFull"), row_named(f, "AmmoFull_INF")};
    for (const DefPowerupDef *r : rows) CHECK(r != nullptr);
    if (!rows[0] || !rows[1] || !rows[2] || !rows[3]) return;
    const DefPowerupDef &fullhp = *rows[0];
    CHECK(std::strcmp(fullhp.name, "FULLHP") == 0);
    CHECK(fullhp.hp == -1);
    CHECK(fullhp.respawn_time == 30);
    CHECK(fullhp.max_respawns == 1);
    CHECK(fullhp.mana == 0);
    CHECK(fullhp.allammo == 0);
    CHECK(fullhp.weapon_all == 0 && fullhp.weapon[0] == '\0');
    CHECK(fullhp.ammo_count == 0);
    CHECK(fullhp.pickup.present == 1);
    CHECK(std::strcmp(fullhp.pickup.function, "powerup_pickup") == 0);
    CHECK(std::strcmp(fullhp.pickup.soundset, "HEALTH_UP") == 0);
    CHECK(fullhp.respawn.present == 1);
    CHECK(std::strcmp(fullhp.respawn.function, "powerup_respawn") == 0);
    CHECK(fullhp.respawn.soundset[0] == '\0');

    const DefPowerupDef &fullhp_inf = *rows[1];
    CHECK(fullhp_inf.max_respawns == 0); // unset -> the row's zero (unlimited at the bind)
    CHECK(fullhp_inf.respawn_time == 30);

    const DefPowerupDef &ammo = *rows[2];
    CHECK(ammo.allammo == 1);
    CHECK(ammo.hp == 0);
    CHECK(ammo.respawn_time == 100);
    CHECK(ammo.max_respawns == 1);
    CHECK(std::strcmp(ammo.pickup.soundset, "PU_AMMO") == 0);

    // JO:CA authors respawn_time 100 with a PU_AMMO pickup; JOTAC's row is
    // respawn_time 180 with no pickup block. Both are `allammo`, unlimited.
    const DefPowerupDef &ammo_inf = *rows[3];
    CHECK(ammo_inf.allammo == 1 && ammo_inf.max_respawns == 0);
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);

    // The shipped table, inline.
    {
        DefPowerupFile f;
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kShipped),
                                       sizeof(kShipped) - 1, &f) == 0);
        check_shipped_rows(f);
        def_free_powerup(&f);
    }

    // The edge arms of the property pass.
    {
        static const char kEdges[] =
                "hp 5 ; a line outside any block is ignored\r\n"
                "powerup \"ALPHA\"\r\n"
                "  powerup \"NESTED\" // 'definition missing end': dropped, ALPHA stays open\r\n"
                "  hp 40\r\n"
                "  mana -1\r\n"
                "  weapon all\r\n"
                "  ammo 5.56mm 90\r\n"
                "  ammo grenade -1\r\n"
                "  bogus_key 1 // 'unrecognized token'\r\n"
                "  action \"fire\" // 'Invalid for powerup': opens nothing\r\n"
                "  respawn_time 7\r\n"
                "  action \"pickup\"\r\n"
                "    function null\r\n"
                "    delay auto\r\n"
                "    delaystart 3\r\n"
                "    texttoken PICKED\r\n"
                "    particle spark\r\n"
                "    action_value 9\r\n"
                "  end\r\n"
                "end\r\n"
                "end ; 'definition missing start'\r\n"
                "powerup \"BETA\"\r\n"
                "  weapon WPN_M4\r\n"
                "  respawn_time 1\r\n"
                "powerup \"GAMMA_UNTERMINATED\"\r\n"
                "  hp 1\r\n";
        DefPowerupFile f;
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kEdges),
                                       sizeof(kEdges) - 1, &f) == 0);
        // BETA's block is still open when the nested `powerup` lines arrive and
        // the file ends: only ALPHA registers (rows register at `end`).
        CHECK(f.count == 1);
        if (f.count >= 1) {
            const DefPowerupDef &a = f.entries[0];
            CHECK(std::strcmp(a.name, "ALPHA") == 0);
            CHECK(a.hp == 40);
            CHECK(a.mana == -1);
            CHECK(a.weapon_all == 1);
            CHECK(a.respawn_time == 7);
            CHECK(a.max_respawns == 0);
            CHECK(a.ammo_count == 2);
            if (a.ammo_count == 2) {
                CHECK(std::strcmp(a.ammo[0].class_name, "5.56mm") == 0 && a.ammo[0].count == 90);
                CHECK(std::strcmp(a.ammo[1].class_name, "grenade") == 0 && a.ammo[1].count == -1);
            }
            CHECK(a.pickup.present == 1);
            CHECK(std::strcmp(a.pickup.function, "null") == 0);
            CHECK(a.pickup.delayend == -1);
            CHECK(a.pickup.delaystart == 3);
            CHECK(std::strcmp(a.pickup.texttoken, "PICKED") == 0);
            CHECK(std::strcmp(a.pickup.particle, "spark") == 0);
            CHECK(a.pickup.action_value == 9);
            CHECK(a.respawn.present == 0);
        }
        def_free_powerup(&f);
    }

    // A 16-character name fills the row head; longer names are cut at 16.
    {
        static const char kLong[] = "powerup ABCDEFGHIJKLMNOPQRSTUV\r\nend\r\n";
        DefPowerupFile f;
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kLong),
                                       sizeof(kLong) - 1, &f) == 0);
        CHECK(f.count == 1);
        if (f.count == 1) CHECK(std::strcmp(f.entries[0].name, "ABCDEFGHIJKLMNOP") == 0);
        def_free_powerup(&f);
    }

    // The retail walk cuts at CR LF only and drops the last byte of an
    // unterminated tail: a bare final `end` never registers its block, and an
    // LF-only file is one line with no `end` at all.
    {
        static const char kTail[] = "powerup A\r\nhp 1\r\nend";
        DefPowerupFile f;
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kTail),
                                       sizeof(kTail) - 1, &f) == 0);
        CHECK(f.count == 0);
        def_free_powerup(&f);
        static const char kTerminated[] = "powerup A\r\nhp 1\r\nend\r\n";
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kTerminated),
                                       sizeof(kTerminated) - 1, &f) == 0);
        CHECK(f.count == 1);
        if (f.count == 1) CHECK(f.entries[0].hp == 1);
        def_free_powerup(&f);
        static const char kLfOnly[] = "powerup A\nhp 1\nend\n";
        CHECK(def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(kLfOnly),
                                       sizeof(kLfOnly) - 1, &f) == 0);
        CHECK(f.count == 0);
        def_free_powerup(&f);
    }

    // The shipped table out of the packed install (SKIP-LEG retail leg): the
    // file rides resource.pff, read the way the mission start mounts it
    // [orig: Game_StartMission @0x5256CD -> PowerUpDef_LoadFromFile @0x443350].
    const std::string install = retail::install();
    if (!install.empty()) {
        opennova::Vfs vfs;
        std::vector<uint8_t> bytes;
        if (!vfs.mount_game(install, std::string(), opennova::VfsMountMode::Packed)) {
            std::printf("FAIL mount_game(%s): %s\n", install.c_str(), vfs.last_error().c_str());
            ++failures;
        } else if (!vfs.read_file("powerup.def", bytes) || bytes.empty()) {
            std::printf("FAIL powerup.def is not on the install mount %s\n", install.c_str());
            ++failures;
        } else {
            DefPowerupFile f;
            CHECK(def_parse_powerup_memory(bytes.data(), bytes.size(), &f) == 0);
            check_shipped_rows(f);
            def_free_powerup(&f);
        }
    } else {
        retail::skip_leg("OPENNOVA_JO_DIR (the packed install's powerup.def)");
    }

    if (failures == 0) std::printf("def_parse_powerup: OK\n");
    return failures == 0 ? 0 : 1;
}
