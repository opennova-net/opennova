#include <net/napi/session.h>
#include <net/napi/tlv.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Read a field's value (as a string) from a container by name.
std::string field_str(const opennova::NapiMessage &m, const char *name) {
	for (const auto &f : m.fields) {
		if (f.name == name) return std::string(f.data.begin(), f.data.end());
	}
	return {};
}

// Error → NWEC string mapping matches jodemo's CNapiGameSession_ConnectOrHost
// error-dispatch table.
bool check_error_nwec_mapping() {
	using opennova::NovaWorldError;
	using opennova::novaworld_error_tag;
	if (!expect(novaworld_error_tag(NovaWorldError::Banned) == "NWEC11", "200 -> NWEC11")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Restricted) == "NWEC12", "203 -> NWEC12")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3000) == "NWEC04", "3000 -> NWEC04")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3001) == "NWEC05", "3001 -> NWEC05")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3002) == "NWEC06", "3002 -> NWEC06")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3003) == "NWEC07", "3003 -> NWEC07")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3004) == "NWEC08", "3004 -> NWEC08")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3005) == "NWEC09", "3005 -> NWEC09")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::Reject3006) == "NWEC10", "3006 -> NWEC10")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::TimeoutPoll) == "NWEC02", "timeout -> NWEC02")) return false;
	if (!expect(novaworld_error_tag(NovaWorldError::UserCancelled) == "NWEC03", "user-cancel -> NWEC03")) return false;
	return true;
}

// from_code handles both known codes and the "default fall-through".
bool check_error_from_code() {
	using opennova::novaworld_error_from_code;
	using opennova::NovaWorldError;
	if (!expect(novaworld_error_from_code(200) == NovaWorldError::Banned, "200")) return false;
	if (!expect(novaworld_error_from_code(203) == NovaWorldError::Restricted, "203")) return false;
	if (!expect(novaworld_error_from_code(3006) == NovaWorldError::Reject3006, "3006")) return false;
	// [D-NET-25] 1009 -> NWEC14; an unknown NONZERO reject -> UnknownReject -> NWEC13 (NOT the NWEC02
	// poll-timeout path, which is code -1).
	if (!expect(novaworld_error_from_code(1009) == NovaWorldError::Reject1009, "1009 -> Reject1009")) return false;
	if (!expect(opennova::novaworld_error_tag(NovaWorldError::Reject1009) == "NWEC14", "1009 -> NWEC14")) return false;
	if (!expect(novaworld_error_from_code(9999) == NovaWorldError::UnknownReject, "unknown reject -> UnknownReject")) return false;
	if (!expect(opennova::novaworld_error_tag(NovaWorldError::UnknownReject) == "NWEC13", "unknown reject -> NWEC13")) return false;
	if (!expect(novaworld_error_from_code(-1) == NovaWorldError::TimeoutPoll, "poll timeout -> NWEC02")) return false;
	return true;
}

// Timing constants match the witnessed immediates.
bool check_timing_constants() {
	using namespace opennova;
	if (!expect(SESSION_CONNECT_TIMEOUT_MS == 60000u, "connect/host poll timeout 60s (0xEA60)")) return false;
	if (!expect(SESSION_PERIODIC_UPDATE_TIMEOUT_MS == 20000u, "periodic-update timeout 20s (0x4E20)")) return false;
	if (!expect(SESSION_APPID_RANDOM_MIN == 1000u, "session AppId random min")) return false;
	if (!expect(SESSION_APPID_RANDOM_MAX == 9999u, "session AppId random max")) return false;
	// (tick + rand) % 0x2328 + 1000 [orig: CNapiNetwork_RandomizeTimeout @0x4c4d80]
	if (!expect(make_session_app_id(0, 0) == 1000u, "AppId floor")) return false;
	if (!expect(make_session_app_id(8999, 0) == 9999u, "AppId ceiling")) return false;
	if (!expect(make_session_app_id(9000, 0) == 1000u, "AppId wraps at 0x2328")) return false;
	if (!expect(make_session_app_id(1234, 5) == 2239u, "AppId sums tick and rand")) return false;
	if (!expect(SESSION_CONNECT_RETRANSMIT_MS == 3000u, "stage retransmit 3000 ms")) return false;
	if (!expect(SESSION_GATE_PROBE_RETRY_MS == 3000u && SESSION_GATE_PROBE_TIMEOUT_MS == 30000u,
	            "gate probe 3000 ms retry / 30000 ms deadline")) return false;
	if (!expect(SESSION_GLSVSS_POLL_MS == 1000u, "GLSVSS poll 1000 ms")) return false;
	if (!expect(SESSION_HOST_INFO_REFRESH_TICKS == 1860u, "host info refresh 0x744 ticks")) return false;
	// The six-slot cookie-key ring [orig: CSessionIdRing_AdvanceAndGenerate @0x4dbb80].
	SessionIdRing ring;
	if (!expect(ring.current() == 0 && ring.count == 0, "ring starts empty")) return false;
	if (!expect(ring.advance(0x12345678u, 0x11) == 1 && ring.index == 1, "first advance fills slot 1")) return false;
	if (!expect(ring.current() == ((0x12345678u + 0x11u) & 0xFFFFFFu) + 0x1000000u,
	            "the key is ((rand + tick) & 0xFFFFFF) + 0x1000000")) return false;
	for (int i = 0; i < 5; ++i) ring.advance(1, 1);
	if (!expect(ring.index == 0 && ring.count == 6, "the index wraps at 6 and the count caps at 6")) return false;
	ring.advance(1, 1);
	if (!expect(ring.index == 1 && ring.count == 6, "a seventh advance keeps the cap")) return false;
	return true;
}

