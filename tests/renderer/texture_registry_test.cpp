// The game's texture registry (renderer/texture_registry.h): the key each loader keeps a texture
// under, and the first load of a key deciding the texture every later request of it draws. Two
// model rows naming one file share a texture when their loaders print one key (whatever their
// case), and keep two when they print two.

#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>
#include <runtime/renderer/texture_registry.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <set>
#include <string>

using namespace opennova::renderer;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

std::string volume(const std::string &key) {
	return std::string(1, '\0') + key;
}

void test_keys_by_type() {
	CHECK(texture_registry_key("Body.tga", 0) == "body.tga:1", "the stage loader's key is the name and :1");
	for (uint8_t type : {uint8_t{1}, uint8_t{2}, uint8_t{8}})
		CHECK(texture_registry_key("Body.tga", type) == "body.tga:1",
				"the plain loader and the stage rows of types 2 and 8 print the same key");
	CHECK(texture_registry_key("Body.tga", 4) == "body.tga:ba:1", "a normal map's key");
	CHECK(texture_registry_key("Body.tga", 5) == texture_registry_key("Body.tga", 4),
			"types 4 and 5 share the normal-map loader's key");
	CHECK(texture_registry_key("Body.tga", 6) == volume("body.tga:hz:1"), "the horizon volume's key");
	CHECK(texture_registry_key("Body.tga", 7) == "body.tga:ao:1", "the occlusion map's key");
	CHECK(texture_registry_key("Body.nq8", 16) == "body.nq8:nml", "the NQ8B chunk's key");
	CHECK(texture_registry_key("Body.hrz", 17) == volume("body.hrz:hz"), "the HRZ8 chunk's key");
	CHECK(texture_registry_key("Body.aoc", 18) == "body.aoc:ao", "the AOC8 chunk's key");
	for (uint8_t type : {uint8_t{3}, uint8_t{9}, uint8_t{12}, uint8_t{15}, uint8_t{19}, uint8_t{255}})
		CHECK(texture_registry_key("Body.tga", type).empty(), "a type no loader takes has no key");
	CHECK(texture_registry_key("", 0).empty(), "a row with no name has no key");
}

void test_keys_compare_without_case() {
	CHECK(texture_registry_key("x.mdt", 0) == texture_registry_key("x.MDT", 0),
			"x.mdt and x.MDT are one key, though the stage rule reads their case apart");
	CHECK(texture_registry_key("WALL.TGA", 1) == texture_registry_key("wall.tga", 0),
			"a plain row and a stage row of one name in two cases are one key");
}

void test_keys_are_the_name_as_written() {
	CHECK(texture_registry_key("bark.dds.tga", 0) != texture_registry_key("bark.dds", 0),
			"the whole name, not the cut query: two keys, though both open bark.dds");
	CHECK(texture_registry_key("body.tga", 0) != texture_registry_key("body.tga", 4),
			"a stage row and a normal-map row of one name keep two textures");
	CHECK(texture_registry_key("foo.tga:HZ", 0) != texture_registry_key("foo.tga", 6),
			"a 2D key never meets a volume key, though the text matches");
}

// A mounted set, case-insensitively.
MaterialTextureFileTest files_of(std::set<std::string> files) {
	const auto lower = [](std::string s) {
		for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	};
	std::set<std::string> lowered;
	for (const std::string &f : files) lowered.insert(lower(f));
	return [lowered, lower](const std::string &name) { return lowered.count(lower(name)) != 0; };
}

// What one row's own loader opens (file and reader), as a texture handle; null when it opens
// nothing or the file it opens is not there.
using Made = std::shared_ptr<const TextureLoad>;
Made row_load(const std::string &name, uint8_t type, const MaterialTextureFileTest &exists) {
	const TextureLoad load = material_texture_load(material_texture_source(name, type, exists), type);
	if (load.reader == TextureReader::None || !exists(load.file)) return Made();
	return std::make_shared<const TextureLoad>(load);
}

struct Rows {
	TextureRegistry<Made> registry;
	MaterialTextureFileTest exists;
	int loads = 0;
	Made draw(const std::string &name, uint8_t type) {
		return registry.find_or_load(texture_registry_key(name, type),
				[&] {
					++loads;
					return row_load(name, type, exists);
				},
				[](const Made &made) { return made != nullptr; });
	}
};

