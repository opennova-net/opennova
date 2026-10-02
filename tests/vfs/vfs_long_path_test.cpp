// A game install deeper than MAX_PATH, under a name outside the ANSI code page: the VFS
// mounts it, reads its archives and its loose files (the retail loose-first lookup too),
// lists a non-ASCII file name as the same UTF-8 bytes, scans its expansions, the resource
// index reports its loose paths, and the session reads its _NSTMOUT.TXT. Every path is a
// UTF-8 std::string; the OS sees io::os_path of it (base/io/os_path.h). On Windows a
// narrow path is read in the ANSI code page and a path of 260 characters or more fails
// without the \\?\ prefix (this test executable is not long-path aware); elsewhere the
// test passes trivially (PATH_MAX is 4096, paths are bytes).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "pff/pff_test_writer.h"
#include <base/io/os_path.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/pff/pff.h>
#include <runtime/inmatch/session_timeout_config.h>

namespace fs = std::filesystem;
namespace io = opennova::io;

namespace {

const std::string kJose = "Jos\xC3\xA9";                          // José
const std::string kModel = "\xE3\x83\xA2\xE3\x83\x87\xE3\x83\xAB"; // モデル

bool write_bytes(const std::string &utf8_path, const std::string &bytes) {
	std::ofstream out(io::os_path(utf8_path), std::ios::binary);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

// A one-entry PFF minted by the test writer at a short path, its bytes moved to `utf8_path`.
bool write_pff(const std::string &scratch, const std::string &utf8_path, const char *name, const std::string &content) {
	const std::string short_path = (fs::path(scratch) / "minted.pff").string();
	PffTestEntry entry = {name, reinterpret_cast<const uint8_t *>(content.data()), static_cast<uint32_t>(content.size()), 0};
	if (pff_test_write_modern(short_path.c_str(), &entry, 1) != 0) return false;
	std::ifstream in(short_path, std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return !bytes.empty() && write_bytes(utf8_path, bytes);
}

// Removes a tree level by level through io::os_path, so each long descendant gets its own
// \\?\ prefix (std::filesystem::remove_all from a short root walks into the long paths
// without one, and fails there on Windows).
void remove_tree(const std::string &utf8) {
	std::error_code ec;
	const fs::path path = io::os_path(utf8);
	if (fs::is_directory(path, ec)) {
		std::vector<std::string> children;
		for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
			children.push_back(utf8 + "/" + io::utf8_path(it->path().filename()));
		for (const std::string &child : children) remove_tree(child);
	}
	fs::remove(path, ec);
}

std::string read_vfs(const opennova::Vfs &vfs, const std::string &name,
                     opennova::VfsLookupPolicy policy = opennova::VfsLookupPolicy::SessionDefault) {
	std::vector<uint8_t> out;
	if (!vfs.read_file(name, out, policy)) return "<none>";
	return std::string(out.begin(), out.end());
}

} // namespace

int main() {
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);
	TEST_EXPECT(!ec);
	const std::string scratch = (temp / test_paths_unique("opennova_vfs_long_path_scratch")).string();
	fs::remove_all(scratch, ec);
	fs::create_directories(scratch, ec);
	// The base as UTF-8: the temp directory itself may hold a name outside the code page.
	const std::string base = io::utf8_generic_path(temp) + "/" + test_paths_unique("opennova_vfs_long_path");
	remove_tree(base);
	TEST_EXPECT(!fs::exists(io::os_path(base), ec));

	std::string root = base + "/" + kJose + "/" + kModel;
	while (root.size() <= 300) root += "/install_directory_component_0123456789";
	fs::create_directories(io::os_path(root), ec);
	TEST_EXPECT(!ec && fs::is_directory(io::os_path(root), ec));

	TEST_EXPECT(write_pff(scratch, root + "/resource.pff", "archived.txt", "ARCHIVE"));
	TEST_EXPECT(write_bytes(root + "/loose.txt", "LOOSE"));
	TEST_EXPECT(write_bytes(root + "/" + kModel + ".txt", "MODEL"));
	TEST_EXPECT(write_bytes(root + "/skies.env", "fog_level 640\r\n"));
	TEST_EXPECT(write_bytes(root + "/_NSTMOUT.TXT", "NEVER"));
	const std::string exp_dir = root + "/expansion/x";
	fs::create_directories(io::os_path(exp_dir), ec);
	TEST_EXPECT(!ec && fs::is_directory(io::os_path(exp_dir), ec));
	TEST_EXPECT(write_pff(scratch, exp_dir + "/x.pff", "expansion.txt", "EXPANSION"));
	TEST_EXPECT(write_bytes(exp_dir + "/version.txt", "1"));

	// The archive opens by its UTF-8 path.
	{
		opennova::pff::PffArchive archive{};
		TEST_EXPECT(opennova::pff::pff_open(&archive, (root + "/resource.pff").c_str()) == 0);
		opennova::pff::pff_close(&archive);
	}

	// The base game: its archive, its loose files (the flat index and the retail
	// loose-first lookup), a non-ASCII name listed as the same bytes.
	{
		opennova::Vfs vfs;
		TEST_EXPECT(vfs.mount_game(root, "", opennova::VfsMountMode::PackedWithLooseOverride));
		TEST_EXPECT(vfs.has_mounted_archive());
		TEST_EXPECT(read_vfs(vfs, "archived.txt") == "ARCHIVE");
		TEST_EXPECT(read_vfs(vfs, "loose.txt") == "LOOSE");
		TEST_EXPECT(read_vfs(vfs, "LOOSE.TXT", opennova::VfsLookupPolicy::ForceLooseFirst) == "LOOSE");
		TEST_EXPECT(read_vfs(vfs, "archived.txt", opennova::VfsLookupPolicy::ForceArchiveOnly) == "ARCHIVE");
		bool listed = false;
		for (const opennova::VfsFileLocation &file : vfs.list_files()) listed = listed || file.logical_name == kModel + ".txt";
		TEST_EXPECT(listed);
		TEST_EXPECT(read_vfs(vfs, kModel + ".txt") == "MODEL");
		TEST_EXPECT(read_vfs(vfs, kModel + ".txt", opennova::VfsLookupPolicy::ForceLooseFirst) == "MODEL");
	}

	// Its expansion: found, its version file read, its archive mounted over the base.
	{
		const std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
		TEST_EXPECT(expansions.size() == 1 && expansions[0] == "x");
		TEST_EXPECT(opennova::vfs_expansion_version_checksum(root, "x") != 0);
		opennova::Vfs vfs;
		TEST_EXPECT(vfs.mount_game(root, "x", opennova::VfsMountMode::Packed));
		TEST_EXPECT(vfs.mounted_expansion() == "x");
		TEST_EXPECT(read_vfs(vfs, "expansion.txt") == "EXPANSION");
		TEST_EXPECT(read_vfs(vfs, "archived.txt") == "ARCHIVE");
	}

	// The resource index: a loose entry's path '/'-separated, under the root, sized.
	{
		opennova::ResourceIndex index;
		TEST_EXPECT(index.scan(root));
		bool found = false;
		for (const opennova::ResourceFileEntry &entry : index.resource_files("environment")) {
			if (entry.logical_name != "skies.env") continue;
			found = true;
			TEST_EXPECT(entry.source_type == "file");
			TEST_EXPECT(entry.path.find('\\') == std::string::npos);
			TEST_EXPECT(entry.path.size() > root.size() && entry.path.substr(entry.path.size() - 10) == "/skies.env");
			TEST_EXPECT(entry.size_bytes == 15);
		}
		TEST_EXPECT(found);
	}

	// The session's connection template reads the install's _NSTMOUT.TXT.
	TEST_EXPECT(opennova::inmatch::load_session_timeout_config(root).timeout_ms == -1);

#ifdef _WIN32
	// A path that already carries a prefix: the system normalizes none of it (a '/' is a
	// name character there, "." and ".." are names), so os_path does, for each prefix form.
	TEST_EXPECT(io::os_path("\\\\?\\C:/a//b/./c/../d").native() == L"\\\\?\\C:\\a\\b\\d");
	TEST_EXPECT(io::os_path("\\\\.\\C:/a/b").native() == L"\\\\.\\C:\\a\\b");
	TEST_EXPECT(io::os_path("\\\\?\\UNC\\server/share/x").native() == L"\\\\?\\UNC\\server\\share\\x");
	TEST_EXPECT(io::os_path("//?/C:/a/b").native() == L"\\\\?\\C:\\a\\b");
	// A prefixed directory, short and long, joined with '/': the file opens.
	for (const std::string &dir : {scratch, root}) {
		const std::string prefixed = "\\\\?\\" + io::utf8_path(fs::absolute(io::os_path(dir), ec));
		TEST_EXPECT(!ec);
		fs::create_directories(io::os_path(io::utf8_join(prefixed, "joined/sub")), ec);
		TEST_EXPECT(!ec && fs::is_directory(io::os_path(dir + "/joined/sub"), ec));
		TEST_EXPECT(write_bytes(io::utf8_join(prefixed, "joined/sub/file.txt"), "JOINED"));
		std::ifstream in(io::os_path(dir + "/joined/sub/file.txt"), std::ios::binary);
		TEST_EXPECT(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) == "JOINED");
	}
#endif

	remove_tree(base);
	TEST_EXPECT(!fs::exists(io::os_path(base), ec));
	fs::remove_all(scratch, ec);
	std::printf("OK: a %zu-byte UTF-8 install root mounts and reads\n", root.size());
	return 0;
}