// The host-leg, gate-leg and server-message maps.
bool check_host_and_gate_error_maps() {
	using namespace opennova;
	if (!expect(novaworld_host_error_tag(0x3E9) == "NWEC53", "1001 -> NWEC53")) return false;
	if (!expect(novaworld_host_error_tag(0x3EC) == "NWEC54", "1004 -> NWEC54")) return false;
	if (!expect(novaworld_host_error_tag(0x3ED) == "NWEC55", "1005 -> NWEC55")) return false;
	if (!expect(novaworld_host_error_tag(0x3F0) == "NWEC56", "1008 -> NWEC56")) return false;
	if (!expect(novaworld_host_error_tag(0x3F1) == "NWEC57", "1009 -> NWEC57")) return false;
	if (!expect(novaworld_host_error_tag(0x3F3) == "NWEC60", "1011 -> NWEC60")) return false;
	if (!expect(novaworld_host_error_tag(1010) == "NWEC58", "other -> NWEC58")) return false;
	if (!expect(std::string(NWEC_HOST_TIMEOUT) == "NWEC52", "host poll timeout NWEC52")) return false;
	if (!expect(novaworld_gate_error_tag(-2, false) == "NWEC18", "gate -2 -> NWEC18")) return false;
	if (!expect(novaworld_gate_error_tag(-8, false) == "NWEC24", "gate -8 -> NWEC24")) return false;
	if (!expect(novaworld_gate_error_tag(-9, false) == "NWEC15", "gate -9 in phase 1 -> the NWEC15 default")) return false;
	if (!expect(novaworld_gate_error_tag(-9, true) == "NWEC25", "gate -9 in phase 3 -> NWEC25")) return false;
	if (!expect(novaworld_gate_error_tag(-1, true) == "NWEC16", "gate other in phase 3 -> NWEC16")) return false;
	if (!expect(novaworld_gate_error_tag(-5, true) == "NWEC21", "gate -5 in phase 3 -> NWEC21")) return false;
	if (!expect(novaworld_server_msg_code_key(1) == "NWUSERVERMSGCODE_UNKNOWNERROR", "msgcode 1")) return false;
	if (!expect(novaworld_server_msg_code_key(6) == "NWUSERVERMSGCODE_NOVAWORLDDOWNFORMAINTENANCE", "msgcode 6")) return false;
	if (!expect(novaworld_server_msg_code_key(1004) == "NWUSERVERMSGCODE_REJECTEDHOSTINGINFO", "msgcode 1004")) return false;
	if (!expect(novaworld_server_msg_code_key(6002) == "NWUSERVERMSGCODE_JOINTICKETIPADDRESSDOESNOTMATCH", "msgcode 6002")) return false;
	if (!expect(novaworld_server_msg_code_key(999) == "NWUSERVERMSGCODE_UNKNOWNERROR", "unknown msgcode -> the unknown key")) return false;
	return true;
}

// The Server*Result parsers (atol semantics, case-insensitive names) and the HostCommands walk.
bool check_server_result_parsers() {
	using namespace opennova;
	NapiMessage result;
	result.name = "ServerHostResult";
	auto add = [](NapiMessage &m, const char *name, const std::string &value) {
		NapiField f;
		f.name = name;
		f.data.assign(value.begin(), value.end());
		m.fields.push_back(f);
	};
	add(result, "success", " 7abc");
	add(result, "MSGCODE", "-3");
	add(result, "MsgParam1", "12");
	add(result, "MsgParam2", "x");
	const ServerResultFields fields = parse_server_result_fields(result);
	if (!expect(fields.success == 7 && fields.msg_code == -3 && fields.msg_param1 == 12 && fields.msg_param2 == 0,
	            "Success/MsgCode/MsgParam1/MsgParam2 atol like retail, names case-insensitive")) return false;
	NapiMessage commands;
	commands.name = "ServerVarList";
	add(commands, "VarList", "HostCommands");
	NapiMessage var;
	var.name = "ServerVar";
	add(var, "VarFNum", "0");
	add(var, "VarName", "GSID");
	add(var, "VarValue", "GSID-01-DEADBEEF");
	commands.children.push_back(var);
	NapiMessage other;
	other.name = "ServerVarList";
	add(other, "VarList", "ConnectCommands");
	other.children.push_back(var);
	result.children.push_back(other);
	result.children.push_back(commands);
	const auto parsed = parse_host_commands(result);
	if (!expect(parsed.size() == 1 && parsed.count("GSID") == 1 && parsed.at("GSID") == "GSID-01-DEADBEEF",
	            "only the HostCommands ServerVarList feeds the map")) return false;
	return true;
}

