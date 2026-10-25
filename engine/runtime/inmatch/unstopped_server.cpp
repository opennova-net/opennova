// The held server: a server a mission exit's error arm left unstopped (UnstoppedServer,
// napi_np_connection.h) between the exit and the next session reset. The reset's StopServer over
// it, the manager's read of a datagram for it (a held node's socket legs, the protocol's hello
// handler, its join handler's socket pass) and the browse's and a join's protocol pump over it
// (the join handler's queued pass among them); napi_np_protocol.h declares them
// (D-NET-NEW-L1-26).
#include <runtime/inmatch/napi_np_protocol.h>

#include <runtime/inmatch/punt_log.h>       // ConnectLog (the manager's lines)
#include <runtime/inmatch/server_console.h> // the /INOUT lines

#include <base/io/le.h>       // read_u32_le (a datagram's key)
#include <base/io/strutil.h> // iequals (Napi_StrCaseEqual: the held join's PW and PV2)
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/outgoing_packets.h> // plan_outgoing_packets (a held node's build)
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h> // the held hello's identity gate

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

// The reset calls StopServer whatever +0x68 holds (its test gates only the reset's own
// Lobby_UpdateServerInfo(1) and the table release); StopServer gates itself on host_running,
// which a held server keeps set, and walks the list: each node
// takes the STOP record if none is latched, and its destroy writes the /INOUT removal line and
// sends the burst over whatever socket the manager holds then, a reopened one carrying it (a
// closed one fails the first SendTo and ends the loop); host_running clears and HOST STOPPED
// reads the manager's socket then and the session name the next create has not yet replaced.
// The destroy's removal callback, after the burst, reaches Server_HandlePlayerDisconnect @0x51B5C0
// for a node whose player was added (gated on is_authority @0x51B5D0, which the next host's
// connection mode has set again, and the slot's active byte @0x51B5DC, which the teardown's
// reset keeps): its first leg is the NovaWorld session's ClientHostPlayerRemoved, sent in state 6
// alone, then the slot clock to phase 0 and the PLAYER gate report on a NovaWorld host
// (`player_removed`, the caller's; D-NET-NEW-L9-21, D-NET-NEW-L9-22). Its game sends queue to
// nodes this walk destroys.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the protocol's +0x2D8 callback
//  called @0x625438; CNapiGameSession_CreateSession @0x4C97C0 - NapiNPServer_DestroyPlayerCtx
//  installed there @0x4C99D0; NapiNPServer_DestroyPlayerCtx @0x4C8400 -
//  Server_ForwardEventToSession called @0x4C8426 (its is_authority test @0x51B8E7);
//  Server_HandlePlayerDisconnect @0x51B5C0 - the removal called @0x51B5F3;
//  HostDialog_StartSession @0x558760 - CGameSession_SetConnectionMode called @0x55878F]
// [orig: CNapiGameSession_ResetActiveSession @0x4C8A70 - the +0x68 tests @0x4C8A73 (to the
//  Lobby_UpdateServerInfo(1) call @0x4C8A7B) and @0x4C8AA6 (to the release
//  @0x4C8AAC..0x4C8AB1), StopServer called @0x4C8AA1 unconditionally;
//  NapiNPProtocol_StopServer @0x62A820 - host_running @0x62A85F..0x62A869, the record
//  @0x62A8C3..0x62A8F6, latched-if-invalid @0x62A904..0x62A919, CNapiNPConnection_Destroy
//  @0x62A924, host_running
//  cleared @0x62A95C, CNapiNPConnection_LogHostStopped @0x62A97B; TeardownActiveConnection
//  @0x6253C0 - the removal line @0x6253D2, the burst @0x6253EF..0x625424;
//  CNapiNPConnection_LogHostStopped @0x61E780 - the socket read @0x61E7A6; PreMenu_HostSetup
//  @0x5691F0 ahead of CNapiGameSession_BuildAndCreateSession @0x5694D0 (called @0x56A46A; the
//  session create's name copy @0x4C9985 comes after)]
void stop_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket *socket, const PeerAddr *local_address,
		const std::function<void(NapiNPConnection &slot)> &player_removed) {
	if (!server.running) return;
	// A held own pair (one no browse or join start released, release_unstopped_own_client): the
	// reset destroys the session's own client connection first, its
	// removal line with the record it holds (latched or not); its 0x46 goes to the manager's
	// loopback. The stop's walk then takes its twin, the first node, as any node. Its client
	// callback's process latch write and switch reach nothing here: a host's reset precedes its
	// mission start, which clears the exit reason and the input queue, a later own leave's record
	// (CIDEMIS) maps nothing as the held record does, and a join clears the latch at its connect
	// start [orig: Game_StartMission @0x524360 - g_MissionExitReason cleared @0x525B16,
	// Input_ResetAllState called @0x525C66; CNapiNetwork_StartClientConnection @0x4CA160 - the
	// latch's memset @0x4CA1A6..0x4CA1B4]. The line's
	// gate, the active byte +0x28, holds for every held own client: a session's create
	// succeeds only once its own connection reached state 5, whose state change sets the byte,
	// and only a teardown clears it.
	// [orig: CNapiGameSession_ResetActiveSession @0x4C8A70 - the +0xE60 test @0x4C8A83, its
	//  CNapiNPConnection_Destroy @0x4C8A93 ahead of StopServer @0x4C8AA1;
	//  CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the +0x28 test @0x6253C9, the
	//  removal line @0x6253D2, the clear @0x625535; CNapiGameSession_CreateSession -
	//  CNapiGameSession_WaitForHostResponse called @0x4C9C6F, the failure's destroy @0x4C9C79;
	//  CNapiGameSession_WaitForHostResponse @0x62B2D0 - state 5 @0x62B4C6, return 0 @0x62B4DD;
	//  CNapiNPConnection_OnStateChange @0x626060 - states 1 / 5 @0x626079, +0x28 set @0x626172]
	if (server.own_client)
		write_inout_line(logs,
				inout_player_removed_line(*server.own_client, NapiNPConnection::kTypeClientSide));
	for (UnstoppedNode &node : server.nodes) {
		NapiNPConnection &conn = node.conn;
		latch_disconnect_event(conn, make_disconnect_event(1, 9, 0, 0, "", 0, "NP.C:SH:STOP"));
		teardown_unstopped_node(conn, logs, socket);
		if (player_removed &&
				(conn.link.owned_entity.valid() || conn.phase >= ConnectionPhase::PlayerAdded))
			player_removed(conn);
	}
	const std::string session_name = std::move(server.session_name);
	server = UnstoppedServer{};
	write_inout_line(logs, inout_host_line(local_address, session_name, /*started=*/false));
}

