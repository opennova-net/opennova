#include <algorithm>
#include <filesystem>
#include <fstream>

#include "common/test_expect.h"
#include "resource_index/resource_index.h"

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path &path, const char *text) {
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << text;
}

bool has_relative_path(const std::vector<opennova::ResourceFileEntry> &entries,
                       const std::string &relative_path) {
	return std::any_of(entries.begin(), entries.end(), [&](const opennova::ResourceFileEntry &entry) {
		return entry.relative_path == relative_path;
	});
}

} // namespace

int main() {
	const fs::path root = fs::temp_directory_path() / "opennova_resource_index_test";
	fs::remove_all(root);
	fs::create_directories(root);

	write_file(root / "Alpha.TRN", "trn");
	write_file(root / "Bravo.env", "env");
	write_file(root / "object.3dp", "3dp");
	write_file(root / "preview.3di", "3di");
	write_file(root / "scene.ASE", "ase");
	write_file(root / "vehicle.glb", "glb");
	write_file(root / "Menus.BIN", "RTXTstrings");
	write_file(root / "Scratch.bin", "DATA");
	write_file(root / "missions" / "first.bms", "bms");
	write_file(root / "models" / "tree.glb", "glb");
	write_file(root / "models" / "tree.3di", "3di");
	write_file(root / "models" / "source.ase", "ase");
	write_file(root / "credits" / "finale.kda", "kda");
	write_file(root / "fonts" / "Serpen24.fnt", "fnt");

	opennova::ResourceIndex index;
	TEST_EXPECT(!index.scan((root / "missing").string(), true));
	TEST_EXPECT(!index.last_error().empty());

	TEST_EXPECT(index.scan(root.string(), false));
	TEST_EXPECT(index.resource_files("all").size() == 6);
	TEST_EXPECT(index.resource_files("terrain").size() == 1);
	TEST_EXPECT(index.resource_files("environment").size() == 1);
	TEST_EXPECT(index.resource_files("mission").empty());
	TEST_EXPECT(index.resource_files("object_project").size() == 1);
	TEST_EXPECT(index.resource_files("object_model").size() == 1);
	TEST_EXPECT(index.resource_files("object_scene").size() == 1);
	TEST_EXPECT(index.resource_files("strings").size() == 1);
	TEST_EXPECT(index.resource_files("glb").empty());

	TEST_EXPECT(index.scan(root.string(), true));
	const std::vector<opennova::ResourceFileEntry> all_files = index.resource_files("*");
	TEST_EXPECT(all_files.size() == 11);
	TEST_EXPECT(has_relative_path(all_files, "Alpha.TRN"));
	TEST_EXPECT(has_relative_path(all_files, "Menus.BIN"));
	TEST_EXPECT(has_relative_path(all_files, "missions/first.bms"));
	TEST_EXPECT(has_relative_path(all_files, "object.3dp"));
	TEST_EXPECT(has_relative_path(all_files, "preview.3di"));
	TEST_EXPECT(has_relative_path(all_files, "scene.ASE"));
	TEST_EXPECT(has_relative_path(all_files, "models/tree.3di"));
	TEST_EXPECT(has_relative_path(all_files, "models/source.ase"));
	TEST_EXPECT(has_relative_path(all_files, "credits/finale.kda"));
	TEST_EXPECT(has_relative_path(all_files, "fonts/Serpen24.fnt"));
	TEST_EXPECT(!has_relative_path(all_files, "Scratch.bin"));
	TEST_EXPECT(!has_relative_path(all_files, "vehicle.glb"));
	TEST_EXPECT(!has_relative_path(all_files, "models/tree.glb"));
	TEST_EXPECT(index.resource_files("bms").size() == 1);
	TEST_EXPECT(index.resource_files("trn").size() == 1);
	TEST_EXPECT(index.resource_files("env").size() == 1);
	TEST_EXPECT(index.resource_files("3dp").size() == 1);
	TEST_EXPECT(index.resource_files("3di").size() == 2);
	TEST_EXPECT(index.resource_files("ase").size() == 2);
	TEST_EXPECT(index.resource_files("object").size() == 5);
	TEST_EXPECT(index.resource_files("object_model").size() == 2);
	TEST_EXPECT(index.resource_files("object_model")[0].display_name == "preview" ||
	            index.resource_files("object_model")[0].display_name == "tree");
	TEST_EXPECT(index.resource_files("credits").size() == 1);
	TEST_EXPECT(index.resource_files("kda").size() == 1);
	TEST_EXPECT(index.resource_files("credits")[0].display_name == "finale");
	TEST_EXPECT(index.resource_files("font").size() == 1);
	TEST_EXPECT(index.resource_files("fnt").size() == 1);
	TEST_EXPECT(index.resource_files("font")[0].display_name == "Serpen24");
	TEST_EXPECT(index.resource_files("strings").size() == 1);
	TEST_EXPECT(index.resource_files("bin").size() == 1);
	TEST_EXPECT(index.resource_files("rtxt").size() == 1);
	TEST_EXPECT(index.resource_files("strings")[0].display_name == "Menus");

	fs::remove_all(root);
	return 0;
}