// ServerCommand: the quoted tokenizer, the verb prefix/equality match and the target suffixes.
bool check_server_command_parse() {
	using namespace opennova;
	const auto tokens = tokenize_quoted("  PuntPlayerByName \"Some Guy\" extra\targ ");
	if (!expect(tokens.size() == 4 && tokens[0] == "PuntPlayerByName" && tokens[1] == "Some Guy" &&
	                    tokens[2] == "extra" && tokens[3] == "arg",
	            "quotes group one token, whitespace splits, quotes are dropped")) return false;
	// The whitespace is the CRT isspace under the game's ".ACP" LC_CTYPE, pinned to cp1252: the
	// six C-locale spaces and 0xA0 (the no-break space) split outside quotes, and no other byte
	// does (D-NET-381). [orig: String_TokenizeQuotedToArray @0x616d60 — isspace @0x616da6;
	// System_InitTimerAndLocale @0x762a6e]
	for (int b = 1; b < 256; ++b) {
		if (b == '"') continue; // the quote toggles; it never splits
		const std::string text = std::string("a") + static_cast<char>(b) + "b";
		const auto split = tokenize_quoted(text);
		const bool space = b == ' ' || (b >= '\t' && b <= '\r') || b == 0xA0;
		const bool ok = space ? (split.size() == 2 && split[0] == "a" && split[1] == "b")
		                      : (split.size() == 1 && split[0] == text);
		if (!expect(ok, "exactly the six C-locale spaces and 0xA0 split a token")) {
			std::fprintf(stderr, "  byte 0x%02X\n", b);
			return false;
		}
	}
	const auto nbsp_quoted = tokenize_quoted("x \"Some\xA0Guy\" y");
	if (!expect(nbsp_quoted.size() == 3 && nbsp_quoted[1] == "Some\xA0Guy",
	            "a quoted 0xA0 stays in its token")) return false;
	auto make = [](const std::string &cmd) {
		NapiMessage m;
		m.name = "ServerCommand";
		NapiField f;
		f.name = "Cmd";
		f.data.assign(cmd.begin(), cmd.end());
		m.fields.push_back(f);
		return m;
	};
	ServerCommand out;
	if (!expect(parse_server_command(make("puntplayerbyindex 3"), out) &&
	                    out.verb == ServerCommandVerb::PuntPlayer && out.target == ServerCommandTarget::ByIndex &&
	                    out.args.size() == 1 && out.args[0] == "3",
	            "PuntPlayerByIndex parses by prefix + suffix, case-insensitively")) return false;
	if (!expect(parse_server_command(make("KillPlayerByIpAndPort 10.0.0.1:32768"), out) &&
	                    out.verb == ServerCommandVerb::KillPlayer && out.target == ServerCommandTarget::ByIpAndPort,
	            "KillPlayerByIpAndPort")) return false;
	if (!expect(parse_server_command(make("TextChatServer \"hello all\""), out) &&
	                    out.verb == ServerCommandVerb::TextChatServer && out.target == ServerCommandTarget::None &&
	                    out.args.size() == 1 && out.args[0] == "hello all",
	            "TextChatServer takes the quoted text")) return false;
	if (!expect(!parse_server_command(make("TextChatServerX hi"), out),
	            "whole-token verbs do not prefix-match")) return false;
	if (!expect(parse_server_command(make("PuntPlayerByName Some\xA0Guy"), out) &&
	                    out.verb == ServerCommandVerb::PuntPlayer && out.args.size() == 2 &&
	                    out.args[0] == "Some" && out.args[1] == "Guy",
	            "an unquoted 0xA0 splits the Cmd's tokens, as a cp1252 host's reader does")) return false;
	if (!expect(parse_server_command(make("SetMPReset\xA0" "3"), out) && out.verb == ServerCommandVerb::SetMPReset &&
	                    out.args.size() == 1 && out.args[0] == "3",
	            "0xA0 ends the verb token too")) return false;
	if (!expect(parse_server_command(make("DisarmPlayerByPCID guy"), out) &&
	                    out.verb == ServerCommandVerb::DisarmPlayer && out.target == ServerCommandTarget::ByPCID,
	            "DisarmPlayerByPCID")) return false;
	if (!expect(parse_server_command(make("Cycle"), out) && out.verb == ServerCommandVerb::Cycle && out.args.empty(),
	            "Cycle")) return false;
	if (!expect(!parse_server_command(make("Frobnicate 1"), out), "an unknown verb is dropped")) return false;
	NapiMessage no_cmd;
	no_cmd.name = "ServerCommand";
	if (!expect(!parse_server_command(no_cmd, out), "no Cmd param -> nothing")) return false;
	return true;
}

