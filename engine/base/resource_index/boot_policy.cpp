// Launch flags and path helpers -- see boot_policy.h.

#include <base/resource_index/boot_policy.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <base/title/title.h>
#include <base/vfs/vfs.h>
#include <formats/update/apply.h>
#include <formats/update/command_file.h>
#include <formats/update/update_wizard.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string_view>

namespace opennova {

namespace {

// ASCII-only by design (io/strutil.h): flag matching never follows the locale.
bool has_flag(const std::vector<std::string> &args, const char *flag) {
    for (const std::string &a : args)
        if (strutil::iequals(a, flag)) return true;
    return false;
}

// The token following `flag`, stripped, or "" when absent/empty; a flag given
// twice takes its last value.
std::string value_after(const std::vector<std::string> &args, const char *flag) {
    std::string value;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (strutil::iequals(args[i], flag)) value = strutil::trim(args[i + 1]);
    return value;
}

// A whole decimal or 0x-prefixed integer, or `fallback` for an empty,
// non-numeric or trailing-garbage token.
long parse_long(const std::string &text, long fallback) {
    if (text.empty()) return fallback;
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str() || *end != '\0') return fallback;
    return value;
}

// The integer following `flag` when it lies in [lo, hi], else `fallback`.
int int_after(const std::vector<std::string> &args, const char *flag, long lo, long hi,
              int fallback) {
    const long value = parse_long(value_after(args, flag), fallback);
    if (value < lo || value > hi) return fallback;
    return static_cast<int>(value);
}

} // namespace

