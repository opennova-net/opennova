#include <runtime/inmatch/client_console.h>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h> // vfs_version_crc (CRC_ComputeCustomTable)
#include <runtime/controls/controls.h> // the binding records by action code
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace opennova::inmatch {

namespace {

void post(ClientConsoleContext &ctx, std::string text) {
	if (ctx.system_lines != nullptr) ctx.system_lines->push_back(std::move(text));
}

// CNapiNetwork_QueueReliableMessage(ctx, tag, 1, user_param, body): reliable,
// the user parameter its retention in send flushes (0 = until acked).
void queue(ClientConsoleContext &ctx, uint8_t tag, std::vector<uint8_t> body,
		uint32_t user_param) {
	if (ctx.queued == nullptr) return;
	ProtocolMessage message = make_protocol_message(tag, std::move(body));
	message.retention_flushes = user_param;
	ctx.queued->push_back(std::move(message));
}

// NetPacket_WriteCStringB: the text and its NUL [orig: @0x42B3E0].
std::vector<uint8_t> cstring(const char *text) {
	std::vector<uint8_t> body;
	for (const char *p = text; *p != '\0'; ++p) body.push_back(static_cast<uint8_t>(*p));
	body.push_back(0);
	return body;
}

bool is_space(char ch) { return cp1252_isspace(static_cast<uint8_t>(ch)); }

// The CRT toupper under LC_ALL ".ACP" over a sign-extended char: an ASCII
// letter is folded; a byte past 0x7F reaches the locale's map and comes back as
// its unsigned upper-case byte (cp1252: a-acute..thorn less the divide sign, s-caron,
// oe, z-caron, y-diaeresis).
// [orig: NapiClientCmd_HandleDebug @0x42cd16..0x42cd1b, `movsx` then toupper]
int32_t locale_toupper(char ch) {
	const uint8_t b = static_cast<uint8_t>(ch);
	if (b >= 'a' && b <= 'z') return b - 0x20;
	if (b < 0x80) return b;
	if (b == 0x9A || b == 0x9C || b == 0x9E) return b - 0x10;
	if (b == 0xFF) return 0x9F;
	if (b >= 0xE0 && b <= 0xFE && b != 0xF7) return b - 0x20;
	return b;
}

} // namespace

uint8_t client_console_obfuscate(uint8_t ch) {
	const auto high = static_cast<uint8_t>(static_cast<int8_t>(ch) >> 4); // sar cl, 4
	const auto low = static_cast<uint8_t>(ch << 4);                       // shl al, 4
	return static_cast<uint8_t>((high | low) ^ 0xEA);
}

std::string client_console_echo_line(const std::vector<uint8_t> &body) {
	std::string line;
	for (std::size_t i = 0; i < body.size() && line.size() < 1023; ++i) {
		if (body[i] == 0) break;
		line.push_back(static_cast<char>(body[i]));
	}
	return line;
}

// [orig: NapiClientCmd_HandleDebugCommand @0x42C4D0 - String_TokenizeQuotedToArray
//  @0x42c51c, fewer than one token @0x42c524; CLIENTDEBUGMESSAGELIST @0x42c550..0x42c552
//  and its two lines; DIRTYUPSTREAM: the usage @0x42c5fd, PRE (off_7C5538) @0x42c62b /
//  @0x42c666, POST @0x42c695 / @0x42c6cf, atol @0x42c6f9, below 0 stored 0 @0x42c70a,
//  above 5 stored 5 @0x42c720, 0 "turned off" @0x42c775, the format @0x42c733; BAN:
//  is_in_session and NovaWorld @0x42c7b8, is_authority @0x42c7c1, the whole line as
//  NetPacket_WriteCStringB @0x42c7c3 -> QueueReliableMessage(0x24, 1, 0) @0x42c7ec, the
//  two refusals @0x42c812 / @0x42c821; SERVER: the three space walks @0x42c87b..0x42c8ed,
//  then the same 0x24 with the rest @0x42c8f7; GETPLAYERLIST: NetPacket_WriteByteAndInt16
//  (0, 23799) @0x42c983 -> 0x22 (1, 0) @0x42c988, then a bodiless 0x23 (1, 0) @0x42c9a9;
//  every line Chat_AddMessageChannel2(.., palette[0], 930)]
bool client_handle_debug_command(const std::string &line, ClientConsoleContext &ctx) {
	const std::vector<std::string> tokens = tokenize_quoted(line);
	if (tokens.empty()) return false;
	ClientConsoleState scratch;
	ClientConsoleState &state = ctx.state != nullptr ? *ctx.state : scratch;
	if (strutil::iequals(tokens[0], "CLIENTDEBUGMESSAGELIST")) {
		const bool was_enabled = state.debug_message_list;
		state.debug_message_list = !was_enabled;
		post(ctx, was_enabled ? "CLIENTDEBUGMESSAGELIST:  Disabled"
		                      : "CLIENTDEBUGMESSAGELIST:  Enabled");
		return true;
	}
	if (strutil::iequals(tokens[0], "DIRTYUPSTREAM")) {
		if (tokens.size() < 2) {
			post(ctx, "Usage:  DIRTYUPSTREAM [ PRE | POST | 0....5 ]");
			return true;
		}
		if (strutil::iequals(tokens[1], "PRE")) {
			post(ctx, "DIRTYUPSTREAM set to PRE mode");
			state.dirty_upstream_post = 0;
			return true;
		}
		if (strutil::iequals(tokens[1], "POST")) {
			post(ctx, "DIRTYUPSTREAM set to POST mode");
			state.dirty_upstream_post = 1;
			return true;
		}
		const long parsed = std::strtol(tokens[1].c_str(), nullptr, 10);
		int32_t level = static_cast<int32_t>(parsed);
		if (level < 0) level = 0;
		else if (level > 5) level = 5;
		state.dirty_upstream_level = level;
		if (level == 0) {
			post(ctx, "DIRTYUPSTREAM turned off");
		} else {
			char text[64];
			std::snprintf(text, sizeof(text), "DIRTYUPSTREAM set to %ld", static_cast<long>(level));
			post(ctx, text);
		}
		return true;
	}
	if (strutil::iequals(tokens[0], "BAN")) {
		if (ctx.in_session && ctx.novaworld) {
			if (ctx.authority) {
				queue(ctx, c2s::BAN_PUNT_COMMAND, cstring(line.c_str()), 0);
				return true;
			}
			post(ctx, "You must be the server to BAN a player.");
		} else {
			post(ctx, "BAN only works in multiplayer novaworld games.");
		}
		return true;
	}
	if (strutil::iequals(tokens[0], "SERVER")) {
		const char *cursor = line.c_str();
		if (is_space(*cursor)) {
			do {
				if (*cursor == '\0') break;
				++cursor;
			} while (is_space(*cursor));
		}
		if (!is_space(*cursor)) {
			do {
				if (*cursor == '\0') break;
				++cursor;
			} while (!is_space(*cursor));
		}
		if (is_space(*cursor)) {
			do {
				if (*cursor == '\0') break;
				++cursor;
			} while (is_space(*cursor));
		}
		queue(ctx, c2s::BAN_PUNT_COMMAND, cstring(cursor), 0);
		return true;
	}
	if (strutil::iequals(tokens[0], "GETPLAYERLIST")) {
		queue(ctx, c2s::PLAYER_SYNC_REQUEST, {0x00, 0xF7, 0x5C}, 0);
		queue(ctx, c2s::VISIBLE_PLAYERS_REQUEST, {}, 0);
		return true;
	}
	return false;
}

// [orig: NapiClientCmd_HandleDebug @0x42CCE0 - the leading spaces @0x42cceb; the walk
//  over the sixteen ten-byte entries from the length byte (unk_A821F9) @0x42cd00..0x42cd50:
//  a zero length skipped, a negative one taken at once with nothing compared @0x42cd14,
//  else toupper(movsx line char) against movsx(name byte) XOR 0xFE @0x42cd16..0x42cd30 (an
//  ASCII name's stored byte sign-extends, so no typed line meets it); the parameters past
//  the spaces @0x42cd59..0x42cd60; NetPacket_WriteObfuscatedString(.., entry id, rest)
//  @0x42cd9f -> QueueReliableMessage(7, 1, 310) @0x42cda4; the host drops it,
//  NapiNPServerMsg 0x07 @0x4FC970]
bool client_handle_server_command(const std::string &line, ClientConsoleContext &ctx) {
	if (ctx.server_commands == nullptr) return false;
	const replication::ClientServerCommandTable &table = *ctx.server_commands;
	std::size_t start = 0;
	while (start < line.size() && line[start] == ' ') ++start;
	// The walk reads the line's buffer past its end as zero bytes.
	const auto at = [&](std::size_t i) { return start + i < line.size() ? line[start + i] : '\0'; };
	// The entries sit back to back: a name longer than its eight bytes reads on into the
	// next entry's (past the table, other globals: read as zero here).
	const auto flat = [&](std::size_t f) -> uint8_t {
		const std::size_t n = replication::kServerCommandEntryBytes;
		return f < n * replication::kServerCommandSlots ? table[f / n][f % n] : uint8_t{0};
	};
	for (std::size_t e = 0; e < replication::kServerCommandSlots; ++e) {
		const replication::ServerCommandEntry &entry = table[e];
		const auto length = static_cast<int8_t>(entry[1]);
		if (length == 0) continue;
		std::size_t matched_chars = 0;
		if (length > 0) {
			bool matched = true;
			for (int k = 0; k < length; ++k) {
				const uint8_t stored =
						flat(e * replication::kServerCommandEntryBytes + 2 + static_cast<std::size_t>(k));
				const int32_t want = static_cast<int32_t>(static_cast<int8_t>(stored)) ^ 0xFE;
				if (locale_toupper(at(static_cast<std::size_t>(k))) != want) matched = false;
			}
			if (!matched) continue;
			matched_chars = static_cast<std::size_t>(length);
		}
		std::size_t rest = start + matched_chars;
		if (rest > line.size()) rest = line.size();
		while (rest < line.size() && line[rest] == ' ') ++rest;
		std::vector<uint8_t> body;
		body.push_back(entry[0]);
		for (std::size_t i = rest; i < line.size() && line[i] != '\0'; ++i)
			body.push_back(client_console_obfuscate(static_cast<uint8_t>(line[i])));
		body.push_back(0);
		queue(ctx, c2s::RESERVED_NOOP_07, std::move(body), 310);
		return true;
	}
	return false;
}

namespace {

// Record i's config token: the catalog row whose action code is i, "" for a zero record.
const char *binding_token(int record) {
	const controls::ActionDef *def = controls::action_for_code(record);
	return def != nullptr && def->token != nullptr ? def->token : "";
}

// The token field's first dword: up to four token bytes over the zero-filled field.
int32_t token_dword(const char *token) {
	uint32_t value = 0;
	for (int i = 0; i < 4 && token[i] != '\0'; ++i)
		value |= static_cast<uint32_t>(static_cast<uint8_t>(token[i])) << (8 * i);
	return static_cast<int32_t>(value);
}

// The MSVC CRT atol on the 32-bit long: leading white space, a sign, the digits;
// an out-of-range value saturates.
int32_t crt_atol(const char *text) {
	if (text == nullptr) return 0;
	const long long value = std::strtoll(text, nullptr, 10);
	if (value > INT32_MAX) return INT32_MAX;
	if (value < INT32_MIN) return INT32_MIN;
	return static_cast<int32_t>(value);
}

int32_t line_crc(const std::string &text) {
	return vfs_version_crc(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

} // namespace

int client_binding_by_token(const std::string &name) {
	for (int record = 0; record < kClientBindingRecords; ++record)
		if (strutil::iequals(name, binding_token(record))) return record;
	return -1;
}

int client_binding_by_crc(int32_t crc) {
	for (int record = 0; record < kClientBindingRecords; ++record)
		if (token_dword(binding_token(record)) == crc) return record;
	return -1;
}

// PunkBuster's command hook comes first in retail and is not modeled (D-NET-371). A `pb_`
// line is taken whatever PunkBuster's state: retail hands it to the PunkBuster module
// (`pb_sv_` to its server half, any other to its client half), each a call that does
// nothing with no PunkBuster context, and returns 1.
// [orig: Client_ProcessLocalCommand @0x520BF0 - PBServer_InvokeCommandCallback @0x520C2A;
//  the `pb_` arm @0x520C3B..0x520CB6 (the '/' or '\' skip @0x520C3B..0x520C45,
//  strnicmp(.., "pb_", 3) @0x520C50 (the string @0x7CCF84), "sv_" @0x520C6B, the PunkBuster
//  calls @0x520C94 / @0x520CC9 (0x4DA3F0 / 0x4D7ED0, each a no-op with no context
//  0xB608F4)), the WAC overlays @0x520CF1, SAVEKEYS @0x520D10 (KeyBinding_DumpAllToFile
//  @0x54C0A0), SAVENFO @0x520D4B, VERSION @0x520D8E (the format @0x7D00AC,
//  Chat_AddMessageChannel2(.., -1, 930) @0x520DC2), the out-of-session
//  arm @0x520DF2..0x520E5B: `is_in_session || !*line` returns 0, CRC_ComputeCustomTable
//  @0x520e0d over the whole line, the record walk @0x520e17..0x520e30,
//  Input_QueueEvent(record's code word, 0, 0, 0) @0x520e5b]
bool client_process_local_command(const std::string &line, const ClientLocalCommandSeams &seams) {
	const std::size_t skip = !line.empty() && (line[0] == '/' || line[0] == '\\') ? 1 : 0;
	if (strutil::starts_with_icase(std::string_view(line).substr(skip), "pb_")) return true;
	if (strutil::starts_with_icase(line, "SAVEKEYS")) {
		if (seams.save_keys) seams.save_keys();
		return true;
	}
	if (strutil::starts_with_icase(line, "SAVENFO")) {
		if (seams.save_nfo) seams.save_nfo();
		return true;
	}
	if (strutil::starts_with_icase(line, "VERSION")) {
		if (seams.version_line)
			seams.version_line("Version " + seams.version_text + "  - " + seams.title_text);
		return true;
	}
	if (seams.in_session || line.empty()) return false;
	const int record = client_binding_by_crc(line_crc(line));
	if (record < 0) return false;
	// The record's own code word: its index for a catalog row, 0 for a zero record.
	const int code = controls::action_for_code(record) != nullptr ? record : 0;
	if (seams.queue_action) seams.queue_action(code, 0, 0, 0);
	return true;
}

// [orig: NapiClientCmd_ParseAndExecute @0x4329C0 - the name walk @0x432a64..0x432a94, the
//  CRC walk @0x432aa9..0x432b3e (the "%8x" text it formats is unused), the queue by the token
//  count @0x432b43..0x432ba1 (1: no parameters; 2: atol(token 2); 3: tokens 2, 3; 4: tokens
//  2, 3, 4; more: return 0)]
bool client_parse_and_execute(const std::string &line, ClientConsoleContext &ctx) {
	if (client_handle_debug_command(line, ctx)) return true;
	io::ConfigTokens tokens;
	io::tokenize_config_line(line.c_str(), tokens);
	if (tokens.count == 0) return false;
	if (client_handle_server_command(line, ctx)) return true;
	const std::string name = tokens.tokens[0] != nullptr ? tokens.tokens[0] : "";
	int record = client_binding_by_token(name);
	if (record < 0) record = client_binding_by_crc(line_crc(name));
	if (record < 0) return false;
	int32_t p1 = 0, p2 = 0, p3 = 0;
	switch (tokens.count) {
	case 1: break;
	case 2: p1 = crt_atol(tokens.tokens[1]); break;
	case 3:
		p2 = crt_atol(tokens.tokens[2]);
		p1 = crt_atol(tokens.tokens[1]);
		break;
	case 4:
		p3 = crt_atol(tokens.tokens[3]);
		p2 = crt_atol(tokens.tokens[2]);
		p1 = crt_atol(tokens.tokens[1]);
		break;
	default: return false;
	}
	if (ctx.queue_action) ctx.queue_action(record, p1, p2, p3);
	return true;
}

} // namespace opennova::inmatch