// The service side: ServerCommand composed and built, then read back by the host's parser for
// every verb and target form; ServerStopHosting read back by the result-field parser.
bool check_server_statement_builders() {
	using namespace opennova;
	const NapiMessage cycle = make_server_command("Cycle");
	ServerCommand out;
	if (!expect(cycle.name == "ServerCommand" && cycle.fields.size() == 1 && cycle.fields[0].name == "Cmd" &&
	                    field_str(cycle, "Cmd") == "Cycle",
	            "ServerCommand carries exactly one Cmd param")) return false;
	if (!expect(parse_server_command(cycle, out) && out.verb == ServerCommandVerb::Cycle &&
	                    out.target == ServerCommandTarget::None && out.args.empty(),
	            "the built Cycle parses back")) return false;

	// Every verb in the enum, so a verb added later fails here rather than slipping by (the
	// unnamed-value check after the walk catches one appended past DisarmPlayer).
	const ServerCommandTarget kTargets[] = {ServerCommandTarget::ByIndex, ServerCommandTarget::ByIpAndPort,
	                                        ServerCommandTarget::ByName, ServerCommandTarget::ByPCID};
	auto player_targeted = [](ServerCommandVerb v) {
		switch (v) {
			case ServerCommandVerb::PuntPlayer:
			case ServerCommandVerb::TextChatPlayer:
			case ServerCommandVerb::CmdEchoPlayer:
			case ServerCommandVerb::KillPlayer:
			case ServerCommandVerb::ChangeTeam:
			case ServerCommandVerb::SwapTeam:
			case ServerCommandVerb::ReloadPlayer:
			case ServerCommandVerb::DisarmPlayer: return true;
			default: return false;
		}
	};
	// The witnessed token-count gates, as args after the verb (`cmp edi, N; jle` to the no-op tail;
	// 0 where every arg is optional or none is read).
	auto min_args = [](ServerCommandVerb v) -> size_t {
		switch (v) {
			case ServerCommandVerb::TextChatPlayer:
			case ServerCommandVerb::CmdEchoPlayer: return 2;
			case ServerCommandVerb::Cycle:
			case ServerCommandVerb::EndMission:
			case ServerCommandVerb::GameOver:
			case ServerCommandVerb::Earthquake:
			case ServerCommandVerb::Lightning:
			case ServerCommandVerb::TimeOfDay: return 0;
			default: return 1;
		}
	};
	auto round_trip = [&](ServerCommandVerb v, ServerCommandTarget t, const std::vector<std::string> &args) {
		const std::string text = server_command_text(v, t, args);
		ServerCommand parsed;
		return !text.empty() && parse_server_command(make_server_command(text), parsed) && parsed.verb == v &&
		       parsed.target == t &&
		       parsed.command == std::string(server_command_verb_name(v)) + server_command_target_name(t) &&
		       parsed.args == args;
	};
	int covered = 0;
	for (int i = 1; i <= static_cast<int>(ServerCommandVerb::DisarmPlayer); ++i) {
		const auto v = static_cast<ServerCommandVerb>(i);
		if (player_targeted(v)) {
			for (const ServerCommandTarget t : kTargets) {
				if (!expect(round_trip(v, t, {"Some Guy", "42"}),
				            "a player-targeted verb round-trips every target form")) {
					std::fprintf(stderr, "  verb %s target %s\n", server_command_verb_name(v),
					             server_command_target_name(t));
					return false;
				}
			}
		} else if (!expect(round_trip(v, ServerCommandTarget::None, {"hello all"}),
		                   "a whole-token verb round-trips its quoted arg")) {
			std::fprintf(stderr, "  verb %s\n", server_command_verb_name(v));
			return false;
		}
		if (!expect(server_command_verb_takes_target(v) == player_targeted(v),
		            "server_command_verb_takes_target agrees with the witnessed prefix verbs")) {
			std::fprintf(stderr, "  verb %s\n", server_command_verb_name(v));
			return false;
		}
		// The name parser inverts the spelling, case-insensitively as the reader compares.
		std::string lower = server_command_verb_name(v);
		for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		ServerCommandVerb named = ServerCommandVerb::None;
		ServerCommandVerb named_lower = ServerCommandVerb::None;
		if (!expect(server_command_verb_from_name(server_command_verb_name(v), named) && named == v &&
		                    server_command_verb_from_name(lower, named_lower) && named_lower == v,
		            "server_command_verb_from_name inverts every verb's name")) {
			std::fprintf(stderr, "  verb %s\n", server_command_verb_name(v));
			return false;
		}
		// The arity gate: exactly the verb's minimum composes and parses back; one short is refused.
		const ServerCommandTarget t0 = player_targeted(v) ? ServerCommandTarget::ByName : ServerCommandTarget::None;
		const size_t need = min_args(v);
		if (!expect(round_trip(v, t0, std::vector<std::string>(need, "7")), "the verb's minimum args compose") ||
		    !expect(need == 0 || server_command_text(v, t0, std::vector<std::string>(need - 1, "7")).empty(),
		            "one arg short of the verb's token-count gate composes nothing")) {
			std::fprintf(stderr, "  verb %s\n", server_command_verb_name(v));
			return false;
		}
		++covered;
	}
	if (!expect(covered == 18, "all eighteen verbs walked")) return false;
	{
		ServerCommandVerb v = ServerCommandVerb::Cycle;
		if (!expect(!server_command_verb_from_name("", v) && !server_command_verb_from_name("Punt", v) &&
		                    !server_command_verb_from_name("PuntPlayerByName", v) && v == ServerCommandVerb::Cycle,
		            "a name that is no verb parses to nothing and leaves `out`")) return false;
		ServerCommandTarget t = ServerCommandTarget::ByPCID;
		if (!expect(server_command_target_from_name("", t) && t == ServerCommandTarget::None,
		            "an empty target is none")) return false;
		t = ServerCommandTarget::ByPCID;
		if (!expect(server_command_target_from_name("none", t) && t == ServerCommandTarget::None,
		            "\"None\" is no target")) return false;
		for (const ServerCommandTarget want : kTargets) {
			if (!expect(server_command_target_from_name(server_command_target_name(want), t) && t == want,
			            "server_command_target_from_name inverts every suffix")) return false;
		}
		if (!expect(server_command_target_from_name("byname", t) && t == ServerCommandTarget::ByName,
		            "a suffix name is case-insensitive")) return false;
		if (!expect(!server_command_target_from_name("ByNumber", t) && t == ServerCommandTarget::ByName,
		            "an unknown suffix parses to nothing and leaves `out`")) return false;
	}
	// An unnamed value past DisarmPlayer: a verb appended to the enum gets a name and fails here.
	if (!expect(std::string(server_command_verb_name(static_cast<ServerCommandVerb>(covered + 1))).empty(),
	            "no verb past DisarmPlayer")) return false;

	// Quoting is the tokenizer's inverse: the empty token and an embedded tab survive.
	const std::vector<std::string> odd = {"Some Guy", "", "x\ty"};
	const std::string quoted = server_command_text(ServerCommandVerb::PuntPlayer, ServerCommandTarget::ByName, odd);
	if (!expect(quoted == "PuntPlayerByName \"Some Guy\" \"\" \"x\ty\"", "empty and whitespace-bearing args quoted"))
		return false;
	if (!expect(parse_server_command(make_server_command(quoted), out) && out.args == odd,
	            "the quoted args parse back as three tokens")) return false;
	if (!expect(server_command_text(ServerCommandVerb::PuntPlayer, ServerCommandTarget::ByIndex, {"42"}) ==
	                    "PuntPlayerByIndex 42",
	            "a plain arg is not quoted")) return false;
	// A byte >= 0x80 is quoted: the host's tokenizer runs isspace in its ANSI code page (0xA0 is a
	// space on cp1252, as in our reader), and a quoted run is copied as it is.
	const std::vector<std::string> high = {"Some\xA0Guy", "caf\xE9"};
	const std::string high_text =
		server_command_text(ServerCommandVerb::TextChatPlayer, ServerCommandTarget::ByName, high);
	if (!expect(high_text == "TextChatPlayerByName \"Some\xA0Guy\" \"caf\xE9\"",
	            "a high-byte arg is quoted")) return false;
	if (!expect(parse_server_command(make_server_command(high_text), out) && out.args == high,
	            "the high-byte args parse back whole")) return false;

	// Refusals: no verb, a pairing the reader drops, too few args, an unrepresentable quote or NUL,
	// a text the reader would clip.
	if (!expect(server_command_text(ServerCommandVerb::None, ServerCommandTarget::None, {}).empty(),
	            "verb None composes nothing")) return false;
	if (!expect(server_command_text(ServerCommandVerb::PuntPlayer, ServerCommandTarget::None, {"42"}).empty(),
	            "a player-targeted verb with no suffix composes nothing")) return false;
	if (!expect(server_command_text(ServerCommandVerb::Cycle, ServerCommandTarget::ByName, {}).empty(),
	            "a whole-token verb with a suffix composes nothing")) return false;
	if (!expect(server_command_text(ServerCommandVerb::TextChatPlayer, ServerCommandTarget::ByName, {"Some Guy"})
	                    .empty(),
	            "TextChatPlayer with no message composes nothing")) return false;
	if (!expect(server_command_text(ServerCommandVerb::TextChatServer, ServerCommandTarget::None, {"say \"hi\""})
	                    .empty(),
	            "an arg holding a quote composes nothing")) return false;
	if (!expect(server_command_text(ServerCommandVerb::PuntPlayer, ServerCommandTarget::ByName,
	                                {std::string("a\0b", 3)})
	                    .empty(),
	            "an arg holding a NUL composes nothing")) return false;
	const std::string too_long =
		server_command_text(ServerCommandVerb::Cycle, ServerCommandTarget::None, {std::string(506, 'a')});
	if (!expect(too_long.empty(), "a 512-char text is refused (the reader keeps 511)")) return false;
	const std::string fits =
		server_command_text(ServerCommandVerb::Cycle, ServerCommandTarget::None, {std::string(505, 'a')});
	if (!expect(fits.size() == SERVER_COMMAND_CMD_CAP - 1 && parse_server_command(make_server_command(fits), out) &&
	                    out.verb == ServerCommandVerb::Cycle && out.args.size() == 1 && out.args[0].size() == 505,
	            "a 511-char text round-trips whole")) return false;
	// Each refusal names its reason; a composed line leaves the reason alone.
	{
		auto reason = [](ServerCommandVerb v, ServerCommandTarget t, const std::vector<std::string> &args) {
			const char *why = nullptr;
			const std::string text = server_command_text(v, t, args, &why);
			return text.empty() && why != nullptr ? std::string(why) : std::string("<composed>");
		};
		if (!expect(reason(ServerCommandVerb::None, ServerCommandTarget::None, {}) == "no verb", "reason: no verb") ||
		    !expect(reason(ServerCommandVerb::PuntPlayer, ServerCommandTarget::None, {"42"}).find("needs a target") !=
		                    std::string::npos,
		            "reason: a missing suffix") ||
		    !expect(reason(ServerCommandVerb::Cycle, ServerCommandTarget::ByName, {}).find("no target") !=
		                    std::string::npos,
		            "reason: a suffix on a whole-token verb") ||
		    !expect(reason(ServerCommandVerb::SetServerName, ServerCommandTarget::None, {}).find("token-count") !=
		                    std::string::npos,
		            "reason: too few args") ||
		    !expect(reason(ServerCommandVerb::TextChatServer, ServerCommandTarget::None, {"a\"b"}).find("quote") !=
		                    std::string::npos,
		            "reason: a quote") ||
		    !expect(reason(ServerCommandVerb::Cycle, ServerCommandTarget::None, {std::string(506, 'a')}).find("511") !=
		                    std::string::npos,
		            "reason: the cap"))
			return false;
		const char *untouched = "kept";
		if (!expect(!server_command_text(ServerCommandVerb::Cycle, ServerCommandTarget::None, {}, &untouched).empty() &&
		                    std::string(untouched) == "kept",
		            "a composed line leaves the reason untouched")) return false;
	}
	const std::string raw = "TextChatServer " + std::string(585, 'b');
	const NapiMessage raw_cmd = make_server_command(raw);
	if (!expect(raw_cmd.fields[0].data.size() == 600, "make_server_command never clips")) return false;
	if (!expect(parse_server_command(raw_cmd, out) && out.args.size() == 1 &&
	                    out.command.size() + 1 + out.args[0].size() == SERVER_COMMAND_CMD_CAP - 1,
	            "the reader clips the raw Cmd to 511 characters")) return false;

	// ServerStopHosting: the three params HandleServerMessage reads, no Success.
	const NapiMessage stop = make_server_stop_hosting(7, 1, 2);
	if (!expect(stop.name == "ServerStopHosting" && stop.fields.size() == 3 && stop.fields[0].name == "MsgCode" &&
	                    stop.fields[1].name == "MsgParam1" && stop.fields[2].name == "MsgParam2" &&
	                    field_str(stop, "MsgCode") == "7" && field_str(stop, "MsgParam1") == "1" &&
	                    field_str(stop, "MsgParam2") == "2",
	            "ServerStopHosting carries MsgCode/MsgParam1/MsgParam2 in order, no Success")) return false;
	const ServerResultFields fields = parse_server_result_fields(stop);
	if (!expect(fields.msg_code == 7 && fields.msg_param1 == 1 && fields.msg_param2 == 2 && fields.success == 0,
	            "the result-field parser reads the triple back")) return false;
	if (!expect(novaworld_server_msg_code_key(fields.msg_code) == "NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT",
	            "MsgCode 7 maps to the sysop punt key")) return false;
	const NapiMessage stop_default = make_server_stop_hosting(4000);
	if (!expect(field_str(stop_default, "MsgCode") == "4000" && field_str(stop_default, "MsgParam1") == "0" &&
	                    field_str(stop_default, "MsgParam2") == "0",
	            "the params default to 0")) return false;

	// The wire leg: both statements survive the TLV stream encode/decode.
	const NapiMessage punt = make_server_command(
		server_command_text(ServerCommandVerb::PuntPlayer, ServerCommandTarget::ByName, {"Some Guy", "42"}));
	const std::vector<NapiMessage> stream = {punt, make_server_stop_hosting(3)};
	std::vector<uint8_t> buf(napi_stream_size(stream) + 16, 0);
	size_t enc_size = 0;
	if (!expect(napi_stream_encode(stream, buf.data(), buf.size(), &enc_size) == 0, "encode the service statements"))
		return false;
	std::vector<NapiMessage> decoded;
	size_t consumed = 0;
	if (!expect(napi_stream_decode(buf.data(), enc_size, decoded, &consumed) == 0 && decoded.size() == 2,
	            "decode the service statements")) return false;
	for (size_t i = 0; i < stream.size(); ++i) {
		bool same = decoded[i].name == stream[i].name && decoded[i].fields.size() == stream[i].fields.size();
		for (size_t f = 0; same && f < stream[i].fields.size(); ++f) {
			same = decoded[i].fields[f].name == stream[i].fields[f].name &&
			       decoded[i].fields[f].data == stream[i].fields[f].data;
		}
		if (!expect(same, "a service statement round-trips the TLV stream")) return false;
	}
	if (!expect(parse_server_command(decoded[0], out) && out.verb == ServerCommandVerb::PuntPlayer &&
	                    out.target == ServerCommandTarget::ByName &&
	                    out.args == std::vector<std::string>{"Some Guy", "42"},
	            "the decoded ServerCommand parses to the composed command")) return false;
	return true;
}