RetailSwitches walk_retail_switches(const std::vector<std::string> &tokens) {
    RetailSwitches s;
    LogSwitches &log = s.log_switches;
    const std::size_t count = tokens.size();
    static const std::string kPastTheLast; // the slot past the tokens a no-bound step reads
    const auto is = [](const std::string &token, const char *name) { return strutil::iequals(token, name); };
    for (std::size_t i = 1; i < count; ++i) {
        // The switches ahead of /UDPNOVAWORLD's step test the token the iteration starts at.
        const std::string &first = tokens[i];
        if (is(first, kLaunchFlagInout)) log.inout = true; // [orig: @0x4A7443 -> @0x4A744F]
        if (is(first, kLaunchFlagNoPunt)) s.nopunt = true; // [orig: @0x4A7473 -> @0x4A747F]
        // [orig: "/CONNECTLOG" @0x4A74A3 -> 1 @0x4A74AF, else "/NOCONNECTLOG" @0x4A74BD -> 0 @0x4A74C9]
        if (is(first, kLaunchFlagConnectLog))
            log.connect_log = true;
        else if (is(first, kLaunchFlagNoConnectLog))
            log.connect_log = false;
        if (is(first, kLaunchFlagLan)) s.lan = true; // [orig: @0x4A74DF -> @0x4A74EB]
        // [orig: "/UDPNOVAWORLD" @0x4A74F1, the step @0x4A7503..0x4A750C, the count test
        //  @0x4A750E..0x4A7516, Napi_CopyString(.., 0x20) @0x4A751C..0x4A7528]
        if (is(first, kLaunchFlagUdpNovaWorld) && i + 1 < count) {
            ++i;
            if (s.udp_novaworld.size() < kLaunchUdpNovaWorldMax)
                s.udp_novaworld.push_back(tokens[i].substr(0, kLaunchUdpNovaWorldChars));
        }
        // The rest test the token the walk stands on, reloaded here and moved by every step
        // [orig: @0x4A7536].
        const std::string *token = &tokens[i];
        // A value-taking switch with a next token steps onto it.
        const auto takes = [&](const char *name) {
            if (!is(*token, name) || i + 1 >= count) return false;
            token = &tokens[++i];
            return true;
        };
        // One that reads its next token with no bound steps past the last too.
        const auto steps_onto = [&](const char *name) {
            if (!is(*token, name)) return false;
            ++i;
            token = i < count ? &tokens[i] : &kPastTheLast;
            return true;
        };
        // [orig: "/GLSVSSRIMS" @0x4A753A..0x4A7569, "/GLSVSSAGRMS" @0x4A756F..0x4A759E]
        if (takes(kLaunchFlagGlsvssRims)) s.glsvss_rims = io::retail_atol(token->c_str());
        if (takes(kLaunchFlagGlsvssAgrms)) s.glsvss_agrms = io::retail_atol(token->c_str());
        // [orig: "/POSTIPADDRESS" @0x4A75A4..0x4A75CF, "/POSTPORTNUMBER" @0x4A75D4..0x4A75FE]
        if (takes(kLaunchFlagPostIpAddress)) s.post_ip_address = *token;
        if (takes(kLaunchFlagPostPortNumber)) s.post_port_number = io::retail_atol(token->c_str());
        // [orig: "/USEJUNCTION" @0x4A7603..0x4A762D, "/CLEARJUNCTION" @0x4A7632..0x4A765C]
        if (takes(kLaunchFlagUseJunction)) s.use_junction = io::retail_atol(token->c_str());
        if (takes(kLaunchFlagClearJunction)) s.clear_junction = io::retail_atol(token->c_str());
        if (is(*token, kLaunchFlagLooseOverride)) s.loose_override = true; // [orig: "/D" @0x4A7667]
        // `/mod` and `/exp` share one arm: the token stepped onto, its first 32 bytes kept
        // [orig: the two stricmps @0x4A7694..0x4A76AC, the bound @0x4A76B8, the step @0x4A76C1 onto the
        // token @0x4A76C3, strncpy @0x4A76CF].
        if ((is(*token, kLaunchFlagMod) || is(*token, kLaunchFlagExpansion)) && i + 1 < count) {
            token = &tokens[++i];
            s.expansion = launch_expansion_name(*token);
        }
        if (is(*token, kLaunchFlagNoReload)) s.no_reload = true; // [orig: @0x4A76DD -> @0x4A76E9]
        // [orig: "/maxplayers" @0x4A76F5, the step with no bound @0x4A7703, j__atol @0x4A7710]
        if (steps_onto(kLaunchFlagMaxPlayers)) s.max_players = io::retail_atol(token->c_str());
        // [orig: "/HOST" @0x4A7749, the step with no bound @0x4A7757, strcpy @0x4A7760]
        if (steps_onto(kLaunchFlagHost)) s.host_file = *token;
        // [orig: "/PROFILE" @0x4A7795, the step with no bound @0x4A77A3, the copy into g_ProfileLogPath
        //  @0x4A77A7..0x4A77B9, g_RunningWithProfile @0x4A77BB]
        if (steps_onto(kLaunchFlagProfile)) log.profile_path = *token;
        if (is(*token, kLaunchFlagScreenshotTga)) s.screenshot_tga = true; // [orig: @0x4A77C7 -> @0x4A77D3]
        if (is(*token, kLaunchFlagCheatLog)) log.cheat_log = true; // [orig: @0x4A7802 -> @0x4A7813]
        if (is(*token, kLaunchFlagPuntTxt)) log.punt_log = true; // [orig: @0x4A7833 -> @0x4A7844]
        if (is(*token, kLaunchFlagPuntLog)) log.punt_log = true; // [orig: @0x4A7869 -> @0x4A787A]
        if (is(*token, kLaunchFlagNoHud)) s.no_hud = true; // [orig: @0x4A79F6..0x4A7A09]
        if (is(*token, kLaunchFlagNetLog)) log.net_log = true; // [orig: @0x4A7A17 -> @0x4A7A29]
        // The startup page's switches step onto their value, which nothing here reads; their
        // `=` forms carry it in the token and step nowhere [orig: "/URL", "/STARTUP",
        // "/STARTUPHTM", "/STARTUPURL" @0x4A7AAC..0x4A7AF4, the step @0x4A7B71].
        if ((is(*token, "/URL") || is(*token, "/STARTUP") || is(*token, "/STARTUPHTM") ||
                    is(*token, "/STARTUPURL")) &&
                i + 1 < count)
            token = &tokens[++i];
        // `/AA <n>` steps onto its value, unread here [orig: "/AA" @0x4A7B90, the step @0x4A7BA5].
        takes("/AA");
    }
    return s;
}

std::vector<std::string> retail_command_line_tokens(const std::string &command_line) {
    io::ConfigTokens tokens;
    io::tokenize_config_line(command_line.c_str(), tokens);
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(tokens.count));
    for (int i = 0; i < tokens.count; ++i) out.emplace_back(tokens.token(i));
    return out;
}

std::string retail_command_line(const std::vector<std::string> &args) {
    std::string line = "opennova";
    for (const std::string &arg : args) {
        line += ' ';
        const bool quoted = arg.empty() || arg.find_first_of(" \t") != std::string::npos;
        if (quoted) line += '"';
        for (const char c : arg)
            if (c != '"') line += c;
        if (quoted) line += '"';
    }
    return line;
}

