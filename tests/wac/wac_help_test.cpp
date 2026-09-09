#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

namespace fs = std::filesystem;
using namespace opennova::wac;
using namespace opennova::world;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

// Help's fixed relative filenames belong to the process working directory.
// Each test process owns a fresh directory and restores its prior cwd on exit.
struct TemporaryWorkingDirectory {
    fs::path previous = fs::current_path();
    fs::path directory;
    TemporaryWorkingDirectory() {
        const fs::path base = fs::absolute(fs::temp_directory_path()).lexically_normal();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int suffix = 0; suffix < 100; ++suffix) {
            const fs::path candidate = base / ("opennova-wac-help-" +
                    std::to_string(stamp) + "-" + std::to_string(suffix));
            if (fs::create_directory(candidate)) {
                directory = candidate;
                fs::current_path(directory);
                return;
            }
        }
        throw std::runtime_error("cannot allocate Help fixture directory");
    }
    ~TemporaryWorkingDirectory() {
        fs::current_path(previous);
        std::error_code error;
        const fs::path base = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if (!directory.empty() && directory.parent_path() == base)
            fs::remove_all(directory, error);
    }
};

static std::string read(const char *path) {
    std::ifstream input(path); // text mode normalizes the CRT's Windows newlines
    return std::string(std::istreambuf_iterator<char>(input), {});
}
static size_t count(const std::string &text, const std::string &needle) {
    size_t total = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
            pos += needle.size()) ++total;
    return total;
}
static void run_help(World &world) {
    const auto program = compile_source("Help()\n", {});
    CHECK(program.ok());
    WacVm vm;
    vm.load(program);
    vm.execute(world);
    CHECK(world.diagnostics.empty());
}

int main() {
    TemporaryWorkingDirectory cwd;
    const auto storage = std::make_unique<World>();
    World &world = *storage;
    run_help(world);
    const std::string text = read("help.wac");
    const std::string xml = read("events.xml");
    CHECK(text.find("// WAC Quick Reference Help File\n") == 0);
    CHECK(count(text, "//   ") == 165);
    CHECK(text.find("//   elapse (seconds)\n") != std::string::npos);
    CHECK(text.find("//   AddExp (ssn, number)\n") != std::string::npos);
    CHECK(text.find("//   Help ()\n") != std::string::npos);
    CHECK(text.find("//   ssnturn (ssn, heading)\n") != std::string::npos);
    CHECK(text.find("// WAC Triggers") < text.find("// WAC Actions"));
    CHECK(text.find("// WAC Actions") < text.find("// WAC Debug Commands"));
    // Fixed IDs/counts recovered from the retail 165x44-byte registry and
    // the XML helper's hash instructions; these are not bytecode opcodes.
    CHECK(count(xml, "<CONDITION id=") == 31);
    CHECK(count(xml, "<ACTION id=") == 106);
    CHECK(xml.find("<CONDITION id=\"276853067\"><NAME>elapse</NAME>"
                   "<FORMAT>elapse %1</FORMAT><WAC>elapse(seconds)</WAC></CONDITION>\n")
            != std::string::npos);
    CHECK(xml.find("<ACTION id=\"1175785544\"><NAME>AddExp</NAME>"
                   "<FORMAT>AddExp %1, %2</FORMAT><WAC>AddExp(ssn,number)</WAC></ACTION>\n")
            != std::string::npos);
    CHECK(xml.find("<ACTION id=\"4396560\"><NAME>Help</NAME>"
                   "<FORMAT>Help</FORMAT><WAC>Help()</WAC></ACTION>\n")
            != std::string::npos);
    const size_t debug = xml.find("<ACTION_TYPE id=\"1\">");
    CHECK(debug != std::string::npos && count(xml.substr(debug), "<ACTION id=") == 45);
    CHECK(xml.size() >= 15 && xml.substr(xml.size() - 15) == "</EVENTS_RSRC>\n");
    CHECK(world.out.effects.entries().size() == 2);
    CHECK(world.out.effects.entries()[0].kind == "debug_text");
    CHECK(world.out.effects.entries()[0].str == "Current Help.wac file saved");
    CHECK(world.out.effects.entries()[1].str == "Current events.xml file saved");

    // Failure of the first open must leave an existing XML file untouched.
    CHECK(fs::remove("help.wac"));
    CHECK(fs::create_directory("help.wac"));
    { std::ofstream old("events.xml"); old << "retained XML"; }
    world.out.effects.clear();
    run_help(world);
    CHECK(read("events.xml") == "retained XML");
    CHECK(world.out.effects.entries().empty());

    // Failure of the second open keeps the new text export and reports only
    // that successful file. A later retry overwrites both complete files.
    CHECK(fs::remove("help.wac"));
    CHECK(fs::remove("events.xml"));
    CHECK(fs::create_directory("events.xml"));
    run_help(world);
    CHECK(read("help.wac") == text);
    CHECK(world.out.effects.entries().size() == 1);
    CHECK(world.out.effects.entries()[0].str == "Current Help.wac file saved");
    CHECK(fs::remove("events.xml"));
    world.out.effects.clear();
    run_help(world);
    CHECK(read("help.wac") == text && read("events.xml") == xml);
    CHECK(world.out.effects.entries().size() == 2);
    std::printf("wac_help: %d failures\n", failures);
    return failures ? 1 : 0;
}