// A held node's destroy, as any running server's: the removal line with the latched record,
// then the burst over the socket the manager holds (none open: nothing on the wire).
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the removal line @0x6253D2, the
//  burst @0x6253EF..0x625424 (SendDisconnectPacket's host_running gate @0x61F329)]
void teardown_unstopped_node(const NapiNPConnection &conn, const ServerLogs &logs,
		opennova::IDatagramSocket *socket) {
	write_inout_line(logs, inout_player_removed_line(conn));
	// The own pair's twin, at address 0, bursts into the manager's loopback, not the socket
	// [orig: CNapiNPManager_SendTo @0x61EC20 - the address test @0x61EC59, the loopback FIFO's
	//  NapiFifo_WritePacketAtomic call @0x61ECC6].
	if (socket == nullptr || conn.peer.ip == 0) return;
	for (const std::vector<uint8_t> &datagram : host_goodbye_burst(/*host_running=*/true, conn))
		socket->send_to(conn.peer, datagram.data(), datagram.size());
}

// The create's DisconnectActiveConnection over the held own client (its active byte set: the
// session's create reached state 5), then its destroy: the record {role 2 (a client), 2, 0, 0,
// "N.C:CSSAC", 0, ""} latched only where none is, the request, then the teardown's state-5 arm:
// the CLIENT removal line, the 0x46s to address 0 (the manager's loopback FIFO, no wire), and the
// client callback, whose process latch keeps the first record. Its switch runs over the latch:
// over the CSSAC record (reason 0) it does nothing, and over a stale record an earlier kick
// latched it stores that record's exit reason (7, 9..19) or queues its input event (36 for code
// 34, else 3). Neither reaches anything: no reader sees the exit reason before the next mission
// start clears it, and the input queue is read only by the in-mission frames' input pass, after
// that start has emptied it. Its other stores have no reader on this path either: the active
// connection id, the loading timeout flag the same create sets 0 again, and the joined session's
// strings. Its NET DISCONNECTING line needs the connect log, which this host's own new connection
// turned off at its session create, so it is never written here.
// The loopback's 0x46s reach the twin at the next manager receive (drained ahead of the socket):
// its handler latches the client's record on it only where none is and requests its disconnect,
// and the protocol pump's pending walk destroys it.
// [orig: CNapiNetwork_DisconnectActiveConnection @0x4C9140 - conn_flag0 tested, the role (1 for
//  a server-side node @0x4C91CF, else 2 for a client), dc 2 @0x4C91EF, the text @0x4C91FF,
//  latched-if-invalid @0x4C922D..0x4C923E,
//  RequestDisconnect @0x4C924A; CNapiGameSession_CreateServerConnection @0x4C9D30 - the destroy
//  @0x4C9D7D; CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the line @0x6253D2, the
//  state-5 burst @0x6254BF..0x6254D3, the client callback @0x6254E7;
//  CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 - the connect log's test ahead of
//  @0x4C6486, active_connection_id @0x4C6495, g_NetDisconnectedFlag @0x4C64A4, the strings
//  @0x4C64AE..0x4C6521, the latch @0x4C6535..0x4C6559, the switch's `dc == 2` @0x4C6563 and
//  reason test @0x4C6571, its stores @0x4C65A2..0x4C677C and Input_QueueEvent called @0x4C67A4
//  (36 from code 34's arm @0x4C6594..0x4C6596, else 3);
//  CNapiGameSession_CreateServerConnection - g_NetDisconnectedFlag 0 @0x4C9DF8;
//  Session_ResetForStart - the exit reason cleared @0x5202CF; Game_StartMission - the exit
//  reason cleared @0x525B16, Input_ResetAllState called @0x525C66; Input_ProcessFrame @0x49D520
//  (the queue's one reader, Input_DequeueEvent called @0x49D5BA) - called by
//  Game_ProcessMainFrame @0x52661D and UI_ProcessMenuScreenFrame @0x54E695, the in-mission
//  screen pass; Input_ResetAllState - the queue's memset @0x497EE1..0x497EE6;
//  NapiNPServer_HandleNewConnection @0x4C8040 - g_ConnectLogEnabled cleared
//  @0x4C80AF; CNapiNPManager_SendTo @0x61EC20 - the address test @0x61EC59, the FIFO's write
//  called @0x61ECC6; Nwu_HandleDisconnect @0x623CE0 - the latch @0x6240A8..0x6240BA,
//  RequestDisconnect called @0x6240C4; NapiNPProtocol_Pump @0x62A650 - the pending walk
//  @0x62A6FD..0x62A793]
void release_unstopped_own_client(UnstoppedServer &server, const ServerLogs &logs,
		ProcessDisconnectLatch *latch) {
	if (!server.own_client) return;
	NapiNPConnection &own = *server.own_client;
	latch_disconnect_event(own, make_disconnect_event(2, 2, 0, 0, "N.C:CSSAC", 0, ""));
	write_inout_line(logs,
			inout_player_removed_line(own, NapiNPConnection::kTypeClientSide));
	const DisconnectEvent record = own.disconnect_event;
	if (latch != nullptr) latch->latch(record);
	for (UnstoppedNode &node : server.nodes) {
		if (node.conn.peer.ip != 0) continue; // the twin, the own client's server-side node
		latch_disconnect_event(node.conn, record);
		node.conn.pending_disconnect = true;
		break;
	}
	server.own_client.reset();
}