std::string without_own_options(const std::string &command_line, const OwnOption *options, std::size_t count) {
    // The arguments as written, each a run of the line up to a blank outside quotes.
    struct Span {
        std::size_t begin, end;
    };
    std::vector<Span> spans;
    for (std::size_t i = 0; i < command_line.size();) {
        if (command_line[i] == ' ' || command_line[i] == '\t') {
            ++i;
            continue;
        }
        const std::size_t begin = i;
        bool quoted = false;
        for (; i < command_line.size(); ++i) {
            const char c = command_line[i];
            if (c == '"') quoted = !quoted;
            else if (!quoted && (c == ' ' || c == '\t')) break;
        }
        spans.push_back({begin, i});
    }
    const auto own = [&](const Span &span) -> const OwnOption * {
        std::string name;
        for (std::size_t i = span.begin; i < span.end; ++i)
            if (command_line[i] != '"') name += command_line[i];
        for (std::size_t at = 0; at < count; ++at)
            if (strutil::iequals(name, options[at].name)) return &options[at];
        return nullptr;
    };
    std::string out;
    for (std::size_t at = 0; at < spans.size(); ++at) {
        if (at > 0) {
            if (const OwnOption *option = own(spans[at])) {
                if (option->takes_value) ++at;
                continue;
            }
        }
        if (!out.empty()) out += ' ';
        out.append(command_line, spans[at].begin, spans[at].end - spans[at].begin);
    }
    return out;
}

namespace {

// The game's own options (LaunchFlags), and `--`, Godot's end of its own.
constexpr OwnOption kLaunchOwnOptions[] = {
    {"--", false},
    {kLaunchFlagGame, true},
    {kLaunchFlagNoUpdate, false},
    {kLaunchFlagAfterUpdate, false},
    {kLaunchFlagResourceDir, true},
    {kLaunchFlagLooseMission, true},
    {kLaunchFlagLooseRoot, false},
    {kLaunchFlagMission, true},
    {kLaunchFlagLanHost, true},
    {kLaunchFlagLanRotation, true},
    {kLaunchFlagLanJoin, true},
    {kLaunchFlagLanPort, true},
    {kLaunchFlagLanGametype, true},
    {kLaunchFlagLanMode, true},
    {kLaunchFlagLanMaxPlayers, true},
    {kLaunchFlagSpectator, false},
    {kLaunchFlagSpectatorPassword, true},
    {kLaunchFlagCallsign, true},
    {kLaunchFlagCapturePcap, true},
    {kLaunchFlagMcpPort, true},
    {kLaunchFlagNovaWorldHost, true},
    {kLaunchFlagNovaWorldJoin, true},
    {kLaunchFlagWorkingDir, true},
};

} // namespace

LaunchFlags parse_launch_flags(const std::vector<std::string> &args, const std::string &command_line) {
    LaunchFlags f;
    static_cast<RetailSwitches &>(f) = walk_retail_switches(retail_command_line_tokens(
            without_own_options(command_line.empty() ? retail_command_line(args) : command_line,
                    kLaunchOwnOptions, sizeof(kLaunchOwnOptions) / sizeof(kLaunchOwnOptions[0]))));
    f.game = strutil::to_lower(value_after(args, kLaunchFlagGame));
    f.game_given = has_flag(args, kLaunchFlagGame);
    f.no_update = has_flag(args, kLaunchFlagNoUpdate);
    f.after_update = has_flag(args, kLaunchFlagAfterUpdate);
    f.resource_dir = value_after(args, kLaunchFlagResourceDir);
    f.resource_dir_given = has_flag(args, kLaunchFlagResourceDir);
    f.loose_mission = value_after(args, kLaunchFlagLooseMission);
    f.loose_root = has_flag(args, kLaunchFlagLooseRoot);
    f.mission = value_after(args, kLaunchFlagMission);
    f.lan_host = value_after(args, kLaunchFlagLanHost);
    const std::string rotation = value_after(args, kLaunchFlagLanRotation);
    for (std::size_t at = 0; at <= rotation.size();) {
        const std::size_t comma = std::min(rotation.find(',', at), rotation.size());
        if (comma > at) f.lan_rotation.push_back(rotation.substr(at, comma - at));
        at = comma + 1;
    }
    f.lan_join = value_after(args, kLaunchFlagLanJoin);
    f.lan_port = int_after(args, kLaunchFlagLanPort, 1, 65535, 0);
    f.lan_gametype = int_after(args, kLaunchFlagLanGametype, 0, 0x7fffffffL, -1);
    f.lan_mode = int_after(args, kLaunchFlagLanMode, 1, 4, 0);
    f.lan_max_players = int_after(args, kLaunchFlagLanMaxPlayers, 1, 64, 0);
    f.spectator = has_flag(args, kLaunchFlagSpectator);
    f.spectator_password = value_after(args, kLaunchFlagSpectatorPassword);
    f.callsign = value_after(args, kLaunchFlagCallsign);
    f.capture_pcap = value_after(args, kLaunchFlagCapturePcap);
    f.mcp_port = int_after(args, kLaunchFlagMcpPort, 1, 65535, 0);
    f.novaworld_host = value_after(args, kLaunchFlagNovaWorldHost);
    f.novaworld_join = int_after(args, kLaunchFlagNovaWorldJoin, 1, 0x7fffffffL, 0);
    f.working_dir = value_after(args, kLaunchFlagWorkingDir);
    return f;
}

