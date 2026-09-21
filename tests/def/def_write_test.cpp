#include <formats/def/def_write.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include "common/retail_paths.h"

#include <cstdio>
#include <map>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::def;

static int check(const char *family, const std::vector<uint8_t> &bytes, bool retail_corpus = false) {
	DefParseReport diagnostics;
	DefWriteResult written;
	size_t count = 0;
	if (std::strcmp(family, "items.def") == 0) {
		DefItemsFile file{};
		def_parse_items_memory(bytes.data(), bytes.size(), &file, &diagnostics);
		count = file.count;
		written = def_write_items(file);

        if (retail_corpus && !diagnostics.empty()) {
            if (written.ok()) return 1;
            std::vector<DefItemDef> supported;
            for (size_t i = 0; i < file.count; ++i)
                if (!file.entries[i].unmodeled_count) supported.push_back(file.entries[i]);
            auto excerpt = file;
            excerpt.unmodeled_count = 0;
            excerpt.entries = supported.data(); excerpt.count = supported.size();
            written = def_write_items(excerpt);
        }
		def_free_items(&file);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes.data(), bytes.size(), &file, &diagnostics);
		count = file.count;
		written = def_write_weapons(file);

        if (retail_corpus && !diagnostics.empty()) {
            if (written.ok()) return 1;
            std::vector<DefWeaponDef> supported;
            for (size_t i = 0; i < file.count; ++i)
                if (!file.entries[i].unmodeled_count) supported.push_back(file.entries[i]);
            auto excerpt = file;
            excerpt.unmodeled_count = 0;
            excerpt.entries = supported.data(); excerpt.count = supported.size();
            written = def_write_weapons(excerpt);
        }
		def_free_weapons(&file);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes.data(), bytes.size(), &file, &diagnostics);
		count = file.count;
		written = def_write_ammo(file);

        if (retail_corpus && !diagnostics.empty()) {
            if (written.ok()) return 1;
            std::vector<DefAmmoDef> supported;
            for (size_t i = 0; i < file.count; ++i)
                if (!file.entries[i].unmodeled_count) supported.push_back(file.entries[i]);
            auto excerpt = file;
            excerpt.unmodeled_count = 0;
            excerpt.entries = supported.data(); excerpt.count = supported.size();
            written = def_write_ammo(excerpt);
        }
		def_free_ammo(&file);
	}
	std::map<std::string,int> unsupported;
    for (const auto &d : diagnostics) ++unsupported[d.field];
    for (const auto &[key,n] : unsupported) std::printf("  unsupported %s: %d\n",key.c_str(),n);
    std::printf("%s: %zu records, %zu parse issues, %zu write issues\n", family, count, diagnostics.size(), written.diagnostics.size());
	for (size_t i = 0; i < diagnostics.size() && i < 16; ++i)
		std::printf("  line %zu %s.%s: %s\n", diagnostics[i].line, diagnostics[i].record.c_str(), diagnostics[i].field.c_str(), diagnostics[i].message.c_str());
	for (size_t i = 0; i < written.diagnostics.size() && i < 16; ++i)
		std::printf("  %s.%s: %s\n", written.diagnostics[i].record.c_str(), written.diagnostics[i].field.c_str(), written.diagnostics[i].message.c_str());
	if ((!retail_corpus && !diagnostics.empty()) || !written.ok()) return 1;
	for (size_t i = 0; i < written.text.size(); ++i)
		if (written.text[i] == '\n' && (i == 0 || written.text[i - 1] != '\r')) return 1;
	return 0;
}
static int snippet(const char *family, const char *text) {
	return check(family, std::vector<uint8_t>(text, text + std::strlen(text)));
}
int main() {
    int failures = 0;
    for (const auto &empty : {def_write_items({}), def_write_weapons({}), def_write_ammo({})})
        if (!empty.ok() || empty.text.empty()) ++failures; // runtime reads distinguish empty payloads from empty tables
	failures += snippet("items.def",
		"// source comment\nbegin \"Test item\"\nid 100001\ntype vehicle\nhp 125\narmor 20 30\n"
		"graphic model\nanim_def anim\nkz 0.5\ndebris_scale 1.25\n"
		"particlefx smoke exhaust\naddeweap gun 100002 30 40 50 60\n"
		"pcvehicle_spawnlist 8 4\npcvehicle_spawnlist 4\nend\n"
		"begin \"Second\"\npcvehicle_spawnlist 8\nend\n");
	failures += snippet("weapon.def",
		"ammoclass_max_carry bullets 300\nweapon \"WPN_TEST\"\ncategory 1\n"
		"round_type AT_TEST\nweaponweight 3.25\nclipweight 0.1\n"
		"stability 1, 0.5, 0\nerror 0.1, 0.2, 0.3, 0.4, 0.5, 0.6\n"
		"pos 1 2 3 359.99 180 90\nsights scope 0 0 640 480 blend scale slide 3\n"
		"action \"Fire\"\nfunction Shoot 1 2 3 4\nctrlreg trigger\nctrlreginc 2\n"
		"texttoken Fire\ndupsound 2 4\ndelay auto\nend\nend\n");
	failures += snippet("ammo.def",
		"ammo AT_TEST\nvelocity 1500\nflag silenced\nmax_age 1.5\nerror 0.125\n"
		"turnrate_maxyaw 1.5\nboresight_maxang 360\nkz_pieslice 180\n"
		"light_move 3 255 120 20\nlight_impact 4 10 20 30 0.3\n"
		"effects_table\nmetal hit spark 4\nend\nend\n");
    failures += snippet("items.def",
        "begin \"Coupled\"\nsound_profile male\nsound_profilefemale \"\"\nacceleration 2\ndeceleration 0\n"
        "score -23\ngraphicenemy enemy\ntextid label\nrotor_parts 1 2 3 4\naux_parts 5 6 7 8\nend\nbegin \"Powerup\"\ntype powerup\npowerupdef PU_TEST\nend\n");
    failures += snippet("ammo.def", "ammo TRACER\ntracer_type stdred stdgreen\ndopplerdiv 3\nkz_sound hit\nend\n");
    for (const char *source : {"weapon \"Bad\nend\n", "ammoclass_max_carry bullets nope\n",
            "weapon \"Bad\"\nsights a 0 0 1 1 unknown\nend\n"}) {
        DefWeaponsFile invalid{}; DefParseReport issues;
        def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(source), std::strlen(source), &invalid, &issues);
        if (issues.empty() || def_write_weapons(invalid).ok()) ++failures;
        def_free_weapons(&invalid);
    }
    for (const char *source : {"ammo Bad\neffects_table\nmetal hit snd nope\nend\nend\n",
            "effects_table\nend\n", "ammo Bad\ntracer_type unknown\nend\n"}) {
        DefAmmoFile invalid{}; DefParseReport issues;
        def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(source), std::strlen(source), &invalid, &issues);
        if (issues.empty() || def_write_ammo(invalid).ok()) ++failures;
        def_free_ammo(&invalid);
    }
    DefItemsFile incomplete{};
	DefParseReport report;
	const char *bad = "begin \"Bad\"\nunknown_field 7\nend\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(bad), std::strlen(bad), &incomplete, &report);
	if (report.size() != 1 || report[0].line != 2 || def_write_items(incomplete).ok()) ++failures;
	def_free_items(&incomplete);

	const std::string root = retail::install();
	if (root.empty()) retail::skip_leg("def_write retail corpus needs OPENNOVA_JO_DIR");
	else {
		opennova::Vfs vfs;
		vfs.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		if (!vfs.mount_game(root, "", opennova::VfsMountMode::Packed)) return 1;
		for (const char *family : {"items.def", "weapon.def", "ammo.def"}) {
			std::vector<uint8_t> bytes;
			if (!vfs.read_file(family, bytes)) { std::printf("Cannot read %s\n", family); return 1; }
			failures += check(family, bytes, true);
		}
	}
	return failures ? 1 : 0;
}