namespace {

void held_connect_log_line(const ServerLogs &logs, const std::string &text) {
	if (logs.connect != nullptr) logs.connect->line(text);
}

// NapiNPProtocol_FindConnection(1, sender) over the held nodes: type, address and port.
// [orig: NapiNPProtocol_FindConnection @0x622750]
UnstoppedNode *find_unstopped_node(UnstoppedServer &server, const PeerAddr &from) {
	for (UnstoppedNode &node : server.nodes)
		if (node.conn.peer == from) return &node;
	return nullptr;
}

// CNapiNPConnection_ParseMessages over what one admission took for a held node: the receive
// stamp the reap measures, the peer's ACK over the node's sent list and the ACK the node now
// owes. Each record reaches DispatchMessage, whose game handler returns at once: every handler
// of the server table opens on is_authority (or g_RoundOver, then is_authority), which the
// router's error arm cleared (SetConnectionMode(0)) and no browse or join sets again; the
// unknown-tag callback is a bare return and the pre-handler slot is empty.
// [orig: CNapiNPConnection_ParseMessages @0x625BC0 - the stamp @0x625D54, the ack sweep
//  @0x625D90..0x625DA2, has_pending_out @0x625DFF, DispatchMessage called @0x625EF1;
//  CNapiNPConnection_DispatchMessage @0x622570 - the pre-handler +0x2E8 @0x6226B1 (unset by
//  CNapiNetwork_Init @0x4CA4A0, @0x4CA902..0x4CA9CD), the handler called @0x6226EF, the unknown-tag
//  callback +0x2EC @0x622703 (NapiNPServer_OnUnknownMessageStub, nop / retn); g_NPMsgInfoServer @0x82B5D8 (each
//  handler's `cmp g_NapiNPCtx.is_authority, 0` at its head, e.g. NapiNPServerMsg_UpdateAck
//  @0x5024B0, NapiNPServerMsg_0x00C @0x501C30; Server_ProcessClientRequestRespawn @0x519AF0 -
//  g_RoundOver @0x519B00, is_authority @0x519B0C); PostMenu_RouteMissionExit @0x568460 -
//  CGameSession_SetConnectionMode(0) called @0x5686ED; CGameSession_SetConnectionMode @0x4C49F0
//  - mode 0 stores is_authority 0 @0x4C4A2E]
void admit_unstopped_packet(NapiNPConnection &conn, const SessionDeframeAdmission &admission,
		uint32_t now_ms) {
	if (!admission.admitted) return;
	conn.receive_inactive_ms = 0;
	conn.receive_stamp_ms = now_ms;
	for (const SessionDeframeAdmission::Packet &packet : admission.packets)
		if (!packet.messages.empty()) conn.session_ack_owed = true;
	if (admission.max_ack_count > conn.peer_acked_seq)
		conn.peer_acked_seq = admission.max_ack_count;
	acknowledge_session_packets(conn.seq, admission.max_ack_count);
}

// The held protocol's hello handler over a 0x41 from any source. It gates on host_running alone
// (with the processing and drop bytes, written only zero), never on is_authority, which the
// router's error arm cleared, so a held server answers every hello its socket reads: a LAN
// browser's probe, a join's enumerate, the process's own broadcast. The hello passes the
// identity gate (a PM-less probe, as the live host's) and takes the 0x81 over `socket`: the
// protocol's words as the exit left them, the uptime against the start stamp (host_running
// still set), NP and NC the held nodes (each a type-1 node its 0x42 brought to state 1, counted
// in at its new-connection callback and its link, and out at its destroy), and the
// hello's CI, external triple and source. Returns the handler's result, which the manager's
// dispatch line follows: a PM-less hello's answer only.
// [orig: NapiNPProtocol_HandleClientHello @0x6213B0 - host_running @0x621454, +0x1F5 @0x621463,
//  +0x1F6 @0x62146B, the PM bypass @0x6217C2, the identity gate @0x6217F1..0x6218B0,
//  SendServerInfoPacket called @0x6218FC, `return protocol_version == 0` @0x62190B;
//  NapiNPProtocol_SendServerInfoPacket @0x6204B0 - host_running @0x620520, UT @0x62064F..
//  0x620683, NP +0x520 @0x6209D8, NC +0xED4 @0x620A4D, RIP / RPN @0x620A7D..0x620ABC;
//  NapiNPServer_HandleNewConnection @0x4C8040 - ++np_count @0x4C820D;
//  NapiNPServer_DestroyPlayerCtx @0x4C8400 - --np_count @0x4C845A; CNapiNPConnection_Create
//  @0x62ACB0 - ++proto+0xED4 for type 1 @0x62B1BD..0x62B1CB; CNapiNPConnection_Destroy @0x62A4B0
//  - the decrement @0x62A5FF]
bool answer_unstopped_hello(const UnstoppedServer &server, opennova::IDatagramSocket &socket,
		const PeerAddr &from, const std::vector<uint8_t> &body, uint32_t now_ms) {
	ClientHello hello;
	if (server.identity == nullptr || !parse_client_hello(body.data(), body.size(), hello) ||
			!client_hello_admits(*server.identity, hello))
		return false;
	ServerHello reply = server.hello;
	const ServerHello request = build_server_hello(hello, from.ip, from.port);
	reply.ci = request.ci;
	reply.rip = request.rip;
	reply.rpn = request.rpn;
	reply.eip = request.eip;
	reply.epn = request.epn;
	reply.et = request.et;
	reply.ut = now_ms - server.host_start_tick;
	reply.np = static_cast<uint32_t>(server.nodes.size());
	reply.nc = reply.np;
	const std::vector<uint8_t> datagram =
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply));
	socket.send_to(from, datagram.data(), datagram.size());
	return hello.pm == 0;
}