int launch_lan_port(const LaunchFlags &flags, int fallback) {
    return flags.lan_port > 0 ? flags.lan_port : fallback;
}

int launch_lan_mode(const LaunchFlags &flags, int fallback) {
    return flags.lan_mode > 0 ? flags.lan_mode : fallback;
}

int launch_lan_max_players(const LaunchFlags &flags, int fallback) {
    return flags.lan_max_players > 0 ? flags.lan_max_players : fallback;
}

LanEndpoint launch_lan_join_endpoint(const LaunchFlags &flags, int default_port) {
    LanEndpoint endpoint;
    endpoint.port = default_port;
    const std::string &target = flags.lan_join;
    if (target.empty()) return endpoint;
    const std::size_t colon = target.find(':');
    endpoint.ip = strutil::trim(colon == std::string::npos ? target : target.substr(0, colon));
    if (strutil::iequals(endpoint.ip, "browse")) {
        endpoint.browse = true;
        endpoint.ip.clear();
    }
    if (colon == std::string::npos) return endpoint;
    const long port = parse_long(strutil::trim(target.substr(colon + 1)), 0);
    if (port >= 1 && port <= 65535) endpoint.port = static_cast<int>(port);
    return endpoint;
}

std::string launch_expansion_name(const std::string &token) {
    const std::string name = strutil::trim(token);
    // [orig: Game_ParseCommandLineAndInit @ 0x4a76cf] strncpy(g_ExpansionName, token, 0x20)
    return name.size() > kExpansionNameBytes ? name.substr(0, kExpansionNameBytes) : name;
}

bool launch_token_breaks_at(char c) {
    // [orig: Config_TokenizeLine @ 0x53cb60: ' ', '\t' and ',' split outside quotes
    //  @ 0x53cc44, '"' toggles quoting and is never kept @ 0x53cc51, ';' ends the line @ 0x53cc31]
    return c == ' ' || c == '\t' || c == ',' || c == '"' || c == ';';
}

std::string launch_expansion(const LaunchFlags &flags, const std::string &fallback) {
    return flags.expansion.empty() ? fallback : flags.expansion;
}

std::string launch_game(const LaunchFlags &flags, const std::string &fallback) {
    if (!flags.game.empty()) return flags.game;
    const std::string fb = strutil::to_lower(strutil::trim(fallback));
    return fb.empty() ? std::string(default_title().code) : fb;
}

WizardProduct wizard_product(const update::UpdateWizard &wizard) {
    const std::string name = strutil::trim(wizard.cmd);
    // A stock wizard: the title whose command file its CMD names, run as itself. OpenNova's own
    // game (no program of its own: the runtime runs it) is native.
    if (const Title *stock = title_by_command_file(name)) {
        const std::string code = stock->code;
        return { code, code, stock->executable == nullptr };
    }
    // A player's game: GAME names the title it runs as, one a stock wizard names (a title with a
    // command file: JO's, DFX2's, OP: Acid Gambit's).
    const std::string game = strutil::to_lower(strutil::trim(wizard.game));
    const std::string login = strutil::to_lower(strutil::trim(wizard.product_login));
    const Title *runs_as = game.empty() ? nullptr : title_by_code(game);
    if (!name.empty() && !login.empty() && runs_as != nullptr && runs_as->command_file != nullptr)
        return { login, game, true };
    return {};
}

FolderProduct folder_product(const std::string &dir) {
    FolderProduct out;
    std::string text;
    if (!vfs_update_wizard_text(dir, text)) return out;
    const update::UpdateWizard wizard = update::read_update_wizard(text);
    const WizardProduct product = wizard_product(wizard);
    out.command = strutil::trim(wizard.cmd);
    out.foreign = product.product.empty();
    out.product = product.product;
    out.game = product.game;
    // A player's game: the stock products' names are the runtime's own.
    if (product.native && title_by_code(out.product) == nullptr) {
        std::string bytes, ignored;
        std::optional<update::CommandFile> installed;
        if (update::is_safe_row_name(out.command) &&
                io::read_file_text(boot_path_join(dir, out.command), bytes, ignored))
            installed = update::read_command_file(bytes);
        out.name = installed && !installed->product.empty() ? installed->product
                                                            : strutil::trim(wizard.product_login);
    }
    return out;
}

