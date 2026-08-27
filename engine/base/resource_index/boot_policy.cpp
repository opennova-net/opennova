// Launch flags and the boot resource directory ladder -- see boot_policy.h.

#include <base/resource_index/boot_policy.h>

#include <base/vfs/vfs.h>

#include <cctype>

namespace opennova {

namespace {

std::string lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip(const std::string &s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool has_flag(const std::vector<std::string> &args, const char *flag) {
    const std::string wanted = lower_ascii(flag);
    for (const std::string &a : args)
        if (lower_ascii(a) == wanted) return true;
    return false;
}

// The token following `flag`, stripped, or "" when absent/empty.
std::string value_after(const std::vector<std::string> &args, const char *flag) {
    const std::string wanted = lower_ascii(flag);
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (lower_ascii(args[i]) == wanted) return strip(args[i + 1]);
    return std::string();
}

} // namespace

LaunchFlags parse_launch_flags(const std::vector<std::string> &args) {
    LaunchFlags f;
    f.loose_override = has_flag(args, "/d");
    f.expansion = value_after(args, "/exp");
    f.game = lower_ascii(value_after(args, "/game"));
    f.resource_dir = value_after(args, "--resource-dir");
    f.loose_mission = value_after(args, "--loose-mission");
    f.loose_root = has_flag(args, "--loose-root");
    return f;
}

std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback) {
    return flags.expansion.empty() ? fallback : flags.expansion;
}

std::string launch_game(const LaunchFlags &flags, const std::string &fallback) {
    if (!flags.game.empty()) return flags.game;
    const std::string fb = lower_ascii(strip(fallback));
    return fb.empty() ? std::string("jo") : fb;
}

std::string launch_resource_dir(const LaunchFlags &flags, const std::string &fallback) {
    return flags.resource_dir.empty() ? strip(fallback) : flags.resource_dir;
}

std::string boot_path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

std::string bundled_game_dir(const std::string &exe_dir, const BootDirProbe &fs) {
    if (exe_dir.empty() || !fs.file_exists) return std::string();
    for (const char *archive : kBootArchiveTable)
        if (fs.file_exists(boot_path_join(exe_dir, archive))) return exe_dir;
    return std::string();
}

std::string bundled_assets_dir(const std::string &exe_dir, const BootDirProbe &fs) {
    if (exe_dir.empty() || !fs.dir_exists) return std::string();
    const std::string dir = boot_path_join(exe_dir, "assets");
    return fs.dir_exists(dir) ? dir : std::string();
}

std::string boot_resource_dir(const LaunchFlags &flags, const std::string &persisted,
                              const std::string &exe_dir, const BootDirProbe &fs) {
    std::string dir = launch_resource_dir(flags, persisted);
    if (dir.empty()) dir = bundled_game_dir(exe_dir, fs);
    if (dir.empty()) dir = bundled_assets_dir(exe_dir, fs);
    return dir;
}

bool boot_loose_allowed(const LaunchFlags &flags, const std::string &dir,
                        const std::string &exe_dir, const BootDirProbe &fs) {
    if (flags.loose_root) return true;
    return !dir.empty() && dir == bundled_assets_dir(exe_dir, fs);
}

} // namespace opennova