// The held protocol's join handler on the socket pass (its last argument 0), over a 0x42 from any
// source: the head gate is host_running and the processing byte alone, never is_authority. The
// TLV walk ending on a CU bound returns 1 with nothing queued; an identity that is not the
// protocol's returns 0; any other 0x42 is queued on the protocol's server-opcode queue for the
// pump's queued pass and returns 1. Returns the handler's result.
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - host_running @0x62B7D7, +0x1F5 and the
//  size @0x62B802 (return 0), the CU bounds @0x62BBEB..0x62BBFA with the socket pass's return 1
//  @0x62C331, the identity compares through @0x62BDAE with their return 0 @0x62B7E6, the
//  is_server test @0x62BDC5, NapiFifo_WritePacketAtomic(+0x22C) @0x62C323, return 1 @0x62C328]
bool queue_unstopped_join(UnstoppedServer &server, const PeerAddr &from,
		std::vector<uint8_t> body) {
	ClientAuth auth;
	if (!parse_client_auth(body.data(), body.size(), auth)) return !body.empty();
	if (server.identity == nullptr || !matches_title_identity(*server.identity, auth))
		return false;
	server.queue.push_back(UnstoppedPacket{from, SESSION_OPCODE_CLIENT_AUTH, std::move(body)});
	return true;
}

// The held protocol's join handler on the queued pass (its last argument 1), in the queue's
// arrival order: the identity again, then the protocol's own gates, each a CR=0 0x82 over
// `socket` (HK against the 0x81's host key 3, the server password 4, PV2 7, an empty NA 5, the
// new-connection refusal byte 6); a held node at the sender's address whose CI and CK match is
// sent its 0x82 again and nothing more, any other is destroyed (its removal line, its burst);
// then the create (the process's connection counter, the server keys), the validate callback
// over the dead session's words (the join lock 14 / 2, the address bans 14 / 3, the held nodes
// against the capacity 14 / 4, or 14 / 5 with the lineup queue open), a refused node going with
// no line. The lock and ban tests run under is_in_session, which every pump of a held server
// meets set: the error arm cleared it with the network type, and every join attempt follows a
// browse whose network type sets it again (the LAN search's, or the NovaWorld menu's entry; the
// join's own state machine sets none), while the one clear that keeps the network type, the
// full destroy's, follows its session reset, which stops the held server. Then the
// accept: the node filled as the live host fills one (fill_joined_node), its state change's
// receive stamp at `now_ms`, the new-connection callback's disabling line while the connect log
// is still on and its MPS negotiation, the 0x82 over `socket`, the /INOUT line, and the two CS
// updates queued for the node's first build. The removal callbacks reach no handler
// (is_authority, which the router's error arm cleared).
// [orig: NapiNPProtocol_PumpRecvQueues @0x6266A0 - the handler called with 1 @0x6267AD;
//  NapiNPProtocol_HandleClientJoin @0x62B750 - HK @0x62BDD5 (3), PW @0x62BE18 (4), PV2 @0x62BE40
//  (7), NA @0x62BE7D (5), +0x1F7 @0x62BE8F (6), FindConnection(1, sender) @0x62BEBA, the
//  retransmit's state 1 @0x62BECA, CI @0x62BED4, CK @0x62BEE0 and SendSessionInit @0x62BEE9
//  (return @0x62BEF1), CNapiNPConnection_Destroy @0x62BEFB, CNapiNPConnection_Create @0x62BF06,
//  the keys @0x62BF5E / @0x62BF99, the validate callback +0x2D0 (loaded @0x62C06E, `call edi`
//  @0x62C0AA, its result's `jge` @0x62C0B1), the refusal @0x62C0E7 and the destroy @0x62C248,
//  the state set @0x62C2D2 and CNapiNPConnection_OnStateChange called @0x62C2D7;
//  CNapiNPConnection_Create @0x62ACB0 - the counter @0x62B282..0x62B296;
//  CNapiNPConnection_Destroy @0x62A4B0 - TeardownActiveConnection only in states 1 / 5
//  @0x62A4F0..0x62A4F9;
//  CNapiNetwork_ValidateJoinRequest @0x4C61B0 - is_in_session @0x4C61C1, g_ServerJoinLocked
//  @0x4C61CA (2), g_BannedIdList @0x4C6217 (3), the count @0x4C623F (4 / 5 @0x4C624E);
//  PostMenu_RouteMissionExit @0x568460 - the error arms' SetNetworkType(0) called @0x5686E1;
//  UI_StartLANSessionSearch @0x558300 - CNapiNetwork_SetNetworkType called @0x55831F;
//  UI_ProcessLANSessionStateMachine - SetNetworkType(1) called @0x558E8C;
//  MultiPlayer_JoinSessionStateMachine @0x56A320 (no SetNetworkType call);
//  CNapiNetwork_SetNetworkType @0x4C4A50 - is_in_session @0x4C4A85;
//  CNapiGameSession_FullDestroy @0x4C96A0 - CNapiGameSession_ResetActiveSession called
//  @0x4C96E2 ahead of the is_in_session clear @0x4C973D;
//  CNapiNPConnection_OnStateChange @0x626060 - connection_id from the counter @0x62619C,
//  cb_server_1 called @0x6261C6, SendSessionInit @0x6261FC, LogPlayerAdded @0x626204;
//  NapiNPServer_HandleNewConnection @0x4C8040 - the disabling line @0x4C80A7, the MPS
//  @0x4C818D..0x4C81E4, ++np_count @0x4C820D]
void join_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket &socket, const PeerAddr &from, const std::vector<uint8_t> &body,
		uint32_t now_ms) {
	ClientAuth auth;
	if (server.identity == nullptr || !parse_client_auth(body.data(), body.size(), auth) ||
			!matches_title_identity(*server.identity, auth))
		return;
	const auto refuse = [&](uint32_t family, uint32_t reason) {
		const std::vector<uint8_t> datagram = join_rejection_datagram(auth, family, reason);
		socket.send_to(from, datagram.data(), datagram.size());
	};
	uint32_t reject = 0;
	if (auth.hk != server.hello.hk) reject = 3;
	else if (!server.server_password.empty() &&
			!strutil::iequals(auth.pw, std::string_view(server.server_password.c_str())))
		reject = 4;
	else if (!strutil::iequals(auth.pv2, server.identity->pv2)) reject = 7;
	else if (auth.na.empty()) reject = 5;
	else if (server.reject_new_connections) reject = 6;
	if (reject != 0) {
		refuse(reject, 0);
		return;
	}
	if (UnstoppedNode *existing = find_unstopped_node(server, from)) {
		// Retail's retransmit also wants the node in state 1, which every held node is: a
		// server-side node enters it at the accept and leaves it only at its destroy, a
		// disconnect request on it setting just its pending byte.
		// [orig: NapiNPProtocol_HandleClientJoin - `cmp [eax+1Ch], 1` @0x62BECA, the state store
		//  @0x62C2D4; CNapiNPConnection_RequestDisconnect @0x61E0F0 - state 1's pending store
		//  @0x61E107]
		if (existing->conn.client_ci == auth.ci && existing->conn.client_ck == auth.ck) {
			const std::vector<uint8_t> datagram =
					make_server_auth_datagram(auth, from, existing->conn);
			socket.send_to(from, datagram.data(), datagram.size());
			return;
		}
		teardown_unstopped_node(existing->conn, logs, &socket);
		server.nodes.erase(server.nodes.begin() + (existing - server.nodes.data()));
	}
	const uint32_t connection_id = next_connection_index();
	const ConnectionKeys server_keys = manager_connection_keys();
	// The lock and ban tests' is_in_session gate always passes here (the witness above).
	// [orig: CNapiNetwork_ValidateJoinRequest - is_in_session @0x4C61C1]
	if (server.join_locked) {
		refuse(14, 2);
		return;
	}
	if (std::find(server.banned_addresses.begin(), server.banned_addresses.end(), from.ip) !=
			server.banned_addresses.end()) {
		refuse(14, 3);
		return;
	}
	if (static_cast<int32_t>(server.nodes.size()) >= server.join_capacity) {
		refuse(14, server.lineup_queue_open ? 5u : 4u);
		return;
	}
	UnstoppedNode node;
	NapiNPConnection &conn = node.conn;
	conn.type = NapiNPConnection::kTypeServerSide;
	conn.peer = from;
	conn.timeouts = server.node_template;
	conn.seq = make_jo_game_session_sequencing();
	sync_session_sequencing_limits(conn.seq, conn.timeouts);
	const bool negotiated =
			fill_joined_node(conn, auth, from, *server.identity, connection_id, server_keys, now_ms);
	if (logs.connect != nullptr && logs.connect->enabled())
		logs.connect->disable_at_server_connection();
	const std::vector<uint8_t> datagram = make_server_auth_datagram(auth, from, conn);
	socket.send_to(from, datagram.data(), datagram.size());
	write_inout_line(logs, inout_player_added_line(conn));
	if (negotiated) node.queued = make_game_session_initial_settings(conn.timeouts.max_packet_bytes);
	server.nodes.push_back(std::move(node));
}

} // namespace