// The host-side and client-side statement builders added for the registration/play legs.
bool check_leg_builders() {
	using namespace opennova;
	const NapiMessage added = make_client_host_player_added(2, "Joiner", "10.0.0.2:32768", "P2", "1", "0");
	if (!expect(added.name == "ClientHostPlayerAdded" && added.fields.size() == 6 &&
	                    added.fields[0].name == "PlayerNumber" && field_str(added, "PlayerNumber") == "2" &&
	                    added.fields[1].name == "PlayerName" && added.fields[2].name == "PlayerIpAndPort" &&
	                    added.fields[3].name == "PlayerPCID" && added.fields[4].name == "PlayerTeam" &&
	                    added.fields[5].name == "PlayerType",
	            "ClientHostPlayerAdded carries the six params in order")) return false;
	const NapiMessage removed = make_client_host_player_removed(2);
	if (!expect(removed.name == "ClientHostPlayerRemoved" && removed.fields.size() == 1 &&
	                    field_str(removed, "PlayerNumber") == "2",
	            "ClientHostPlayerRemoved carries PlayerNumber only")) return false;
	const NapiMessage enter = make_client_player_enter_request(5, 0x0100007Fu, 32768, "T-1");
	if (!expect(enter.name == "ClientPlayerEnterRequest" && enter.fields.size() == 4 &&
	                    field_str(enter, "ConnectionId") == "5" && field_str(enter, "IpAddress") == "16777343" &&
	                    field_str(enter, "PortNumber") == "32768" && field_str(enter, "JoinTicket") == "T-1",
	            "ClientPlayerEnterRequest: ConnectionId, IpAddress (decimal u32), PortNumber, JoinTicket")) return false;
	const NapiMessage glsvss = make_client_glsvss_request("REQ", {{0, "NWUID", "u"}});
	if (!expect(glsvss.name == "ClientGLSVSSRequest" && field_str(glsvss, "GLSVSSRequest") == "REQ" &&
	                    glsvss.children.size() == 1 && field_str(glsvss.children[0], "VarList") == "Cookie",
	            "ClientGLSVSSRequest: the GLSVSSRequest param + the Cookie list")) return false;
	const auto setup = make_play_setup_vars("Row", "1.2.3.4", "32768", "3225", 7);
	if (!expect(setup.size() == 5 && setup[0].name == "ServerName" && setup[1].name == "IpAddress" &&
	                    setup[2].name == "PortNumber" && setup[3].name == "AppId" && setup[4].name == "Lan" &&
	                    setup[4].value == "7",
	            "PlaySetup: ServerName, IpAddress, PortNumber, AppId, Lan")) return false;
	return true;
}