void test_first_row_decides() {
	Rows rows;
	rows.exists = files_of({"body.dds", "body.tga"});
	const Made stage = rows.draw("Body.tga", 0);
	CHECK(stage && stage->file == "Body.dds" && stage->reader == TextureReader::Dds,
			"the stage row loads the .dds beside its name");
	const Made plain = rows.draw("BODY.TGA", 1);
	CHECK(plain == stage, "a later plain row of the name in another case takes the stage row's .dds");
	CHECK(rows.loads == 1, "and opens nothing of its own");
	const Made detail = rows.draw("body.tga", 2);
	CHECK(detail == stage && rows.loads == 1, "a detail row of the name takes it too, whatever its flags");

	Rows reversed;
	reversed.exists = rows.exists;
	const Made first = reversed.draw("BODY.TGA", 1);
	CHECK(first && first->file == "BODY.TGA" && first->reader == TextureReader::Tga,
			"a plain row first loads the named TGA");
	CHECK(reversed.draw("Body.tga", 0) == first && reversed.loads == 1,
			"and the stage row after it draws that TGA, never the .dds");
}

void test_the_case_the_stage_rule_reads() {
	Rows rows;
	rows.exists = files_of({"x.dds", "x.mdt"});
	const Made upper = rows.draw("x.MDT", 0);
	CHECK(upper && upper->reader == TextureReader::Tga, "x.MDT through the TGA reader");
	CHECK(rows.draw("x.mdt", 0) == upper, "x.mdt after it shares that texture, never the .dds");

	Rows lower;
	lower.exists = rows.exists;
	const Made dds = lower.draw("x.mdt", 0);
	CHECK(dds && dds->reader == TextureReader::Dds, "x.mdt first takes the .dds");
	CHECK(lower.draw("x.MDT", 0) == dds, "and x.MDT after it shares the .dds");
}

void test_other_loaders_keep_their_own() {
	Rows rows;
	rows.exists = files_of({"brick.tga"});
	const Made stage = rows.draw("brick.tga", 0);
	const Made normal = rows.draw("brick.tga", 4);
	CHECK(stage && normal && stage != normal && rows.loads == 2,
			"a stage row and a normal-map row of one file are two textures");
	CHECK(rows.draw("BRICK.TGA", 5) == normal && rows.loads == 2, "a type-5 row shares the type-4 row's");
	CHECK(rows.registry.size() == 2, "two keys");
}

void test_a_failed_load_keeps_nothing() {
	Rows rows;
	rows.exists = files_of({"sign.tgaz"});
	// The stage rule cuts "sign.tgaz" to "sign.tga", which is not there; the plain rule opens
	// the whole name.
	CHECK(rows.draw("sign.tgaz", 0) == nullptr, "the stage row loads nothing");
	CHECK(rows.registry.size() == 0, "and keeps nothing");
	const Made plain = rows.draw("sign.tgaz", 1);
	CHECK(plain && plain->file == "sign.tgaz", "so a plain row of the key loads by its own rule");
	CHECK(rows.draw("sign.tgaz", 0) == plain, "and a stage row after it takes that");
}

void test_no_key_registers_nothing() {
	Rows rows;
	rows.exists = files_of({"glass.tga"});
	CHECK(rows.draw("glass.tga", 3) == nullptr && rows.draw("glass.tga", 3) == nullptr,
			"a type no loader takes loads nothing");
	CHECK(rows.loads == 2 && rows.registry.size() == 0, "asked each time, kept never");
	int calls = 0;
	TextureRegistry<int> numbers;
	const auto load = [&] { return ++calls; };
	const auto loaded = [](int value) { return value != 0; };
	CHECK(numbers.find_or_load("", load, loaded) == 1 && numbers.find_or_load("", load, loaded) == 2,
			"an empty key loads each time");
	CHECK(numbers.find_or_load("k", load, loaded) == 3 && numbers.find_or_load("k", load, loaded) == 3,
			"a key keeps its first");
	CHECK(numbers.find("k") != nullptr && *numbers.find("k") == 3 && numbers.find("j") == nullptr, "find");
	// A load that registers its own key (a loader delegating to another of the key) stands.
	const int inner = numbers.find_or_load("d", [&] { return numbers.find_or_load("d", load, loaded) + 100; },
			loaded);
	CHECK(inner == 4 && *numbers.find("d") == 4, "the inner registration stands");
	numbers.clear();
	CHECK(numbers.size() == 0, "clear");
}

} // namespace

int main() {
	test_keys_by_type();
	test_keys_compare_without_case();
	test_keys_are_the_name_as_written();
	test_first_row_decides();
	test_the_case_the_stage_rule_reads();
	test_other_loaders_keep_their_own();
	test_a_failed_load_keeps_nothing();
	test_no_key_registers_nothing();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_registry: all checks passed\n");
	return 0;
}