// The manager's read of a held node's datagram. The manager writes RAW, COOKED and its dispatch
// line around the opcode handler as for any datagram (the connect log the host's first new
// connection turned off writes none of them). The server opcodes' socket legs find the node by
// the sender: a 0x43 needs only the node and its key (the active byte is the queued leg's), and
// is queued for the pump's server-opcode pass; a 0x46 the same; a resend list and a ping need
// the active node and act at once over the socket the manager read from; the probe is taken for
// the node. A 0x41 needs no node: it reaches the protocol's hello handler from any source
// (answer_unstopped_hello); nor does a 0x42, whose handler's socket pass queues it for the
// pump's queued pass (queue_unstopped_join). The protocol's processing and drop bytes are
// written only zero.
// [orig: NapiNPManager_PumpReceive @0x623010 - cb_on_raw @0x623121, cb_on_packet @0x623254,
//  HandlePacket called @0x623284; NapiNPManager_HandlePacket @0x622F10 - DispatchOpcode called
//  @0x622F96 with last argument 0, cb_on_dispatch @0x622FCD, cb_on_error(4) @0x622FF9;
//  NapiNPProtocol_DispatchOpcode @0x622B40 - g_NPOpcodeHandlers @0x849D90 walked, the handler
//  called @0x622BFE; NapiNPProtocol_HandleSessionPacket @0x626A00 - the type-1 arm
//  @0x626A96..0x626AA8, FindConnection called @0x626AC5, the gates @0x626B01 (conn_flag0
//  only with has_seq), the key @0x626B72, the queue @0x626C8C; Nwu_HandleDisconnect @0x623CE0 -
//  FindConnection(1) called @0x623D89, the gates @0x623DF2, the key @0x623E74, the type-1 queue
//  @0x624125; NapiNP_HandleResendList @0x623800 -
//  @0x6238EA, the key @0x623959, SendSessionPacket called @0x6239B6, cb_server_6 @0x623A0E
//  (NapiNPServer_OnResendListCallback @0x4C62A0 - the g_NetQuality flag-1 store @0x4C62CE);
//  Nwu_HandlePing @0x623A70 - @0x623B55, the key @0x623BC2, the stamp @0x623C56, SendPing
//  called @0x623C6F; Nwu_HandleClientProbe @0x624280; NapiNPProtocol_Create @0x625730 - the
//  +0x1F5 store @0x625846; NapiNPServer_HandleNewConnection @0x4C8040 - g_ConnectLogEnabled
//  cleared @0x4C80AF, set again only by Game_ParseCommandLineAndInit @0x4A74AF / @0x4A74C9]
UnstoppedReceive receive_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket &socket, const PeerAddr &from, const uint8_t *raw,
		std::size_t len, uint32_t now_ms) {
	UnstoppedReceive out;
	if (!server.running || raw == nullptr || len == 0 || napi_punkbuster_datagram(raw, len))
		return out;
	std::vector<uint8_t> unpacked;
	if (!nw_unpack_inbound(raw, len, unpacked) || napi_baffle_packet(unpacked)) return out;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_split_unpacked(unpacked, opcode, body)) return out;
	const int32_t unpacked_len = static_cast<int32_t>(unpacked.size());
	if (opcode == SESSION_OPCODE_CLIENT_HELLO) {
		out.taken = true;
		held_connect_log_line(logs, napi_got_raw_text(static_cast<int32_t>(len), from));
		held_connect_log_line(logs, napi_got_cooked_text(unpacked_len, from));
		const bool handled = answer_unstopped_hello(server, socket, from, body, now_ms);
		held_connect_log_line(logs, handled
				? napi_got_protocol_text(unpacked_len, from, server.protocol_name)
				: napi_tossed_text(unpacked_len, from, NapiTossReason::NoProtocol));
		return out;
	}
	// A 0x42 needs no node either: the join handler's socket pass queues it for the pump.
	if (opcode == SESSION_OPCODE_CLIENT_AUTH) {
		out.taken = true;
		held_connect_log_line(logs, napi_got_raw_text(static_cast<int32_t>(len), from));
		held_connect_log_line(logs, napi_got_cooked_text(unpacked_len, from));
		const bool handled = queue_unstopped_join(server, from, std::move(body));
		held_connect_log_line(logs, handled
				? napi_got_protocol_text(unpacked_len, from, server.protocol_name)
				: napi_tossed_text(unpacked_len, from, NapiTossReason::NoProtocol));
		return out;
	}
	UnstoppedNode *node = find_unstopped_node(server, from);
	if (node == nullptr || opcode < SESSION_OPCODE_PROTOCOL_MESSAGE || opcode > kClientProbeOpcode)
		return out;
	out.taken = true;
	held_connect_log_line(logs, napi_got_raw_text(static_cast<int32_t>(len), from));
	held_connect_log_line(logs, napi_got_cooked_text(unpacked_len, from));
	NapiNPConnection &conn = node->conn;
	const bool active = conn.phase >= ConnectionPhase::Joined;
	const bool keyed = body.size() >= 4 && io::read_u32_le(body.data()) == conn.server_sk;
	bool handled = false;
	std::vector<std::vector<uint8_t>> sends;
	switch (opcode) {
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
	case SESSION_OPCODE_CLIENT_GOODBYE:
		if (keyed) {
			server.queue.push_back(UnstoppedPacket{from, opcode, std::move(body)});
			handled = true;
		}
		break;
	case SESSION_OPCODE_CLIENT_RESEND_LIST: {
		bool named = false;
		handled = active && resend_session_packets(conn, body, named, sends);
		if (named) out.net_quality_link_errors |= kNetQualityLinkErrorOutgoing;
		break;
	}
	case SESSION_OPCODE_CLIENT_PING:
		handled = active && answer_session_ping(conn, body, now_ms, now_ms, sends);
		break;
	default: // the client probe: taken for the node
		handled = true;
		break;
	}
	for (const std::vector<uint8_t> &datagram : sends)
		socket.send_to(from, datagram.data(), datagram.size());
	held_connect_log_line(logs, handled
			? napi_got_protocol_text(unpacked_len, from, server.protocol_name)
			: napi_tossed_text(unpacked_len, from, NapiTossReason::NoProtocol));
	return out;
}

