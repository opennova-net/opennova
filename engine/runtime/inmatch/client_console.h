#pragma once

// The client's console command walk (D-NET-710): what a client runs over the
// S2C 0x06 echo of its C2S 0x04 line, and what an authority that is not a
// session peer ends its own console line with. Its debug commands come first
// (CLIENTDEBUGMESSAGELIST, DIRTYUPSTREAM, BAN, SERVER, GETPLAYERLIST), then the
// line is tokenized, then the server's command table (S2C 0x2A) is matched,
// whose hit sends C2S 0x07, then the key-binding table queues an input action.
// [orig: NapiClientCmd_ParseAndExecute @0x4329C0 - NapiClientCmd_HandleDebugCommand
//  @0x4329f9, Config_TokenizeLine @0x432a3c, NapiClientCmd_HandleDebug @0x432a4e,
//  the binding walks @0x432a64..0x432b3e, Input_QueueEvent @0x432b4c / @0x432ba1;
//  NapiNPClientMsg_HandleChatCommand @0x432BC0 (the 0x06 copy, 1023 characters);
//  Client_ProcessConsoleCommand @0x520E80 (the authority's call @0x520f70)]

#include <base/io/ascii_config.h>
#include <net/npwire/protocol_message.h>
#include <runtime/replication/client_state.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::inmatch {

// The process's debug words the console's commands set.
// [orig: g_ClientDebugMessageList @0x814FD4 (CLIENTDEBUGMESSAGELIST),
//  g_DirtyUpstreamLevel @0x24D21BC (DIRTYUPSTREAM's level), g_DirtyUpstreamPostMode
//  @0x24D21C0 (its PRE 0 / POST 1 mode)]
struct ClientConsoleState {
	bool debug_message_list = false;
	int32_t dirty_upstream_level = 0;
	int32_t dirty_upstream_post = 0;
};

// One run's inputs and sinks.
struct ClientConsoleContext {
	bool in_session = false; // g_NapiNPCtx.is_in_session
	bool novaworld = false;  // the transport mode is NovaWorld's
	bool authority = false;  // g_NapiNPCtx.is_authority
	// The server's command table (S2C 0x2A); null reads as empty.
	const replication::ClientServerCommandTable *server_commands = nullptr;
	ClientConsoleState *state = nullptr;
	// CNapiNetwork_QueueReliableMessage on the process's connection, in order;
	// `retention_flushes` carries the user parameter (0 = held until acked).
	std::vector<ProtocolMessage> *queued = nullptr;
	// Chat_AddMessageChannel2(text, g_HUDColors.palette[0], 930): white SYSTEM lines.
	std::vector<std::string> *system_lines = nullptr;
	// The key-binding table's arm: by name, then by the name's CRC, an action whose
	// Input_QueueEvent takes up to three numeric parameters (the tokenized line).
	// True when a binding took the line. Unset: no binding takes it.
	std::function<bool(const io::ConfigTokens &)> queue_binding;
};

// NapiClientCmd_HandleDebugCommand: true when one of the five debug commands
// took the line.
bool client_handle_debug_command(const std::string &line, ClientConsoleContext &ctx);
// NapiClientCmd_HandleDebug: the server command table's walk; true when an
// entry took the line (its C2S 0x07 queued).
bool client_handle_server_command(const std::string &line, ClientConsoleContext &ctx);
// NapiClientCmd_ParseAndExecute: true when the line was taken.
bool client_parse_and_execute(const std::string &line, ClientConsoleContext &ctx);
// The S2C 0x06 body as its handler copies it: up to the NUL, the body's end or
// 1023 characters [orig: NapiNPClientMsg_HandleChatCommand @0x432beb..0x432c0f].
std::string client_console_echo_line(const std::vector<uint8_t> &body);

// NetPacket_WriteObfuscatedString's byte: the nibbles swapped through an 8-bit
// arithmetic shift, then XOR 0xEA [orig: @0x42aed0..0x42aedd].
uint8_t client_console_obfuscate(uint8_t ch);

} // namespace opennova::inmatch