// State-code values match the witnessed dword_989574 transitions.
bool check_state_values() {
	using opennova::SessionState;
	if (!expect(static_cast<int>(SessionState::HostStarting) == 5, "HostStarting == 5")) return false;
	if (!expect(static_cast<int>(SessionState::HostEstablished) == 6, "HostEstablished == 6")) return false;
	if (!expect(static_cast<int>(SessionState::Connecting) == 7, "Connecting == 7")) return false;
	if (!expect(static_cast<int>(SessionState::Connected) == 8, "Connected == 8")) return false;
	return true;
}

// Handshake message builders produce containers with the exact names used
// by the jodemo senders.
bool check_handshake_names() {
	using opennova::make_client_connected;
	using opennova::make_client_host_request;
	using opennova::make_client_host_update;
	using opennova::make_client_play_request;
	using opennova::make_client_stop_hosting;
	using opennova::make_client_stop_playing;
	using opennova::NapiMessage;
	if (!expect(make_client_connected().name == "ClientConnected", "ClientConnected name")) return false;
	if (!expect(make_client_stop_hosting().name == "ClientStopHosting", "ClientStopHosting name")) return false;
	if (!expect(make_client_stop_playing().name == "ClientStopPlaying", "ClientStopPlaying name")) return false;

	// ClientHostRequest = CurrentlyHosting + VarCheck fields, then the Cookie,
	// HostSetup, Host, PlayerList var-lists (each a ClientVarList carrying a
	// VarList field + ClientVar children). [orig: CNapiGameSession_SendHostRequest
	// @ 0x4d3700 / NapiStatement_SerializeVarList @ 0x4d0660]
	using opennova::ClientVar;
	auto hr = make_client_host_request(
		1,
		{{0, "NWUID", "u"}},                                   // Cookie
		{{0, "AppId", "0"}, {0, "MaxPlayers", "65"}},          // HostSetup
		{{0, "ServerName", "OpenNova Host"}, {0, "Players", "1"}}, // Host
		{{0, "Slot0", "Taylor"}});                             // PlayerList
	if (!expect(hr.name == "ClientHostRequest", "ClientHostRequest name")) return false;
	if (!expect(field_str(hr, "CurrentlyHosting") == "1", "CurrentlyHosting == \"1\"")) return false;
	if (!expect(field_str(hr, "VarCheck") == "1", "VarCheck == \"1\"")) return false;
	if (!expect(hr.children.size() == 4, "ClientHostRequest has 4 var-lists")) return false;
	const char *hr_lists[] = {"Cookie", "HostSetup", "Host", "PlayerList"};
	for (int i = 0; i < 4; ++i) {
		if (!expect(hr.children[i].name == "ClientVarList", "host-request child is ClientVarList")) return false;
		if (!expect(field_str(hr.children[i], "VarList") == hr_lists[i], "host-request VarList name")) return false;
	}
	// Host var-list carries its ClientVar entries (VarName/VarValue).
	if (!expect(hr.children[2].children.size() == 2, "Host has 2 ClientVar")) return false;
	if (!expect(field_str(hr.children[2].children[0], "VarName") == "ServerName" &&
			field_str(hr.children[2].children[0], "VarValue") == "OpenNova Host",
			"Host first ClientVar ServerName")) return false;

	auto hu = make_client_host_update(
		{{0, "Players", "2"}},                                 // Host
		{{0, "Slot0", "Taylor"}, {0, "Slot1", "Joiner"}});     // PlayerList
	if (!expect(hu.name == "ClientHostUpdate", "ClientHostUpdate name")) return false;
	if (!expect(hu.children.size() == 2, "2 var-lists")) return false;
	if (!expect(hu.children[0].name == "ClientVarList" &&
			field_str(hu.children[0], "VarList") == "Host", "update child 0 ClientVarList(Host)")) return false;
	if (!expect(hu.children[1].name == "ClientVarList" &&
			field_str(hu.children[1], "VarList") == "PlayerList", "update child 1 ClientVarList(PlayerList)")) return false;

	// ClientPlayRequest = a top-level CurrentlyPlaying field, then the Cookie and
	// PlaySetup var-lists (each a ClientVarList carrying a VarList field + ClientVar
	// children), Cookie FIRST. [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920 /
	// NapiStatement_SerializeVarList @ 0x4d0660]
	using opennova::ClientVar;
	auto pr = make_client_play_request(
		7,
		{{0, "NWUID", "abc"}, {0, "NWHWI", "gpu"}},  // Cookie vars
		{{0, "Mission", "ASH_G11A"}});               // PlaySetup vars
	if (!expect(pr.name == "ClientPlayRequest", "ClientPlayRequest name")) return false;
	if (!expect(pr.fields.size() == 1 && pr.fields[0].name == "CurrentlyPlaying",
			"top-level CurrentlyPlaying field")) return false;
	if (!expect(field_str(pr, "CurrentlyPlaying") == "7", "CurrentlyPlaying == \"7\"")) return false;
	if (!expect(pr.children.size() == 2, "2 var-lists")) return false;
	if (!expect(pr.children[0].name == "ClientVarList" &&
			field_str(pr.children[0], "VarList") == "Cookie",
			"child 0 ClientVarList(Cookie)")) return false;
	if (!expect(pr.children[1].name == "ClientVarList" &&
			field_str(pr.children[1], "VarList") == "PlaySetup",
			"child 1 ClientVarList(PlaySetup)")) return false;
	// Cookie var-list carries two ClientVar children (VarFNum/VarName/VarValue).
	if (!expect(pr.children[0].children.size() == 2, "Cookie has 2 ClientVar")) return false;
	if (!expect(pr.children[0].children[0].name == "ClientVar", "Cookie child is ClientVar")) return false;
	if (!expect(field_str(pr.children[0].children[0], "VarName") == "NWUID" &&
			field_str(pr.children[0].children[0], "VarValue") == "abc" &&
			field_str(pr.children[0].children[0], "VarFNum") == "0",
			"first Cookie ClientVar VarFNum/VarName/VarValue")) return false;
	return true;
}