namespace {

// BuildOutgoingPackets over a held node's queue, as the host frame's boundary builds a
// connection's (host_session.cpp build_s2c_queue / send_session_batches, the node in hand):
// the packets framed before the exit leave first, then the queued messages planned under the
// node's ceiling and its build budget, the description's and the overflow's latches in queue
// order; what the budget or a failed frame leaves stays queued. Returns whether a staged packet
// left. [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430 - the ceiling @0x628436, the
//  packet loop @0x62860B..0x628619; NapiNPMessage_Create @0x627FC0 (the overflow's
//  RequestDisconnect @0x628112)]
bool build_unstopped_node(UnstoppedNode &node, opennova::IDatagramSocket &socket,
		uint32_t now_ms) {
	NapiNPConnection &c = node.conn;
	std::size_t budget = max_packets_per_build(c.timeouts.max_packets_per_tick);
	bool released = false;
	if (!node.staged.empty()) {
		std::size_t sent = 0;
		while (sent < node.staged.size() && sent < budget) {
			socket.send_to(c.peer, node.staged[sent].data(), node.staged[sent].size());
			++sent;
		}
		node.staged.erase(node.staged.begin(),
				node.staged.begin() + static_cast<std::ptrdiff_t>(sent));
		budget -= sent;
		c.last_session_send_ms = now_ms;
		released = true;
	}
	if (node.queued.empty() || budget == 0) return released;
	OutgoingPacketPlan plan = plan_outgoing_packets(c.seq, node.queued,
			packet_ceiling_bytes(c.timeouts.max_packet_bytes), budget,
			OutgoingOverflow::DropRefused);
	for (std::size_t m = 0; m < node.queued.size(); ++m) {
		const ProtocolMessage &message = node.queued[m];
		if (message.capacity_exempt && message.full_tag == PROTOCOL_TAG_CONNECTION_DESCRIPTION) {
			DisconnectEvent description;
			if (parse_disconnect_event(message.payload.data(), message.payload.size(), description))
				latch_disconnect_event(c, description);
		}
		if (plan.overflow && m == plan.overflow_message) {
			latch_disconnect_event(c, make_disconnect_event(1, 4, plan.overflow_count,
					static_cast<uint32_t>(c.seq.outbound_message_limit), "", 0, "NP.C:MSGCRE"));
			c.pending_disconnect = true;
		}
	}
	for (std::size_t p = 0; p < plan.packets.size(); ++p) {
		std::vector<uint8_t> datagram;
		if (!c.server_scrk.empty()) datagram = frame_session_replies(c, plan.packets[p]);
		if (datagram.empty()) {
			std::vector<ProtocolMessage> rest;
			for (std::size_t q = p; q < plan.packets.size(); ++q)
				for (ProtocolMessage &record : plan.packets[q]) rest.push_back(std::move(record));
			for (ProtocolMessage &record : plan.unbuilt) rest.push_back(std::move(record));
			node.queued = std::move(rest);
			return released;
		}
		socket.send_to(c.peer, datagram.data(), datagram.size());
		c.last_session_send_ms = now_ms;
	}
	node.queued = std::move(plan.unbuilt);
	return released;
}

// One header-only packet carrying the node's current ACK (BuildOutgoingPackets with
// has_pending_out set and nothing queued @0x62847E).
void send_unstopped_header_only(NapiNPConnection &c, opennova::IDatagramSocket &socket,
		uint32_t now_ms) {
	if (c.server_scrk.empty()) return;
	const std::vector<uint8_t> packet = frame_session_replies(c, {});
	if (packet.empty()) return;
	socket.send_to(c.peer, packet.data(), packet.size());
	c.last_session_send_ms = now_ms;
}

} // namespace