LaunchProduct launch_product(const LaunchFlags &flags, const FolderProduct &folder) {
    LaunchProduct out;
    if (flags.game_given) out.product = flags.game;
    else if (!folder.foreign) out.product = folder.product.empty() ? std::string(default_title().code) : folder.product;
    const bool folders = !out.product.empty() && out.product == folder.product;
    if (folders || (folder.foreign && !flags.game_given)) out.command = folder.command;
    if (folders) out.name = folder.name;
    out.supported = folders;
    // A stock product: a title a stock wizard names (its command file), by its /game name; its
    // command file and its name on the runtime's screens are the title's.
    const Title *stock = out.product.empty() ? nullptr : title_by_code(out.product);
    if (stock != nullptr && stock->command_file != nullptr) {
        if (out.command.empty()) out.command = stock->command_file;
        if (out.name.empty()) out.name = stock->display_name;
        out.supported = true;
    }
    out.game = folders ? folder.game : launch_game(flags, out.product);
    return out;
}

std::optional<Version> launch_product_version(
		const std::string &dir, const LaunchProduct &product) {
	// version_mark's read: the command file at its name in the folder, its header's VERSION.
	std::string bytes, ignored;
	if (product.command.empty() || !update::is_safe_row_name(product.command) ||
			!io::read_file_text(boot_path_join(dir, product.command), bytes, ignored))
		return std::nullopt;
	const std::optional<update::CommandFile> file = update::read_command_file(bytes);
	if (!file)
		return std::nullopt;
	return Version::parse(file->version);
}

std::string boot_path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

bool mount_install(Vfs &vfs, const std::string &root, const LaunchFlags &flags) {
    vfs.set_scr_policy(title_scr_policy_for_code(launch_game(flags, std::string()).c_str()));
    const VfsMountMode mode =
            flags.loose_override ? VfsMountMode::PackedWithLooseOverride : VfsMountMode::Packed;
    return vfs.mount_game(root, flags.expansion, mode, VfsArchiveDiscovery::RetailTable) &&
           vfs.has_mounted_archive();
}

namespace {

std::vector<std::string> updater_product_arguments(const UpdaterProduct &product) {
    return {kLaunchFlagGame, product.product, "--runtime-dir", product.runtime_dir,
            kLaunchFlagResourceDir, product.resource_dir};
}

} // namespace

std::vector<std::string> updater_check_arguments(const UpdaterProduct &product) {
    std::vector<std::string> arguments{"--check"};
    const std::vector<std::string> rest = updater_product_arguments(product);
    arguments.insert(arguments.end(), rest.begin(), rest.end());
    return arguments;
}

std::vector<std::string> updater_install_arguments(const UpdaterProduct &product,
        long long parent_pid, const std::vector<std::string> &run_args, bool bootstrap,
        long long join_rid) {
    std::vector<std::string> arguments{"/RUN", "--parent-pid", std::to_string(parent_pid)};
    const std::vector<std::string> rest = updater_product_arguments(product);
    arguments.insert(arguments.end(), rest.begin(), rest.end());
    // The launch's own arguments, an earlier launch's markers never carried into this relaunch.
    std::vector<std::string> relaunch;
    bool skip_value = false;
    for (const std::string &argument : run_args) {
        if (skip_value)
            skip_value = false;
        else if (argument == kLaunchFlagNovaWorldJoin)
            skip_value = true;
        else if (argument != kLaunchFlagAfterUpdate)
            relaunch.push_back(argument);
    }
    std::vector<std::string> marks;
    if (bootstrap) marks.push_back(kLaunchFlagAfterUpdate);
    if (join_rid > 0) {
        marks.push_back(kLaunchFlagNovaWorldJoin);
        marks.push_back(std::to_string(join_rid));
    }
    if (!marks.empty()) {
        if (std::find(relaunch.begin(), relaunch.end(), "--") == relaunch.end())
            relaunch.push_back("--");
        relaunch.insert(relaunch.end(), marks.begin(), marks.end());
    }
    for (const std::string &argument : relaunch) {
        arguments.push_back("--run-arg");
        arguments.push_back(argument);
    }
    return arguments;
}

} // namespace opennova