// End-to-end: build a handshake message, serialize via napi_stream_encode,
// round-trip-decode, and confirm structural equality.
bool check_handshake_wire_roundtrip() {
	using opennova::ClientVar;
	auto req = opennova::make_client_host_update(
		{{0, "ServerIP", "127.0.0.1"}, {0, "ServerPortNumber", "4444"}},  // Host
		{{0, "Slot0", "player"}});                                        // PlayerList
	std::vector<opennova::NapiMessage> stream = {req};
	std::vector<uint8_t> buf(opennova::napi_stream_size(stream) + 16, 0);
	size_t enc_size = 0;
	if (!expect(opennova::napi_stream_encode(stream, buf.data(), buf.size(), &enc_size) == 0,
			"encode handshake stream")) return false;
	std::vector<opennova::NapiMessage> decoded;
	size_t cons = 0;
	if (!expect(opennova::napi_stream_decode(buf.data(), enc_size, decoded, &cons) == 0,
			"decode handshake stream")) return false;
	if (!expect(decoded.size() == 1 && decoded[0].name == "ClientHostUpdate",
			"decoded root is ClientHostUpdate")) return false;
	if (!expect(decoded[0].children.size() == 2, "two children")) return false;
	// Both children survive as ClientVarLists with their VarList names.
	if (!expect(decoded[0].children[0].name == "ClientVarList", "child 0 ClientVarList")) return false;
	if (!expect(field_str(decoded[0].children[0], "VarList") == "Host", "child 0 VarList=Host")) return false;
	if (!expect(decoded[0].children[1].name == "ClientVarList", "child 1 ClientVarList")) return false;
	if (!expect(field_str(decoded[0].children[1], "VarList") == "PlayerList", "child 1 VarList=PlayerList")) return false;
	// The Host var-list's first ClientVar round-trips with its name/value.
	const auto &host_list = decoded[0].children[0];
	if (!expect(host_list.children.size() == 2, "Host var-list has 2 ClientVar")) return false;
	if (!expect(host_list.children[0].name == "ClientVar", "Host child is ClientVar")) return false;
	if (!expect(field_str(host_list.children[0], "VarName") == "ServerIP" &&
			field_str(host_list.children[0], "VarValue") == "127.0.0.1",
			"Host first ClientVar ServerIP")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_error_nwec_mapping()) return 1;
	if (!check_error_from_code()) return 1;
	if (!check_timing_constants()) return 1;
	if (!check_host_and_gate_error_maps()) return 1;
	if (!check_server_result_parsers()) return 1;
	if (!check_server_command_parse()) return 1;
	if (!check_server_statement_builders()) return 1;
	if (!check_leg_builders()) return 1;
	if (!check_state_values()) return 1;
	if (!check_handshake_names()) return 1;
	if (!check_handshake_wire_roundtrip()) return 1;
	std::printf("OK: session state + NWEC map + handshake builders + wire roundtrip + service statement builders\n");
	return 0;
}