// PumpFlags(-1) over each held node: the type test passes a type-1 node, a node already
// pending skips every leg; 0x10 steps the holdoff; 0x20 builds at a zero holdoff when the node
// holds queued messages or owes an ACK, and prunes; 0x40 runs the state-1 arm, the send
// intervals (host_session.cpp pump_send_interval_legs' two legs) and then the receive reap,
// which leaves the node pending and ends its legs; 0x80 steps the flush counter, 0x100 prunes
// and 0x200 reloads a zero holdoff. Right behind each node's legs, the protocol pump's pending
// test: a pending node takes the PDESTME record only where none is latched, the destroy's
// teardown, and leaves the list before the next node's legs run (V-ROLL-H on L1-s19 A2).
// [orig: NapiNPProtocol_Pump(-1) from CNapiGameSession_InitTransportConnection @0x4CA14B,
//  CNapiNetwork_PumpTransportAndProtocol @0x4C6E3D and CNapiNetwork_PumpAndCheckState @0x4C6EA2;
//  CNapiNPConnection_PumpFlags @0x629780 - the type test @0x629788..0x62978F, the pending test
//  @0x629791, 0x10 @0x62979D..0x6297AC, 0x20 @0x6297B7, 0x40 @0x6297C8, 0x80 @0x6297D5, 0x100
//  @0x6297E6, 0x200 @0x6297F3..0x629802; CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - the
//  state 1 / 5 test @0x629279, the holdoff @0x62927B, `queued > 0 || has_pending_out`
//  @0x6292A9, BuildOutgoingPackets called @0x6292B4, PrunePacketQueue @0x6292BB;
//  CNapiNPConnection_PumpStateMachine @0x6292E0 state 1 - PumpSendIntervals called @0x62933F,
//  timeout_ms @0x629344 (negative: no reap @0x62934C), the source address +0x30 @0x629352
//  (NapiNPProtocol_HandleClientJoin's store @0x62BF28; 0 only for the in-process loopback), the
//  elapsed `now - +0x5E8` @0x62935C against it @0x629362, the record {1, 3, elapsed, timeout,
//  "", 0, "NP.C:PT:SERTMOUT"} @0x6293B7..0x6293E6, RequestDisconnect @0x62940A;
//  CNapiNPConnection_PumpSendIntervals @0x628FD0 - the active leg @0x628FF1..0x629017, the
//  empty leg @0x629041..0x629067, the build @0x62907B, the prune @0x629082, the stamp
//  @0x629089; NapiNPProtocol_Pump @0x62A650 - the walk's PumpFlags call @0x62A6F8, then that
//  node's pending test @0x62A6FD, the latch only where none is @0x62A770, the destroy @0x62A793]
uint32_t pump_unstopped_server(UnstoppedServer &server, const ServerLogs &logs,
		opennova::IDatagramSocket &socket, uint32_t now_ms) {
	uint32_t link_errors = 0;
	if (!server.running) return link_errors;
	// Flag 8's receive first. Its server-opcode pass, in arrival order: each queued packet's
	// handler again, its last argument 1, now needing the node active (conn_flag0): a 0x43 takes
	// the in-order leg (a future one waits on the node's ordered queue and latches the
	// missing-sequence check; a stale one is dropped), a 0x46 latches the client's record and
	// requests the node's disconnect. A packet whose node a destroy took finds none.
	// [orig: NapiNPProtocol_Pump @0x62A650 - PumpRecvQueues called @0x62A6AC;
	//  NapiNPProtocol_PumpRecvQueues @0x6266A0 - mask bit 0 @0x6266C5, the +0x22C queue read
	//  @0x6266FB, the handler called with 1 @0x6267AD; NapiNPProtocol_HandleSessionPacket @0x626A00
	//  - the in-order leg @0x626BEB, the future leg @0x626C0C..0x626C3A; Nwu_HandleDisconnect
	//  @0x623CE0 - the latch @0x6240A8..0x6240BA, RequestDisconnect called @0x6240C4]
	std::vector<UnstoppedPacket> queued = std::move(server.queue);
	server.queue.clear();
	for (UnstoppedPacket &packet : queued) {
		// A queued 0x42 takes the join handler's queued pass (join_unstopped_server).
		if (packet.opcode == SESSION_OPCODE_CLIENT_AUTH) {
			join_unstopped_server(server, logs, socket, packet.from, packet.body, now_ms);
			continue;
		}
		UnstoppedNode *node = find_unstopped_node(server, packet.from);
		if (node == nullptr || node->conn.phase < ConnectionPhase::Joined) continue;
		NapiNPConnection &conn = node->conn;
		if (packet.opcode == SESSION_OPCODE_CLIENT_GOODBYE) {
			if (latch_client_goodbye(conn, packet.body)) conn.pending_disconnect = true;
			continue;
		}
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> flat;
		SessionDeframeAdmission admission;
		if (deframe_session_packet(conn.seq,
				SessionCrypto{{}, conn.client_scrk, 0, conn.server_sk}, packet.body.data(),
				packet.body.size(), hdr, flat, &admission, /*drain_queued_successors=*/false))
			admit_unstopped_packet(conn, admission, now_ms);
	}
	// Then its connection walk, in list order: each node's queued successors whose gap closed,
	// then, latched with its ordered queue still nonempty, its missing-sequence request (the
	// 0x84) over `socket`; a request naming a sequence raises the incoming link flag.
	// [orig: NapiNPProtocol_PumpRecvQueues @0x6266A0 - the walk @0x626910..0x6269E8, the head
	//  test @0x626960, ParseMessages called @0x62696B, the latch @0x6269BB,
	//  CNapiNPConnection_SendMissingSeqList called @0x6269CE, cleared @0x6269D6]
	for (UnstoppedNode &node : server.nodes) {
		NapiNPConnection &conn = node.conn;
		SessionDeframeAdmission admission;
		while (take_ready_session_packet(conn.seq, admission))
			admit_unstopped_packet(conn, admission, now_ms);
		if (!conn.seq.missing_request_pending) continue;
		conn.seq.missing_request_pending = false;
		if (conn.seq.queued_inbound.empty()) continue;
		const std::vector<uint32_t> missing =
				build_session_missing_sequence_list(conn.seq, false);
		std::vector<uint8_t> missing_body;
		if (!encode_session_resend_list(conn.client_ck, missing, missing_body)) continue;
		const std::vector<uint8_t> request =
				nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(missing_body));
		socket.send_to(conn.peer, request.data(), request.size());
		if (std::any_of(missing.begin(), missing.end(),
				[](uint32_t sequence) { return sequence != 0; }))
			link_errors |= kNetQualityLinkErrorIncoming;
	}
	// One node's PumpFlags legs; the reap ends them.
	const auto pump_legs = [&](UnstoppedNode &node) {
		NapiNPConnection &c = node.conn;
		if (c.type != NapiNPConnection::kTypeServerSide || c.pending_disconnect) return;
		// The own pair's twin (address 0): its builds, keepalives and requests go to the
		// manager's loopback for the held own client, never the socket, and its reap reads the
		// address word 0 [orig: CNapiNPManager_SendTo @0x61EC20 - @0x61EC59, the FIFO's write
		//  called @0x61ECC6;
		//  CNapiNPConnection_PumpStateMachine - the address word +0x30 @0x629352].
		if (c.peer.ip == 0) return;
		// 0x10: the holdoff.
		if (c.s2c_send_holdoff_countdown > 0) --c.s2c_send_holdoff_countdown;
		c.s2c_send_boundary_open = c.s2c_send_holdoff_countdown == 0;
		// 0x20: the build.
		if (c.s2c_send_holdoff_countdown == 0 &&
				(!node.staged.empty() || !node.queued.empty() || c.session_ack_owed)) {
			const uint32_t sequence_before = c.seq.next_outbound_seq;
			const bool released = build_unstopped_node(node, socket, now_ms);
			if (c.session_ack_owed && !released && c.seq.next_outbound_seq == sequence_before)
				send_unstopped_header_only(c, socket, now_ms);
			prune_session_send_boundary(c.seq);
			c.session_ack_owed = false;
		}
		// 0x40: the state-1 arm, its send intervals first.
		if (!c.server_scrk.empty()) {
			if (c.last_session_send_ms == 0) {
				c.last_session_send_ms = now_ms;
			} else {
				const uint64_t since_ms = static_cast<uint32_t>(now_ms - c.last_session_send_ms);
				const int32_t active_ms = c.timeouts.active_send_interval_ms;
				const int32_t empty_ms = c.timeouts.idle_send_interval_ms;
				const bool retained = c.seq.retained_outbound_message_count > 0;
				const bool active_due =
						active_ms >= 0 && retained && since_ms > static_cast<uint64_t>(active_ms);
				const bool empty_due = !active_due && empty_ms >= 0 && !retained &&
						c.seq.queued_inbound.empty() && since_ms > static_cast<uint64_t>(empty_ms);
				if (active_due || empty_due) {
					const uint32_t sequence_before = c.seq.next_outbound_seq;
					const bool released = build_unstopped_node(node, socket, now_ms);
					if (!released && c.seq.next_outbound_seq == sequence_before)
						send_unstopped_header_only(c, socket, now_ms);
					prune_session_send_boundary(c.seq);
					c.last_session_send_ms = now_ms;
				}
			}
		}
		// Then the receive reap.
		const uint32_t silent_ms = now_ms - c.receive_stamp_ms;
		if (c.timeouts.timeout_ms >= 0 && c.peer.ip != 0 &&
				silent_ms > static_cast<uint32_t>(c.timeouts.timeout_ms)) {
			c.receive_inactive_ms = silent_ms;
			latch_disconnect_event(c, make_disconnect_event(1, 3, silent_ms,
					static_cast<uint32_t>(c.timeouts.timeout_ms), "", 0, "NP.C:PT:SERTMOUT"));
			c.pending_disconnect = true;
			return;
		}
		// 0x80, 0x100, 0x200.
		advance_session_send_flush_counter(c.seq);
		prune_session_send_boundary(c.seq);
		if (c.s2c_send_holdoff_countdown == 0 && c.s2c_send_holdoff_dictated)
			c.s2c_send_holdoff_countdown = c.s2c_send_holdoff_ticks;
		c.s2c_send_boundary_open = c.s2c_send_holdoff_countdown == 0;
	};
	// The protocol pump's walk: each node's legs, then at once, when the node is pending, its
	// destroy, before the next node's legs run, so a node reaped ahead of a node that sends in
	// the same pump bursts its 0x86 ahead of that node's packets.
	for (auto it = server.nodes.begin(); it != server.nodes.end();) {
		pump_legs(*it);
		if (!it->conn.pending_disconnect) {
			++it;
			continue;
		}
		latch_disconnect_event(it->conn,
				make_disconnect_event(1, 8, 0, 0, "", 0, "NP.C:PMP:PDESTME"));
		teardown_unstopped_node(it->conn, logs, &socket);
		it = server.nodes.erase(it);
	}
	return link_errors;
}

} // namespace opennova::inmatch
