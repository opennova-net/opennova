# NovaWorld networking - protocol + struct RE record

> **Status**: the NovaWorld stack (libs/novacrypto, libs/napi, libs/npwire, libs/novaworld, the
> standalone server app, and the web portal) is **landed on master**; this protocol RE is the
> durable wire record. Navigation: §5 = tag-level findings (index at its top; discovery-order
> numbering, never renumbered), §7 = landed architecture + the per-system grill waves, §8 = the
> D-NET divergence catalog (stable IDs). Per-system parity verdicts roll up in
> [correspondence.md](../correspondence.md).

Consolidated 2026-06-10 from `notes/novaworld_protocol_matrix.md`, `notes/dispatcher_table.md`,
`notes/dispatcher_findings.md`, `notes/spawn_gate_24C1928.md`, `notes/tag_cross_capture_diff.md`,
`notes/struct_typing_2026-04-26.md`, `notes/struct_refine_NapiGameSettings.md`,
`notes/struct_refine_NapiNPProtocol_pad500.md`, `notes/struct_refine_NapiPingManager.md`,
`notes/g_napi_np_ctx_layout.md`, and `notes/architecture.md`.

Binary: `Jointops.exe` (retail JO: Combined Arms, V1.7.5.7), imagebase `0x400000`. All
addresses are absolute in that image unless a section states otherwise. Retail captures
referenced: capture3 (dvxi5, AS Dormant Volcano Isle), capture4 (dvxc1, TD Tenaga Delta),
capture7 (spawn-flow session), retail_capture2 (g11, AS Kendari Airport).

---

## 1. Protocol layering

NovaLogic multiplayer UDP is a layered protocol with a universal wire format and pluggable
per-game message sets:

| Layer | Contents | Reimpl home |
|---|---|---|
| 4 — message sets | Selected by the `PN` field at CLIENT_HELLO. `PN=NOVAWORLDUDP` → container-based browser/session services (ClientHostRequest, ClientPlayRequest, ServerVerifyResult, ...; §3). `PN="JointOperations"` etc. → in-match TLV game traffic dispatched via the NAPI msginfo tables (§4). | `libs/novaworld` (service) / `libs/npruntime` (game session) |
| 3 — session + framing | Per-(ip,port) session state (`CK`, `SK`, `SCRK`, fragment buffers); the opcode `0x43`/`0x83` protocol-message envelope (flags/len/seq/frag). | `libs/napi` |
| 2 — NWU wire framing | 4-byte LSB CRC32 header; 1-byte opcode `0x41` HELLO / `0x42` JOIN / `0x43` SESSION / `0x46` GOODBYE (server replies `0x81`/`0x82`/`0x83`/`0x86`); NWU stream-cipher payload encryption. | `libs/novacrypto` + `libs/napi` |
| 1 — UDP sockets | Owned by the app, not a lib. | server app / Godot client |

*(The "Reimpl home" column records the pre-NET-2 homes; the in-game wire codec, NWU session framing, and capture/replay chain now live in `libs/npwire` — ADR 0019.)*

The **gate probe** (`novaworld_gate`, UDP 7597) is a separate, simpler protocol: plain
`GATEPROTOCOL` text encrypted with a static `"GATEAPI"` key, no NWU framing. It bootstraps the
client with the HTTP service URL and the address of the Layer-3+ NW UDP server.
`CNapiGateManager` (§6.8) hard-codes `gs.novaworld.net`, port 7597, probe tag `jop:cus2`
(retail JO; jodemo uses `jopd:cus4`).

**Container wire format** (Layer-4 NOVAWORLDUDP messages): marker bytes `0x01` root end,
`0x02` container start, `0x03` container end, `0x04` field start (NUL-terminated name + LE16
length + value + `0x00`), `0x05` field end. `libs/napi/tlv.h::NapiMessage` is byte-for-byte
equivalent to this container model; no separate abstraction is needed.

## 2. Legacy HTTP / web flow

The `NW*.dll` routes as implemented in the reverted stack, matching retail wire behavior:

| Route | Direction | Behavior | Data carried | Notes |
| --- | --- | --- | --- | --- |
| `/nwprepare.dll` | client GET | Issues `EPASK` and persistent login seed cookies. | `EPASK`, `PERSISTENTEXPRESSLOGINDATA`, client IP. | Retail uses `EPASK` to encrypt the login form. |
| `/NWStart.dll` | client GET | Renders the selected start/login template. | Template names, `EPASK`, `YOURIP`. | Lowercase route also accepted. |
| `/NWLogin.dll` POST | client POST | Authenticates EPASK `NAME`/`PASSWORD`; rejects maintenance, bad password, banned/restricted users, denied game access, duplicate active sessions. | `pfid`, `success`, `failure`, `relay`, `msgbase`; resolved `PCID`, `NWH`, `NWHANDLE`, `EXPBITS`. | `pfid=28` maps JO, `pfid=38` maps DFX2. |
| `/NWLogin.dll` GET | client relay GET | Completes login and sets identity cookies. | `NWH`, `NWHANDLE`, `CHAR`, `PCID`, `EXPBITS`, `LOGINSESSIONTAG`. | Relay tag may come from query string: retail HTTP/1.0 drops cookies. |
| `/NWLogout.dll` | client GET | Clears pending login session, active-user row, persistent-cookie pin. | `LOGINSESSIONTAG`, `PERSISTENTEXPRESSLOGINDATA`. | UDP state still tears down via GOODBYE or timeout. |
| `/NWHost.dll` | client GET relay | Issues `HOSTKEY` used by later UDP host registration. | `HOSTKEY`, `NWPF`, `NWPF2`, `PUB*` placeholders. | Host identity is finalized by UDP `ClientHostRequest`. |
| `/NWJoin.dll` | client GET relay | Resolves RID, expansion-gates the joiner, emits `.joi` tokens and PUB cookies. | `RID`, `NK`, `CK`, `BK`, `PUBPCID`, `PUBNAMEINFO`, `PUBSQUADINFO`, `PUBJOINTICKET`. | `NK` is encoded host `ip:port`; `CK` is encoded host app id; `PUBNAMEINFO` is `NWHANDLE\0`, and `PUBSQUADINFO` is `[u32 zero][display\0][short\0]` for the retail host join handler. |
| `/NWCharacter.dll`, `/NWAccount.dll` | client GET | Renders account/character templates from login cookies. | `NWHANDLE`, `PCID`, `NWH`. | Best-effort identity refresh. |
| `/*.gsb` | client GET | Builds encrypted GSB response from active hosts. | RID, host IP, 26 server-browser fields. | `jop_2.gsb` and `dfx2_0.gsb` routed. |

### GSB server row (positional, 26 fields)

Stream = flat `[magic:4][len:u32 LE][payload]` chunks, each payload encrypted independently
under the 22-digit GSB key. Tags: `GSB ` = init/**reset** (honored only when payload dword0
== `0x00010000`, else the record is skipped), `FLDS` = field-name table (replaces), `SVRS` =
server rows (**accumulate** across records — a multi-SVRS stream yields the union), `XXXX` =
finalize. Undersized records (FLDS/SVRS payload < 2, `GSB ` payload < 4) are skipped, never
errors. Row = `[u32 rid][4-byte host IPv4, in_addr order]`, then one NUL-terminated ASCII
value per FLDS name, then `[u16 playerCount][playerCount × name]`. `rid` is the `@RID@` join
substitution in the markup NWJoin URL, printed `%d` [orig: CLanServerBrowser_UpdateServerList_0
@ 0x660200, sprintf @ 0x660386]; the IPv4 is the ping target the browser formats from entry+4
on the XXXX finalize [orig: NapiGameList_StartPingSweep @ 0x63bcf0]. Full semantics:
D-NET-32..36 + D-NET-190..193, §7 Wave 9.

Field names:
`ServerName`, `GameType`, `MissionName`, `Region`, `Players`, `MaxPlayers`, `Dedicated`,
`TimeLeft`, `Password`, `Country`, `Msg`, `Age`, `TimeOfDay`, `Stat`, `LevelRange`, `Locked`,
`Tracers`, `Skins`, `BBMode`, `Mod`, `PIX`, `PBSERVER`, `VER1`, `Exp`, `Expbits`, `Joicon2`.

Host-var aliases observed from clients:

| Stored field | Client var aliases |
| --- | --- |
| `GameType` | `GameType`, `GameTypeName`, `GameMode` |
| `MissionName` | `MissionName`, `Mission`, `MapName` |
| `Country` | `Country`, `CountryCode` |
| `Password` | `Password`, `Passworded`, `RequiresPassword` |
| `Locked` | `Locked`, `Private` |
| `Dedicated` | `Dedicated` |
| `Stat` | `Stat`, `Stats`, `StatsEnabled` |
| `Exp` | `Exp`, `EXP`, `Expansion` |
| `Expbits` | `Expbits`, `ExpBits`, `EXPBITS` |
| `VER1` | `VER1`, `Ver1`, `Version1` |
| `Joicon2` | `Joicon2`, `JOICON2` |

Open RE items: exact retail rejection message text, complete status-code semantics beyond the
known NWEC mappings, additional host-var aliases retail emits for specific expansions.

## 3. NOVAWORLDUDP session flow

### Session opcodes

| Opcode | Name | Direction | Behavior | Key data |
| --- | --- | --- | --- | --- |
| `0x41` | ClientHello | C→S | Accepts supported PN values (`NOVAWORLDUDP` lobby, `JointOperations`/`JOINTOPERATIONS` game), replies ServerHello. | `CI`, `PN`, client address. |
| `0x42` | ClientAuth | C→S | Promotes addr-keyed session; stores client SCRK and server SCRK/SK; replies ServerAuth. | `CI`, `CK`, `SCRK`, `NA`, generated `SK`, `NWUID`. |
| `0x43` | ProtocolMessage | both | Decodes encrypted session packet, ACKs every valid packet, dispatches browser/session containers, fragments/reassembles Layer-4 payloads. | Header `session_id`, `seq_num`, `ack_count`; one or more protocol messages. |
| `0x46` | ClientGoodBye | C→S | Drops connection/session state and active host/player rows. | Client connection id if present. |

### Session containers (browser/session services)

| Container | Direction | Reply | Semantic data |
| --- | --- | --- | --- |
| `ClientConnected` | C→S | `ServerStartVerify` | Starts session verification. |
| `ClientRequestVerifyResult` | C→S | `ServerVerifyResult` | `Success=1` + stable `SessIdString`; maintenance mode → `Success=0`, `MsgCode=3000`. |
| `ClientHostRequest` | C→S | `ServerHostResult` | Registers active host; returns `RID`, `GSID`, `HostRequiresJoinTicket=0`. |
| `ClientHostUpdate` | C→S | ACK only | Refreshes host address, player counts, keys, GSB fields. |
| `ClientPlayRequest` | C→S | `ServerPlayResult` | Stores play setup, acknowledges selected `GSID`. |
| `ClientPlayerEnterRequest` | C→S | `ServerPlayerEnterResult` | Confirms player entry; normalizes `ConnectionId` → `ConnectionID`. |
| `ClientHostPlayerAdded` | C→S | ACK only | Increments host player count until next host update. |
| `ClientHostPlayerRemoved` | C→S | ACK only | Decrements host player count. |
| `ClientStopHosting` | C→S | ACK only | Clears hosting state, removes active host row. |
| `ClientStopPlaying` | C→S | ACK only | Clears play state. |

### Key wire fields

| Field | Meaning | Source |
| --- | --- | --- |
| `CI` | Client connection id. Retail often sends `1` per process, so runtime state is keyed by address. | ClientHello. |
| `CK` | Client local key; server uses it as outbound session header `session_id`. | ClientAuth, join `CK` token. |
| `SK` | Server local key; client echoes it as inbound session header `session_id`. | ServerAuth. |
| `SCRK` | String cipher key for encrypted protocol-message bodies. | ClientAuth/ServerAuth. |
| `NWH` | NovaWorld account handle id/counter. | HTTP login cookies. |
| `NWHANDLE` / `CHAR` | Display handle (retail UI + join identity lookup). | HTTP login cookies. |
| `PCID` | Player account id used by PUBcrypto join cookies. | HTTP login cookies. |
| `EXPBITS` | Expansion ownership/access bitmask. | player game-access, GSB `Expbits`. |
| `RID` | Browser-visible hosted-server row id. | `ClientHostRequest`, GSB, `/NWJoin.dll`. |
| `GSID` | Game session id returned to the host. | `ClientHostRequest`. |
| `HOSTKEY` | HTTP-issued host key copied into UDP host registration. | `/NWHost.dll`. |
| `PCIDKey` | Host PUBcrypto key for join cookies. | `ClientHostUpdate`. |
| `NK` | Encoded host network endpoint token. | `/NWJoin.dll` response. |
| `BK` | Static join token value (onnet + retail captures). | `/NWJoin.dll` response. |

### Host-registration statement shapes (Jointops.exe, retail)

`ClientHostRequest` and `ClientHostUpdate` are NAPI statements built the same way the
play-request is (a `CurrentlyXxx` param then `NapiStatement_SerializeVarList`-emitted
`ClientVarList`s — each a `VarList`-named container of `ClientVar{VarFNum,VarName,VarValue}`).
The earlier OpenNova builders modeled `HostSetup`/`Host`/`PlayerList` as containers named
literally, which `extract_var_lists` (keyed on `ClientVarList` + the `VarList` field) could not
parse — the same bug the play-request builder already had corrected.

| statement | params (direct fields) | var-lists, in order | orig |
|---|---|---|---|
| `ClientHostRequest` | `CurrentlyHosting` (decimal), `VarCheck`="1" | `Cookie`, `HostSetup`, `Host`, `PlayerList` | `CNapiGameSession_SendHostRequest @ 0x4d3700` — `NapiStatementParam_Create` ×2 then `NapiStatement_SerializeVarList @ 0x4d0660` ×4 from `this+388/+460/+532/+604` |
| `ClientHostUpdate` | (none) | `Host`, `PlayerList` | `CNapiGameSession_SendHostUpdate @ 0x4d3860` — two `NapiStatement_SerializeVarList` from `this+532/+604` |

The gate reads `HostSetup{AppId, LobbyName, MaxPlayers, ServerPortNumber?}` and
`Host{ServerIP, ServerPortNumber, ServerName, Players, Region/Country, + GSB fields}`
(`handle_client_host_request`); `ClientHostUpdate` refreshes the same `Host` keys plus
`HostKey`/`PCIDKey`. Ported in `libs/napi/session.cpp` (`make_client_host_request` /
`make_client_host_update`) and sent over a verified session via
`ClientSession::build_lobby_message` (host direction of ADR 0010); round-tripped against the
gate parser in `tests/novaworld/client_session_loopback_test.cpp` (steps 8–9).

## 4. NAPI in-match dispatch

### Naming convention (load-bearing)

The `NapiNPClientMsg_0xNNN` / `NapiNPServerMsg_0xNNN` name suffix equals the **wire `msg_id`
field**, not the handler-table index. `NapiNPMsgInfo` (16 B) is
`{u32 msg_id, u32 magic, handler, handler2}`. At init, `[orig: NapiNPProtocol_FindMsgInfo @
0x61E380]` builds an inverted index `msginfo_*_index[msg_id] → NapiNPMsgInfo*`; the wire
`msg_type` byte indexes that array directly. The flat tables are **not** sorted by msg_id
(handler-table-index order; e.g. msg_id 0x05 sits at flat index 4, before 0x04 at index 5;
0x24 sits near the table tail). Verified by spot-checking 10+ entries.

### Opcode dispatcher — `[orig: NapiNPProtocol_DispatchOpcode @ 0x622B40]`

Reads the first byte of an inbound packet as opcode and linear-searches `g_np_opcode_handlers`
(`NapiNPOpcodeInfo`, 16 B: `{u32 index, u32 opcode, u32 magic, handler}`; magic always
`0x7C08C6`). Opcode space `0x41`-`0x47` (C→S) and `0x81`-`0x87` (S→C); 14 entries + sentinel
(`index=0xFFFFFFFF` @ `0x849E40`). Table at `0x849D90`:

| Index | Opcode | Handler | IDA name / role |
|---|---|---|---|
| 0 | 0x41 'A' | 0x6213B0 | `NapiNPProtocol_HandleClientHello` (TLV ClientHello) |
| 1 | 0x42 'B' | 0x62B750 | `NapiNPProtocol_HandleClientJoin` (TLV ClientAuth/Join) |
| 2 | 0x43 'C' | 0x626CF0 | `Nwu_HandleClientSession` — thunks to `NapiNPProtocol_HandleSessionPacket(..., has_seq=1)` → ParseMessages. **The ProtocolMessage path.** |
| 3 | 0x44 'D' | 0x6241F0 | resend list / NACK (name pending) |
| 4 | 0x45 'E' | 0x624220 | resend list / NACK (name pending) |
| 5 | 0x46 'F' | 0x624250 | `Nwu_HandleClientGoodbye` (thunks `Nwu_HandleDisconnect`) |
| 6 | 0x47 'G' | 0x624280 | pending decompile (possibly NAT probe / session recovery) |
| 7 | 0x81 | 0x626D20 | `Nwu_HandleServerHello` (response to 0x41) |
| 8 | 0x82 | 0x629840 | `NapiNP_HandleServerJoinResponse` (response to 0x42) |
| 9 | 0x83 | 0x627F90 | server-direction ProtocolMessage mirror of 0x43 |
| 10-13 | 0x84-0x87 | 0x6242B0/0x6242E0/0x624310/0x624340 | server-direction mirrors of 0x44-0x47 (pending) |

`[orig: CNapiNPConnection_SendEncryptedPayload @ 0x61F5A0]` writes opcodes `0x47`/`0x87` — a
different category of encrypted packet, **not** the ProtocolMessage path.

### Message dispatcher — `[orig: CNapiNPConnection_DispatchMessage @ 0x622570]`

Handles fragment flags (bit `0x06` mid-fragment, `0x04` continuation, `0x02` end), reassembles
into a per-connection `NapiBuffer` at connection offset `+860`, then resolves the handler via
`FindMsgInfo(proto, dir_flag, msg_type)`. `dir_flag`: `0x01` = client-receive
(`msginfo_client_table`), `0x02` = server-receive (`msginfo_server_table`), `+0x04` when
`flags >> 7` (high-table control mode; selects the `msginfo_high_*` tables).

### Dispatcher table globals

| Global | Address | Typed as | Notes |
|---|---|---|---|
| `g_np_msginfo_client` | `0x82AE28` | `NapiNPMsgInfo[123]` | S2C dispatch, 122 entries + sentinel (counted from delta to next named global). |
| `g_np_msginfo_server` | `0x82B5D8` | `NapiNPMsgInfo[72]` | C2S dispatch, msg_ids 0x00..0x51 + sentinel (conservative count). |
| `g_np_msginfo_highbit` | `0x849E80` | `NapiNPMsgInfo[64]` | High-table control dispatch; four registered entries (`H:0x00..H:0x03`) plus sentinel witnessed. |

Tables terminate on a sentinel entry with `magic == 0`
(`[orig: NapiNPMsgInfo_BuildIndex @ 0x61e2b0]`); they are assigned to `NapiNPProtocol`'s
`msginfo_*_table` fields in `[orig: CNapiNetwork_Init @ 0x4CA4A0]`. Discrepancy on record: the
dispatcher byte-decode session placed the server table at `0x82B6D8`, the later typing pass at
`0x82B5D8` (= `0x82AE28` + 123×16, self-consistent and applied to the IDB). Treat the IDB
typing as authoritative; re-verify if exact counts ever matter.

### High-table control messages (NAPI control, not gameplay)

The protocol-message flag bit `0x80` is a **high-table selector**, not a low-table msg_id
modifier and not an in-game message namespace. Retail dispatches these packets through the
`msginfo_high_*` indexes populated by `[orig: NapiNPProtocol_InitMsgInfoIndex @ 0x61E400]`;
`[orig: NapiNPProtocol_FindMsgInfo @ 0x61E380]` picks the low or high index by direction plus
the `+0x04` high-table bit. `[orig: CNapiNPConnection_DispatchMessage @ 0x622570]` gets the
selector from the protocol-message flags/raw type high bit (`0x80`). If a high-table index has no
entry, retail does **not** fall through to the normal low msg_id callback.

Only four high-table entries are registered in the retail JO binary:

| High tag | Handler | Working name | Purpose |
|---|---|---|---|
| `H:0x00` | `0x621940` | `CNapiNPConnection_HandleCSConfigUpdate` | Runtime connection-settings sync; sparse update form of the `CS` entries sent in opcode `0x82`. |
| `H:0x01` | `0x6219F0` | `CNapiNPConnection_HandleNameTagUpdate` | Connection name/tag update, not a player display name. |
| `H:0x02` | `0x62A040` | `CNapiNPConnection_ProcessDataTransferControl` | Data-transfer side channel control. |
| `H:0x03` | `0x621AE0` | `CNapiNPConnection_HandleDescriptionPacket` | Connection description packet = the **DISCONNECT/PUNT** carrier (full tag `0x103`): a flat `DS`/`DC`/`DP1`/`DP2`/`DSTR`/`DPC`/`DDSTR` TLV run, terminal on receipt. Full field map + the three punt families §5.64 (decoded). |

`H:0x00` payload format:

```
u8  direction
u32 field_mask_le
for each set bit i in field_mask, low to high:
    u32 value_le_for_cs_field_i
```

The same 15 CS field indexes appear in opcode `0x82` SessionInit as repeated `CS` TLVs with
payload `[direction:u8][field_index:u8][value:u32le]` (`[orig:
CNapiNPConnection_SendSessionInit @ 0x620EF0]`; receiver `[orig:
NapiNP_HandleServerJoinResponse @ 0x629840]`). `H:0x00` is therefore the runtime sparse-update
form of the SessionInit CS block, not an unknown gameplay message. Observed captures line up with
the IDA callers:

| Caller | Mask | Field | Meaning |
|---|---|---|---|
| `[orig: NapiNPServer_HandleNewConnection @ 0x4C8040]` | `0x00002000` | 13 | `max_packet_bytes`; observed value `1300` (`0x514`). |
| `[orig: NapiNPServer_UpdateHoldoffTicks @ 0x4C5F40]` | `0x00000008` | 3 | `send_holdoff_ticks`; observed value `12`. |

Direction is peer-relative and mirrored. On receive, `[orig:
CNapiNPConnection_HandleCSConfigUpdate @ 0x621940]` applies `direction != 0` to local
`conn+0x17C` and `direction == 0` to local `conn+0x1B8`; the SessionInit receiver uses the same
pair after applying the `CS` TLVs. The send side (`[orig:
CNapiNPConnection_SendConfigUpdate @ 0x6286E0]`, gated by `[orig:
CNapiNPConnection_SendConfigUpdateIfEnabled @ 0x629730]`) flips the outbound direction byte so the
peer lands the update in its corresponding local array.

Terminology guard: in the NOVAWORLDUDP lobby/gate flow, `NA` values such as `jop:cus2` are
connection/game/gate tags. They are not `NWHANDLE`/`CHAR` player display names, and high-table
`H:0x01` should likewise be treated as connection tag/name state unless a future witness proves
otherwise.

### S2C message table (server emits, client handles) — `0x82AE28`

This is what a reimplemented server must **emit**. Semantics merged from the per-tag decompile
sweep; blank = not yet characterized.

| msg_id | Handler | IDA name (suffix = msg_id) | Semantics / wire notes |
|---|---|---|---|
| 0x00 | 0x42E0E0 | `NapiNPClientMsg_0x000` | tiny stub no-op |
| 0x01 | 0x425360 | `_0x001` | reads u32 sync state |
| 0x02 | 0x42E0F0 | `_HandleJoinResponse` | join position-ack + PADDING PROBE: reads [i32 posX][i32 posY][i32 paddingLen] (rest of the ~512-B body = ignored filler); client replies C2S 0x02 = position + paddingLen random bytes + resets send holdoff. Field map §5.55 (decoded) |
| 0x03 | 0x425390 | `_0x003` | u8 + 2×u16 sync tick |
| 0x04 | 0x425410 | `_SessionSlotConfig` | session slot config (24 B): [4×i32 skipped][u8 cfg][u8 teamMode][u8 maxPlayers → PlayerSlotTable_Reallocate][i32 skipped][u8]. Field map §5.53 (decoded) |
| 0x05 | 0x42E180 | `_HandleGameStart` | the actual GAME-START UI signal: reads u8 flag; if non-zero, client replies C2S 0x4E and resets game-state dwords; toggles `dword_A86C28`, queues UI notification, resets game timers |
| 0x06 | 0x432BC0 | `_HandleChatCommand` | server→client chat |
| 0x07 | 0x422730 | `_0x007` | per-frame keep-alive stub |
| 0x08 | 0x4281D0 | `_HandleSessionConfig` | SESSION CONFIG, fixed 51 B (the old "~2 KB snapshot" note was wrong): [10×i32 (f3=gameType→g_GameType)][7×u8][u32 bitflags, bits 13/15/16 latched]. Field map §5.54 (decoded) |
| 0x0A | 0x42FEC0 | `NapiNPClientMsg_0x00A` | **per-frame local-player + world-state update** (multiplexed player/timer/env/gametype + health + round-event loop); full field map §5.9. Defined 2026-06-16 (was undefined — data blob mis-marked at 0x430000) |
| 0x0B | 0x422660 | `_HandleBMSHeader` | copies the 616-byte BMS header into `g_BmsHeaderBlock @ 0xA761D0` (field map §5.4) — a JOINER's only mission-identity source; it never opens the `.bms` (§5.28 correction, D-NET-194) |
| 0x0C | 0x42E730 | `_0x00C` | pool-0 organic spawn batch (AI infantry + players); `[u16 count]` header + per-record FLAT layout (slotId-first, no flag-gated optionals) per the §5.23 field map; parses name inline (crash-safe on `0x14B9` where 0x0D is not, §5.6); team → entity+354 |
| 0x0D | 0x432C40 | `_0x00D` | pool-entity spawn batch; sets `dword_A82370=3`; `[u16 count]` header + per-entity record per the §5.11 field map (always: 2×u16 flags+slot, u16 type, cstr name, 3×i32 pos, u8 team byte → entity+354 (gate 0x10) + u8 bone byte → entity+290 always; D-NET-58; conditional fields gated by every flag bit 0x01-0x8000); AI-flagged item defs (`ItemDef[+84] & 0x100000`) require the `flags & 0x800` trailer = **`[u32][u32][cstring ai_name]`** (§5.6/§5.11) |
| 0x0F | 0x42E200 | `_0x00F` | **WORLD-STATE-LOAD** (no descriptive Kong name; any "game-start" label is misleading): i32 sessionTick + 3×i32 spawn pos, 3×i16 angles, u8 flags, **fixed 128-i32 score block**, then waypoint records (off-wire gametype gate) + team names; sets `dword_81474C=0` (load-bearing input/heartbeat gate); client replies with the C2S burst 0x22 0x23 0x28 0x29 0x2D 0x32; ~624 B. Full field map **§5.29** (decoded) |
| 0x10 | 0x433400 | `_0x010` | static entity batch (pool 2): u16 start_idx, u16 count, flag-driven per-entity records; 612-644 B in retail, every frame; **full field map §5.9** |
| 0x11 | 0x4226E0 | `_0x011` | one-line stub: `dword_A82358=1` (unblocks WaitForDisconnect); retail only ever ships it bundled last with 0x0B (§5.5) |
| 0x12 | 0x425EE0 | `_0x012` | |
| 0x13 | 0x42EB50 | `_EntityDeath` | entity death (2nd path, beside 0x26) `[u16 handle][i16 killerSource]` → Health=0 + death cb (§5.35) |
| 0x14 | 0x42F240 | `_ChatMessage` | CHAT broadcast [u8 senderSlot][u8 channel][cstr formatted] → Chat_DispatchToChannel; the fan-out of C2S 0x0D. Field map §5.52 (decoded) |
| 0x16 | 0x42FAE0 | `_0x016` | PLAYER-LIST — full layout verified §5.20 (controlled capture 2026-06-17) |
| 0x17 | 0x4226F0 | `_0x017` | |
| 0x18 | 0x433780 | `FullEntitySpawn` | reply to C2S 0x0F: destroy + FULL single-entity rebuild (itemDef/models/playerClass/minimap/anim registration). Absent from healthy sessions (self-heal, §5.46) — the early "does not fire" note meant nothing needed healing, not an inert path. Field map §5.46 (decoded) |
| 0x19 | 0x425E80 | `_0x019` | spawn-ack timestamp [u32] → `dword_A82360` — the requester-only reply to C2S 0x0A spawn-menu request (§5.52a; emitted @0x513260 via NetPacket_WriteTimestampB) |
| 0x1A | 0x425EB0 | `_0x01A` | sets `dword_A82364` (WaitForGameStart return-0 unlock) |
| 0x1B | 0x426080 | `_0x01B` | |
| 0x1C | 0x4227F0 | `_0x01C` | empty stub |
| 0x1D | 0x430840 | `_0x01D` | **spawn-success gate**: sets `dword_24C1928=1` before any payload parse when `is_authority==0` (§5.2) |
| 0x1E | 0x426270 | `_0x01E` (`NetPacket_HandleGameEvent`) | 8-byte game event; does not unblock movement directly |
| 0x1F | 0x427CB0 | `_0x01F` | |
| 0x20 | 0x425C00 | `_0x020` | bulk pool-3 entity sync; sets `dword_A82370=5`; `[u16 start_idx][u16 count]` header + per-entity record per the §5.12 field map (u16 type_id; `type_id==0` ⇒ empty-slot sentinel, no body; else u8 flags + 3×i32 pos always, then u32 movementVal/BAM-heading (f&1; D-NET-59), u32 orient (f&2), u16 ammo (f&4), u16 netHandle ALWAYS, u8 team (f&8), u16 weaponType (f&0x10), u8 score (f&0x20)); allocates pool-3 entries |
| 0x21 | 0x430B10 | `_HandleSpawnEffect` | |
| 0x22 | 0x42EC90 | `_0x022` | part of the 0x0F reply ecosystem |
| 0x23 | 0x4F81E0 | `_0x023` | part of the 0x0F reply ecosystem |
| 0x24 | 0x429E70 | `_0x024` | (sits near the flat-table tail) |
| 0x25 | 0x422800 | `_0x025` | game reset, empty payload. Client side: input reset, clears camera/HUD state dwords, `dword_24C1928=1` + companion `dword_24C195C=1`, increments round counter `dword_24C116C`. Server side: round counter only. Retail does **not** use S2C 0x25 in the spawn flow (§5.2) |
| 0x26 | 0x42EC30 | `_0x026` | entity kill-sync (killer/victim slots); fires on kill events |
| 0x27 | 0x425AA0 | `_0x027` | |
| 0x28 | 0x425B40 | `_0x028` | |
| 0x29 | 0x427D00 | `_CharMinimapUpdate` | per-entity character/minimap update: [u8 pool0Idx][u8 team→+354][u8 flags7→+692][u16 packedCharId→NetId+0x15C] + CharacterEntity rebind (§5.59); renamed from `handle_entity_minimap_update` |
| 0x2A | 0x425BA0 | `_0x02A` | chat-history entry `[i32][i32][i16]` (10 B) → Chat_AddToHistory (§5.35) |
| 0x2B | 0x427DF0 | `_0x02B` | |
| 0x2C | 0x427E10 | `_MissionMapNames` | session + mission-file names (NOT chat — that note was wrong): [cstr sessionName → byte_A82378][cstr bmsFile → g_map_file_name]; bumps g_loading_progress ≥ 1. Field map §5.51 (decoded) |
| 0x2D | 0x427E90 | `_0x02D` | |
| 0x2E | 0x427F80 | `_0x02E` | |
| 0x2F | 0x430E10 | `_0x02F` | |
| 0x30 | 0x431170 | `_HandleChecksumRequest` | entity-checksum request `[u8 entityId][u16 checksum]` → reply **C2S 0x20** (NOT 0x21; §5.35), 5 B `[u8 id][u32 challenge ^ source]`; reply builder + the literal-42 arm §5.65 (shape witnessed; we send NOTHING — a guessed CRC is punted, D-NET-181) |
| 0x31 | 0x4311E0 | `_0x031` | ammo-definition CRC request `[u8 ammoIndex][u16 xorKey]` (3 B) → reply **C2S 0x21**, 9 B `[u8 index][u32 crc][u32 echoed key]`; field map + reply builder §5.65 (decoded; we send NOTHING — a guessed CRC is punted, D-NET-181) |
| 0x32 | 0x428060 | `_0x032` | |
| 0x33 | 0x425FA0 | `_0x033` | |
| 0x34 | 0x4283A0 | `_PlaySoundByName` | PLAY-SOUND [u8 flag][cstr profileName][flag==1: 3×i16 pos <<16]; flag 0 = flat, 1 = positioned 3D full-volume (kong name "GotoTeleport" was a misnomer, renamed). Field map §5.50 (decoded) |
| 0x35 | 0x4261A0 | `_0x035` | |
| 0x36 | 0x426120 | `_0x036` | |
| 0x37 | 0x431250 | `_0x037` | |
| 0x38 | 0x4260B0 | `_0x038` | |
| 0x39 | 0x42E6D0 | `_0x039` | anti-cheat charattr CHARACTER-row CRC challenge `[u32 seed]` → C2S 0x1C (§5.34) |
| 0x3A | 0x422680 | `_0x03A` | |
| 0x3B | 0x431340 | `_0x03B` | |
| 0x3D | 0x422870 | `_0x03D` | |
| 0x3E | 0x4226D0 | `_0x03E` | ack-style |
| 0x3F | 0x42BB20 | `_0x03F` | |
| 0x40 | 0x425A50 | `_0x040` | minimap-overlay update / capture-zone state — `[u8 count][N×6B entry]`, full map §5.19 (controlled capture 2026-06-17) |
| 0x41 | 0x4254C0 | `_0x041` | clears one charattr property across all 16 rows: first byte (missing→0), NINE mapped ids 0=+40 ATTRIBUTES / 2=+8 STEALTH / 3=+12 HPBONUS / 4=+16 MANABONUS / 5=+20 RECOIL_MUTE / 6=+28 XHAIR_MUTE / 7=+36 SCOPE_MUTE / 8=+32 XHAIRDX_MUTE / 9=+24 RELOAD_MUTE (id 1 and ≥10 are the only no-ops); trailing bytes ignored (§5.34) |
| 0x42 | 0x4281A0 | `_0x042` | input/state-flags `[u16]` → Input_UnpackStateFlags (§5.35) |
| 0x43 | 0x42FA90 | `_0x043` | time-sync ping `[u32 serverTs]` → C2S 0x08 (§5.34) |
| 0x44 | 0x422710 | `_0x044` | entity-routed sub-packet: `[u16][i16 netId][u8 subtype]` + class body → entity def+356 callback (§5.36; body partial) |
| 0x45 | 0x422890 | `_0x045` | terrain-tile load batch (§5.37, D-NET-83 — NOT "empty payload"); clamps `g_loading_progress`→6; forwards the body to `PolyTrn_LoadTileData()` (paged tile-array load) |
| 0x46 | 0x431370 | `_0x046` | PLAYER-SYNC — full layout + field read order verified §5.21 (controlled capture 2026-06-17) |
| 0x48 | 0x4284B0 | `_0x048` | |
| 0x49 | 0x42C0A0 | `_0x049` | weapon-reload `[u16 handle][u16 reloadParam]` → WeaponSlot_ReloadAmmo (IDB name `handle_camera_sync_packet_0x049` is wrong; §5.35) |
| 0x4C | 0x428570 | `_0x04C` | target-assignment list (squad/AI orders) |
| 0x4D | 0x4317B0 | `_HandleSpawnSlot` | reads u8 slot; local slot → tip event; otherwise client replies C2S 0x22+0x23; reads `dword_24C1928` as a skip-tip-if-already-spawned guard (never writes it) |
| 0x4E | 0x431870 | `_HandleBatchKill` (Kong: `_HandleBatchSpawn`) | u16 count + per-slot u16; calls `Entity_KillBySlotId` (kill, not spawn), then replies C2S 0x28 |
| 0x4F | 0x4286C0 | `_0x04F` | |
| 0x50 | 0x431910 | `_TeamAssign` | **TEAM ASSIGN — decoded + PORTED 2026-07-25** (`decode_team_assign`). Body (6 B): `[u16 entityHandle][u8 team][u16 netId][u8 animSlot]`; a SHORT body defaults each remaining field to 0. The trailing two fields are the target's IDENTITY, not squad state — the former `spawnPointId`/`squadLeader` names were guesses and are RETIRED 2026-07-25: the producer writes `entity+0x15C` (the packed character / minimap id) and `entity+0x374` (the character selector), and ZEROES both for a non-player behind the `Flags & 0x100` gate `@0x506b3d` (live arms `@0x506b51`/`@0x506b6b`) [orig: `write_entity_handle_packet @0x506ad0`]; the consumer stores them straight back (`@0x431b46` netId, `@0x431b3a` animSlot). Gates: `handle != 0xFFFF`, `(handle & 0xF000) < 0x5000`, `slot < pool capacity`. Legs, in order: (1) entity == local player → `byte_A85B48 = team` @0x4319db — **the SAME latch the S2C `0x04` tail byte writes**, so this message is the SECOND of the latch's three writers (D-NET-168); (2) if NOT authority → `entity->Team = team` @0x4319ee, for ANY pool 0..4 entity; (3) the player-slot team byte mirrors it (slot+14, @0x431a0b); (4) if `entity->Flags & 0x100` (a player) AND entity == local player: retail re-selects the per-side profile (team 1/3 → side A block, else side B), refreshes `restrictionData`, and **RE-SENDS ONE C2S `0x2F`** via `NetPacket_SendLoadoutSubmit` @0x431a9e with the NEW team, the per-side profile class, and slot **195 raw** (the pre-`Player_InitPlayer` form — NOT the live `g_currentWeaponSlot`), then C2S `0x22`/`0x23` acks (@0x431acb..0x431b05), `Player_InitPlayer(1)` @0x431b14 and the netId/animSlot identity restore (@0x431b3a..0x431b91). PORTED: legs 1, 2 and the leg-4 `0x2F` re-submission (slot 195 raw). DEFERRED with witnesses (D-NET-168): the leg-4 per-side profile CLASS reselect @0x431a35..0x431a9a (we hold ONE applied kit), the `0x22`/`0x23` acks, `Player_InitPlayer`, the identity restore, and leg 3 (we keep no client-side player-slot team byte). Producer: also the ZONE-FLIP broadcast — `Server_ChangeEntityTeam @0x518D70` (ex-`Server_ChangePlayerTeam`; retargets ANY entity incl. capture zones/spawn objects, §5.61) |
| 0x51 | 0x431BB0 | `_HandlePlayerSpawn` | TEAM-CHANGE confirm — FIELD-PARSED (8 B): [u16 ackSeed][u16 handle][u8 team→+354][u16 packedCharId→NetId @0x431cad][u8→+884]; acks C2S 0x29 (ackSeed+1 @0x431c99) + REBINDS CharacterEntity @0x431cf3 (§5.59, D-NET-148); retail sends it only for pending team changes |
| 0x52 | 0x428A80 | `_0x052` | |
| 0x53 | 0x428AE0 | `_ZoneTimerWindow` | ZONE-TIMER WINDOW (9 B): [u16 zoneHandle][u8 curTeam][u8 capturingTeam→entity+547][u16 progress][u16 limit][u8 rate], ×62 s→ticks; the timed-capture channel — server emits from `Server_UpdateCaptureZones @0x53B8F0` ×4 (`NetPacket_WriteZoneTimerWindow @0x506D00`). Client map §5.49, producer §5.61 |
| 0x54 | 0x429040 | `_0x054` | death/wounded minimap marker `[u16 entityHandle][u8 state]` — server emits from `GameEvent_PlayerDeath @0x516dd0` ×2, `GameEvent_RevivePlayer @0x517db4`, `Server_BroadcastMedicRequest @0x515390` (D-NET-108); handler body unwitnessed (§5.60) |
| 0x56 | 0x431D10 | `_0x056` | touches `dword_24C1928` (write unconfirmed; decomp on demand) |
| 0x57 | 0x432210 | `_0x057_RTT` | RTT ping/pong `[u32 ts][u8 echoFlag]` (§5.34); ⇄ C2S 0x2C |
| 0x58 | 0x4228C0 | `_SessionStatus` | SESSION-STATUS block (NOT a texture loader — kong `TerrainTexDef_ParseFromBuffer` renamed `SessionStatus_ParseFromBuffer @0x530ED0`): server/mission names + up-time sync + the 39 STROVER_STATVAR scoring rules + kv pairs → g_session_status (end-game stats/loading screen/admin UP-TIME). Field map §5.48 (decoded) |
| 0x59 | 0x4228E0 | `_0x059` | deployed-item / weapon-overlay spawn (32 B): item ids + owner + slot + parent + 3×i32 pos + 3×u16 ang (§5.36) |
| 0x5A | 0x4290E0 | `_0x05A` | weapon-loadout sync: `[u8 avatarClass]` + a `{typeId, ammoP, ammoS, ammoAlt}` slot chain to a `0xFF` terminator (typeId = AdmDef index); resets `dword_81474C=0`. Field map **§5.30** (decoded) |
| 0x5B | 0x4322B0 | `_0x05B` | |
| 0x5C | 0x425200 | `_0x05C` | |
| 0x5D | 0x429730 | `_DestroyEntityList` | **EMPTY-SLOT SWEEP — decoded + PORTED 2026-07-25** (`decode_destroy_entity_list`), gated `!is_authority`. Body = `[i16 pool0Index] × N` with NO count word — **RAW POOL-0 INDICES, not packed handles** (`Pool_GetEntryUnchecked(0, idx)`). Per entry: `Entity_Destroy`, then `PlayerSlot_FindByType(idx)` → if that slot is active (slot+13) `PlayerSlot_ClearAndUnlink @0x434730`. It is the REPLY to C2S `0x32`: the server answers with the pool-0 slots IT considers empty and the client destroys whatever it still holds there. So this is a sweep the client ASKS for, not a per-kill despawn — and it is the ONLY channel that retires an entity permanently (D-NET-176). The 0x46 `0x8000` bit is NOT the per-entity removal (see the D-NET-80 correction) |
| 0x5E | 0x4297B0 | `_0x05E` | |
| 0x5F | 0x4228F0 | `_0x05F` | |
| 0x60 | 0x432350 | `_HandleFileTransferChunk` | **chunked file transfer** (decoded §5.28): `[u32 transferId][u32 totalSize][u32 chunkOffset]` + raw file bytes → `CDataStream`; re-request **C2S 0x33** when incomplete. probe2 completed in one 163-B chunk (transfer content = the `SERVERNAME`/`MISSIONNAME` VarList), so 0x33 never fired — **D-NET-74 corrects the D-NET-69 "announce VarList / type=1,bodyLen,reserved" reading** (a single-chunk artifact). **probe3 forced multi-chunk** (a >200-B MOTD → total=239: chunk0 200 `[more]` + chunk1 39 `[FINAL]`), so **C2S 0x33 fired** — first multi-chunk witness (D-NET-75) |
| 0x61 | 0x4297C0 | `_HandleSessionKey` (MISNOMER — it is the per-player **TICK SEED**, not an SCRK/session-key exchange; grilled 2026-07-24) | `[u32 seed]` → stored into BOTH `currentTick @0xA8229C` (@0x4297f8) and `g_lastKeepaliveTick @0xA822A0` (@0x4297fd); a body shorter than 4 B seeds **0** (@0x4297eb). This is the ONLY source of a non-zero client tick — `Client_ProcessNetworkFrame @0x42c193` skips both the `++` and the 0x34 keepalive leg while it is zero — and the client stamps that tick at off-0 of every C2S `0x06`. Host side: `Server_SendRandomSeedToPlayer @0x5101a0` rolls `((rand() & 0xFE) + 1) << 16` (0x10000..0xFF0000) PER PLAYER (@0x5101d4), ships it as this message, and stamps the same value into that player's slot `+0x178D8` as the fire-freshness floor + arms `+0x178E0`; senders are join (@0x51a982), death (@0x516ef4 / @0x51796d), revive (@0x517e47), and the deploy release, with a four-zero-byte disarm form (@0x510237). The host then rejects any `0x06` failing `PlayerSlot_IsActive @0x4fc760` (`tick == 0 \|\| tick <= slot+0x178D8`) — the gate has exactly two xrefs in the image, both on the fire path, which is why an unseeded client's movement/stance/reload still work while its fire is silently discarded. **A client that never consumes this message can never land a shot on a stock host** (the live symptom fixed 2026-07-24) |
| 0x62 | 0x42D200 | `_0x062` | |
| 0x63 | 0x42D450 | `_0x063` | |
| 0x64 | 0x432410 | `_HandleMissionDataChunk` | **chunked file transfer** (decoded §5.28): same 12-B `[transferId][totalSize][chunkOffset]` header + raw file bytes → buffer (completion extracts 3×32-B mission-name strings); re-request **C2S 0x37** when incomplete. probe2 completed in one 180-B chunk (D-NET-74). **No compression codec** — raw file content (resolves the deferred "0x64 inner codec") |
| 0x65 | 0x429870 | `_0x065` | |
| 0x66 | 0x42D4C0 | `_HandleWeaponRestrictions` | count + (slot, restriction) pairs |
| 0x67 | 0x42D570 | `_0x067` | |
| 0x68 | 0x42DAA0 | `_0x068` | loaded-model snapshot page request `[u32 startIdx]` → C2S 0x3D (§5.34; “entity-index” is the retired provisional name) |
| 0x6A | 0x432510 | `_0x06A` | |
| 0x6B | 0x425520 | `_0x06B` | minimap overlay batch `[u8 count]`+count×12B (handle@+0; blip rebuilt from entity state, 10 trailing B unused) (§5.35) |
| 0x6C | 0x428FC0 | `_0x06C` | zone presence count (3 B): [u16 zoneHandle][u8 count 1..32] — players inside an active timed capture, emitted on change (`CaptureCtx_UpdateActiveCaptureRate @0x53B600` → `NetPacket_WriteZonePresenceCount @0x506DE0`). §5.61 |
| 0x6D | 0x430C50 | `_HandleEntityDeath` | |
| 0x6E | 0x429880 | `_0x06E` | spawn-wave / deploy-screen status (§5.31's "squad roster"): `[u8 groupCount]` + per group `{u16 zoneHandle, u16 zoneIdx, u8 queuedCount, u16 waveCountdown, u16 members[]}` — sent on wave-queue join + 1 Hz to dead/deploying players (`NetPacket_WriteSpawnWaveStatus @0x507490`). Client map **§5.31**, producer §5.61 |
| 0x6F | 0x428D60 | `_ZoneTimerValue` | ZONE-TIMER VALUE (15 B; NOT cinematic camera — that label was wrong): [u16 zoneHandle][u8 team][i32 control 16.16 (0..1.0)][i32 limit=0x10000][i16 delta][u8 friendlies→+544][u8 enemies→+545], ×62 into g_zone_timer_list; the SECURE channel — server emits every 1 Hz pass per numbered zone (`Server_UpdateCaptureZoneEntities @0x519690` → `NetPacket_WriteZoneTimerValue @0x506E70`). Client map §5.49, producer §5.61 |
| 0x70 | 0x429A30 | `_0x070` | |
| 0x71 | 0x425600 | `_0x071` | |
| 0x72 | 0x425710 | `_0x072` | |
| 0x73 | 0x425770 | `_0x073` | |
| 0x74 | 0x4258B0 | `_0x074` | |
| 0x75 | 0x4259E0 | `_0x075` | spectator-mode flags (2 B); sets `g_death_screen_active`, `dword_24D1DF4` |
| 0x76 | 0x42D540 | `_0x076` | u16 → `dword_24D59FC` |
| 0x78 | 0x425970 | `_0x078` | (address verified from the dispatch table @0x82B4F4 — the prior 7-digit `0x4259070` was a typo) |
| 0x79 | 0x429B00 | `_0x079` | spectator-mode flag (1 B → `dword_82BEE4`) (§5.35) |
| 0x7A | 0x429B40 | `_0x07A` | player name (max 64 chars) → server-info struct |
| 0x7B | 0x429BB0 | `_0x07B` | full player/session info: 5×cstring + u32 + 2×cstring. Field map **§5.32** (decoded) — roles witnessed from the landing globals + PunkBuster cvars (the Hex-Rays "clan/squad/rank" comment is wrong): name / **playerId** (NovaWorld account id, **not** a clan tag) / serverName / missionName / mapFile / … / gameName |
| 0x7C | 0x426020 | `_0x07C` | |
| 0x7D | 0x432690 | `_0x07D` | |
| 0x7E | 0x425E20 | `_0x07E` | two cstrings → `byte_A86520` / `byte_A86120` (server config strings) |
| 0x7F | 0x429E60 | `_0x07F` | |
| 0x80 | 0x42A070 | `_0x080` | |
| 0x81 | 0x42A0B0 | `_ScoreDeltaSound` | [i32 score] → dword_A82300; positive delta plays a tiered hit-confirm sound (thresholds word_24D5A10) |
| 0x82 | 0x42A0E0 | `_0x082` | |
| 0x83 | 0x4326E0 | `_0x083` | |

Note: a separate `0x42FEC0` S2C 0x0A handler exists; the previously cited
`NetPacket_SerializePlayerState @ 0x4C09C0` is a server-side serializer, not the client
handler for tag 0x0A.

### C2S message table (client emits, server handles) — `0x82B5D8`

This is what a reimplemented server must **handle**.

| msg_id | Handler | Notes |
|---|---|---|
| 0x00 | 0x512AA0 | **JOIN** — initial client→server packet (allocates session, returns session key) |
| 0x01 | 0x512ED0 | likely FORM_POST; also compares side passwords during early join (§6.4) |
| 0x02 | 0x512FD0 | likely GLB_JOIN |
| 0x03 | 0x501BE0 | `[i32]` → requester player entity+372 (0x174) |
| 0x04 | 0x5199D0 | |
| 0x06 | 0x513310 | `_0x006_ClientFiredRound` |
| 0x07 | 0x4FC970 | |
| 0x08 | 0x502210 | time-sync / anti-speedhack — `[u32 sessionId][u32 gameTimestamp]`; host checks the client's reported game-time deltas stay within 3% of wall-clock (`GetTickCount`). NOT movement. [orig: `validate_time_sync @ 0x502210`] |
| 0x09 | 0x513200 | client checksum response |
| 0x0A | 0x513260 | SPAWN-MENU REQUEST (len 0) — **THE world-stream unlock** (`NapiNPServerMsg_HandlePlayerSpawnRequest`): game state → 9, session+32 → 4, world-stream phase (+89882) RESET to 0, replies S2C 0x19 timestamp (mask 0x20). The §5.2a world stream never starts (and, retail-only, RE-runs on every later spawn-menu visit) without it — D-NET-150 |
| 0x0B | 0x51AB10 | |
| 0x0C | 0x501C30 | entity sub-packet: `[u16 handle][u16 itemTypeId][u8 sub_op][payload]` → per-type callback at `entity_def+356`; §5.9 |
| 0x0D | 0x513760 | CHAT MESSAGE uplink (`NapiNPServer_HandleChatMessage`; the old "replication frame ACK" note was WRONG): [u8 channel][cstr text]; strips `<...>` tags, 1000 ms rate limit, prepends name(/squad), fans out S2C 0x14 per recipient (2=team, 4/5=side, 11/12=squad, 13=proximity ≤100 u, default=all). §5.52 |
| 0x0E | 0x519AF0 | RESPAWN/DEPLOY request: [i16 spawnHandle] — the deploy-map pick. 0xFFFF = parameter-0/default pick; 0xFFFE = auto team spawn (frontier zone, gametype-keyed); real handle → `Server_ResolveSpawnTargetHandle @0x4FE110` (pools 0/1/2, def 0x40000, team gate) + zone control ≥ 1.0 + vehicle-seat gates; wave-queues via `g_spawn_wave_list` else deploys via `Server_ProcessPlayerDeath`. Full path §5.61 — `Server_ProcessClientRequestRespawn` |
| 0x06 | 0x513310 | client-fired-round — fixed 45 B (§5.16); anti-spoof + fire-rate gate, then `Server_ClientFiredRound @0x50baa0`: net primary fire runs the adm 'fire' action (slot-latched client pose) → local re-entry → `RoundData_AddRound @0x4fdb40` → the per-recipient §5.9.1 tag-2 round-event echo; ammo clip decremented (`consume_weapon_ammo @0x540850`), cooldown stamped (slot+96472 = tick + adm[276]). D-NET-152 |
| 0x0F | 0x514180 | player/entity-info request — `[u16 pool-0/1 handle]`; host serializes that entity's info + broadcasts it as S2C 0x18. The fallback spawn-menu "query loop" (pool-1 slots `0x10NN`) is this — NOT an input/movement frame (JO has no raw-input channel; see D-NET-68). [orig: `NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180`] |
| 0x13 | 0x514330 | `NapiNPServerMsg_HandleSectorAction` — pool-3 def-type-2044 sector actions (action byte + nearest-sector resolve; action 6 arms a 30-tick timer); NOT a death message — only S2C 0x13 is the death notify (§5.60) |
| 0x14 | 0x501E00 | |
| 0x16 | 0x511A70 | client→server chat |
| 0x17 | 0x514850 | |
| 0x18 | 0x51A020 | |
| 0x19 | 0x514250 | |
| 0x1A | 0x514B20 | |
| 0x1B | 0x501D90 | |
| 0x1C | 0x501D40 | charattr CHARACTER-row CRC reply (to S2C 0x39, §5.34) |
| 0x1D | 0x501C60 | STANCE CHANGE `[i16 stanceCode]` — the crouch/prone replication leg (witness 2026-07-03). Authority-gated; subject = the SENDER connection's player (conn+352 → +192 → entity). Codes are the stance-TRANSITION anim ids: 169 → `MoveOrder = (MoveOrder & ~0x300) \| 0x200` (crouch), 170 → `\| 0x100` (prone), 172 → `& ~0x300` (stand); subject == local player additionally re-latches `dword_B76484/dword_B76480`. Feeds the body-anim stance bases (§5.10 off-14) + the recipient's own 0x0A tail echo (§5.9). [orig: `NapiNPServerMsg_HandleStanceChange @ 0x501C60`] |
| 0x20 | 0x501F70 | entity-checksum reply (to S2C 0x30, §5.35) |
| 0x21 | 0x502050 | anti-cheat CRC reply (§5.17) — reads u8 player_index + u32 expected_crc; host XORs computed CRC against per-connection salt at `playerCtx+89924`, mismatch logs "ACRC" + sends "PUNT ACRC" |
| 0x22 | 0x514C90 | player-sync request `[u8 slot][u16 fieldFlags]` → host serializes & replies S2C 0x46 (§5.33); the client queues it on a 0x46 `0x4000`-ack and as a 0x0F/0x4D reply-burst member |
| 0x23 | 0x514D50 | visible-players request (empty body) → host replies S2C 0x4C snapshot (§5.33); 0x0F/0x4D reply-burst member |
| 0x24 | 0x514DC0 | |
| 0x25 | 0x514DF0 | weapon-reload request (mid-game) — note the direction asymmetry vs S2C 0x25 (§5.3) |
| 0x26 | 0x502390 | VEHICLE-ATTACH request — server overwrites wire word0 with the requester's OWN handle (anti-spoof) → Entity_ProcessVehicleAttach(vehicle/seat words) |
| 0x27 | 0x4FC980 | VEHICLE-DETACH request: [u16 handle] → Entity_DetachFromVehicle(entity, entity+364) |
| 0x28 | 0x51A550 | weapon-loadout request `[u32 loadoutFilter][u32 flags][u16 extra]` → host replies S2C 0x4E (§5.33); 0x0F reply-burst member |
| 0x29 | 0x514F10 | team/spawn ack `[u16 team_change_index]` (client 0x51-apply sends team+1 @0x431c99; also sent at deploy/team pick) → S2C 0x51 ONLY for a pending `g_team_change_entity_list @0xC947C8` entry via write_entity_packet @0x506bb0; a plain join-deploy 0x29 draws NO reply (§5.59, D-NET-148; the old "entity-packet request → always 0x51" reading was wrong) |
| 0x2B | 0x514FE0 | |
| 0x2C | 0x515070 | RTT ping/pong consumed `[u32 ts][u8 echoFlag]` (§5.34); ⇄ S2C 0x57 [HandlePingResponse, enforces min/max ping] |
| 0x2D | 0x502430 | burst-member receiver |
| 0x2E | 0x515390 | |
| 0x2F | 0x515790 | LOADOUT SUBMIT (spawn-menu accept): [u8 team 1..4][u8 class 5..9][u32 weaponSlotIdx] + [u8 admIdx][u8 ammoPri][u8 ammoSec][u8 variant]× until 0xFF; class → entity+660 playerClass (class-allow mask g_hostClassAllowMask @0x24D59FC, out-of-range → 8); replies S2C 0x5A. Field map §5.56; client builder NetPacket_SendLoadoutSubmit @0x42cdc0 (decoded) |
| 0x30 | 0x5029B0 | |
| 0x31 | 0x5024A0 | |
| 0x32 | 0x51A600 | **EMPTY-SLOT SWEEP REQUEST — decoded + PORTED 2026-07-25** (`NapiNPServerMsg_SendEmptySlots`; the old "burst-member receiver" label was a placeholder). The handler reads NO fields from the request: it is authority-gated, SKIPPED while `g_net_spawn_suspended` or `g_spawn_success_gate` (round over) is set, and otherwise builds a body via @0x5160f0 — walk pool 0, and for every EMPTY entry (`entry_dword[7] == 0`) append `u16 pool_index` — shipped as S2C `0x5D` to the REQUESTER ONLY (send_mask 32, target = requester slot). The only client sender is inside `NapiNPClientMsg_0x00F @0x42e647`, the world-state-load reply burst (§5.29, beside `0x28`/`0x29`/`0x2D`). Our joiner queues exactly this one leg of that burst; our host answers it with the witnessed builder (D-NET-176) |
| 0x33 | 0x515230 | next-chunk request for the S2C 0x60 file transfer — payload `[u32 transferId][u32 nextOffset]` (8 B). Fires only when a transfer spans >1 chunk; probe2's 0x60 fit in one chunk so it didn't fire — NOT because the semantic is unverified (D-NET-74 corrects D-NET-69) |
| 0x34 | 0x5024B0 | |
| 0x35 | 0x500DF0 | |
| 0x36 | 0x500E00 | |
| 0x37 | 0x5152E0 | next-chunk request for the S2C 0x64 file transfer — same `[transferId][nextOffset]` 8-B payload as 0x33. Didn't fire in probe2 because that transfer fit in one chunk, NOT because the protocol is re-request-free (D-NET-74) |
| 0x38 | 0x502510 | |
| 0x39 | 0x500E20 | |
| 0x3C | 0x519110 | |
| 0x3D | 0x500EC0 | loaded-model snapshot page reply (to S2C 0x68, §5.34; formerly mislabeled entity-index list) |
| 0x3E | 0x500E10 | |
| 0x3F | 0x518F10 | |
| 0x40 | 0x51C4C0 | VEHICLE-SPAWN request (`NapiNPServerMsg_HandleVehicleSpawnRequest`): [u16 sourceHandle][u8 typeIndex]; gates itemDef+2772 bit + team + availability; spawns at the source model's `boat`/`helo` userpoint (else z+2.0) and broadcasts S2C 0x18 (mask 0x90) for the spawned entity + every pool-1 entity sharing refNum(+0x215). §5.46 |
| 0x41 | 0x510540 | |
| 0x42 | 0x510930 | |
| 0x43 | 0x510990 | |
| 0x44 | 0x510AE0 | |
| 0x45 | 0x510C00 | |
| 0x46 | 0x510D20 | |
| 0x47 | 0x510ED0 | client requests host re-broadcast its entity state — host serializes via sub_510890 and emits **S2C 0x75** to all sessions with NapiNPServer_SendFiltered(filter=1, flag=0x20). Confirmed in loopback: C f=715 0x47→S f=716 0x75. |
| 0x48 | 0x510F30 | server-side no-op stub (handler body is empty). 4-byte payload observed in capture (`03 00 00 00`) is read off the wire and discarded. The `NapiNPClientMsg_0x048 @ 0x4284b0` exists on the client side for the inverse S2C 0x48 path, but no S2C 0x48 was observed in the 3-player capture. |
| 0x49 | 0x510F40 | |
| 0x4B | 0x510DC0 | |
| 0x4C | 0x5111B0 | client quality/state byte `[u8 value]` (host clamps 0..4, sets the player's connection-quality); field map §5.33 (decoded) |
| 0x4D | 0x518F70 | |
| 0x4E | 0x511210 | client reply when the S2C 0x05 flag byte is non-zero |
| 0x4F | 0x514A40 | |
| 0x50 | 0x5112B0 | |
| 0x51 | 0x51C840 | |

### Open questions

- **RESOLVED (D-NET-118)** — Magic `0x7C08C6` in every dispatch entry is the **address of
  `g_empty_str`** (the shared empty/default C-string, `[orig: g_empty_str @ 0x7C08C6]`, bytes
  `00 00…`). Every *valid* `NapiNPMsgInfo`/`NapiNPOpcodeInfo` entry carries `&g_empty_str` in the
  field; the terminator carries `0`. So it is a pointer initialized to the empty-string default,
  NOT a build/version stamp, and the dispatcher's `magic != 0` test is a "non-null = valid entry"
  check (plausibly a per-message label pointer that is empty for all shipped entries — struct
  field retype to `const char *` left as a low-priority follow-up, value/use unchanged).
- **CONFIRMED** — `handler2` of `NapiNPMsgInfo` is `0` across every entry of all three tables
  (client/server/high-bit); only the high-bit entry H:0x02 carries a non-null `handler2`
  (`nullsub_280 @ 0x61db90`, a no-op). Vestigial/unused for the gameplay tables.
- High-table payload depth beyond `H:0x00` — registered entries and routing are witnessed
  (§4 high-table control messages), but `H:0x01..H:0x03` still have only purpose-level names.
- **RESOLVED (D-NET-118)** — Opcode handlers `0x6213B0`..`0x624340` are all named: the full
  `g_np_opcode_handlers @ 0x849D90` table (14 legs + sentinel) is 0x41 `HandleClientHello` /
  0x42 `HandleClientJoin` / 0x43 `Nwu_HandleClientSession` / 0x44 `Nwu_HandleClientResendList` /
  0x45 `Nwu_HandleClientPing` / 0x46 `Nwu_HandleClientGoodbye` / 0x47 `Nwu_HandleClientProbe` /
  0x81 `Nwu_HandleServerHello` / 0x82 `NapiNP_HandleServerJoinResponse` / 0x83
  `Nwu_HandleServerSession` / 0x84 `Nwu_HandleServerResendList` / 0x85 `Nwu_HandleServerPing` /
  0x86 `Nwu_HandleServerGoodbye` / 0x87 `Nwu_HandleServerProbe`.
- **RESOLVED (D-NET-118)** — the earlier "msg_id `0x86`/`0x87`/`0x88+` in the client-table tail"
  was a conflation of *opcode* with *msg_id*: the S2C `g_np_msginfo_client` table tops out at
  msg_id `0x83` (123 entries + sentinel) and the C2S `g_np_msginfo_server` at `0x51` (72 + sentinel);
  `0x86`/`0x87` exist only as session *opcodes* (`Nwu_HandleServerGoodbye`/`Probe`, above), not
  message ids. No dead client-table entries.
- Phase B (full C2S decompile sweep): 0x47 / 0x48 / 0x06 / 0x21 / 0x0C-extended landed
  (§5.10, §5.16, §5.17, plus the 0x47/0x48 entries above) against the 3-player loopback
  pcap. The then-remaining candidates have since landed: 0x22 / 0x23 / 0x28 (§5.33),
  0x29 (§5.59 — a team/spawn ack, not a "burst member"), 0x0F (§5.46), 0x33 / 0x37 (§5.28).

## 5. Tag-level findings (audited against retail captures)

Sections are numbered in discovery order and never renumbered; lettered entries (5.0a,
5.2a…) are same-topic follow-ups, and unnumbered rows are cross-notes filed where they
were found. Divergence IDs referenced here are defined in the §8 catalog.

| § | Finding |
|---|---|
| 5.0 | Session bring-up — single player is an in-process listen server |
| 5.0a | Join-leg lifecycle fixes (grill 2026-06-26; D-NET-104/105/106) |
| 5.0b | The game-session 0x42 CU var set (the joiner's character/profile upload; D-NET-146) |
| 5.0c | Retail LAN enumeration and pre-load join lifecycle (2026-07-22) |
| 5.0d | Retail game-session admission and load boundary (golden LAN capture, 2026-07-23) |
| 5.1 | Loading-progress counter — `dword_A82370` |
| 5.2 | Spawn-success gate — `dword_24C1928` |
| 5.2a | Host-side spawn flow — how the listen-server host spawns its own player (R1, 2026-06-16) |
| 5.2b | Entity build + spawn-state init — the field-init sequence (2026-06-20) |
| 5.2c | Map spawn-marker selection — where the human player's pose comes from (2026-06-22) |
| 5.3 | Tag direction asymmetry (durable warning) |
| 5.4 | Tag 0x0B — 616-byte BMS header field map |
| 5.5 | Tag 0x11 bundling policy (retail) |
| 5.6 | Tag 0x0D — local-player spawn dead end (durable warning) |
| 5.7 | Tag 0x18 — superseded by §5.46 |
| 5.8 | Wire-format verification gaps |
| 5.9 | Core in-game replication loop — field maps (loopback capture 2026-06-16) |
| 5.10 | Per-entity-type serialize callback at `ItemDef+356` (loopback capture 2026-06-16b) |
| 5.10b | Per-entity-type callback at `ItemDef+356` — class dispatch table |
| 5.11 | Tag 0x0D — pool-entity spawn batch (loopback capture 2026-06-16b) |
| 5.12 | Tag 0x20 — bulk pool-3 entity sync (loopback capture 2026-06-16b) |
| 5.13 | Vehicle compact record (S2C 0x0A trailing event) |
| 5.14 | Infantry / AI compact record (S2C 0x0A trailing event) |
| 5.15 | Guided weapon record — per-(mode, field-group) codec |
| 5.16 | C2S 0x06 — client-fired-round (3-player loopback 2026-06-16d) |
| 5.17 | C2S 0x21 — anti-cheat CRC reply (3-player loopback 2026-06-16d) |
| 5.18 | C2S 0x47 / 0x48 — request entity-state broadcast + stub (3-player loopback 2026-06-16d) |
| – | Cross-witness append for §5.10 — 3-player loopback 2026-06-16d |
| 5.19 | Tag 0x40 — minimap-overlay update / capture-zone state (controlled capture 2026-06-17) |
| 5.20 | Tag 0x16 — PLAYER-LIST / SCOREBOARD (controlled capture 2026-06-17; header/trailer + HUD-count semantics witnessed 2026-07-03) |
| 5.21 | Tag 0x46 — PLAYER-SYNC (controlled capture 2026-06-17) |
| – | Cross-note — pool-0 organic spawn path (controlled capture 2026-06-17) |
| 5.22 | `/PROFILE` `.sph` server-log recording — independent value oracle (controlled capture 2026-06-17) |
| 5.23 | Tag 0x0C — pool-0 organic spawn batch (field map; D-NET-62) |
| 5.24 | Authored-mission cross-validation — pools 1/2/3 (D-NET-62) |
| 5.25 | Replay timeline — assembling a capture into per-entity tracks (tooling, 2026-06-17) |
| 5.26 | Tags 0x1E / 0x26 / 0x4E — game events, kills, batch despawn (the kill feed; one-host/one-client capture 2026-06-17) |
| 5.27 | Replay event + environment streams (tooling, 2026-06-17) |
| 5.28 | Mission delivery to a joiner — a chunked file transfer (probe2, 2026-06-18; header corrected 2026-06-18b) |
| 5.29 | Tag 0x0F — world-state-load (joiner spawn + scores + location names; probe2, 2026-06-18; server writer + field roles witnessed 2026-07-03) |
| 5.30 | Tag 0x5A — weapon-loadout sync (probe2, 2026-06-18) |
| 5.31 | Tag 0x6E — team/squad roster sync (probe2, 2026-06-18) |
| 5.32 | Tag 0x7B — full player/session info (probe2 + loopbacks, 2026-06-18) |
| 5.33 | C2S burst replies 0x22 / 0x23 / 0x28 / 0x29 / 0x4C (probe2, 2026-06-18) |
| 5.34 | Session/transport control pings — RTT 0x57/0x2C + request trio 0x68/0x43/0x39 (probe3_again, 2026-06-19) |
| 5.35 | Minimap overlays, weapon reload, second death path, checksum + misc scalars (probe3_again, 2026-06-19) |
| 5.36 | Deployed-item spawn 0x59 + entity-routed sub-packet 0x44 (probe3_again, 2026-06-19) |
| 5.37 | Terrain-tile load batch 0x45 (operation_whitenoise stock Co-op, 2026-06-19) |
| 5.38 | Local-player input→pose locomotion — the player simulates, never interpolates (Phase 2, 2026-06-20) |
| 5.39 | First/third-person player camera (Phase 2.5, 2026-06-20) |
| 5.40 | First-person weapon viewmodel placement — weapon.def `pos`/`tpos` (2026-06-21) |
| 5.41 | `Player_*` family — naming validation + decomp cleanup grill (2026-06-26) |
| 5.42 | `Server_*` family — naming validation + decomp cleanup grill (2026-06-26) |
| 5.43 | Player add → spawn: id allocation, team, burst cadence, placement (2026-06-26) |
| – | P4 `Server_TickUpdate` host-loop review (2026-06-27) |
| 5.44 | `Client_ProcessNetworkFrame` — the per-frame client net role (P5, 2026-06-27) |
| 5.45 | P8 reactive-reply gate — the `game_session.cpp` reply machine vs the witnessed serializers (2026-06-27) |
| 5.46 | C2S 0x0F entity-info query → S2C 0x18 FULL-ENTITY-SPAWN — the self-heal path (2026-07-01) |
| 5.47 | Server per-frame S2C 0x0A emit — phase counter + sub-block cycle + priority/budget entity loop (2026-07-01) |
| 5.48–5.56 | The 2026-07-01 wire-coverage sweep — session/HUD state channel (decoded) |
| 5.57 | The weapon.def loadout pipeline — AdmDef table, C2S 0x2F → S2C 0x5A derivation, ammo semantics (2026-07-02) |
| 5.58 | The reload round-trip — C2S 0x25 → S2C 0x49 (2026-07-02) |
| 5.59 | The character-slot binding family — C2S 0x29, S2C 0x29/0x50/0x51, and the registry/blip structures (2026-07-02) |
| 5.60 | The authoritative round simulation, exact recoil/spread, damage, and the death broadcast family (2026-07-03; recoil/spread re-grill 2026-07-31) |
| 5.61 | Advance & Secure — spawn selection, the zone chain, and the capture loop (engine-research scope, 2026-07-03) |
| 5.62 | The FP weapon action FSM — weapon.def ACTION rows → the 12-state pump (2026-07-09) |
| 5.63 | The spawn-kit chain, the per-player slot pool, map availability rules, and manual switching (the loadout grill, 2026-07-18) |
| 5.64 | The host-initiated session close — the connection-description punt (live retail capture, 2026-07-26) |
| 5.65 | S2C `0x30` / `0x31` — the anti-cheat challenge pair, and the client reply builders (2026-07-26) |
| 5.66 | The multiplayer loadout SOURCE — the per-side/per-class profile page in `weapon.sav` (live retail↔retail capture, 2026-07-26) |
| 5.67 | The same-weapon first-person viewmodel bleed — a RETAIL defect in the unguarded idle anim re-seed (live retail↔retail A/B, 2026-07-26) |

### 5.0 Session bring-up — single player is an in-process listen server

A single-player mission is **not** an offline codepath. It stands up a NovaWorld **host** in the
same process and connects a **local client** to it, then runs the full in-game replication loop
(§5.1–§5.17) over an in-memory (socketless) transport. Witnessed 2026-06-16 in `Jointops.exe`.

`[orig: SinglePlayer_StartMission @ 0x561af0]` (the menu "start mission" action) does, in order:

1. `[orig: CGameSession_SetConnectionMode @ 0x4c49f0]` with mode **3**. The function maps the
   connection mode to two booleans and stores all three on `g_napi_np_ctx` (§6.3): mode →
   `connection_mode (+0x5C)`, is_host → `is_authority (+0x60)`, is_client →
   `is_mp_session_peer (+0x64)`.

   | mode | is_host = `is_authority` | is_client = `is_mp_session_peer` | role |
   |---|---|---|---|
   | 0 | 0 | 0 | none |
   | 1 | 1 | 0 | host only |
   | 2 | 0 | 1 | client only (join a remote host) |
   | **3** | **1** | **1** | **host + client = single-player / co-op listen server** |

2. `[orig: CNapiNetwork_SetTransportMode @ 0x4c8750]` with mode **1**. Despite its name it writes
   the socket-state field (`CNapiNetwork+0x54`, §6.2) and calls
   `[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` **only for values 2/3/4**. SP passes
   **1**, so **no UDP socket is opened** — host↔local-client delivery is in-process.

3. Fills the same `NapiGameSettings` (§6.4) multiplayer uses — `server_name = "SINGLEPLAYERGAME"`,
   max_players = 1 — then `[orig: CNapiGameSession_CreateSession @ 0x4c97c0]`.

`CreateSession` is the **shared SP/MP session creator**. It installs the host-side callbacks —
`[orig: CNapiNetwork_ValidateJoinRequest @ 0x4c61b0]`, `[orig: NapiNPServer_HandleNewConnection @
0x4c8040]`, `[orig: NapiNPServer_DestroyPlayerCtx @ 0x4c8400]`, `[orig:
CNapiServer_OnPlayerDisconnected @ 0x4c94d0]` — builds the server config flags
(`[orig: CNapiServerConfig_BuildFlags @ 0x4c4dc0]`), then calls **StartServer**
`[orig: NapiNPProtocol_StartServer @ 0x62b5e0]` (renamed from Kong `sub_62B5E0`, applied
2026-06-16). StartServer is identified by its callee set: `NapiNPProtocol_StopServer`,
`[orig: NapiNP_GenerateSessionKey @ 0x61ea70]` (→ `host_key`, §6.5 +0x534), `GetTickCount` (→
`host_start_tick`, +0x53C), and `[orig: CNapiNPConnection_LogHostStarted @ 0x61e6a0]` (the
`"HOST STARTED \"%s\""` log) — matching the §6.5 host-state init exactly.

Finally, because `is_host && is_client` (mode 3), `CreateSession` creates a **local client
connection** with `[orig: CNapiNPConnection_Create @ 0x62acb0]` (connection type **2**) and blocks
on `[orig: CNapiGameSession_WaitForHostResponse @ 0x62b2d0]` — the loopback client↔host handshake
inside one process. Control then enters the `"Game Loop"` UI state.

**Consequence.** SP, co-op, and MP are one architecture; only the client count and transport
differ. Single player = `SetConnectionMode(3)` + `SetTransportMode(1)` (socketless); a co-op/MP
host raises the transport to a socket-opening mode (2/3/4) and accepts remote clients — no new
gameplay or replication path. `is_in_session @ g_napi_np_ctx+0x58` (§6.3) gates the entire
replication loop in every case, which is why §5.1–§5.17 run identically under single player.

**OpenNova startup values (fixed 2026-07-24).** Production `start_host_session` no
longer seeds the host protocol with capture-oriented zeroes: it mints a nonzero
`host_key`, uses a nonzero monotonic-millisecond `host_start_tick`, and chooses a
six-digit `session_seed_id`. `HostConfig` retains explicit nonzero overrides for
deterministic goldens/tests. `ServerHello.HK`, the ClientAuth echo gate,
uptime-derived replies, and the session-seed consumers therefore share real
per-session state.

**Open follow-ups (gated on this):**
- **Host's own-player spawn.** Substantially resolved in **§5.2a** (R1): the host runs its own
  server-side spawn machinery in-process (`Server_InitNewRoundState` → `ProcessPendingPlayerSpawns`
  + `Server_BuildPlayerInfoAndAdd` → `Server_SendInitialGameStateToPlayer`) while sitting in the
  in-process pump of `NapiClient_WaitForGameStart`. The narrow open sub-thread is the exact
  `dword_24C1928` write — probably the per-frame `0x0A` `flags1 & 0x01` spawn signal (the host
  cannot use the joiner-only `0x1D`); byte-confirmation pending.
- **In-process delivery faithfulness.** *Substantially resolved 2026-06-16 (R2).* Transport mode 1
  runs the **same byte serialize/parse path** as a socket session — only the UDP I/O is skipped, not
  the wire encoding. Witness chain: the host emits via `[orig: NapiNPServer_SendFiltered @ 0x4C87E0]`
  → `[orig: NapiNPServer_SendToConn @ 0x4c4f20]` → `[orig: CNapiNPConnection_QueueMessage @ 0x628640]`
  — i.e. each S2C is **built as a wire message on the connection's `msg_queue`**, never handed to the
  client handler by pointer. The receive side `[orig: NapiNPProtocol_PumpRecvQueues @ 0x6266a0]` (under
  `[orig: NapiNPProtocol_Pump @ 0x62a650]`) drains a **byte circular-buffer FIFO** (`recv_buf[55]` via
  `CCircularBuffer_Read2`), dispatches by the first opcode byte through `g_np_opcode_handlers`, then
  `[orig: CNapiNPConnection_ParseMessages @ 0x625bc0]` runs the msg_id dispatch — the identical path for
  socket and in-process datagrams. `[orig: CNapiNetwork_SetTransportMode @ 0x4c8750]` opens **no
  socket** for mode 1 (`[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` is reached only for modes
  2/3/4), so mode-1 delivery must loop the serialized datagram back into that same recv FIFO in-process.
  **Consequence for the reimpl:** the faithful SP path is a literal in-process byte loopback (serialize
  real entity state → datagram → recv FIFO → decode), *not* a direct snapshot hand-off — this is what
  ADR 0011 records. **Remaining open (narrow):** (a) the exact mode-1 transmit substitution that writes
  the datagram into the local recv FIFO in place of `sendto`; (b) whether the SCRK stream cipher runs on
  that in-memory datagram — the `0x43`/`0x83` recv dispatch decrypts with SCRK, so crypto **likely** runs
  end-to-end in-process, but the transmit-side encrypt on the loopback is not yet byte-witnessed.
- **Socket-open / pump path (witnessed P6, 2026-06-27).** The MP socket transport that mode 1 skips:
  `[orig: CNapiNetwork_OpenTransportSocket @0x4c6a40]` early-returns `if (!is_in_session || np_manager->
  udp_socket)`, picks a PORTMIN/PORTMAX/PORTDELTA/PORTRANDOM range from the per-mode config block,
  resolves the host, opens the UDP socket via `CNapiNPManager_OpenTransportSocket @0x6232c0`, and sets
  **recv+send buffer sizes to 0x10000 (64 KB)** (`CNapiUdpSocket_SetRecvBufferSize` /
  `CNapiNPManager_SetSendBufferSize` / `_SetRecvBufferSize`), logging to `_connectlog.txt`. The per-frame
  pump is `[orig: CNapiNetwork_PumpManagerReceive @0x4c4d10]` → `NapiNPManager_Pump(mgr, flags=4, 250ms)`
  (the recv pass) and the wire send is `[orig: CNapiNetwork_SendUDPPacket @0x4c4d30]` →
  `CNapiNPManager_SendTo @0x61ec20`. **Reimpl (P6):** `apps/common/net_sockets::udp_bind` /
  `udp_recv_from` / `udp_send_to` cover the open / recv / send; the 64 KB `SO_RCVBUF`/`SO_SNDBUF` and the
  PORTMIN..RANDOM selection are noted faithful details (irrelevant on the loopback `apps/nw_server` binds).
  `apps/nw_server/host_owner_loop.h` is the owner pump; the real-socket peers run full SCRK via
  `frame_in_match_s2c` (so the loopback crypto-bypass question (b) stays orthogonal to the MP path).
- `font_name @ 0x7C08C6` is the empty/default-string global that `SinglePlayer_StartMission` and
  `CreateSession` copy (via `[orig: Napi_CopyString @ 0x617e10]`) into the unused password/config
  fields. Its **address** coincides with the dispatch-table `magic` constant `0x7C08C6` (§4 open
  items, §6.1) — the "magic = build/version stamp" guess should be re-examined as a possible
  pointer to this default-string global.

### 5.0a — Join-leg lifecycle fixes (grill 2026-06-26; D-NET-104/105/106)

A code review of the `libs/npruntime` P0–P2 promotion surfaced three places where the promoted
handshake legs had only *part* of the witnessed `0x42` join behavior. All three are now witnessed
against `Jointops.exe` and ported; the in-match flow is `0x41 → 0x81`, `0x42 → 0x82`,
`0x43 → 0x83` (§5.0 / §3).

- **D-NET-104 — a retransmitted `0x42` re-sends the cached ServerAuth, it does NOT re-mint.**
  `[orig: NapiNPProtocol_HandleClientJoin @ 0x62b750]` does `FindConnection(proto, 1, addr, port)`
  first and branches: if the found connection is `conn_state == 1` **and** `session_keys.client_id
  == CI` (`0x7DFCBC`) **and** `session_keys.remote_key == CK` (`0x7DFD6C`), it calls
  `[orig: CNapiNPConnection_SendSessionInit @ 0x620ef0]` — which re-emits the *same* `0x82` from the
  connection's stored keys — and returns. Only a connection with a **different** CI/CK (a new client
  reusing the addr) is `[orig: CNapiNPConnection_Destroy @ 0x62a4b0]`-ed and recreated. The promoted
  legs were unconditionally re-minting `server_scrk`/`server_sk` on every `0x42`, so a normal lossy-UDP
  ClientAuth retransmit rotated the session keys the joiner had already latched from the first `0x82`
  → all later `0x83` failed to decrypt → silent join stall. **Reimpl:** `handle_client_join`
  (npruntime) / `HostSessionAccept` `CLIENT_AUTH` (novaworld) now find-first, re-send on a
  CI+CK-matching already-joined node, and only recreate on a genuine different-client collision.
  CI/CK are stored on the connection (`client_ci`/`client_ck`) for the match.

- **D-NET-105 — the `0x82` MI TLV is the host-assigned ConnectionId (the dcb), not a "machine id".**
  `[orig: CNapiNPConnection_SendSessionInit @ 0x620ef0]` writes the `MI` TLV (`0x7DFDF4`) from
  `conn->connection_id` (`+0x18`); the connection's `connection_id` is the join-order dcb assigned at
  `[orig: CNapiNPConnection_Create @ 0x62acb0]` from `++protocol[947]` (the non-zero, wrapping
  per-protocol counter at `+0xECC`). The **client** stores the received MI as its own
  `NapiNPConnection.connection_id` (`+0x18`, read back by `[orig: NapiNP_GetLocalConnectionId @
  0x4c6d40]`) and echoes it in its in-match `0x48` client-ack; the host stamps that id into the
  joiner's `0x0C` `ownerConnectionId` (`+0x78`), which the client self-matches in
  `[orig: Player_FindLocalPlayerEntity @ 0x4e0090]` (`Flags & 0x100 && ownerConnectionId == own id`).
  Wire proof: the working retail LAN join had `MI(0x82) == 0x48-ack == 0x0C eFlags == 3` (all the same
  join-order id). The promoted legs left MI at the `0x113f` placeholder, the client mirror never read
  MI, and the bundled `JoinerConnection`/`JoinerSession` never emitted a `0x48` — so the host's
  `self_id_seen` latch (the F3 streaming-entered gate) never tripped for an opennova client and F3 was
  dead (the "Could not find player dcb" class — D-NET-92/101). **Reimpl:** on a LAN listen host the
  host *assigns* the dcb at the `0x42`, ships it as MI, and latches `self_id_seen` on its own
  assignment (it does not wait for the client); the client adopts MI as its own ConnectionId and
  echoes it in the `0x48` member of frame 17's grouped
  `0x4E/0x03/0x48/0x47/0x33` admission packet. The `0x37` mission-data request is a later,
  standalone packet after the final S2C `0x60`; it is **not** bundled with `0x48` (§5.0d).
  The existing `0x48`-learning path is retained as the override for the NovaWorld case, where the
  dcb is gate-assigned, not host-assigned (TODO(P6): gate the host-assignment on the LAN network
  type once NovaWorld transport lands). This refines, but does not contradict, D-NET-92 (the
  client self-ID is numeric in retail; the opennova `JoinerConnection` keeps its name-match per
  the ROADMAP).

- **D-NET-106 — the join leg enforces capacity (`current_player_count >= max_players`).**
  `[orig: CNapiNetwork_ValidateJoinRequest @ 0x4c61b0]` (installed as the join-validate callback by
  `[orig: CNapiGameSession_CreateSession @ 0x4c97c0]`, invoked at the `0x42` join) rejects when
  `networkCtx[11]` (current player count) `>= networkCtx[970]` (`max_players`, `+0xF28`; plus
  `networkCtx[972]` spectator slots when `networkCtx[971]` spectator-enabled). It also rejects on
  server-locked (`dword_C94794`, reason 2) and ban-list (`dword_C8FF28`, reason 3); a full server is
  reason **4** (or **5** with spectators), overlay state **14**. The promoted legs admitted on
  `is_authority && host_running` only, never reading the stored `max_players`. **Reimpl (npruntime
  only):** `handle_client_join` rejects a new join when (host loopback + already-`Joined` joiners,
  excluding the joining peer) `>= max_players`. Modeled as a silent drop (no ServerAuth, no node) —
  consistent with the other `0x42` reject legs; the witnessed draw-overlay reject packet (state 14 /
  reason 4) is **not modeled yet** (tracked divergence). **Copy divergence:** the capacity gate lives
  only in npruntime, which models the `NapiNPProtocol.max_players` / CNapiNetwork capacity layer;
  `libs/novaworld/host_session_accept.cpp` is the pre-`NapiNPProtocol` simplified copy (no
  `max_players` model) and does NOT enforce capacity — it retires at ROADMAP P8 when npruntime takes
  over. D-NET-104/105 *are* mirrored in both copies.

### 5.0b — The game-session 0x42 CU var set (the joiner's character/profile upload; D-NET-146)

The game-session ClientAuth carries a CU chunk set DISTINCT from the NovaWorld-gate connect set
(§7 Wave 1 NW-S3's Application/BuildDateAndTime/.../UdpCode1/UdpCode2): the joiner's client/profile
environment plus its per-SIDE character selection. Emitter: `[orig:
CNapiServerInfo_SerializeToSession @ 0x4c3650]` — one `NapiNPChunk_Create(conn, 2, name, value)`
per field, each omitted when the value is 0/empty, values printed as decimal strings. Host parse:
`[orig: NapiNPProtocol_HandleClientJoin @ 0x62b750]`'s CU loop stores type-1/2 chunks on the
connection tag list; `[orig: NapiNetConfig_LoadFromConnTags @ 0x4c7260]` folds type-2 tags into
the per-player `NapiNetConfig` (case-insensitive names via `Napi_StrCaseEqual @ 0x616e70`, `atol`
values). Wire witness: the golden retail-ashi5a f=199140 (retail joiner → retail host) and
retail_join_v18 f=47676 (retail joiner → opennova host) carry the identical 18-tag set.

| tag | NapiNetConfig landing | meaning | witnessed profile wire value |
|---|---|---|---|
| `BT`/`VN`/`BN`/`DB`/`MBN`/`SOPD` | +0x00..+0x14 | build/version/debug stamps | 0/2/1/0/20042002/180 |
| `VERSIONSTRING` | +0x58 | client version | "V1.7.5.7" |
| `COUNTRYCODE` | +0x98 | locale | "us" |
| `APPID` | (+0x40 slot) | application id | 9360 (host-varies) |
| `CI0` / `CI1` | jsp[56] / jsp[58] (u16 of atol) | per-SIDE minimap/character-slot id | 512 (0x0200) / 33287 (0x8207) |
| `TR` | jsp[60] (≠0xFF && ≥2 → 0xFF @ 0x4c752f) | requested side (0=A, 1=B, 0xFF auto) | -1 |
| `CTA` / `CTB` | jsp[61] / jsp[62] | per-SIDE soldier class (5..9) | 8 / 8 |
| `VCA` / `VCB` | jsp[63] / ci0 low byte | per-SIDE avatar byte (→ entity+0x374) | 1 / 4 |
| `TZB` | +0x1A8 | timezone bias | 300 |
| `MPS` | +0x1AC | max packet size | 1300 |

"Side" is the team pairing {1,3} = A, {2,4} = B — the player add picks the side by the ASSIGNED
team and the session gametype's team-based bit (see D-NET-146 for the full consume chain
`Server_BuildPlayerInfoAndAdd @ 0x51d560 → Server_PlayerAdd @ 0x51cbc0`). Note the IDB's
`NapiNetConfig` field names are first-seen-tag artifacts shifted by one from these landings
(e.g. the struct's `bt` field holds APPID); the table above is the witnessed tag→offset truth.

### 5.0c — Retail LAN enumeration and pre-load join lifecycle (2026-07-22)

LAN is a local transport choice, not a NovaWorld-service flow. It does not contact a gate,
master server, HTTP endpoint, account service, or cookie jar. Retail reuses the low-level NP/NAPI
**game-session** hello codec over UDP broadcast:

1. `UI_RegisterLANMultiplayerCallbacks @ 0x558d20` wires `LAN_SEARCH`; its callback at `0x558c50`
   starts the enumerator initialized by the `0x558300` block and
   `CNapiGameSession_InitTransportConnection @ 0x4c9e10`.
2. `CNapiNPConnection_PumpEnumeratorAndSend @ 0x6290c0` walks the configured LAN port range
   (stock JO defaults `32768..32787`). `NapiNPSession_SendAnnouncePacket @ 0x61fa00` broadcasts one
   ordinary game-session ClientHello (`0x41`) to each port. There is no second discovery protocol.
   The exact retail identity is `NVS="NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright
   2004 NovaLogic"`, `CO="NovaLogic Inc, Calabasas CA U.S.A."`, `AP="Jointops.exe"`,
   `BDAT="Jul 21 2009 18:54:42"`, `PN="JOINTOPERATIONS"`,
   `PG=46 D6 74 B0 F9 81 5F 47 92 DA DE A7 24 7F 14 68`,
   `PV1="0.0.0 1/12/2004 EM"`, and `PV2="16"` `[orig: CNapiNetwork_Init @ 0x4ca4a0;
   JO protocol GUID static initializer @ 0x7937a0]`.
3. A listening game host answers that datagram with its ordinary ServerHello (`0x81`) through
   `NapiNPProtocol_SendServerInfoPacket @ 0x6204b0`. Enumeration is **stateless**: receiving a
   search `0x41` does not create a player/connection node; the actual join begins at ClientAuth
   (`0x42`). The reply's retail-visible session fields include server name/config/player counts
   (`SN`, `P1`, `P2`, `NP`, `MP`) and the gated opaque user strings (`SUS1`, `SUS2`). OpenNova
   emits only values backed by live host state; unmodeled `P2`/`SUS1` are omitted rather than
   populated with capture-shaped placeholders (D-NET-165 — retail-browser tolerance of the
   absent tags is unwitnessed).
   The OpenNova game host applies retail's identity gates too: `0x41` requires exact
   `NVS`/`PN`/`PG`/`PV1`; `0x42` rechecks those fields and additionally requires exact `PV2`,
   non-empty callsign `NA`, and the `HK` echoed from `0x81`. Invalid probes/joins are silently
   dropped as in the original handlers.
4. `UI_ProcessLANSessionStateMachine @ 0x558de0` pumps replies for about 30 seconds, deduplicates
   rows, and updates the list immediately. The observed source IP/port is the join target.
   Enumeration is a CADENCE, not one burst: while enumerating, the pump re-broadcasts the whole
   port walk every 3000 ms (`CNapiNPConnection_PumpEnumeratorAndSend @ 0x6290c0` sets the
   enumerating send interval to 3000 at conn+368 and gates on `tick - last_send_tick`), which is
   how a cold host that binds mid-window still appears. The 3000 leg of the pump's interval
   select (`+37 ? 3000 : enum+20`) is confirmed to be the ENUMERATOR identity: the 0x41 announce
   builder (`NapiNPSession_SendAnnouncePacket @ 0x61fa00`) skips the player-count TLV exactly
   when +37 is set — matching the captured client probe shape, which carries none — while host
   announces (the `enum+20` leg) include it. `NovaLanSession` re-announces the stored
   probe burst on that witnessed 3-second cadence, and its 30-second browse window is the LAN
   screen's own witnessed stop (`UI_ProcessLANSessionStateMachine @ 0x558de0` re-enables
   LAN_SEARCH once `GetTickCount() - search_start > 0x7530`).

Crucially, LAN enumeration does **not** carry the map filename. Retail does not repurpose reserved
`SUS3`/`SUS4` or add an OpenNova-only side channel. After the player selects an endpoint, the normal
game connection authenticates (`0x41→0x81`, `0x42→0x82`), then performs the exact settings/JOIN and
reactive admission sequence in §5.0d. Its S2C `0x7B` full session record supplies `serverName`,
`missionName`, `mapFile`, `g_GameType`, and expansion (§5.32). The client loads that local mission
while retaining the same socket/session, then releases the existing load/spawn drive. While awaiting
`0x81` or `0x82`, the pending retail datagram is resent unchanged on the connection's active-send
interval; this covers both ordinary UDP loss and a joiner launched before a cold host has finished
loading and bound its requested port. Sequenced reliable messages use the different, probe-induced
recovery described in §5.0d and D-NET-164.

The game ClientAuth's character-selection CUs (`CI0`/`CI1`, `TR`, `CTA`/`CTB`, `VCA`/`VCB`) are
not protocol identity and do not participate in enumeration. They are derived from the player's
profile/selected characters, but they are mandatory presentation inputs once the gameplay
connection starts: omitting them makes the host stamp a zero-avatar/fallback character record.
OpenNova now loads the mounted `Avatars.def` before authentication, packs the selected
nationality/division/combo for both alignments, and uploads the resulting ids, classes, and avatar
bytes. The §5.0b values remain one capture witness, not universal constants. In particular, decimal
`33287` is the captured packed side-B registry id `0x8207`, **not a UDP port**. IDA also resolves a
fresh stock profile more precisely: `PlayerProfile_InitDefaults @0x54BB40` chooses the first combo
of each alignment and clears both explicit voice overrides; `apply_session_settings_to_globals
@0x551500` then derives the avatar byte through `sub_57AE60 @0x57AE60`. Stock JO therefore starts
at `CI0=0x0200, CI1=0x8207, VCA=1, VCB=10`; the captured `VCB=4` is a saved profile voice override.

**OpenNova mapping.** `NovaLanSession` owns only UDP broadcast/receive and normalized endpoint rows;
the socket-free probe/reply projection lives in `libs/npruntime/lan_discovery`. Selecting a row enters
the same `ClientRuntime` used by direct joins. The pre-load driver holds that runtime at its
world-ready boundary until `0x7B` identifies an installed `.bms`, so discovery, authentication,
mission load, and gameplay never require a reconnect or an invented metadata field.

### 5.0d — Retail game-session admission and load boundary (golden LAN capture, 2026-07-23)

The retail-host/retail-client golden fixes both message order and packet grouping. `S` and `C`
below are the server/client session directions (`0x83`/`0x43`); `seq/ack` are the decrypted
session-header values. A slash-separated tag list is one protocol-message packet, not several
datagrams:

| frame | direction/header | packet contents |
|---|---|---|
| 6 | `S seq=1 ack=0` | Initial `H:0x00` settings pair: `00 00 20 00 00 14 05 00 00` and `01 00 20 00 00 14 05 00 00` (directions 0/1, mask `0x2000`, value 1300). |
| 7 | `C seq=1 ack=1` | Header-only cumulative ACK. |
| 8 | `C seq=2 ack=1` | `0x00` JOIN with the exact 21-byte body: ASCII `VERSIONCRCSTRING` followed by `00 02 00 30 00`. |
| 9 | `S seq=2 ack=2` | Empty `0x00` JOIN acknowledgement. |
| 10 | `C seq=3 ack=2` | Header-only cumulative ACK. |
| 11 | `C seq=4 ack=2` | `0x01 {00}` form post; an empty `0x01` is not equivalent. |
| 12 | `S seq=3 ack=4` | `0x02` 512-byte position/padding challenge with `paddingLen=256`. |
| 13 | `C seq=5 ack=3` | `0x02` 256-byte padding echo. |
| 14–15 | `S seq=4 ack=5`, then `C seq=6 ack=4` | Post-handshake metadata (`H:0x00×2/0x01/0x7A/0x7B/0x03`), followed only by a header ACK. |
| 16 | `S seq=5 ack=6` | `0x03/H:0x00×2/0x05{01}/0x04(24 B)/0x7B`; nonzero `0x05` is the admission trigger. |
| 17 | `C seq=7 ack=5` | One packet: `0x4E {4×00} / 0x03 {4×00} / 0x48 {le32(ServerAuth.MI)} / 0x47 {} / 0x33 {8×00}`. |
| 18 | `S seq=6 ack=7` | `0x75 {00 02} / 0x60 {id=1,total=171,offset=0,+171 data bytes}` (final server-info chunk). |
| 19 | `C seq=8 ack=6` | Standalone empty `0x47`. |
| 20 | `C seq=9 ack=6` | Standalone initial `0x37 {8×00}` mission-data request. |
| 21 | `S seq=7 ack=9` | `0x75 {00 02} / 0x64 {id=1,total=180,offset=0,+180 data bytes}` (final mission-data chunk). |
| 22 | `S seq=8 ack=9` | Structurally valid `0x16` player list. |
| 23 | `C seq=10 ack=8` | One packet: empty `0x09 / 0x22 {00 F7 1C}`. |

Frame 8 above is the base-game golden. On an expansion host, retail prefixes the JOIN
body with a second string TLV: `EXP\0 [u16 len] <expansion>\0`, followed by the same
`VERSIONCRCSTRING` TLV. The value is the CLIENT's OWN ACTIVE expansion, `g_ExpansionName` —
NOT a copy of what the host advertised. It equals `ServerHello.SUS2` only after the
pre-connect `Expansion_SwitchTo` succeeded (§5.0c / D-NET-178); when that switch is a no-op
(the expansion is not installed) retail sends its unchanged local name and lets the host's
compare reject it. The `revx02` capture could not distinguish the two readings because the
joiner there was already mounted on the host's expansion. Witness chain:
`Napi_CopyString(&net_config.<field>, g_ExpansionName, 32) @0x569dc4` in
`UI_JoinSelectedSession @0x5699d0` -> the config `qmemcpy @0x4ca22c` (the block lands at
`ctx+0xFD8`; the IDB's typed `net_config` at `+0xF84` is shifted by one dword, which is why the
write site renders as `.mbn` and the read site as `.sopd` — same bytes) -> the `EXP` TLV
write `@0x42a23d..0x42a26e` (first-byte gate `@0x42a244`) in
`NapiNP_WriteClientAuthPayload @0x42a180`. The game-session `JSP` CU is unrelated. The host
copies JOIN `EXP` into the pending player record and `Server_ValidatePlayerJoinRequest @0x512100`
compares it with the active expansion. A mismatch rejects with generic disconnect
class `DC=2` and the specific reason `DPC=47`—`DC` alone is not the validator result.
`VERSIONCRCSTRING` remains `"0"` when the active expansion has no loose
`expansion/<name>/version.txt`; retail otherwise computes its expansion-version
checksum from that file. OpenNova currently emits `"0"` unconditionally: this matches the
live `revx02` install used here because that loose file is absent, while nonzero expansion
checksums remain a compatibility gap until the runtime supplies the resource path
(D-NET-166).

The game ClientAuth's environment CU block is validated against LITERALS, not host
config (decompile witness 2026-07-24): `Server_ValidatePlayerJoinRequest @0x512100`
requires `BN==1` (DC=2), `VN==2` (DC=3), `MBN==20042002` (DC=4 — the `0x131D112`
immediate, which decompilers render as a spurious data ref), and `SOPD==180` (DC=8);
`BT` is the account ban state (1/2 reject as DC=6/7). Every other uploaded field
(`VERSIONSTRING`, `COUNTRYCODE`, `TZB`, `DB`) is stored/display-only by
`NapiNetConfig_LoadFromConnTags @0x4c7260`. The four validated values are therefore
patch constants of the 1.7.5.7 binary — hardcoding them is faithful; a version-mismatched
join failing is retail behavior. The encrypted `NAMEINFO`/`PCID`/`JOINTICKET` KV section
of the same validator runs ONLY when `CNapiNetwork_GetLocalAddress @0x4c4f60` succeeds,
and that helper returns 0 unless the transport is NovaWorld — so omitting the blob is
faithful for LAN and becomes mandatory (DC=23) when the NovaWorld transport lands. The
squad/side password legs (`FID`/`JSP`, DC=18..21) run on every transport: password-
protected hosts are unjoinable until those CUs exist (D-NET-167).

**OpenNova host admission (ported 2026-07-24).** The 0x42 gate now parses the CU
list with retail's ordered/last-value-wins `atol` semantics and rejects
missing/wrong `BN`/`VN`/`MBN`/`SOPD` or banned `BT=1/2` before allocating a
connection. A validated 0x42 creates only a pending authenticated node. Its explicit
`AwaitJoinRequest → AwaitFormPost → AwaitPaddingEcho → Complete` state accepts
exactly one gameplay record per reactive turn, validates the
expansion/`VERSIONCRCSTRING` string-TLV body, requires `0x01 {00}`, and accepts the
256-byte 0x02 echo only when its witnessed position-prefix dwords match the
challenge. Header-only ACKs are inert; malformed, duplicated-in-turn, or
out-of-order messages tear the pending node down. `self_id_seen`, player spawn,
roster publication, and the world stream remain gated until the final echo. The
unmodeled retail rejection overlay/reason packet is still D-NET-171; the modeled
no-`version.txt` CRC remains D-NET-166.

The admission exchanges through frame 23 are **reactive and pre-world**: they run even while the
binding has not installed the advertised mission. Initial `0x33`/`0x37` requests carry eight zero
bytes; only an incomplete `0x60`/`0x64` transfer uses the continuation body
`[u32 transferId][u32 nextOffset]` (§5.28). A player list received before final `0x64` must be
latched, but it cannot release the grouped `0x09/0x22` until that transfer completes. The terminal
S2C `0x11` needs the same treatment: a host emits it exactly once, paced behind its C2S `0x02`
reply regardless of joiner progress (our host: ~13 ticks, the §5.5 bundle), so a joiner still
mid-transfer must latch an early arrival and apply it on reaching the sync tail — dropping it
parks both sides with no semantic recovery (`npruntime_client_runtime`
`run_early_sync_tail_latch`).

The later BMS-state packet ends in S2C `0x11` (§5.5). That terminal tag is the safe
`preload_ready` boundary: the client ACKs it but emits neither C2S `0x0A` nor loadout while its
local world is held. After the advertised mission is installed, the client sends the empty C2S
`0x0A` spawn-menu request; the host advances sync state 3→4 and begins the §5.2a world stream.
The world-stream terminator S2C `0x1A` then triggers one grouped client packet
`0x2F / 0x2F / 0x0B` (the two profile-side loadouts plus mission status). If S2C `0x0F`
`gameFlags` bit 0 says spawn zones exist, the initial S2C `0x5A` pair only grants those
loadouts: the joined player remains respawn-pending/hidden (`0x0A flags1` bit 1 and player-record
state `0x01`). The client then sends one C2S `0x0E {FF FF}` parameter-0 deployment pick — the
DEATH deploy screen's Default Spawn row (node 0; §5.61's screen witness); a player picking a
zone row sends that zone's registry handle instead, and a silently-dropped invalid pick simply
leaves the screen up for a re-pick.
Retail answers with the post-pick `0x5A` release (alongside `0x61` in the live witness), and the
next player record becomes visible at state `0x00`. When no spawn-zone/deploy screen exists, the
initial `0x5A` is itself the final release and no `0x0E` is sent. A joiner reaches InMatch only
after both its self `0x0C` organic-spawn record and the applicable final `0x5A` have arrived.

The send pump has THREE legs, all read from the connection template
(`CNapiNPConnection_PumpSendIntervals @ 0x628fd0`): the ACTIVE interval below (retained records
outstanding), the packet-queue/missing-seq interval, and the **EMPTY interval** — with nothing
queued AND nothing retained, the pump still force-builds a packet once
`cs_dir0.idle_send_interval_ms` (**30000** ms for JOINTOPERATIONS) elapses since the last build
(@0x629041..0x629067). That empty leg is the connection KEEPALIVE: the peer reaps a silent
connection at `cs_dir0.timeout_ms` = **120000** ms (both stored by `CNapiNetwork_Init @ 0x4ca4a0`).
A client that only transmits while deployed — no per-frame `0x0C`, no housekeeping — therefore
survives on this leg alone; omitting it drops the session after ~2 minutes of quiet (the live
retail-host symptom fixed 2026-07-24). OpenNova's joiner ports the empty leg; the host side is
tracked as D-NET-173.

Reliable loss recovery does not blindly retransmit an unanswered semantic datagram. While reliable
records remain retained, a sender with no ordinary application packet to send waits **strictly more
than 10000 ms** — the JOINTOPERATIONS connection template's `active_send_interval_ms` (idle 30000;
`CNapiNetwork_Init @ 0x4ca4a0` stores both directions @0x4caac5/@0x4cab98; the 1000 ms an earlier
revision recorded here is the NOVAWORLDUDP service template's value @0x4d3e60) — then emits a fresh
header-only session sequence. The missing semantic sequence makes
the peer request the gap via client `0x44` or server `0x84`; the sender rebuilds the requested old
sequence from retained message records with its current ACK. This applies in both directions—for
example, a dropped initial settings packet requires the host's active-send probe. `[orig:
CNapiNetwork_Init @ 0x4ca4a0 /
CNapiNPConnection_PumpSendIntervals @ 0x628fd0 / CNapiNPConnection_BuildOutgoingPackets @ 0x628430 /
CNapiNPConnection_SendSessionPacket @ 0x61edd0 / NapiNP_HandleResendList @ 0x623800]`

### 5.1 Loading-progress counter — `dword_A82370`

The server walks the client's loading progress up via specific S2C tags. Each handler
increments at most (sets only if current < N):

| Tag | Sets `dword_A82370` to | Note |
|---|---|---|
| 0x0D | 3 | pool-entity spawn batch |
| 0x20 | 5 | bulk pool-3 entity sync |
| 0x45 | 6 | terrain LOD load trigger (`PolyTrn_LoadTileData`) |

A client below 6 may sit in a half-loaded state that prevents spawn confirmation.

### 5.2 Spawn-success gate — `dword_24C1928`

`[orig: NapiClient_WaitForGameStart @ 0x42cc10]` is the loading-screen wait loop. It returns
1 (success — drop loading screen, enter level) when `dword_24C1928` is set; returns 4 on
`dword_24C1878` (timeout), 3 on `dword_24C187C` (user cancel), 0 on `dword_A82364` (set by
S2C 0x1A).

Two S2C client handlers set the gate, but only one is used by retail:

- **Tag 0x1D** `[orig: NapiNPClientMsg_0x01D @ 0x430840]` — sets `dword_24C1928 = 1` **before
  any payload parsing**, gated only on `is_authority == 0`; even an empty payload clears the
  gate. Also writes `dword_C8D820 = 0x7FFFFFFF` (round-end sentinel at +infinity). Present in
  retail capture7's S2C inventory: **this is the retail spawn-gate trigger.**
  - Wire format, slow path (taken when ctx `+0x58` (`is_in_session`) is non-zero and
    `g_GameType & 0x10000` is clear): 3×cstring (max 32 each → `byte_A81B40/60/80`; server /
    mission / scenario names) + 3×i16 sign-extended (→ `dword_A81BA0/A4/A8`; score caps).
  - Fast path (otherwise): u8 → `dword_A81B30`, 2×i16 → `dword_A81B34/38`, u8 →
    `byte_A81BAC`, u8 → `dword_A81B3C`.
  - Minimum non-crashing payload: 9 zero bytes (three empty cstrings + three zero i16).
    Zeroes leave HUD/scoreboard strings blank but do not block gameplay.
- **Tag 0x25** `[orig: NapiNPClientMsg_0x025 @ 0x422800]` — empty payload; client side resets
  input bindings mid-frame, clears camera/HUD state, sets the gate plus companion
  `dword_24C195C = 1`, and increments round counter `dword_24C116C`. Retail uses 0x25 **only
  C2S** (weapon reload); the S2C direction is not part of the retail spawn flow.

Of 70+ writers of `dword_24C1928`, most are server-side (`NapiNPServerMsg_*`, `Server_*`,
`SaveFile_*`) and never run on a retail client. S2C 0x4D only reads the gate; 0x56/0x57 were
not confirmed as writers. `dword_24C1878` (timeout) writers are mission-load state
transitions: `[orig: CNapiNetwork_OnConnectedToServer @ 0x4c62e0]`,
`[orig: CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0]`,
`[orig: CNapiGameSession_CreateServerConnection @ 0x4c9d30]`,
`[orig: Game_StartMission @ 0x524360]`, `[orig: SaveFile_SendAndWaitForServerAck @ 0x5204b0]`.

Live-test record (2026-04-26, reverted stack vs retail client):

1. Emitting 0x1D cleared the gate ("Mission loading complete") but the `dword_C8D820 =
   0x7FFFFFFF` side effect made the client show "game has ended" and send ClientGoodBye.
2. Emitting 0x25 cleared the gate, but the round-counter increment likely triggered a
   re-load, after which the client fell into the spawn-select-menu fallback: a C2S tag 0x0F
   len-2 query loop (22+ queries per packet, pool-1 slot indices `0x10NN`) without ever
   spawning. The menu-bypass tag retail sends to skip that fallback was still unidentified at
   the time of the revert; candidates were the 0x05 flag byte (must be non-zero) and tags
   retail emits that we did not (e.g. 0x40).

### 5.2a Host-side spawn flow — how the listen-server host spawns its own player (R1, 2026-06-16)

Resolves §5.0's "host's own-player spawn" follow-up. On a listen server the host is
`is_authority == 1` and never processes the S2C 0x1D gate (§5.2, joiner-only), yet it must spawn
its own player and clear `dword_24C1928`. The host runs its own server-side spawn machinery
in-process. The flow:

1. **Local-player context** — `[orig: Server_InitNewRoundState @ 0x51c8e0]` (called directly by
   `SinglePlayer_StartMission`, §5.0). When `is_authority`, allocates the player-slot table
   (`[orig: Server_AllocatePlayerSlotTable @ 0x51c180]`, capped 1..251) and sets up the local
   player object `playerCtx @ 0x24C0CC4` (guard `dword_24C0CA0`): name from `STRSRV19`
   ("Server") or, for the SP host specifically (`is_in_session && is_authority &&
   is_mp_session_peer && transport_mode == 1`), from the profile `CHAR` var; assigns team
   (`[orig: Server_AssignPlayerTeam @ 0x4fe310]`). Clears the *timeout* gate `dword_24C1878 = 0`,
   not the spawn gate. A second witness that `transport_mode == 1` is the SP-host signature (§5.0).

2. **Server accepts the (local) player + builds the entity** —
   `[orig: CNapiServer_ProcessPendingPlayerSpawns @ 0x4c8dc0]`, gated
   `is_authority && !dword_24D1DE0 && !dword_24C1928` (runs while the gate is clear). Walks the
   pending-connection list, applies team-balance, then per accepted player calls
   `[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]` (builds the player-INFO buffer and delegates
   entity registration to `[orig: player_ServerAdd @ 0x51cbc0]`, stored at `CGameSession+4512`;
   the entity field-init sequence is §5.2b), sends spawn msgs `3` (weapon-restriction flag) /
   `5` (bool true) / `4` /
   `0x7B`, then `[orig: CNetPlayer_SetGameState @ 0x4c4060]` → state 8 and
   `CServerTick_SetPhase(slot, 1)`. The host's local client is just another entry in this list.
   Reads the gate as a guard; does not write it.

3. **Server streams the loading sequence** —
   `[orig: Server_SendInitialGameStateToPlayer @ 0x51bba0]`, gated `!dword_24C1928`, is a
   per-frame phase machine and the **server-side source of the S2C loading messages** (the
   sequence consumed by §5.1/§5.4–§5.12). Two phase tracks, each ending with
   `CNetPlayer_SetGameState(.., 9)` (in-game). All sends go through
   `[orig: NapiNPServer_SendFiltered @ 0x4C87E0]` with the send descriptor at
   `g_napi_np_ctx+0x1198..0x11A0` (§6.3) stamped per message:
   - player-sync track (`slot+0x20 == 2`, subPhase 8→16): `0x2C` type/base name
     (`NetPacket_WriteTypeNameAndBaseName`) → `0x08` server config → `0x2A`×6 table rows → `0x1C`
     → `0x0B` 616-B BMS header (`[orig: NetPacket_WriteBMSHeader @ 0x502ca0]`, §5.4) → `0x66`
     weapon restrictions → `0x76` server tick16 → `0x11`.
   - world-stream track (`slot+0x20 == 4`, phases 0→7): `0x10` static batch → `0x0D` pool spawn
     → `0x0C` entity states → `0x20` bulk pool-3 → `0x45` → `0x7E` → `0x1A` timestamp.

   **The tracks do NOT chain directly (2026-07-02, D-NET-150):** the player-sync tail sets
   sync-state **3** (`@ 0x51c134`) and the emitter has no state-3 arm — the world stream starts
   only when the client's empty C2S 0x0A spawn-menu request advances 3 → 4 + resets the phase
   (`[orig: NapiNPServerMsg_HandlePlayerSpawnRequest @ 0x513260]`). Every send in BOTH tracks is
   additionally gated on the connection's sent-unacked reliable count `conn+0x768 < 20`
   (`@ 0x51bbfd/@ 0x51bf14`; ack sweep decrements it in `[orig: CNapiNPConnection_ParseMessages
   @ 0x625bc0]`) — client-paced backpressure that stretches the stream across a cold client's
   mission build. Full mechanism: D-NET-150.

   This is the emitter ordering the planned P6 host world-stream must reproduce.

   **Server-side S2C serializer map (witnessed 2026-06-16, `[orig: Server_SendInitialGameStateToPlayer
   @ 0x51bba0]`).** Each load-track tag is produced by a dedicated serializer; these are the ENCODE
   functions the host reimpl ports (the client-side decoders are §5.4/§5.9/§5.11/§5.12). The emitter
   gates on `dword_C8FC58` and caps at 20 messages/frame (`conn[+1896] < 20`). Two tracks keyed on
   `playerSlot+0x20` (sync state), driven by phase counters `playerSlot+89878` (player-sync subPhase) /
   `playerSlot+89882` (world-stream phase) / `playerSlot+89884` (per-phase loop counter). Every send is
   `NapiNPServer_SendFiltered(.., msgId, 1, 0, buf, len)` with `g_napi_np_ctx[+1126]=32` (filter = the
   just-spawned player) and `[+1128]=playerSlot`.

   Player-sync track (`+0x20 == 2`, subPhase 8→16+, then sync state → 3):

   | order | tag | serializer |
   |---|---|---|
   | subPhase 8 | 0x2C | `[orig: NetPacket_WriteTypeNameAndBaseName @ 0x505780]` |
   | subPhase 9 | 0x08 | `[orig: ServerConfig_SerializeToPacket @ 0x505bd0]` |
   | subPhase 10-15 | 0x2A ×6 | `[orig: NetPacket_CopyTenBytes @ 0x503900]` (rows `byte_82F1DC`, gate `dword_82F1D8` vs `playerSlot+7`) |
   | ≥16 | 0x1C | (empty payload) |
   | ≥16 | 0x0B | `[orig: NetPacket_WriteBMSHeader @ 0x502ca0]` (from `byte_A761D0`; §5.4) |
   | ≥16 | 0x66 | `[orig: NetPacket_SerializeWeaponRestrictionTable @ 0x5102c0]` |
   | ≥16 | 0x76 | `[orig: NetPacket_WriteServerTick16 @ 0x510350]` |
   | ≥16 | 0x11 | (empty payload, **last** — the §5.5 bundle) |

   World-stream track (`+0x20 == 4`, phases 0→7, then sync state → 5). Each phase serializes a
   whole entity pool and advances when its cursor reaches that pool's used-entry count
   `[orig: Pool_GetUsedCount @ 0x441f80]` — a one-line getter `return g_pool_list[poolIndex].used`,
   the sibling of `Pool_GetEntryUnchecked @ 0x441fc0`. **Kong misnames it `PowerUpDef_LoadAll` with a
   bogus "loads powerup definitions" comment** (its arg is a pool index 0..3, not a filename;
   `Server_SendInitialGameStateToPlayer` passes 0/1/2/3). IDB rename `0x441f80 → Pool_GetUsedCount`
   + corrective comment applied 2026-06-16:

   | phase | tag | serializer |
   |---|---|---|
   | 1 | 0x10 | `[orig: serialize_pool2_static_to_buffer]` (static batch; §5.9) |
   | 2 | 0x0D | `[orig: serialize_entity_pool_to_packet_0 @ 0x503940]` (pool spawn; §5.11) |
   | 3 | 0x0C | `[orig: serialize_entity_states_to_buffer @ 0x5030a0]` (entity states) |
   | 4 | 0x20 | `[orig: serialize_entity_pool_to_packet @ 0x503460]` (bulk pool-3; §5.12) |
   | 5 | 0x45 | `[orig: NetPacket_WriteTerrainTiles]` (terrain; repeats until it returns 0) |
   | 6 | 0x7E | `[orig: NetPacket_WriteBriefingText]` |
   | 7 | 0x1A | `[orig: NetPacket_WriteTimestamp @ 0x5046c0]`; then `CNetPlayer_SetGameState(9)` |

   The per-entity payload bodies these serializers emit are the inverse of the witnessed decoders: the
   player class via `[orig: NetPacket_SerializePlayerState @ 0x4C09C0]` (mode 1 = write compact / mode 3
   = write extended; §5.10 gives both directions, write side = `Network_CompressFixedPoint` instead of
   decompress, `Entity_TransformWorldToLocal` for the vehicle-mounted branch), AI infantry via
   `NetPacket_SerializeInfantryEntityState @ 0x4C0320` (§5.14), vehicles via
   `Entity_SerializeVehicleState @ 0x460560` (§5.13; renamed 2026-07-04 from Entity_SerializeMountedVehicleState — the short form is the DEAD-pose form, not a mounted one).

4. **The host waits in-process** — `[orig: NapiClient_WaitForGameStart @ 0x42cc10]` (the shared
   host+client loading-screen loop, §5.2) sends the client-ready `0x0A`, then pumps the network
   in-process — `[orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400]` →
   `[orig: NapiNPProtocol_Pump @ 0x62a650]` / `[orig: CNapiGameSession_ProcessNetwork @ 0x4d09f0]`
   — until `dword_24C1928` is set (return 1). On the host these pumps drive steps 2–3 and deliver
   the messages to the host's own local client *in the same process* (transport mode 1, §5.0); no
   socket round-trip.

**The gate write (probable, byte-confirmation pending).** Ruled out for the host: the C2S uplink
path (`[orig: Client_ProcessNetworkFrame @ 0x42c180]` — its `dword_24C1928` reads and the
`0x2C`/`0x0C`-input sends are all gated `!is_authority`, joiner-only) and the two server spawn
functions above (read-as-guard only). The S2C `0x1D` handler is `is_authority == 0` only (§5.2)
and is never emitted by `Server_SendInitialGameStateToPlayer`, so the host cannot use it. The
remaining client-side writer the host's local client *can* hit under the pump is the per-frame
`0x0A` handler: `[orig: NapiNPClientMsg_0x00A @ 0x42fec0]` sets `dword_24C1928` on `flags1 & 0x01`
(§5.9, the spawn/respawn signal). **Probable:** once the host's player reaches game-state 9, the
host's per-frame `0x0A` to its local client carries `flags1 & 0x01` and the local `0x0A` handler
clears the gate. Open: byte-confirm that the first post-spawn `0x0A` sets `flags1 & 0x01` (the
server-side `0x0A` builder's spawn-flag logic is unwitnessed; the `0x430000`-page `0x1D`/`0x0A`
handlers currently fail to decompile — an analysis gap on that page).

#### 5.2a player-sync + world-stream serializer grill (2026-06-27, D-NET Wave 1)

`[orig: Server_SendInitialGameStateToPlayer @ 0x51bba0]` now decompiles cleanly (no longer the
analysis gap noted above — that was the `0x430000`-page client handlers, not this orchestrator). It
is a two-track state machine over `playerSlot+32` (`sync_state`): **state 2 = player-sync** (subphase
`playerSlot+89878`, 8..16) then **state 4 = world-stream** (phase `playerSlot+89882`, 0..7). Each
phase calls `NapiNPServer_SendFiltered(&g_napi_np_ctx, <tag>, 1, 0, buf, len)` (`send_mask=32`,
`send_target_slot=playerSlot`). The full burst, cross-checked **byte-for-byte vs the
retail-lan-host-join golden** (frames 144-160), with each serializer ported into
`libs/npruntime/src/server_initial_state.cpp` (was "emit nothing / deferred" through P3-P6):

| tag | serializer | body | reimpl source |
|---|---|---|---|
| 0x2C | `NetPacket_WriteServerNameAndMapFile @0x505780` (Kong-misnamed `WriteTypeNameAndBaseName` — FIXED) | `g_server_name_str` ("Untitled") + `g_map_file_name` ("TDH_I5A.BMS"), two NUL C-strings | `GameConfig.server_name`/`mission_file` |
| 0x08 | `ServerConfig_SerializeToPacket @0x505bd0` | 51 B = 10 rule dwords [respawn 30, timelimit 10, _, `g_GameType`, _, score 50, _, startdelay, _, _] + 7 bytes + flags dword (`CNapiServerConfig_BuildFlags @0x4c4dc0`) | `GameConfig` (rule globals; `dword[3]` = `game_type`, the same field the 0x7B body reads, §6.9) + `build_server_config_flags` |
| 0x2A ×6 | `NetPacket_CopyTenBytes @0x503900` over table `@0x82F1D8` | const 10-B record `{00 04 b0 ab b2 b2 bf bc bd ba}` ×6 (table = 6 records, threshold 0 ⇒ all sent; gate `threshold > playerSlot[+7]`) | `k0x2aRecord` const (**byte-exact vs golden**) |
| 0x66 | `NetPacket_SerializeWeaponRestrictionTable @0x5102c0` | count byte + (index,value) pairs for each restricted weapon (value 0/2) in `unused6[255]`; golden = `00` (no restrictions) | `ctx.weapon_restrictions` (restricted-set vector; empty ⇒ `{0}`) |
| 0x76 | `NetPacket_WriteServerTick16 @0x510350` | `dword_24D59FC` server tick, u16 (golden `ff 03`) | `now_tick & 0xFFFF` |
| 0x1C / 0x11 | (empty markers, ≥subphase 16) | 0-length | EmitEmpty |
| 0x0B | `NetPacket_WriteBMSHeader @0x502ca0` | 616-B BMS header (§5.4) | `bms::encode_header_blob` |
| world-stream 0x10/0x0D/0x0C/0x20 | pool serializers (§5.11/5.12/5.23) | per-pool batches | netsim extractors — ALL four pools streamed, paged, in the witnessed phase order [orig: @0x51bba0]; residual divergence is pool-2 CONTENTS (promotion over-populates), not omission (D-NET-98 UPDATE) |
| 0x45 | `NetPacket_WriteTerrainTiles @0x506570` → `serialize_terrain_tiles @0x6080f0` | terrain-tile delta from `entity+89884`; **`return 0` (orig skips) when no delta** | faithfully ABSENT (headless host streams no per-player terrain delta) |
| 0x7E | `NetPacket_WriteBriefingText @0x506620` | MissionText `briefing3` + `briefing2`/`briefing` C-strings; 0 when empty | faithfully ABSENT (no MissionText wired) |
| 0x1A | `NetPacket_WriteTimestamp @0x5046c0` | `GetTickCount()` u32 (OS primitive — excluded; reimpl uses `now_tick`) | `now_tick` |

**Verdict: MATCHING.** The config-independent 0x2A const is byte-exact vs every retail 0x2A body
(golden, 6 records); the host-config serializers (0x2C/0x08/0x66/0x76) reproduce the witnessed layout
(VALUES are the host's own config). 0x45/0x7E are emitted by the original **only** when a terrain
delta / briefing text is present — both `return 0` and the orig skips otherwise — so their absence on
the headless host is faithful, not a deferral (the golden carries neither). Tests:
`npruntime_initial_state_burst` (full order + per-body byte assertions), `npruntime_golden_lan_join`
(retail byte-parity 0x2A, structure-parity 0x2C/0x08/0x66/0x76). **Was: the "§5.2a serializer wave"
the ROADMAP / D-NET-127 repeatedly cited as the deferred grill — now closed for the player-sync
bundle.** The §5.2a world-stream (all four pools, 0x10/0x0D/0x0C/0x20) is now IMPLEMENTED and matches
the witnessed phase order [orig: Server_SendInitialGameStateToPlayer @0x51bba0]; pool-1 (0x0D) is
streamed with the AI-trailer crash fixed (D-NET-97). The residual §5.2a divergence is that our
`promote_mission` over-populates pool 2 with client-local map geometry retail keeps out, so our 0x10
carries more entities than a retail host's (D-NET-98 UPDATE) — a stock client accepts it, but it is
not byte-equal to retail for a mission with many statics.

**IDB names applied (2026-06-16, this grill; addresses are the join key, so older prose keeps the
`dword_*` spellings):** `sub_62B5E0 → NapiNPProtocol_StartServer @ 0x62b5e0`;
`dword_24C1928 → g_spawn_success_gate`; `dword_24C1878 → g_loading_timeout_flag`;
`dword_24C187C → g_loading_cancel_flag`; `dword_A82370 → g_loading_progress`. Each carries a
one-line entry comment in the IDB. Probable-only and left as-is: `dword_24C0CA0` (local-player-ctx
guard), `dword_24D1DE0` (spawn-processing gate), `dword_A82364` (reconnect/ready flag, set by S2C
0x1A).

### 5.2b Entity build + spawn-state init — the field-init sequence (2026-06-20)

Witnessed to give the listen-server host's own-player spawn a faithful field-init sequence
to port (the SP-as-listen-server keystone, libs/netsim Phase 2). All anchored (decompiled
this session); no IDB writes (all four functions already carry correct curated names).

**`Server_BuildPlayerInfoAndAdd` builds the player-INFO buffer, not the entity.**
`[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]` assembles a 226-byte player-info buffer
(`uint16_t[113]`: `NapiNPPlayer*`, name via `Napi_CopyString`, net flags, JSP country codes
resolved through `lookup_entity_slot_and_pack_entry` / `MinimapSlot_HasEntity`, PCID from
`player->pad_0[55]`, squad tag) and delegates registration to
`[orig: player_ServerAdd @ 0x51cbc0]`. The bot/authority branch (`net_flags != 0`) fills
empty texName strings; the human branch reads the `net_cfg` JSP/country/PCID. The in-world
`GamePlayerEntity` geometry is NOT built here — §5.2a step 2's "builds the GamePlayerEntity"
is refined: this builds the *info record* and hands off.

**`player_ServerAdd` is the player-SLOT manager.** `[orig: player_ServerAdd @ 0x51cbc0]`
(gated `is_authority`; strings `"server_PlayerAdd(): Unable to find empty slot!?"`,
`"Robot #%ld"`) memsets the 100584-byte (`0x188E8`) player slot, assigns the team
(`[orig: Server_AssignPlayerTeam @ 0x4fe310]`), writes the `ServerLog` PlayerName /
PlayerIpAndPort / PlayerPCID / PlayerTeam / PlayerType records (`CNapiVarList_SetOrCreate`),
sets up weapon-slot tracking, and on the local-player path stores the `g_local_player_entity`
pointer into the slot. Slot/identity bookkeeping — not entity field-init.

**`Entity_InitFromItemDef` — the item-template → entity field copy (the reusable init).**
`[orig: Entity_InitFromItemDef @ 0x49e550]` reads the type index at `entity+28`; when
`0 < idx < gItemCount` it caches `itemDef = &gItemDefs[idx]` at `entity+32` and copies:

| dst (entity) | src (itemDef) | width |
|---|---|---|
| `itemDef` +0x20 | `&gItemDefs[idx]` | ptr |
| `deathCallback` +0x1c8 | `deathCallback` +0x138 | ptr |
| `updateCallback` +0x1c4 | `updateCallback` +0x158 | ptr |
| `graphicModel` +0x30 | `graphicModel` +0xf0 | ptr |
| `huskModel` +0x34 | `huskModel` +0xf4 | ptr |
| `huskFinalModel` +0x38 | `huskFinalModel` +0xf8 | ptr |
| `destroyTimer` +0x1b0 | `destroyTiming0` +0x1a4 | u32 |
| `Health` +0x11e | `healthMax` +0x17c | u16 |
| `Armor` +0x120 | `armorMax` +0x17e | u16 |

then, if `itemDef->initCallback` +0x148 is non-null and not itself, tail-calls
`callback(entity)`. Confirms §6.9 (`entity+286 = ItemDef.healthMax`) and `entity+288 = armorMax`.
The caller sets `entity+0x1c ItemTypeIndex` (the `gItemDefs` index, from
`ItemList_FindIndexByTypeId(type_id)`; player infantry `type_id 0x14B9`) BEFORE calling.

**Correction (2026-06-25):** the prior version of this table listed `entity+48/52/56` as
`rtCounter0/1/2 (u32)` and `entity+452/456` as `pad_130[40]/[8]`. With `ItemDef` now fully typed
those are the **model-pointer triple** `graphicModel/huskModel/huskFinalModel` (`= ItemDef.graphic/
husk/huskFinal` resolved to `void*` models @+0xf0/+0xf4/+0xf8) and the `updateCallback`/`deathCallback`
pair. `graphicModel` +0x30 is the live render model — *dereferenced as a model pointer* in
`Entity_ClassifyForMinimap @0x50fa70` (`m[56]`/`m[48]`), NOT an integer counter. `huskModel`/
`huskFinalModel` are the damaged/destroyed render swaps (`ItemDef.huskSwapAt` threshold).

**`Entity_ResetToSpawnState` — the spawn/respawn reset that CLEARS the `entity+36` gate.**
`[orig: Entity_ResetToSpawnState @ 0x4B9610]` resolves §5.6's "remaining unsolved spawn
blocker": the `entity+36` bit-1 clear is this reset, not a wire message. It:
- backs up current `Position` (X/Y/Z) → the entity spawn-point fields (`pad9[124/128/132]`);
- splats the current `Yaw` across the heading-field family (`pad9[148/80/72/76/56/60]`,
  `pad5[12]`, `pad8[68]`);
- writes `Flags & 0xFFFFFFFD` to `pad9[152]`, then `entity->Flags &= ~2u` — **clears
  `entity+36` bit 1, the movement gate `[orig: Player_BuildTag0CInputBody @ 0x42A550]`
  checks** before serializing C2S 0x0C input (§5.6);
- zeroes velocities / AI-target refs (`pad5[24..44]`, `pad9[88/156/92]`, `pad7[20]`), sets
  `Roll = 0`;
- on `is_authority`, detaches from vehicle (`Entity_DetachFromVehicleIfServer`) and walks
  pools 0 and 1 removing every cross-reference to this entity;
- rebuilds proximity lists (`Entity_BuildProximityListsFromPools` + `Entity_BuildProximityList`).

**`GamePlayerEntity` offsets (struct size 904 = `0x388`), pinning the §5.x field map:**

| Offset | Field | | Offset | Field |
|---|---|---|---|---|
| +4 | `Position` (i32 X/Y/Z 16.16) | | +280 | `EquippedSlot` (MountSlot*) |
| +16 | `Yaw` (i32 — the D-NET-86 heading) | | +286 | `Health` (i16) |
| +20 | `Pitch` | | +288 | `Armor` (i16) |
| +24 | `Roll` | | +354 | `Team` (i16) |
| +36 | `Flags` (u32 — movement gate bit 1) | | +664 | `Weapon` |

**Faithful host-spawn sequence to port (Phase 2):** alloc a pool-0 entity → set the type
index from `type_id 0x14B9` (`ItemList_FindIndexByTypeId` → `entity+28`) →
`Entity_InitFromItemDef` (health/armor + counters + item init callback) → place
`Position` / `Yaw` / `Team` → `Entity_ResetToSpawnState` (clears `Flags & 2`, the gate). The
host then streams the loading sequence (§5.2a step 3) to its own local client in-process.

**IDB struct expansion (2026-06-23 `Entity_*` grill, deepened pass).** `GamePlayerEntity` (ordinal 357)
now carries a deepened named field set in `Jointops.exe.kong.i64` (**65 members**, size unchanged 904),
and `GamePlayerEntity *` is applied to **234 `Entity_*` prototypes** (every one whose first arg is a
slot pointer; 57 raw-`int`/`void*` params retyped this pass) so field access renders across the family.
The cleanest single witness is the tag-0x0C organic-spawn writer `[orig: NapiNPClientMsg_0x00C @
0x42E730]`, which stores each field at a cited site; cross-checked against the BMS spawner `[orig:
Entity_SpawnFromBMSRecord @ 0x40e9f0]` and `Entity_ResetToSpawnState @0x4b9610`. Fields named:
`ItemTypeIndex` +28, `itemDef` +32 (`ItemDef*`), `Flags` +36 (minimap/render + movement-gate bit 1),
`Ssn` +0x2e, `graphicModel/huskModel/huskFinalModel` +0x30/+0x34/+0x38 (model-ptr triple copied from
`ItemDef` — see the corrected `Entity_InitFromItemDef` table above; the 2026-06-23 `rtCounter0/1/2`
reading was superseded once `ItemDef` was typed), `CharacterEntity` +0x3c, `aiRuntime` +104, `ownerConnectionId` +0x78 (`[@0x42e864]`; was `entityFlags`, D-NET-101 — the join dcb / self-match field), `DcbId`
+0x7c, `commandGroup` +0x11c (u16), `Health` +0x11e, `Armor` +0x120 (`= ItemDef.armorMax`), `ammoCount`
+0x122, `MoveOrder` +0x12C + analog axes +0x130..0x133 (`Player_PackInputStateToEntity` @0x4df450),
`sectionMask` +0x134, `weaponType` +0x157 (`[@0x42ea4a]`), `NetId` +0x15c (u16, `[@0x42e9bc]`), `Team`
+0x162, `parentSlot` +0x168 (`[@0x42ea62]`, resolves to the `parentEntity` ptr +0x16c), `destroyTimer`
+0x1B0, `updateCallback/deathCallback` +0x1c4/+0x1c8 (copied from `ItemDef.updateCallback @0x158` /
`deathCallback @0x138` by `Entity_InitFromItemDef @0x49e550`; `deathCallback` is the death/lifecycle
handler invoked by `Entity_KillByNetId @0x43dbd0`), `subType` +0x214 (`[@0x42ea35]`), `refNum` +0x215 (`[@0x42ea20]`, see
D-NET-94), `weaponByte` +0x21A, `scoreFlag` +0x270, `playerClass` +0x294 (`[@0x42e9d5]`; was `weaponState` — D-NET-103, the soldier CLASS 5-9, not a runtime weapon state), `Weapon`
+0x298, `aiState` +0x2b4 (`[@0x42e991]`; also the `memset(entity,0,0x2B4)` base/extended-region
boundary), `SpawnOrigin` +0x318 (set by `Entity_ResetToSpawnState`), `animSlot` +0x374 (`[@0x42e9a6]`; the character-model / anim-set selector — BMS `AnimSlot` via `Entity_SpawnFromAnimSlotProperty`, the player's avatar via `Player_InitPlayer`, or the wire spawn; D-NET-103).

**Four id fields stay distinct.** The 32-bit id the `Entity_*ByNetId` family matches across pools
0/1/2 is `DcbId` @+124 (BMS/DCB script id) `[orig: Entity_FindByNetId @0x4655b0 matches `+124`;
Entity_KillByNetId @0x43dbd0 matches `+124` and clears `Health` @+286 then calls `Callback1` @+0x1c8]`
— distinct from `NetId` @+0x15c (the streaming id from the spawn packet) and `Ssn` @+0x2e (the
authority id, `Entity_GetNetIdIfAuthority @0x4e4010`). The FOURTH is `ownerConnectionId` @+0x78
(was `entityFlags`) — the owner ConnectionId the joiner self-matches (D-NET-92/101); the "Could not find
player dcb" abort keys on it, NOT on `DcbId@0x7C`.

**Name corrections (this pass):**
- **D-NET-93** — `Entity_SetNetId @0x43b8f0` is an auto-namer misnomer: it writes `Health` @+286, so it
  is renamed **`Entity_SetHealth`** (`int16_t(GamePlayerEntity*, int16_t health)`).
- **D-NET-94** — +0x214/+0x215, auto-named `boneB`/`boneA`, are **not bones**. The wire spawn writes
  them as `subType`/`alertLevel`, but +0x215 is really a small **registration/group id**: BMS registers
  `byte_A77648[bmsRec[153]]` `[@0x40ebcb]` and `Entity_Destroy @0x43e810` uses it as a minimap-DynArray
  group index + streaming-destroy gate. Named **`subType`** (+0x214) and **`refNum`** (+0x215). The
  actual AI alert STANCE is `aiRuntime+136` (0/1/2 stand/crouch/kneel), set by
  `Entity_HandleAlertStateEvent @0x43dee0` — NOT an entity field.
- **D-NET-95** — the +0x11c word, auto-named `pad6_post`, is the trigger **`commandGroup`**: entities
  are matched by it `[orig: Entity_HandleAlertCommand @0x43cf10 matches `+284`]` then driven by
  `TriggerGroup_Set{Patrol,Inactive,Active}`; BMS writes `bmsRec[78]` there `[@0x40ebb7]`.

(Open: `CharacterEntity` @+0x3c — the net 0x0C path writes a minimap-slot handle there
`[@0x42eb1d, MinimapSlot_FindOrAllocByEntityId]`; the prior name is kept pending a reader witness, as
the slot may be a union/overload.)

**Base-region padding named (2026-06-23 pass 2 — `padXX` grill).** The base region `0x0–0x2b3` (up to
the `memset(entity,0,0x2B4)` boundary) held ~660 bytes of unnamed padding; the well-witnessed,
single-meaning fields are now named (`GamePlayerEntity` → 101 members, 73 named, size still 904). Each
is backed by a witnessed access (consumer in parens) and most are already mapped in the cited sections
— this pass makes the IDB render them:
- **Locomotion / motor** — `currentSpeed` +0x29c (per-tick forward speed; `pos += speed<<13`, drives
  footstep sounds), `speedAccel` +0x2a0 (per-tick delta toward target; spawn reuses for
  waypointId/spawnTimer), `bodyHeading/bodyPitch/bodyRoll` +0x8c/+0x90/+0x94 (the rendered body
  orientation, distinct from the input `Yaw/Pitch/Roll` @+0x10/+0x14/+0x18; `bodyRoll` doubles as
  `Entity_FindByNetId`'s scratch result slot), `velocityX/Y` +0x98/+0x9c, `slideDecay` +0xa0,
  `leanAngle` +0xb0 (feeds `Roll` in bone transform), `moveTimer` +0x148 (`62*N` ticks), `spawnPhase`
  +0x2ac `[orig: Entity_UpdatePlayerInfantryMovement @0x483fe0; Entity_ProcessInfantryPhysics @0x46e100]`.
- **Net smooth-target / interp** (the §5.10/§5.38a cluster, now named in the struct) — `savedLivePose`
  +0x80 (`SpecialVec3`), `smoothTargetPos` +0x234 (`SpecialVec3`), `smoothTargetHeading/Pitch/Stage`
  +0x240/+0x244/+0x248 (**= the vehicle Euler triple eulerZ/X/Y on a vehicle entity, §5.13**),
  `interpProgress` +0x27c, `interpStepBucket` +0x27e. The local infantry motor reuses this cluster for
  post-respawn/teleport smoothing (same fields as the net read-apply path).
- **Mount / lifecycle / render** — `renderInstance` +0x64 (the render-instance ptr `Entity_Destroy`
  `memset`s 0x32C), `parentVehicle` +0x170, `overlayFlags` +0x178, `animChannelA/B` +0x18c/+0x188,
  `mountHandles[10]` +0x190 (the seat/attach handle array `Entity_Destroy` walks to detach),
  `effectHandle` +0x1b4, `ownerSession` +0x1cc, `targetHeading` +0x1a8, `thinkCooldown` +0x128
  `[orig: Entity_Destroy @0x43e810; Entity_KillByNetId @0x43dbd0 (+0x178); world-wac-ai-re.md §3]`.

**Extended AI/anim/aim region named (2026-06-23 pass 3 — `0x2b8–0x387`).** The post-`aiState` region
is now named for its INFANTRY/AI interpretation (`GamePlayerEntity` → 135 members, 96 named; size still
904). Witnessed via `Entity_UpdateInfantryAI @0x4b9910` + `Entity_BuildBoneTransformMatrices @0x4b1290`
+ `Entity_ResetToSpawnState @0x4b9610`, corroborated by `world-wac-ai-re.md §3` (entity[N]·4 = offset):
- **Anim state** — `animStateId` +0x2bc (entity[175]; indexes the state→name table `g_animStateFlagsTable` /
  `off_8135F0`), `animSlotIndex` +0x2c0 (`Entity_ComputeAnimSlotIndex`), `prevAnimStateId` +0x2c8
  (entity[178]).
- **Aim / torso / head** (incremental `+= chase` writes; reset to body yaw on spawn) — `aimPitch`
  +0x2d0 (entity[180]), `torsoYaw/Pitch/Roll` +0x2d4/+0x2d8/+0x2dc (entity[181-183]; feed the render
  Yaw/Pitch/Roll bones), `headLookYaw/Pitch` +0x2e4/+0x2e8 (entity[185/186]), `aimHeading` +0x2ec
  (entity[187]; render yaw chases it), `pitchBlend` +0x380 (added to render Pitch), and
  `pitchKickAccum` +0x36c — **not a head-look term**. The IDB name `headLookDecay` was a
  MISNOMER (head-look proper is the separate `headLookTarget` +0x344 system); +0x36c is a
  PITCH-OFFSET accumulator, driven by the arms-dip window +0x371 (§5.58), that lands in the
  entity's OWN Pitch at the held-weapon attach idiom — `Pitch = savedPitch + [0x36C] +
  2*[0x380]` `[orig: @0x4b1bd4..0x4b1bf5]`. Renamed to `pitchKickAccum` in the IDB and
  `pitch_kick_accum` across the port, 2026-07-27.
- **AI targeting** — `aimPoint` +0x30c (`SpecialVec3`, entity[195-197]; cast in
  `Entity_CheckGroundHeightAtPosition`), `aiFocus` +0x338 (`GamePlayerEntity*`, entity[206], the focus
  target), `headLookTarget` +0x344 (`GamePlayerEntity*`, entity[209]), plus `aiRef0/1/2`
  +0x2f0/+0x2f4/+0x350 (`GamePlayerEntity*` cross-refs cleared when the referent despawns).
- **Bone-walk** — `aimFlag` +0x360 (entity+864; gates the render-yaw aim chase), `boneWalkStage` +0x361
  (entity+865), `boneWalkSlot` +0x362 (entity+866), `boneIdxA/B` +0x366/+0x367.

**Polymorphic note (single-member naming):** these are the infantry/AI meanings; on a projectile/shell
entity the same offsets (`0x2bc+`) hold position/status state (`world-wac-ai-re.md §3`). `GamePlayerEntity`
is now substantially mapped — the residual padding is genuine gaps + sparse AI-brain scratch.

**Accessor-function finds (2026-06-23 pass 4).** A sweep of small, single-purpose `Entity_*` accessors
pinned more fields, each named for the function that reveals it (`GamePlayerEntity` now 111 named / 904):
- `groundEntity` +0x28 (`GamePlayerEntity*`) — the nearest entity directly below, cached from a downward
  10.0-unit raycast `[orig: Entity_CreateBoundingProxy @0x407d60]`; NULL on bare terrain; read as the
  support surface (is it a vehicle?) in `Entity_UpdateIdleCheck @0x408430`.
- `attachBone` +0x364 (u8) + `attachParent` +0x184 (`GamePlayerEntity*`) — the model userpoint named
  "attach" (index+1) and its pool-1 parent `[orig: Entity_FindAttachBone @0x4b9580]`.
- `renderInstance` +0x64 (void*, the 812-byte render/anim instance — distinct from the 172-byte
  `aiRuntime` +0x68 AI slot `[orig: Entity_AllocateAISlot @0x40d2c0]`, so NOT the AI brain).
- `modelPtr0/1/2` +0xa4/+0xa8/+0xac `[orig: Entity_SetDefaultModelPtrsA/B/C @0x4435a0/0x4435c0/0x443610]`;
  `orientationMatrix` +0xb4 (i32[9] fixed-point) + `animData` +0x158 (gate) `[orig: Entity_UpdateOrientationMatrix @0x43b440]`;
  `weaponSlots` +0x1d0 `[orig: Entity_GetWeaponSlotsPtr @0x510010 → entity+464]`; `aiTargetRefCount`
  +0x212 (u16) `[orig: Entity_SetAITarget @0x45d760]`; `mountedChild` +0x268 (`GamePlayerEntity*`, the
  attached passenger; passenger's +0x170 = `parentVehicle`) `[orig: Entity_AttachToVehicle @0x43c130]`;
  `damageTimer` +0x2f8 + `wasHit` +0x36b + `lastAttacker` +0x2f4 `[orig: Entity_OnDamageReceived @0x4af800]`;
  `collisionCallback` +0x2b8 `[orig: Entity_InvokeCollisionCallback @0x442350]`; `fireFlag` +0x2c6
  `[orig: Entity_InvokeFireCallback @0x442810]`.
- Callback refinement: `updateCallback` +0x1c4 is the per-frame update/physics fn ptr (`==
  Entity_UpdateShellBounce` for shells `[orig: Entity_IsShellProjectile @0x4e4040]`); `deathCallback` +0x1c8
  stays the death/lifecycle handler. **Polymorphic:** `animStateId` +0x2bc is invoked as a fire callback
  on weapon entities (`Entity_InvokeFireCallback`); `ownerSession` +0x1cc is **resolved (2026-06-25) as
  two disjoint meanings** — a `CNapiTransport` session ref on networked entities (`Entity_Destroy
  @0x43e810`: `if (+0x1cc) CNapiTransport_DetachFromSession`) AND an **effect-emitter handle** on
  effect-bearing props (`sub_4540E0 @0x4540e0`: `entity[115] = CEffectWorld_SpawnEmitterAtPosition(...)`
  for rope-trail def type 6088). The `@0x453580` callback (kong-misnamed `Entity_ClearWeaponTarget`,
  renamed **`Entity_ClearOwnerSessionIfMatches`**) nulls `+0x1cc` when it still equals the dying emitter
  — the effect-destroy cleanup hook, NOT a weapon-target clear. Field name `ownerSession` kept for the
  networked meaning.
- **`boundRadius` +0x0 is NOT padding** — the very first dword is the model **bounding-sphere radius**
  (`+0x1000` margin), the proximity/cull radius every `Entity_*` reads; marker/waypoint entities store
  their trigger radius there instead `[orig: Entity_InitFromModel @0x40dc30; Entity_SpawnFromBMSRecord]`.
  The same model-init also writes `bboxCenter` +0x1fc (`SpecialVec3`) + `bboxRadius` +0x208 (local
  collision bbox, scaled), `heatSig` +0x1a4 / `radarSig` +0x1a6 (from `ItemDef`), and `shadowSlot0/1`
  +0x1b6/+0x1b8 (shadow-decal slots). `weaponSlots` +0x1d0 is the 44-byte block before the bbox.

**0x0C organic spawn verified bidirectionally + name corrections (2026-06-25 grill).** The pool-0
0x0C organic spawn record was re-witnessed on BOTH sides — the WRITER `[orig:
serialize_entity_states_to_buffer @0x5030a0]` and the READER `[orig: NapiNPClientMsg_0x00C @0x42e730]`
— and they are field-for-field symmetric, re-confirming ~18 `GamePlayerEntity` members by witnessed
wire access in *both* directions: `Position` +0x4/8/C, `Yaw` +0x10, `Flags` +0x24 (low u16 on the
wire), `entityFlags` +0x78, `Name` +0xF4, `Team` +0x162, `aiState` +0x2B4, `animSlot` +0x374, `NetId`
+0x15C, `weaponState` +0x294, `aiAction` (`aiRuntime`+0x20), `refNum` +0x215, `subType` +0x214,
`weaponType` +0x157, `parentSlot` +0x168, `parentEntity` +0x16C. The record's leading `[u8
defType=itemDef->type][u16 itemId=itemDef->id]` pair is a non-zero has-body gate (0 ⇒ empty slot) plus
the type-id the reader feeds to `ItemList_FindIndexByTypeId`. One byte — `entity+0x154` (read-side
decompiler "unusedByte") — round-trips on the wire but its semantic is still open (currently inside
`pad_14c`). The IDB struct now carries **154 named members** (was 111 at pass 4), all consistent with
these witnesses. **IDB rename applied:** the kong-misnomer `Entity_SetStateWreckage @0x454d50` →
`EventTrigger_UpdateQuarterRoundRobin` (it processes ¼ of the global `trigger` array per
`Server_TickUpdate` frame via `EventTrigger_UpdateEntry`, NOT entity wreckage — already anchored in
[`mission/bms-event-runtime-re.md`](../mission/bms-event-runtime-re.md)).

### 5.2c Map spawn-marker selection — where the human player's pose comes from (2026-06-22)

§5.2a/§5.2b resolve how the host *builds and field-inits* its own player entity but leave the
"place `Position` / `Yaw`" step's SOURCE unwitnessed. It is the map/camera spawn selector
`[orig: Server_PositionPlayerForSpawn @ 0x50cf60]` (renamed 2026-07-03 from Kong's misnomer
`CMap_SetupSpawnCamera` — it positions the player ENTITY, not just a camera), called on join
`[orig: Server_OnPlayerJoin @ 0x51a680 → 0x51a786]`
and respawn `[orig: Server_ProcessPlayerDeath @ 0x517740 → 0x517863]`. It maps the game type
(`g_GameType @ 0x24D2128`) + the player's team to a marker TYPE-ID, resolves it via
`[orig: ItemList_FindIndexByTypeId @ 0x49e100]`, finds the matching pool-3 marker entities, and
copies the chosen marker's pose into the player. AI/NPCs take their authored BMS position verbatim
on a separate path `[orig: Entity_SpawnFromBMSRecord @ 0x40e9f0]` — so a faithful player spawn must
NEVER read an NPC's position.

**Game-type → marker type-id** (every `push imm; call ItemList_FindIndexByTypeId` in 0x50cf60):

| type-id | role | `g_GameType` branch |
|---|---|---|
| 6094 | co-op insertion | `(g_GameType & 0xFFFDFFFF) == 0x10020` |
| 6095 | non-team primary | non-team, attempted first |
| 6096–6099 | per-team starts (team 1–4) | `g_GameType & 0x10000` (team) |
| 6001 | co-op fallback | co-op only |
| **6002** | **non-team, non-coop = single-player / campaign / DM** | the SP fall-through |
| 6003 / 6004 / 6090 / 6091 | TDM team starts 1–4 | TDM alt path |

`g_GameType` bitfield: `& 0x10000` = team mode; `& 0x20000` = vehicle/coop insertion;
`(g_GameType & 0xFFFDFFFF) == 0x10020` = cooperative; small/zero = SP/campaign/DM. For single
player the selector attempts 6095 first and, with no 6095 markers, falls through to **6002**,
handing it to the distance selector.

**`Entity_FindBestSpawnPoint @ 0x50ccc0` — farthest-from-enemy.** For the resolved type it counts
pool-3 entities whose `entity+28` (the `gItemDefs` index from `ItemList_FindIndexByTypeId`, NOT the
raw type-id) matches; for each candidate the score is the min 2D distance to any pool-0 entity with
`Flags & 0x100` (the "avoid" / enemy set), excluding self `[orig: @0x50ce0d]`; a `CPairList`
shell-sort by score picks the farthest (`rand()` tiebreak when all are equidistant). The winner's
`pos` (`+4/+8/+12`) and orientation (`+16/+20/+24`) are copied into the player; `+16` is the
`(90 − yaw)` BAM heading authored at BMS load `[orig: Entity_SpawnFromBMSRecord @ 0x40eb42]`
(D-NET-86), so it is copied VERBATIM (no re-conversion). In SP the player's TEAM is NOT taken from
the marker — it only selects which type-id to resolve.

**Index vs raw type-id (the reimpl divergence, D-NET-87).** The original matches the items.def
INDEX at `entity+28` (`= ItemList_FindIndexByTypeId(type_id)`, written by
`[orig: Entity_SpawnFromBMSRecord @ 0x40ebfc]`). Our reimpl stores the raw BMS `type_id` in
`Entity.item_id` and has no items.def index; matching `item_id == 6002` is behaviorally identical
(`ItemList_FindIndexByTypeId` is injective on the unique `ItemDef.id`) and strictly safer — a
missing id resolves to index 0 in the original and can alias `gItemDefs[0]`, whereas the raw-id
match cannot. The type-ids are BMS-style `6xxx` used verbatim (no `+100000` items.def-id offset)
`[orig: ItemList_FindIndexByTypeId @ 0x49e120 compares gItemDefs[i].id (stride 2780, .id @+0x50) to
the raw arg]`.

**Real-mission machinery — the SP marker chain alone is incomplete (2026-06-22b).** Grilling
against a shipped SP mission (00TRa.bms, "Training: Basics / Armory", `attrib_flags=0x3` ⇒ no
game-mode bit ⇒ `g_GameType=0`) found the per-game-type chain above does NOT cover real authored
starts. `[orig: Server_OnPlayerJoin @0x51a680]` calls `Server_PositionPlayerForSpawn` with spawn-param
low-word **0** (not 0xFFFF), so the engine first tries `[orig: Server_ResolveSpawnTargetHandle @0x4fe110]` — which is a
live-entity **handle** resolver (`poolType = handle>>12`, `index = handle & 0xFFF`, fetch
`g_pool_list[poolType][index]`, gate on model flag `0x40000` + team), NOT a spawn-point lookup; at
join (handle 0, no live entity yet) it returns null and the marker chain runs. 00TRa ships ZERO
6002 markers and exactly ONE type-**6001** marker (+ 53× type-6005 waypoints), so the SP chain
`6095 → 6002 → Entity_FindBestSpawnPoint(6002)` finds 0 candidates and is a **no-op**
`[orig: Entity_FindBestSpawnPoint @0x50ccc0 zero-candidate epilogue @0x50cf53 — bare ret, no write
to entity+4/+8/+12]`. The 6001 marker is instead consumed by the broader start-point family
machinery: `[orig: build_entity_position_list @0x509660]` enumerates the family
`{6001,6002,6003,6004,6090,6091,6094-6099}`, and the dedicated 6001 reader
`[orig: CineEditor_FindSpectatorSpawn @0x41f25e]` copies the 6001 marker's X/Y/Z. The exact branch
that places 00TRa's player is runtime-`g_GameType`/pool-3-data-dependent, but every path points at
the single 6001 marker.

**Reimpl (the SP-as-listen-server fix, 2026-06-22; revised 2026-06-22b — D-NET-88).** Because a
byte-faithful port of the fragmented, data-dependent machinery is impractical (and the strict SP
path no-ops on a 6001-only mission), the reimpl UNIFIES it: `select_player_spawn`
(`libs/world/src/spawn_select.cpp`) scans the registry's promoted markers (`EntityKind::Marker`)
over the start-marker family priority list
`kSpawnMarkerStartTypes = {6002, 6095, 6094, 6001, 6096-6099, 6003, 6004, 6090, 6091}` — the FIRST
present type wins, returning the one farthest (mission 2D) from any live `EntityKind::Organic` (the
avoid set; the faithful `Flags & 0x100` set is approximated by live soldiers — at select time the
player has not spawned, so every organic is an NPC). A mission is authored for one mode, so
typically exactly one family type is present (00TRa → its 6001). `NovaSimulation::spawn_local_player_at_start`
feeds the result into the §5.2b `spawn_player`; `mission_runtime.gd` calls it instead of the prior
placeholder that read `get_entity_position(0)` (= the first promoted organic = NPC #0), which spawned
the player on top of the first soldier. No family marker → a safe fallback origin, never an NPC
position. **D-NET-88 divergence:** the unified family scan replaces the exact per-game-type
resolution (`g_GameType`-driven order, the `Server_ResolveSpawnTargetHandle` handle pre-check, the cycling-vs-farthest
distinction, the separate 6001/cinematic consumers, the `"psp"` bone offset + `+0x10000` Z nudge) —
all deferred. Guarded by `tests/world/spawn_select_test.cpp` (incl. the 6001-only 00TRa shape).

### 5.3 Tag direction asymmetry (durable warning)

Most/all tags have **both** a `NapiNPClientMsg_*` (S2C receive) and a `NapiNPServerMsg_*`
(C2S receive) handler, and the two usually implement **different protocol meanings** — they
share only the tag byte. When reading capture inventories, always note message direction; a
tag's appearance in one direction says nothing about the other. The 0x25-vs-0x1D confusion in
§5.2 is the canonical case of this asymmetry biting an investigation.

### 5.4 Tag 0x0B — 616-byte BMS header field map

All observed blobs are 616 bytes (the size hardcoded into the handler's copy into
`g_BmsHeaderBlock @ 0xA761D0` — renamed from `byte_A761D0` 2026-07-27; for a JOINER this wire
copy is the ONLY mission-identity source, §5.28 correction / D-NET-194). Cross-capture diff
over three maps (dvxi5 / dvxc1 / g11), sourced from the
fragment-aware dissector (`tools/wireshark/jointops_udp.lua`); never diff from
non-reassembled per-fragment extracts:

| Offset (dec) | Width | Field | dvxi5 | dvxc1 | g11 | Category |
|---|---|---|---|---|---|---|
| 0-3 | 4 | Signature | `42 4d 53 13` | same | same | structural (invariant) |
| 4-35 | 32 | Mission display name | "AS - Dormant Volcano Isle" | "TD - Tenaga Delta" | "AS - Kendari Airport" | map-specific |
| 36-67 | 32 | Designer | "Brophy / Brent / BB" | "Brent Houston / James Payne" | "Brent" | map-specific |
| 68-99 | 32 | Map basename | "dvxi5" | "dvxc1" | "g11" | map-specific — `Bms_MapBaseName @ 0xA76214`, the env/TOD-config key (`Terrain_LoadEnvironmentConfig @ 0x610940` arg1 → `Environment_LoadTimeOfDayConfig @ 0x57db30`). **NOT "the BMS file the client must load"** — a joiner never opens a `.bms` (corrected 2026-07-27, §5.28 / D-NET-194) |
| 116-127 | ~12 | "Default" + padding | "Default\0..." | same | same | structural (invariant string) |
| 136 | 1 | Game-mode primary | 0x03 | 0x03 | 0x02 | map-specific (3=AS-asym, 2=AS-sym?) |
| 138 | 1 | Game-mode secondary | 0x01 | 0x00 | 0x01 | map-specific |
| 139 | 1 | Game-mode tertiary | 0x00 | 0x20 | 0x00 | map-specific |
| 154 | 1 | numeric (count?) | 21 | 18 | 0 | map-specific (spawn count / team size?) |
| 158-159 | u16 | numeric | 700 | 400 | 1000 | map-specific (round time / score limit?) |
| 164 | 1 | numeric | 76 | 47 | 144 | map-specific |
| 168-169 | u16 | numeric | 1102 | 1145 | 1153 | map-specific (likely entity count) |
| 172-173 | u16 | numeric | 432 | 436 | 558 | map-specific (entity count secondary?) |
| 184 | 1 | flag/count | 2 | 0 | 11 | map-specific |
| 192-215 | 24 | 12-slot ID table? | filled with `ff` | zeros | zeros | map-specific |
| 246-253 | 8 | Icon key | "full_00" | same | same | structural (invariant string) |
| 276-307 | 32 | Atlas key | "trntile10" | "trntile10" | "trntilea1" | atlas-specific — the tile-SET basename (field anchor +0x118 = `Bms_TileSetName @ 0xA762E8`, renamed from the misnomer `Bms_TerrainBaseName` 2026-07-27): derives `<name>.TGA` / `<name>.TSD` (ext @ 0x7df3e4) and feeds `XML_ParseTileInfo @ 0x4cc830` |
| 308-615 | 308 | tail metadata + zeros | sparse non-zero | mostly zeros | mostly zeros | map-specific |

Consequence: any fixture blob of this tag is **map-locked** (the basename at +68 names the BMS
the client loads); a server must synthesize it from the actual mission selection.

### 5.5 Tag 0x11 bundling policy (retail)

Retail emits S2C 0x11 **only** inside the BMS-state bundle, always last, never standalone —
verified in both capture3 (frames 197227/197228, the only S2C 0x11 emissions) and capture4
(frames 163797/163798): bundle signature `[0x1C(0), 0x0B(616), 0x66(1), 0x76(2), 0x11(0)]`.

Why it matters: 0x11 sets `dword_A82358 = 1`, which unblocks WaitForDisconnect; the client
then immediately runs `Game_LoadTerrainDuringConnect`, which reads the mission basename out of
the tag-0x0B header copy (`byte_A761D0+0x44`) via `Terrain_LoadEnvironmentConfig`. Sending 0x11 before the 0x0B
header is delivered makes that load fail → unnumbered "Mission loading aborted" → disconnect
~2-3 s later. A reimplementation must deliver 0x0B and 0x11 in the same packet (0x11 last) or
at minimum strictly after 0x0B.

At the network seam, receipt and ACK of this terminal `0x11` is the safe pre-world
`preload_ready` boundary, **not** permission to begin the world stream. While the synchronous
terrain/mission load runs, the client keeps the authenticated session alive but withholds both
the empty C2S `0x0A` spawn-menu request and the later loadout/status submit. Only after the local
world is installed does C2S `0x0A` advance the host's sync state 3→4 and release the §5.2a
stream. The loadout remains a separate reactive leg after the stream's S2C `0x1A` terminator
(§5.0d).

### 5.6 Tag 0x0D — local-player spawn dead end (durable warning)

Do **not** use S2C 0x0D to spawn the local player. Verified by live crash + decompile
(2026-04-25) and corrected by the §5.11 grill (2026-06-16):

- When the record's item def has the AI flag (`ItemDef[+84] & 0x100000` — true for player
  infantry type_id `0x14B9`), the handler `[orig: NapiNPClientMsg_0x00D @ 0x432C40]` runs an
  `Entity_AllocateAISlot @ 0x40D2C0` path that string-copies from pointers populated **only**
  by the optional `flags & 0x800` trailer block. Without the trailer those pointers are NULL →
  access violation at handler+0x730 (`0x433370`).
- The trailer layout is **`[u32 aiProfile1][u32 aiProfile2][cstring aiName]`** (cross-witnessed
  in 195 of 437 retail 0x0D records from the 2026-06-16b loopback — full table §5.11). An
  earlier note here recorded the first field as a `u16`; that was wrong — both pointer/integer
  fields are read with `cursor += 2` on a `uint16_t*`, which advances 4 wire bytes each (D-NET-52).
  `aiProfile1` lands at `aiSlot+16`, `aiProfile2` at `aiSlot+20`, `aiName` at `aiSlot+156`.
- Retail's S2C 0x0D records always use vehicle/AI type_ids (`0x04bf`, `0x050b`, ...) with
  flags like `0x1c21`/`0x1071` — bit `0x800` always set, never the local-player template.
  Tag 0x0C does not crash on the same type_id because it parses name fields inline.
- The earlier theory that 0x0D was "the only wire route" to clear the local player's
  entity[+36] movement gate was wrong. `[orig: Player_BuildTag0CInputBody @ 0x42A550]` checks
  three gates before serializing player input: entity buffer non-NULL, `entity[+286]` non-zero
  (= `ItemDef.healthMax`, §6.9), and `entity[+36]` bit 1 clear. The remaining unsolved spawn
  blocker pointed at `entity[+286]` init paths and the `dword_A82370` progression (§5.1), not
  at +36 — and `entity[+36]` bit 1 is now witnessed as cleared by the spawn-state reset
  `[orig: Entity_ResetToSpawnState @ 0x4B9610]` (§5.2b), not by any wire message.

### 5.7 Tag 0x18 — superseded by §5.46

Early finding: S2C 0x18 "does not fire in normal multiplayer", and the reverted stack's speculative
emission was removed. Both halves are now fully witnessed (§5.46): 0x18 is the REPLY to a C2S 0x0F
entity-info query — the client's self-heal request for a stale/mismatched entity. It is absent from
healthy sessions because nothing needs healing, not because the `NapiNPServerMsg_0x00F → 0x18` path is
inert; a host that leaves 0x0F unanswered strands a broken client entity forever (D-NET-133).

### 5.8 Wire-format verification gaps

Tags emitted by the reverted stack but never byte-compared against retail: 0x60 and 0x64
(file-transfer chunks; a format error makes the client request retransmits forever). **0x0A and
0x10 are now witnessed end to end — see §5.9. 0x16 (per-player record layout) and 0x46 (bitfield
read order) are now byte-compared against the controlled "ON RE Probe AS dvxi5" loopback (all 33
0x16 + all 51 0x46 records decode to the byte) — see §5.20 / §5.21.** Cross-capture diffs still
pending for 0x0F, 0x60, 0x64, 0x7B (+13-byte size delta vs the reverted builder).

### 5.9 Core in-game replication loop — field maps (loopback capture 2026-06-16)

Witness source: a clean loopback capture of a real host+join+play session (both `Jointops.exe`
instances on one host; in-game session `:32768` host ↔ `:32769` joiner, `PN="JOINTOPERATIONS"`,
map "AS - Laba-Laba Archipelago" / `ASH_I1EA.BMS`). Decoded end to end through the shipping libs
by `tests/novaworld/nw_ingame_histogram_test.cpp` (envelope → outer NWU → per-session SCRK →
0x43/0x83 → msg_id dispatch): **0 decode failures over 439 datagrams**, both 61-char SCRKs
recovered. Confirms the JointOperations hello carries `PV1="0.0.0 1/12/2004 EM"` (vs the lobby's
`2/10/2004` — D-NET-47) and that S2C 0x0B is the 616-byte `42 4d 53 13` BMS header (§5.4), here
for `ASH_I1EA.BMS`. Observed phases: **load** (0x0B → pools 0x10/0x0D/0x20 → 0x45) then
**gameplay** (per-frame 0x0A ↔ C2S 0x0C, plus 0x40/0x6F, RTT 0x57↔0x2C, 0x26).

**Second capture, 2026-06-16b** (witness source for §5.10): host+join+play brokered by our
NovaWorld matchmaking (`PN=NOVAWORLDUDP` for the lobby session → `PN=JOINTOPERATIONS` for the
in-match session), both peers retail `Jointops.exe`, same Laba-Laba map. 527 datagrams,
**0 decode failures**, both 61-char SCRKs recovered. The joiner spent the session on foot
AND in a vehicle (handle `0x108b` = pool 1 / slot 139) — provides the cross-witness oracle
for the per-entity-type callback (§5.10), including its world-vs-vehicle-local position branch.
A reusable `tshark`-backed converter ships at `tools/net/pcap_to_hexcap.py` so any future
`.pcapng` produces the hexcap format `nw_ingame_histogram_test` consumes.

**Entity handle encoding — `(pool << 12) | slot`.** Every in-match entity reference is a `u16`:
high 4 bits select the pool, low 12 the slot, resolved as
`g_pool_list[h>>12].base + g_pool_list[h>>12].stride * (h & 0xFFF)`
`[orig: dispatch_entity_packet_callback @ 0x4D6A80; NapiNPClientMsg_0x00A @ 0x42FEC0]`. `0xFFFF`
= "none"; `(h & 0xF000) >= 0x5000` is treated as invalid.

**Tag 0x10 (S2C) — static entity batch** `[orig: NapiNPClientMsg_0x010 @ 0x433400]`. Pool-2
batch; each entity is zeroed before fill. Header `[u16 startIndex][u16 entityCount]`, then per
entity:

| Field | Type | Presence | Stored |
|---|---|---|---|
| itemTypeId | u16 | always (`0` ⇒ empty slot, record ends) | — |
| fieldFlags | u16 | always | — |
| posX/Y/Z | 3× i32 | always | entity+4/+8/+12 (16.16 world) |
| eulerZ / eulerX / eulerY | i32 (32-bit BAM) | `flags & 0x01 / 0x02 / 0x04` | entity+16/+20/+24 |
| sectionMask | i32 | `flags & 0x08` | entity+308 |
| team | u8 | `flags & 0x10` | entity+354 |
| entityFlags | u32 | `flags & 0x20` | entity+36 — the entity FLAGS dword streamed raw (the old "parentSlot" reading was WRONG; witnessed at the serializer source `[orig: serialize_pool2_static_to_buffer @0x5042F0 @0x5044e6]`). Composed at spawn from BMS attributes (Indestructible 1<<21 → 0x4000000, Reflective 1<<23 → 0x400, NoShadow 1<<24 → 0x1000000 `[orig: Entity_SpawnFromBMSRecord @0x40e9f0]`) + def traits (type Building → 0x20000 `[orig: Entity_InitFromModel @0x40e105]`; healthMax 0 → 0x4000000 `[orig: @0x40dc8e]`). Golden ASH_I5A buildings: 0x04020400; bridges add NoShadow → 0x05020400. D-NET-147 |
| ammoCount | u8 | **always** | entity+290 (u16) ← BMS record byte 81 `[orig: @0x40e9f0]` |
| refNum / subType | u8 | `flags & 0x40 / 0x80` | entity+533 / +532 (D-NET-94; not bones). refNum ← BMS byte 153; subType = 0xFF when the def is indestructible (healthMax 0) `[orig: @0x40dc8e]` |
| scoreFlag | u8 | `flags & 0x100` | entity+624 (i32) |
| weaponByte | u8 | **always** | entity+538 |
| attachRef | u16 | `weaponByte != 0 \|\| flags & 0x200` | entity+350 |

Then `ItemList_FindIndexByTypeId` resolves the def (health → entity+452) and allocates
section/action slots. Cross-witnessed: capture record 0 = itemType `0x044a`, flags `0xa1`, pos
`(-2189.6, 1626.9, 25.2)` world units; the next record `0x044c` lands exactly where the layout
predicts.

**Tag 0x0A (S2C) — per-frame local-player + world-state update** `[orig: NapiNPClientMsg_0x00A @
0x42FEC0]`. NOT a generic entity snapshot. The host (authority) returns right after the 12-byte
header; the rest is client-side.

| Field | Type | Notes |
|---|---|---|
| ref0/ref1/ref2 | 3× i32 | tick/reference header → dword_A822E4/E8/EC |
| flags1 | u8 | Write side `[orig: NetPacket_WritePlayerState @ 0x4ff793-0x4ff7dd]`: bit0 = spectator (slot+100567; also ORs entity+36 bit0), **bit1 = RESPAWN-PENDING (slot+89912 & 0x10)** — re-asserted EVERY frame while the recipient is undeployed (also ORs entity+36 bit0 → the record byte13 `0x01`), bit2 = one-shot load hint (entity+44 & 0x1000 && dword_2550850, clears the entity bit). Read side: `0x04`→loadprog `dword_A8235C=10`; **`0x02`→`g_deploy_screen_active @ 0xA860DC` = `(flags1 & 2) != 0` EVERY frame — the deploy screen is HELD open by bit1; one bit1=0 frame closes it** (@ 0x42ff82; D-NET-156); `0x01` EDGES drive `g_death_screen_active @ 0xA860EC` (0→1 opens the death/spectator screen — camera `CameraOffset.Z=0xD000`, tip 22, spawn-gate `dword_24C1928`; 1→0 closes + latches `byte_A85B48 = player.Team`) |
| flags2 | u8 | low 2 bits select the sub-block AND its ROLE: **0** = aim/player-view (client-authoritative — the working golden does NOT send it in gameplay), **1** = server-status/timer, **2** = ENV, **3** = objective (gametype-gated). The retail counter free-runs and cycles all four. bit 3 (`0x08`) gates a 6 B vehicle-passenger record after the fixed tail (joiner-as-passenger; only the `(flags2 & 0xF) == 8` exact value triggers it — i.e. sub-block 0 + passenger bit) [orig: 0x430459] |
| sub-block 0 | `==0`: 6× u8 + u8 (`0xFF` sentinel) + i32, 11 B | player aim/state → dword_A85B5C… [orig: 0x430054..0x43012E] |
| sub-block 1 (server-status/timer) | `==1`: 4× u8 + i16, 6 B | `[u8→dword_C6EAE0][u8→dword_C6EAE4][u8→g_serverFps (0xC8FC64)][u8→g_serverCpuPct (0xC8FC68)][i16 timer]`; each u8 is `movzx`-widened from one wire byte; `dword_24C1958 = 62 × i16` (62 Hz timer; `-1` if negative) [orig: C6EAE0@0x4301a1, C6EAE4@0x4301bc, g_serverFps@0x4301e0, g_serverCpuPct@0x430200, timer@0x430210]. Init defaults [orig: 0x4f638b C6EAE0=20, **C6EAE4=13**, C6EAE8=10]. **`dword_C6EAE4` is the fall-damage tolerance (§5.38d)** — a server that only ever sends sub-block 0 leaves the client's C6EAE4 at 0 → per-frame fall damage [orig: read @0x4b7d0d] |
| sub-block 2 (ENV) | `==2`: u16,u16,u16,u8,u8,u8,u8,u8, 11 B | `Env_FogDistTarget=u16<<16`, `Env_FogDistAccelClamp=u16<<8`, `Env_CurTimeFixed24=u16<<13` (TOD), `Env_QuakeTicks`, `Env_CloudScrollRateTarget=u8<<10`, u8<<8, `Env_OvercastBlendTarget=u8<<8`, u8 [orig: 0x430253..0x430341] |
| sub-block 3 | `==3 && g_GameType & 0x20000`: 4× i32, 16 B (else 0 B) | `won, lost, show_win, show_lose` → dword_AC86F4/F0/EC/E8. The gate is wire-invisible, so `decode_frame_update` reads the body only when its `is_objective_gametype` hint is set. **First witnessed in probe3** (Co-op, `g_GameType 0x30020`; 771 frames, body all-zero); the `flags2 & 0xF0` high bits don't change sub-block selection (D-NET-75) [orig: 0x430361..0x4303D0] |
| state_flag_byte | u8 | **The recipient's OWN stance echo**: bit 0 (prone)→`dword_B76484`, bit 1 (crouch)→`dword_B76480`, bits 0/1→`g_local_player_entity` MoveOrder (+0x12C) bits 8/9 — re-latched EVERY frame (@ 0x430562/@ 0x430570), so a host that hardcodes 0 force-STANDS a crouched client each frame (witness 2026-07-03; the pre-v32 crouch/prone bug). The authoritative source is the server's per-player stance from C2S 0x1D (dispatch table) — vehicle attach/detach clears it (@ 0x435c54/@ 0x43561e). (The `<<8` of older notes was the receiver's internal shift, not a wire-format detail) [orig: 0x4303E5] |
| mountHandle | u16 | vehicle-mount handle (`pool<<12\|slot`; `0xFFFF`=none) [orig: 0x430408] |
| health | i16 | read @0x430428; applied late in the handler — compares the new value to the stored `Health` (`cmp dx,[entity+0x11E]` @0x43059a, `jge` skip @0x4305a1) and, ONLY when it DROPPED, fires INLINE COSMETIC feedback (red flash `dword_B764B4+=0x78` @0x4305a3, camera-shake `dword_B764B0+=0x0A` @0x4305c1; both capped 0xFF; **no `Radar_AddBlip`** — distinct from the body motor's `Player_OnDamageReceived`, §5.38d) — then stores `Health` @0x4305df. ⇒ stream this at full health (`healthMax`) or any below-stored tail self-triggers the flash [orig: 0x430428 / 0x43059a / 0x4305df] |
| state_word | i16 | → `*(WORD*)g_local_player_entity->pad7` (packed state) — bytes 6/7 of the 7 B tail; previously misread as two separate `hdr_trail_a/b` bytes [orig: 0x430442] |
| **passenger record (conditional, `(flags2 & 0xF) == 8`)** | 6 B (or 2 B early-skip) | — |
| passenger_handle | u16 | passenger entity handle; `0xFFFF` early-skips the next 4 B [orig: 0x430474] |
| seat_yaw | u16 | rider body yaw [orig: 0x4304C3] |
| seat_pitch | u16 | rider body pitch [orig: 0x4304DC] |
| event loop | trailing `[u8 tag]…` | tags exactly `{0,1,2}` — `cmp eax,2 / jg` at `0x4306DA` treats any tag ≥3 as silent terminator (same exit as `tag==0`); `tag==1`→`[u16 handle][u16 typeId]` then per-class callback (§5.10b); `tag==2`→§5.9.1 round event; ends at `tag==0`/EOB/tag≥3 [orig: 0x4306A1, handle@0x43070C, typeId@0x43076B] |

**Objective-phase reimplementation (2026-07-21).** The phase-3 writer now emits the four live
`World::subgoals` masks in retail order whenever `game_type & 0x20000`; non-objective modes retain
the faithful zero-byte body. A joiner learns the off-wire gate from the initial S2C `0x08` session
config field 3 and the later S2C `0x7B extra`; the host loopback receives the configured game type
directly. `NetClientView` also consumes both metadata tags itself for chronological capture/replay
folds, while a replay starting midstream can seed `game_type` through `ClientRuntime::seed_session`.
The view commits the masks and advances its objective revision only after all 16
bytes decode, so a truncated phase-3 datagram cannot replace the last authoritative snapshot with
partial/default values. `NovaSimulation` mirrors fresh masks into the joiner's `World::subgoals`,
which is already the objective-HUD source. Codec, truncation, fanout, runtime, and two-simulation
coverage lives in `nw_ingame_encode_test`, `netsim_client_view_class_resolver_test`,
`netsim_two_peer_fanout_test`, `npruntime_client_runtime_test`, and `coop_two_sim_test.gd`.

(The handler was undefined in the IDB — a data blob mis-marked at the `0x430000` page boundary;
defined 2026-06-16. Sub-block + tail field maps fully witnessed 2026-06-16c via
`NapiNPClientMsg_0x00A` grill: the previously empirical `hdr_trail_a/b` 2-byte trailer is refuted
— it's the high half of the `state_word i16`. The `(flags2 & 0xF) == 8` vehicle-passenger record
appears 4× in the 2026-06-16b loopback capture, all on sub-block 0 frames.)

#### 5.9.1 Round-event record (event-loop `tag==2`) — wire decoded 2026-06-16d; server side witnessed 2026-07-03 (D-NET-152)

The trailing event loop's `tag==2` branch is a **fired-round EVENT — the fire origin +
direction of a round shot by another player — not an impact record** (the 2026-06-16 "weapon
hit / impact" reading was a decode-era guess; every "impact" label below was really the
muzzle). The receiving client re-simulates the round locally from origin + direction
(`RoundData_SpawnRound @ 0x4EC0D0` — spread, velocity, tracer/projectile spawn), which is why
no impact ever needs to be on the wire. Client read side:
`NetPacket_DeserializeRoundEvent @ 0x42F270` (renamed from `…DeserializeWeaponHit`) — sole
receiver, called from `0x4306EF` inside `NapiNPClientMsg_0x00A`. Host write side (witnessed
2026-07-03): `NetPacket_SerializeRoundEvent @ 0x504820` (renamed from
`serialize_projectile_to_packet`) serializes one `g_round_ring @ 0xC8D848` record. The record
is 17-20 B, variable by `flags` gate bits `0x80` / `0x40`:

| Field | Bytes | Gate | Source (host write @0x504820) → landing (client read @0x42F270) |
|---|---|---|---|
| `flags` | u8 | always | ring+30 = the C2S 0x06 fire-mode byte (bit0 alt-fire, bit1 adm-indexed, bits 4-5 = the pre-consume MountSlot+0x10 magazine count's low two bits) `\| 0x40` iff the shooter's live fire target is set [orig: 0x5048c1] `\| 0x80` iff ring+32 non-zero [orig: 0x5048c8]; client: bits 0/1 select two MUTUALLY EXCLUSIVE arms, and bit 0 is tested FIRST and wins - bit 0 SET takes the AMMO-DEF arm (`ammoDef+64` fire sound `[orig: 0x42f5dc]`, `ammoDef+68` effect spawned at the wire position `[orig: 0x42f6c2]`); only with bit 0 CLEAR is bit 1 tested, and bit 1 alone takes the ADM arm (`AdmDef_GetEntryByIndex`, then `ActionSlot_ExecuteAction` on the addressed def's action rows - NO ammo-def leg at all). Each arm still calls `RoundData_SpawnRound` exactly once `[orig: @0x42f5ba ammo arm, @0x42fa6c adm arm]`. `[orig: flags read @0x42f2a8; bit-0 test @0x42f50b -> branch @0x42f521; bit-1 test @0x42f6cc -> branch @0x42f6ce]` |
| `adm_index` | u8 | always | ring+33 → `AdmDef_GetEntryByIndex(adm_index)` resolves the weapon [orig: 0x42f2ca] |
| `subtype` | u8 | always | ring+31 = the shooter fire-context composite `(extra_byte2 & 0x3F) \| ((extra_byte2>>7)<<7)` [orig: @0x50bd83 / roundParams[4] @0x50c7bd] → `dword_A822E0` (ex `hit_subtype`) [orig: 0x42f2e2] |
| `slot_byte` | u8 | `flags & 0x80` | ring+32 = the **PowerThrow CHARGE byte** (grilled 2026-07-24: `RoundData_AddRound @0x4fdcfc` stores params+20 = the fire descriptor +20 ← `MountSlot+0x5C` via `WeaponAction_Fire` arg 6 @0x4ec5bb — the local "damage_type" stack name is a misnomer; for the C2S 0x06 leg the same byte is the uplink `misc_byte`). A charged host throw therefore DOES replicate its charge on the flags\|0x80 leg; captures without 0x80 simply contained no charged throws. [orig: 0x4fdcfc] → `hitDataPtr[5]` low byte [orig: 0x42f30a] |
| `shooter_handle` | u16 | always | ring+4 — **the SHOOTER** `(pool<<12)\|slot` (`RoundData_AddRound @ 0x4fdca8` resolves the shooter entity, NOT the hit target — the ex-`target_handle` reading was wrong); the client resolves it as the round's owner (team tracer color, attribution) [orig: 0x42f337] |
| `target_handle` | u16 | `flags & 0x40` | the shooter's claimed fire target, read LIVE off `shooter+104→+12` at serialize time [orig: 0x50485a] (stamped per accepted 0x06 [orig: @0x50c2ad]; ex `weapon_handle`) → `entity_link+12` [orig: 0x42f359] |
| `shot_seq` | u16 | always | ring+28 — per-shot sequence; the C2S 0x06 `hit_part` fire counter round-trips here via `word_B7C670` → the spawned round's +120 word (ex `damage_extra_raw`; the monotonic per-shot counter observed 2026-06-16d) [orig: @0x50c2ba → 0x4fdcf5 → 0x42f37e] |
| `pos_x_compressed` | u16 | always | `Network_CompressFixedPoint(origin_x − g_priority_ref_x)` — the FIRE ORIGIN vs the recipient-eye anchor (the same refs the 0x0A header carries) [orig: 0x504994] → decompress + `dword_A822E4` [orig: 0x42f39c] |
| `pos_y_compressed` | u16 | always | origin_y − ref_y [orig: 0x5049be / 0x42f3c7] |
| `pos_z_compressed` | u16 | always | origin_z − ref_z [orig: 0x5049e8 / 0x42f3f2] |
| `yaw_bam_high` | u16 | always | `(ring+20 + 0x8000) >> 16` — the FIRE DIRECTION yaw BAM32 high word (ring stores the C2S 0x06 raw `dir_x << 16`) [orig: 0x504a18] → `<< 16` on apply [orig: 0x42f41f] |
| `pitch_bam_high` | u16 | always | `(ring+24 + 0x8000) >> 16` — fire direction pitch [orig: 0x504a3c / 0x42f43c] |

**Closed byte-sum table by flags combination:**

| flags & 0xC0 | slot_byte? | target_handle? | total |
|---|---|---|---|
| `0x00` | no | no | **17 B** |
| `0x80` | yes (+1) | no | **18 B** |
| `0x40` | no | yes (+2) | **19 B** |
| `0xC0` | yes (+1) | yes (+2) | **20 B** |

After deserialization the receiver takes ONE of two **mutually exclusive** arms, bit 0 tested
FIRST and winning `[orig: @0x42f521]`:

- **bit 0 set - the AMMO-DEF arm.** Presents from the ammo def, at the WIRE FIRE POSITION:
  the `ammoDef+64` sound `[orig: @0x42f5dc]` and the `ammoDef+68` effect `[orig: @0x42f6c2]`.
- **bit 0 clear + bit 1 set - the ADM arm** `[orig: @0x42f6ce]`. Spawns NO ammo-def leg; it
  calls `ActionSlot_ExecuteAction` on the addressed def's action rows instead - four call
  sites across two sub-paths selected by the shooter's `parentSlot` (`entity+0x168`):
  `[orig: @0x42f777 / @0x42f785]` and `[orig: @0x42f98f / @0x42f9d0]`. Rows `def+684` and
  `def+688` are `actions[2]` fire and `actions[3]` recoil (the 12-slot array at `def+676` in
  `g_weaponActionTable @ 0x830B90` order).

(Corrected 2026-07-27: the earlier "`flags & 1` enters the alt/projectile branch, `flags & 2`
the standard round branch" implied two parallel spawn styles - they are one exclusive
selection, and only the bit-0 arm spawns an ammo-def sound or effect. Running the ammo-def leg
on both is what drew every remote muzzle flash at the shooter's EYE; see D-WPN-33.)

Both arms still call `RoundData_SpawnRound` (renamed from `…ProcessHit` — it spawns the round;
no hit is processed at fire time) with the (`origin`, `shooter`, `adm_index`, `flags`,
`subtype`, `slot_byte`) tuple. The yaw/pitch BAM words feed the round's trajectory and the
FOV-cone fire sound.

**The server-side staging chain (witnessed 2026-07-03, D-NET-152).** Per recipient, per
frame, inside the §5.47 emit:

1. **`RoundData_AddRound @ 0x4FDB40`** — the ONLY ring writer — appends a 36-B record to
   the 256-entry ring `g_round_ring @ 0xC8D848` (cursor `g_round_ring_cursor @ 0xC8FC4C`,
   saturating count `g_round_ring_count @ 0xC8FC48`, reset by `Game_StartMission
   @ 0x525b7a`) and spawns the authoritative round via `RoundData_SpawnRound` inline. The
   ring stores the PRE-SPREAD origin/direction (read from the fire request before the
   spawn applies weapon spread [orig: @0x4fdbce]) and stamps ring+0 = `stat_id @ 0xC86FB0`.
2. **`Server_BuildRoundEventListForPlayer @ 0x4FFEE0`** (renamed from the
   `compute_entity_angular_priority` misnomer; called from `Server_BuildEntityPriorityList
   @ 0x50e59c`) — walks ring records with `stat >` the recipient's watermark
   (`playerSlot+97544`; its non-zero gate arms a fresh player at the current stat so the
   pre-join backlog never replays), **skips rounds whose SHOOTER == the recipient**
   [orig: @0x4fff97] (your own rounds are never echoed back — the firing client already
   simulated them, see §5.16), scores each by the recipient's perpendicular distance from
   the round's LINE OF FIRE (x87 double trig scaled 2^22; projection clamped to
   [0, 1000u]; z half-weighted; `score = 0x4000 − lateral>>12`, floor 0 [orig: @0x500115]),
   shell-sorts descending, writes up to 255 record ptrs to `g_round_event_refs @ 0xC863A0`,
   and stamps the watermark = `stat_id` [orig: @0x5001ae].
3. **`serialize_entity_states_to_packet @ 0x50F070`** — the event loop interleaves ONE
   tag-1 entity record and ONE tag-2 round event per iteration under the shared
   `g_entity_send_budget` [orig: tag-2 write @0x50f326 → NetPacket_SerializeRoundEvent
   @0x50f331], `[0]` terminator.

The whole-path consequence: a round fired by client A reaches client B (origin + direction,
compressed vs B's own eye anchor) and B's engine re-fires it locally — tracer, sound,
impact, and cosmetic damage all client-computed; the HOST's authoritative damage runs in
its own `RoundData_SpawnRound` projectile (impact handlers `Projectile_Handle*Impact
@ 0x4e93xx` — next-round scope).

**Cross-witness (capture `host_and_join_game_on_opennovaworld_loopback_threeplayers_more_gameplay.pcapng`,
2026-06-16d), relabeled 2026-07-03.** 20 round events across 672 0x0A frames, all decoded
byte-exact, zero walker halts. Observed flag bytes: `{0x02, 0x12, 0x22, 0x32}` — exactly the
fire-mode byte shape `((preConsumeClip & 3) << 4) | 2` the fire action builds from
MountSlot+0x10 before calling `consume_weapon_ammo` [orig: WeaponAction_Fire
@0x542c11 / consume @0x542c75], none with 0x80/0x40. Two distinct (`adm_index`, `subtype`)
tuples now read correctly as the two SHOOTERS and their weapons: `(68, 12)` × 13 rounds by
`p0/s1`; `(7, 12)` × 7 rounds by `p0/s0`. `shot_seq` increments monotonically per shooter
(`0x020b…0x0217`, `0x0005…0x0006`) — the per-shot fire counter (= the C2S 0x06 `hit_part`
field, §5.16). Sample wire bytes — first record (17 B):

```
02 44 0c 01 00 0b 02 9a 44 38 6d d6 ea bc 73 02 f9
flags=02 adm=44(=68) sub=0c(=12) shooter=0x0001 shotSeq=0x020b
origin=(0x449a,0x6d38,0xead6) yaw_BAM=0x73bc pitch_BAM=0xf902
```

The decoder is bounds-checked end-to-end (libs/npwire §5.9.1
`decode_round_event_record`, ex `decode_weapon_hit_record`) and the nw_pp walker advances
past tag==2 records to keep decoding the rest of the frame. REIMPL (D-NET-152): the host
side is ported — `world::RoundRing` (round_ring.h), the per-connection watermark + the
line-of-fire scoring in netsim `select_round_events`, and the `RoundEventRecord` codec
rename sweep (struct/codec/nw_pp/replay/tests; old nw_pp dumps show the pre-rename
`weapon-hit`/`target`/`dmgExtra` labels). Live v28: a single-observer session correctly
carries ZERO tag-2 (the idle host is the in-process connection and own rounds are skipped
`@0x4fff97`) — the positive fan-out witness is the `netsim_two_peer_fanout` pin until a
second observer client joins a capture (D-NET-152).

**Tag 0x0C (C2S) — entity sub-packet** `[orig: NapiNPServerMsg_0x00C @ 0x501C30 →
dispatch_entity_packet_callback @ 0x4D6A80]`. The joiner's per-frame uplink for an entity it
owns; authority-gated, ownership-checked, then dispatched to the entity-type callback at
`entity_def+356`.

| Field | Type | Notes |
|---|---|---|
| entityHandle | u16 | `pool<<12\|slot`; server verifies it equals the sender's owned entity |
| itemTypeId | u16 | e.g. `0x14B9` = player infantry (§5.6) |
| sub_opcode | u8 | format tag: `10`=extended (this packet's path), `11`=compact (§5.10) |
| payload | `len-5` B | → entity-type callback at `entity_def+356` (§5.10 — player-class fully witnessed) |

The per-entity-type callback at `ItemDef+356` is fully decoded in §5.10 for the player/infantry
class; other classes (vehicle/AI/weapon) still TBD.

### 5.10 Per-entity-type serialize callback at `ItemDef+356` (loopback capture 2026-06-16b)

Resolves §5.9's open thread. For the player/infantry class (`ItemDef.id == 0x14B9`, id @
`ItemDef+0x50`), the callback pointer at `ItemDef+356` (= `0x164`, inside `ItemDef.pad_130[52]`)
is **`NetPacket_SerializePlayerState @ 0x4C09C0`** — one function with a 4-way switch on
`ctx.mode` (`[esi+0x18]`):

| mode | sense | format | wire user |
|---|---|---|---|
| 1 | write compact | type 11 | server → S2C 0x0A trailing event-loop `tag==1` per-entity record |
| 2 | read compact | type 11 | client ← same record |
| 3 | write extended | type 10 | joiner → C2S 0x0C body |
| 4 | read extended | type 10 | host ← same |

Both directions share one callback; the `sub_opcode` in the §5.9 tag-0x0C header (or the
literal `format` field the calling context stamps for 0x0A) selects compact vs extended. The
5-byte wire header is unchanged: `[u16 handle][u16 itemTypeId][u8 sub_op]` written by
`Pool_SerializeEntityViaVTable @ 0x4D64E0` (send) and parsed by
`dispatch_entity_packet_callback @ 0x4D6A80` (receive). Capture cross-witness: every C 0x0C
sample starts `01 00 b9 14 0a` — handle pool 0 / slot 1 = joiner, type `0x14B9`, sub_op `0x0A`
(= 10 = extended). ✓

Callback context struct (built by `dispatch_entity_packet_callback` and `Pool_SerializeEntityViaVTable`):

| off | field | notes |
|---|---|---|
| +0x00..+0x0C | buf_start / buf_size / buf_end / cursor | classic stream context |
| +0x10 | trunc_flag | set to 1 on under-read / overflow |
| +0x14 | entity | the entity instance (player struct) |
| +0x18 | mode | switch target (1/2/3/4) |
| +0x1C | format | validated: 10 or 11 — must match mode |
| +0x20 | owner_ctx / 0 | host: joiner's per-connection player struct (anti-cheat block target); send: 0 |

**Position-on-wire primitives** [orig: `Network_CompressFixedPoint @ 0x4C2780` /
`Network_DecompressFixedPoint @ 0x4C27E0` / `Entity_TransformWorldToLocal @ 0x43BB50` /
`Entity_TransformLocalToWorld @ 0x43BD00`]. The compressor packs an i32 16.16 into a `u16`:
sign=bit0, exponent=bits1-3, mantissa=bits4-15. World→wire is delta from
`g_priority_ref_x/y/z @ 0xC867A4/A8/AC` — NOT a map origin (the early reading): the
per-recipient send sets them to the RECIPIENT'S EYE position (`entity+4..+0xC` +
`CameraOffset entity+0x6C..`) right before serializing its frame [orig:
`Server_SendEntityStateToPlayer @ 0x517ba0`, stores `@0x517bf5-0x517c13`], and the 0x0A
header refs carry the same three dwords to the client (→ `dword_A822E4/E8/EC`). When a
CARRIER is live — the mount (`entity+0x16C`), else the standing-on **groundEntity
(`entity+0x28`)**, any pool 0-4 (buildings included) — position is CARRIER-LOCAL instead
(`Entity_TransformWorldToLocal` before the cursor write) and the heading field goes
carrier-relative (the transforms are 6-dword POSE transforms: `out[3] = heading ∓ carrier
heading`, pitch/roll pass through — `@0x43bb7b-0x43bb8d` / `@0x43be7e`). D-NET-151.

**The pool table behind every handle resolve** [orig: `EntityPool_Allocate @ 0x442168`]:
`g_pool_list @ 0xA892E0` is five 16-B descriptors `{base, stride, used, capacity}` over one
malloc'd heap (randomized base offset, anti-tamper). The strides ARE the per-pool entity
struct sizes — the pools hold DIFFERENT structures: pool 0 players **904 B** (`0x388`,
`GamePlayerEntity`) × 256, pool 1 vehicles/items **1360 B** (`0x550`) × 1200, pool 2
statics/destructibles **812 B** (`0x32C`) × 1200, pool 3 projectiles **988 B** × 768,
pool 4 effects **988 B** × 128. Handle encode/decode is a base+stride walk
(`(pool<<12)|((ptr-base)/stride)` and back), so field offsets witnessed on one pool's
struct (e.g. GamePlayerEntity's `+0x28 groundEntity`, `+0x157 attachBoneId`, `+0x16C
parentEntity`) do NOT transfer to another pool's — only the low pose block (+4 pos,
+0x10..0x18 euler, +0x1C/+0x20 item, +0x24 flags) is layout-shared across pools.

**Decompressor (the read-side inverse, solved 2026-06-17)** [orig: `Network_DecompressFixedPoint
@ 0x4C27E0`] is a pure one-liner: `sign = (bit0 of c) sign-extended; world_delta = sign ^
(mantissa(bits 4-15) << ((bits 1-3)|1))`. The per-entity records in the §5.9 `0x0A` event loop
reconstruct WORLD position as **`Network_DecompressFixedPoint(compressed) + anchor`**, where the
anchor is the three i32 refs at the head of the `0x0A` body — stored VERBATIM into
`dword_A822E4 / A822E8 / A822EC` by [orig: `NapiNPClientMsg_0x00A @ 0x42FEC0`, writes at
`0x42ff07 / 0x42ff24 / 0x42ff41`] and added back on read by every compact deserializer
([orig: `NetPacket_SerializeInfantryEntityState @ 0x4C0320`, unmounted add at `0x4c04c3`], the
player/vehicle mirrors, and the §5.9.1 weapon-hit reader). When the record's parent/vehicle
handle is a real mount, the decompressed value is vehicle-LOCAL and is lifted to world via
[orig: `Entity_TransformLocalToWorld @ 0x43BD00`, called at `0x4c05a2`] instead of the anchor.
Cross-validated **byte-exact from the wire alone**: the first `0x0A` sample of every dvxi5
entity lands on its `0x0D`/`0x0C` spawn position (Δ = 0.00000 m across all 8 dynamic entities).
Ported as `network_decompress_fixedpoint` + `decode_frame_update` (libs/npwire) and folded
into the replay timeline (§5.25); both cases are implemented — unmounted = decompress + anchor,
mounted (vehicle-local) = lift via `network_transform_local_to_world` (the `0x43BD00` port,
D-NET-67).

#### Tag 0x0A trailing record — compact (type-11) [orig: `NetPacket_SerializePlayerState` case 1/2]

Inside §5.9 tag-0x0A's event loop (`tag==1` branch): the lookup
`ItemList_FindIndexByTypeId @ 0x49E100` on the wire `u16 itemTypeId` indexes
`gItemDefs @ 0xB46250` (stride `2780`), then `(*pad_130[52])(stream)` runs this case with
`mode=2, format=11`. **18 B per record** (no-vehicle path):

| off | bytes | field | landing |
|---|---|---|---|
| 0 | 1 | attachBone (0 unless seat-mounted) | entity+0x157 (`attachBoneId` — renamed from the `weaponType` IDB misnomer, D-NET-151). Write reads +0x157 only when mounted [orig: `@0x4c0a1a`]; the receiving client resolves ITS seat from this raw bone via `Entity_TryAttachOrDetach @ 0x436610` (bone 0 / no carrier = detach) — the 0x26 attach has NO confirm tag, this echo IS the confirmation (D-NET-157) |
| 1 | 1 | seatType (0/1/2; 0 unless seat-mounted) | 0 for a plain vehicle seat; 1 iff the carrier is a mountable GUN (`itemDef.type != 1 && attrib & 0x20 ATTR_EWeap` with `carrier+0x326 & 2`), 2 if additionally `carrier+0x312 & 8` [orig: `@0x4c0a39-0x4c0a50`]; the client apply equips the gun slot |
| 2 | 2 | carrierHandle (`0xFFFF`=none) | op1 select: mount (`entity+0x16C`) wins, else **groundEntity (`entity+0x28`)** — a grounded-standing player echoes its floor/deck (ANY pool) with bone=0 seat=0 [orig: `@0x4c0a08`]. The client mirrors it back into its own groundEntity [orig: `@0x4c1353`] — see the D-NET-151 snap. An unknown-but-valid carrier handle aborts the record + queues C2S 0x0F for it (@ 0x430814 apply path). |
| 4 | 2 | posX *compressed* | `DecompressFixedPoint` → entity+4 (CARRIER-local via `Entity_TransformWorldToLocal` when carrierHandle != none [orig: write `@0x4c0b07`], else world + anchor `C867A4`) |
| 6 | 2 | posY | → entity+8 (+ `C867A8`) |
| 8 | 2 | posZ | → entity+0xC (+ `C867AC`) |
| 10 | 1 | yaw byte | high byte of a 32-bit BAM → `entity+0x10` (heading) on read [D-NET-57]; CARRIER-RELATIVE local heading when carrierHandle != none (`(Yaw − carrierYaw) >> 24`, the `sar 24` of the pose transform's out[3] [orig: `@0x4c0b85`]) |
| 11 | 1 | pitch byte | same shape → `entity+0x14` (pitch) on read; the write sources `(entity+0x14 + 0x800000) >> 24` [D-NET-57] |
| 12 | 1 | moveOrder (movement-INPUT byte) | entity+0x12C low [orig: write `@0x4c0c9c`] — bits 0-2 = 8-way dir, bit 3 = moving, bits 6/7 = lean L/R (the packing `Player_PackInputStateToEntity @ 0x4df68f`); remote players are MOTOR-driven from it (see the apply bullets below) |
| 13 | 1 | state flags | entity+0x24 low, UNMASKED on write [orig: `@0x4c0c7d`]: **bit 0x01 = hidden (respawn-pending — the flags1-bit1 writer ORs it while undeployed, the golden pre-deploy byte)**, bit `0x02` = dead/undeployed, bits 2-4 = the local-UI modifier family (0x4 = NVG `g_NVGActive @0xB7654C`, 0x8 = binoculars `g_binocularsRaised @0xB7653A`, 0x10 = scope `g_weaponScopeActive` [orig: local-only writer `@0x4b5d7f-0x4b5da9`; 0x10 suppresses the run promotion `@0x4b72e2`]; the earlier "0x8 walk-toggle" reading was WRONG — there is no walk toggle, running is the automatic forward-walk promotion, corrected 2026-07-13), bit `0x40` = mounted |
| 14 | 1 | body-anim STATE id | write = pendingAnimStateId (+0x2B8) if nonzero else animStateId (+0x2BC) [orig: `@0x4c0cc7`]; **the server RECOMPUTES this with its own body motor from the replicated input — it is never echoed from an uplink** (the uplink carries no anim state; D-NET-159) |
| 15 | 1 | anim-channel ratio | write = the anim channel's (entity+0x188) elapsed-ticks-in-current-loop, trunc + clamp 255 [orig: `@0x4c0cf2`; `AnimChannel_AdvancePlayback @ 0x40B140` advances normalized time, wraps at 1.0] |
| 16 | 1 | anim def index | → entity+0x2B0 |
| 17 | 1 | health classification | Write side (server): `Entity_GetHealthClassification @ 0x4AD4E0` (called @0x4c0d71, stored @0x4c0d89 as the record's last byte): `byte = (tier<<4) \| (playerClass@+0x294 & 0xF)`, tier from `ratio = (Health@+0x11E << 16) / max(healthMax@itemDef+0x17C, 1)` — tier 2 if `> 49152`, tier 1 if `> 28671`, else 0 (D-NET-138). Read side: `Entity_SetHealthFromDifficultyByte @ 0x4AD580`: `tier=(byte>>4)&3` @0x4ad596 → `Health = {tier2 ≈0.875×, tier1 ≈0.594×, tier0/3 ≈0.219×} × ItemDef.healthMax` (@0x4ad5f4 / 0x4ad65b / 0x4ad68c — the tier midpoints of the write-side boundaries), low nibble → `playerClass` +0x294 @0x4ad5a2, then the item is RE-RESOLVED from playerClass (@0x4c1248 — see §5.46). **Don't-care for the LOCAL player**: `NetPacket_SerializePlayerState` SKIPS this apply for `g_local_player_entity` (`cmp edi,g_local_player_entity; jz` @0x4c11ac → local branch only floors `Health` at 1 @0x4c11c4); the local player's health comes from the §5.9 0x0A tail, not this byte |

**Apply-side field map (case-2 read, witnessed 2026-07-02 — several names above are decode-era
misnomers corrected here):**
- **off 12 is the MOVEMENT-INPUT byte** (entity+0x12C low), not an anim slot: remote players are
  motor-driven from replicated input (`@0x4c11ec`; consumers `Entity_UpdatePlayerInfantryMovement
  @ 0x48496d`, stance bits re-derived from the state table `@0x4c1228-0x4c1246`). Apply is
  REMOTE-only (`@0x4c11d7`).
- **off 13 bit 0x02 means DEAD/UNDEPLOYED**, not "spawning": wire bit2=1 → anim-state stores +
  `Health = 0` (`@0x4c1005-0x4c1027`); the spawn hook fires on the **1→0 edge** (wire clear while
  entity bit2 still set, `@0x4c1109`): live pose snap + `Entity_ResetToSpawnState @ 0x4B9610`
  (all) + `Game_InitNewRound @ 0x422740` (LOCAL only, `@0x4c114c`). Edge-triggered and
  non-refiring: the XOR-apply masks EXCLUDE bit 0x02 — **local mask 0xE1, remote mask 0xFD**
  (`@0x4c12ff/@0x4c1311`). Local-mask consequence: wire bits 0x01/0x20/0x40/0x80 ARE applied to
  the local player's own Flags every record — the byte must mirror the host's copy of that
  client's flags (kept fresh by the uplink), never synthesized constants.
- **off 14 is a body/weapon anim-STATE id** (entity+0x2BC, vs the per-state flags table
  `g_animStateFlagsTable @ 0x8139E8` — renamed 2026-07-03 from the bare dword label, 254 entries: bit1
  idle-class, bit4 uninterruptible → queue to pending, 0x20 exit-needs-bit0, 0x100/0x200 stance
  rebits, 0x400 long blend; transition-arbitrated: 0x20-flagged states uninterruptible except by
  1-flagged, defer bits 0x4/0x20 route to the pending slot +0x2B8 `@0x4c1174/@0x4c118a`; the
  remote apply then re-derives MoveOrder stance bits 8-9 from the table `@0x4c11fd-0x4c1242`). LOCAL
  player skips (`@0x4c1167`) except the wire-bit2 dead path. Init default **0x2B (43, idle)**
  [orig: PlayerClass_InitEntity @ 0x4B1116 sets +0x2BC = +0x2C8 = 0x2B]; **spawn/deploy resets to
  44** (idle2; 153 when the class table maps it) [orig: Entity_ResetToSpawnState @ 0x4b9714].
  State ids (witness 2026-07-03): 1-8 stand-walk + dir, 11-18 crouch, 19-26 prone, 27/28 water,
  37-40 parachute, 41/42 lean, 43 idle-collapse, 44 spawn/long-idle, 45/46/48 jump/land/
  prone-trans, 169-172 stance transitions, 175 falling-death. Write side emits the
  pending state when non-zero, else the current (`@0x4c0cd6/@0x4c0cde`). State 0 has no table
  flags and pins the body to clip 0 — the "null" clip.
- **off 15** (anim-channel ratio) applies REMOTE-only and only inside the anim-state-accept
  branch (`@0x4c11a6` → entity+0x377 — the remote entity's net anim-phase seed, consumed by
  `AnimMap_UpdateEntity @ 0x40B74B`; client-side only, which is why no plain host-side writer
  exists — resolves the 2026-07-02 OPEN). The case-1 write side's source is the +0x188 anim
  channel's elapsed-ticks-in-loop (`@0x4c0cf2`, trunc clamp 255).
- **The SERVER-side source of off 12/14/15 for a remote player (witness 2026-07-03, D-NET-159):**
  the authority runs the player-body anim selection for EVERY player — `Entity_UpdateInfantryPlayerBody
  @ 0x4B40E0` gates on `g_local_player_entity == e || g_napi_np_ctx.is_authority` (@ 0x4b70a3-0x4b70b2)
  and skips mounted/dead/swimming entities (`Flags & 0x2000 / 0x42` @ 0x4b70b8-0x4b70c8), every 4th
  tick (@ 0x4b70ce). Inputs, all REPLICATED: MoveOrder bits 0-2 (8-way dir; state offset table
  {0,7,6,5,4,3,2,1} @ the 0x4b71c7 switch), bit 3 (moving), bits 8/9 (prone/crouch — fed by C2S
  0x1D since the uplink's low byte cannot carry them; prone suppressed by `Flags & 0x10A000`
  @ 0x4b416c). Selection: moving → base 1/11/19 (stand/crouch/prone) + dir offset
  (@ 0x4b7183-0x4b7226); idle → 45 (crouch — promoted to **46 idle_mortar** when the equipped
  def has ForceCrouch 0x40000 and the clip exists @ 0x4b723f-0x4b7279) / 48 (prone) /
  43-then-44 after 62 selection passes (`state = 0x2B + (++entity[0x148] >= 0x3E)`
  @ 0x4b727b-0x4b7293); **the run promotion (decoded 2026-07-13)**: pure-forward standing walk
  (state == 1 only) promotes to `run_2`/`run_3` (ANIMNUM 9/10) by
  `tier = pitchTier(entity+0x37C) + AdmDef.run_anim` — the tier bands are `>0x430000 or <0 → 0`,
  `≥0x210000 → 1`, `else 2`, but **entity+0x37C has NO writer in the retail image** (pool
  zero-init ⇒ the constant 2); `run_anim` is the weapon.def key at AdmDefs+0xAC
  (`dword_24E808C[adm*0x460]`, parser @ 0x543d15, JOX ships only 0/1 ⇒ every weapon runs at
  run_3); tier 1 → 9 if `animMap[9]!=animMap[0]`, tier ≥ 2 → 10 with a 9 fallback; suppressed
  by Flags & 0x10 (scope) (@ 0x4b729d-0x4b731b); prone lean rolls 41/42 from MoveOrder bits 6/7
  (@ 0x4b731b-0x4b7354, gated `!(Flags & 0x112002)`, bit 7 wins); commit via the flag-table
  arbitration (@ 0x4b7356-96). The LOCAL player runs this same selection (the local-or-authority
  gate above) — the reimpl shares one `player_body_select` for both paths.
- **off 16 (ADM anim-def index): 0 is a VALID index — 0xFF is the null sentinel** (entries
  stride 1120): the client stores it to +0x2B0 AND resolves `entity+0x298 =
  AdmDef_GetEntryByIndex(byte)` UNGATED for remote players (`@0x4c11f2-0x4c120d`) — 0xFF nulls
  +0x298 every record, starving the weapon-action layer while the body plays clip 0: the
  live-witnessed remote-player "spazz" (retail-join v15). The value is the player's CURRENT
  WEAPON's AdmDef index [writers: spawn default `AvatarDef_FindIndexByName("WPN_M4AUTO")` →
  +0x2B0 @ 0x4B1116; local switch `Player_SelectWeaponSlot @ 0x4DD727/0x4DD7F5`; net: the host
  ECHOES the extended uplink's own byte — case-4 store @ 0x4C20A3, gated `category < 11`].
- **Local-player position echo**: never applied outside (a) the spawn edge and (b) a mount/
  dismount snap after `Entity_TryAttachOrDetach @ 0x436610` (`@0x4c1329-0x4c1345`; returns 1 =
  attach state CHANGED — a bone+carrier record that differs from the current attach, or a
  bone-less record while `parentEntity`/`parentSlot` are set → `Entity_DetachFromVehicle
  @ 0x4355F0`); the record is parsed whole but the local motor owns the live pose. off-17
  health apply is remote-only; the local branch only floors Health at 1 (`@0x4c11c4`).
- **off 0-3 carrier fields apply to the LOCAL player too**: a resolving carrier handle whose
  entity has a null itemDef ABORTS the record and queues C2S 0x0F for that handle
  (`@0x4c10a9-0x4c10bd`); 0xFFFF while mounted force-dismounts. And UNCONDITIONALLY —
  local player included — the record's carrier is stored into the entity's own
  **groundEntity (`+0x28`)**: `groundEntity = mounted ? mount->groundEntity : wireCarrier`
  [orig: `@0x4c1353/@0x4c1358`]. The client's own movement collision pass re-derives the
  true ground link through its final CB/terrain probe (unconditional groundEntity store
  `[orig: Entity_RaycastGroundHeightAndObject @ 0x414370]`; CL's 0x100000 ladder write is
  separate and transient), so a HOST that echoes
  the matching carrier is steady-state — but a host that echoes `0xFFFF` at a grounded
  client re-nulls the link every 0x0A and destabilizes the standing state (D-NET-151).

**Spawn hook (load-bearing):** superseded detail above (off-13 bullet) — the hook is the wire
bit-0x02 1→0 EDGE, local-player `Game_InitNewRound` + everyone's `Entity_ResetToSpawnState`.
The case-1 write side is the inverse: position via `Network_CompressFixedPoint(entity+4..C −
g_priority_ref)` (the recipient-eye anchor, not a map origin), CARRIER-local via
`Entity_TransformWorldToLocal` when the mount-else-groundEntity carrier is live (D-NET-151).

#### Tag 0x0C body — extended (type-10) [orig: `NetPacket_SerializePlayerState` case 3/4]

Fixed **43 B body** (5-B header + 43 B = 48 B total — every captured C 0x0C frame in
2026-06-16b is exactly 48 B). The joiner's per-frame uplink for its own player entity,
including a host-validated anti-cheat block:

| off | bytes | field | landing (host receiver, case 4) |
|---|---|---|---|
| 0 | 2 | carrierHandle (`0xFFFF`=none; `(h&0xF000)>=0x5000` invalid) | pool resolve via `g_pool_list` (any pool 0-4). The SENDER writes its **groundEntity (`entity+0x28`)** here — the entity it STANDS ON (building floor, vehicle deck; v26: a pool-2 static `0x227e`), pool-encoded at the case-3 head. The old "vehicleHandle / mounted" reading undersold it — D-NET-151. |
| 2 | 4 | posX (i32 LE, 16.16; CARRIER-local when carrierHandle != none) | smooth-target entity+0x234 (free = ABSOLUTE world, NO anchor add on receive; carrier = local lift via `Entity_TransformLocalToWorld @0x43BD00` `@0x4c1de1`; §5.38a / D-NET-91/151) |
| 6 | 4 | posY | entity+0x238 |
| 10 | 4 | posZ | entity+0x23C |
| 14 | 2 | heading (i16 LE, sign-ext ×0x10000) | entity+0x240 / +0x10 (32-bit BAM); CARRIER-RELATIVE when grounded — the pose transform re-adds the carrier heading (`out[3] = ref[3] + local[3]` `@0x43be7e`) |
| 16 | 2 | pitch (i16 LE, sign-ext ×0x10000) | entity+0x244 / +0x14 (pose pass-through, never localized) |
| 18 | 1 | anti-cheat flags (sender's rotating self-check accumulator: IsDebuggerPresent / D3D9-hook / speed checks, the case-3 `dword_B5ABA8` counter switch) | cursor advance, byte DISCARDED by the host apply |
| 19 | 1 | movement-input byte (was misnamed "anim slot low"; witness 2026-07-02) | entity+0x12C low @0x4C1E2C — the locomotion input the host echoes at 0x0A off-12 |
| 20 | 1 | state-flags byte — the sender's RAW `entity+0x24` low byte | bits 2-4 REPLACE the host entity's: `flags ^= (flags ^ wire) & 0x1C` `@0x4c1e4d`. NOT an xor-delta (the old "flagsXor" reading — an xor-apply corrupts already-set stance bits; crouch/prone family). D-NET-151. |
| 21 | 1 | analog X (was "anim def 1" — a decode-era misnomer; witness 2026-07-03) | entity+0x130 @0x4C1E6A, verbatim — the joystick/analog movement axes the packer deposits after the input bits |
| 22 | 1 | analog Y | entity+0x131 @0x4C1E87 |
| 23 | 1 | analog Z | entity+0x132 @0x4C1EA4 |
| 24 | 1 | equipped-weapon AdmDef index (was "RESERVED/discarded" — witness 2026-07-02) | entity+0x2B0 @0x4C20A3, gated `AdmDefs[idx].category < 11`; the host echoes it at 0x0A off-16 |
| 25 | 1 | stat byte 0 | playerSlot+0x15F78 |
| 26 | 1 | stat byte 1 | playerSlot+0x15F79 |
| 27 | 2 | priority handle 0 | playerSlot+0x1708A |
| 29 | 2 | priority score 0 (u16 → zero-ext u32) | playerSlot+0x17094 |
| 31 | 2 | priority handle 1 | playerSlot+0x1708C |
| 33 | 2 | priority score 1 (u32) | playerSlot+0x17098 |
| 35 | 2 | priority handle 2 | playerSlot+0x1708E |
| 37 | 2 | priority score 2 (u32) | playerSlot+0x1709C |
| 39 | 2 | priority handle 3 | playerSlot+0x17090 |
| 41 | 2 | priority score 3 (u32) | playerSlot+0x170A0 |

The 8 trailing u16s are **4 `(handle, score)` ENTITY-PRIORITY pairs** — the sender's top-4
interest list from `Server_BuildEntityPriorityListForPlayer(entity, .., 4)` [orig: case-3
call `@0x4c1be9`] — NOT weapon/fire-counter tallies (the old decode-era guess; v26 shows the
ridden buggy's handle scored first while standing on it). `playerSlot` = `packetCtx+0x20` =
the joiner's per-connection player struct (`connectionCtx+0x160 → playerObj+0xC0`, set by
`NapiNPServerMsg_0x00C`). The case-3 send path mirrors this layout from the joiner's local
state. Pos/orientation/ADM stores are deploy-gated: `!(Flags & 2) && !g_spawn_success_gate &&
connState == 6 && !preround` (@ 0x4c2028); wire flags bit0 forces a hard pos snap; the
staleness counter entity+0x27C resets to 0 on apply.

**The uplink carries NO anim state and NO stance** (witness 2026-07-03): byte 19 is only the
MoveOrder LOW byte (dir/moving/lean — bits 8/9 crouch/prone are above it), and byte 20's bits
2-4 are the local-UI modifier family (walk-toggle/scope/`B7654C`), NOT crouch/prone. Stance
replicates via the dedicated **C2S 0x1D stance-change** (dispatch table), and the server
recomputes the body anim itself (D-NET-159 / the §5.10 off-14 bullet).

#### C2S 0x26/0x27 — vehicle attach/detach flow (witnessed 2026-07-03, D-NET-157)

Client senders: **0x26** = `[u16 senderEntityHandle][u16 vehicleHandle][u8 modelBoneIndex]
[u8 stack-pad]` `[orig: Entity_RequestVehicleAttach @ 0x4364A0 — renamed from
Entity_RequestVehicleAttach; pre-snaps yaw from the seat bone, then authority ?
Entity_ProcessVehicleAttach : queue 0x26 @ 0x436602]`. **0x27** = `[u16 selfHandle]
[u16 vehicleHandle][u16 junk]` `[orig: Entity_SendDetachPacket @ 0x435510; the use-key
dismount SENDS ONLY and waits for the 0x0A echo @ 0x4369c7]`. **There is NO confirm tag** —
the 0x0A compact record's mounted branch (byte0 bone + carrier, §5.10) is the confirmation
for everyone including the requester.

Server 0x26 (`NapiNPServerMsg_HandleVehicleAttach @ 0x502390`): authority-gated; sender conn
→ player block (+0x160) → entity cell (+0xC0); **word0 is OVERWRITTEN with the sender's
authoritative handle** (@ 0x502415 — anti-spoof; the client value is never read); then
`Entity_ProcessVehicleAttach @ 0x435AA0` (thunk @ 0x435cf0), validation order:

1. resolve via `g_pool_list`; reject null vehicle/itemDef/player or either `Flags & 2`
   (dead) @ 0x435b01;
2. `slotType = Entity_GetBoneSlotType(vehicle, bone)` @ 0x434ED0 — the model BONE table
   (count model[47], names model[48], 48-B stride, name +32, **boneIndex 1-based**),
   case-insensitive prefix `sitex→1, ctrlx→2, drvrx→5, UseGun→3`, else 0 → reject;
3. the requester's EquippedSlot (0x118) MountSlot currentAction (+0x2C) must be 0/1/11
   (weapon busy rejects) @ 0x435b29;
4. `Vehicle_HasEnemyOccupant @ 0x4359F0` (renamed 2026-07-03 from find_entity_mounted_on_vehicle —
   returns BOOL, 2 args): a live ENEMY occupying the vehicle or one of its ATTR_EWeap guns
   rejects — pool-0 scan, skips dead/self/same-team (+0x162 compare @ 0x435a5f), hit iff
   `e->parentEntity(0x16C) == root`; same-team occupants never block (co-boarding);
5. seat occupancy: the bone's index in the ItemDef 10-byte seat block
   `[seatBoneIndex[0..7] @ +0x25D, controlBone @ +0x265, useGunBone @ +0x266]`; found +
   `vehicle->mountHandles[idx](0x190) != 0xFFFF` rejects @ 0x435ba9 (NOT found → proceed
   without the occupancy check);
6. already mounted → detach first @ 0x435bce;
7. slotType 1/2/5 → `Entity_AttachToVehicleSlot @ 0x4946D0`; 3 →
   `Entity_AttachToUseGunSlot @ 0x546B80` (renamed 2026-07-03 from the _0 suffix clone;
   server passes forceAttach = 0); success → `player->MoveOrder(0x12C) &= ~0x300`
   @ 0x435c42 (stance clear; the local latches too @ 0x435c54).

`Entity_AttachToVehicleSlot @ 0x4946D0` writes — type 1 (sitex): bone must be in
`seatBoneIndex[0..7]`, occupied → 0, else `mountHandles[idx] = playerHandle` @ 0x494746;
type 2 (ctrlx): `vehicle+0x170` occupant null-or-self AND `attrib & 0x40 ATTR_PlayerControl`,
`vehicle+0x170 = player`, `mountHandles[8](+0x1A0) = handle`, and iff `attrib & 0x20
ATTR_EWeap` save the prev slot to `player+0x308`, `player->EquippedSlot = vehicle+1140`,
`*(vehicle+1176) = player` @ 0x494875; type 5 (drvrx): same gates as 2 @ 0x49492f. Common
tail @ 0x494752-75: `player->Flags = (Flags & 0xFFFF5FBF) | 0x40` (clear 0x8000|0x2000, set
mounted), `parentEntity(0x16C) = vehicle`, `attachBoneId(0x157) = bone`,
`parentSlot(0x168) = slotType`, return 1.

Server 0x27 (`NapiNPServerMsg_HandleVehicleDetach @ 0x4FC980` → tail @ 0x435D00): sender
conn must have a player block+cell; `h = wire word0` — **TRUSTED (no anti-spoof, no range
guard)** in retail; then `Entity_DetachFromVehicle(e, *(e+0x16C))` @ 0x435d40.
`Entity_DetachFromVehicle @ 0x4355F0`: `MoveOrder &= ~0x300`; parentSlot ∈ {2,3} non-local →
`EquippedSlot = *(entity+0x308)` restore; `!(Flags & 0x100)` → EquippedSlot = 0;
`vehicle+0x170 == entity` → clear it (ATTR_PlayerControl → engine-state 7, flush vehicle
ownerSession (0x1CC)); clear every matching `mountHandles[i] == handle → 0xFFFF` (10 slots,
BOTH the passed vehicle and `entity->parentEntity` when different); finally `Flags &= ~0x40;
0x16C = 0; 0x157 = 0; 0x168 = 0`. Reimpl: `world::entity_process_vehicle_attach /
entity_detach_from_vehicle` (libs/world/vehicle_attach.cpp) behind the dispatch cases; the
0x27 subject is clamped to the sender's own entity. Production seat extraction preserves the
USRP row's witnessed 1-based index, so attach classification uses the exact echoed bone.

**Cross-witnessed against the 2026-06-16b capture** (joiner=`32769`, host=`32768`):
- Frame, joiner stationary: vehHdl `ff ff` (none); posX `8f be b0 05` = `0x05b0be8f` / 65536
  = **1457.0** / posY `04 b5 47 fa` = **-1484.0** / posZ `d7 bc 1f 00` = **31.7**. Plausible
  Laba-Laba southern-half ground coord. ✓
- Frame ~30 later, joiner running: Δy ≈ +1261 units in 16.16 world. Monotonic motion. ✓
- Frame, joiner mounted on vehicle handle `0x108b` (pool 1 slot 139): posX/Y read as
  vehicle-LOCAL small i32s; heading goes to zero, attitude carried by the vehicle frame. ✓
- Consecutive in-vehicle frames: posZ-local Δ = -7758. ✓

**Open follow-ups (§5.9 + §5.10):** Pool-entity messages 0x0D and 0x20 closed in §5.11 / §5.12
(Stage C). The vehicle / AI-infantry / weapon-class callbacks at `ItemDef+356` — flagged here
as "TBD" — are decoded in §5.10b (dispatch table) plus §5.13 (vehicle compact),
§5.14 (AI infantry compact) and §5.15 (guided weapons; full (mode×field-group) matrix
+ structural port landed, wire integration + validation deferred until a capture
carries live projectile traffic — D-NET-64). The spawn-batch `entity+16/+20/+24`
fields (§5.9 0x10, §5.11 0x0D, flag-gated) are the **orientation Euler triple**
(`entity+16` = yaw heading, 32-bit BAM), NOT velocity — Hex-Rays mislabels them, the
same correction D-NET-63 made for the vehicle compact record. They carry the spawn
pose the spectator renders; cross-validated field-for-field against authored facings
in `nw_dvxc1_groundtruth` (D-NET-86).

### 5.10b Per-entity-type callback at `ItemDef+356` — class dispatch table

Stage C2 (2026-06-16): the player callback decoded in §5.10 is one of a wider
family. The dispatch is data-driven: a 24-byte class-table entry at
`g_entity_class_table @ 0x813000` holds `{char tag[8]; void* fn[4]}` per entity
class. `fn[3]` is the network-serialize callback (slot semantics:
`fn[0]`=damage/death/per-tick, `fn[1]`=init-from-def/model-binding,
`fn[2]`=network spawn-state companion, `fn[3]`=wire serialize). The table has
**41 entries × 24 B = 0x3D8 bytes** (terminator at `0x8133D8`).

The items.def `ai_function` / `move_function` / `render_function` / `disk_function`
directives on each item select a class (e.g. `move_function cveh`), and at
items.def load time the engine copies the matching `fn[3]` into `ItemDef+0x164`.
**At packet-dispatch time the class table is bypassed.** The 0x0A receiver
inlines `ItemList_FindIndexByTypeId`'s algorithm at `0x430786..0x4307A2` —
linear scan of `gItemDefs @ 0xB46250` (stride 2780, count @ `0xB46254`,
comparing `.id` at `gItemDefs[i] + 0x50`) — then invokes
`(*(void(*)(stream))(gItemDefs[idx] + 0x164))(stream)` directly at
`0x430814..0x430828`. No 4-character tag indirection per packet.

**Desync detection** [orig: 0x4307C4]: receiver runs a consistency check after
the callback — if `entity.defPtr != gItemDefs + idx*2780` OR
`defPtr->id != wire_typeId` OR `entity.defIndex != idx`, it queues reliable
message 0x0F via `CNapiNetwork_QueueReliableMessage @ 0x4C4FA0` (mode 1,
payload = entity handle).

Network-serialize callbacks observed in the table (14 networked entries with
`fn[3] != 0`):

| class tag | callback | wire user | wire formats |
|---|---|---|---|
| `plyr` | `NetPacket_SerializePlayerState @ 0x4C09C0` | player infantry | both compact (type 11) and extended (type 10) — §5.10 |
| `org0` / `org1` | `NetPacket_SerializeInfantryEntityState @ 0x4C0320` | AI infantry / organic | compact only — §5.14 |
| `CHel` / `cveh` / `cbot` / `cpln` / `ctrn` | `Entity_SerializeVehicleState @ 0x460560` | vehicles + AI ground/air units | compact only — §5.13 |
| `rokt` / `stng` / `hlfr` / `jvln` / `arty` | `Entity_SerializeGuidedMissileState @ 0x447C50` | guided weapons (rockets, missiles, artillery) | 4 modes × 6 field-groups; selector = sub_op byte; ported, wire-deferred — §5.15 |

**Non-networked entries (27 with `fn[3] == 0`)**, kept in the table for the
other `fn[]` slots (damage / init / spawn-companion): `null`, `brrl`, `envs`,
`ewep`, `ele0`, `gnrc`, `gnrl`, `gnl2`, `flag`, `squib`, `nade`, `schl`,
`clym`, `vmne`, `lndm`, `bldg`, `bld2`, `cran`, `door`, `target`, `emit`,
`towr`, `tree`, `palm`, `psec`, `pwrp`, `aflr`, `gflr`. These never appear in
S2C 0x0A trailers — they're either static (client-side spawn-only) or
replicated via tag 0x0D / 0x10 batches.

The player gets BOTH formats because it both sends C2S 0x0C (extended uplink)
and is replicated to other clients in S2C 0x0A trailers (compact). Vehicles /
AI / weapons are server-pushed only: their callbacks reject modes 3/4
(extended) with `return -1`.

### 5.11 Tag 0x0D — pool-entity spawn batch (loopback capture 2026-06-16b)

`[orig: NapiNPClientMsg_0x00D @ 0x432C40]`. Pool-entity spawn (vehicles, AI, ground items;
pool resolved from the high nibble of the slot id). The handler raises `dword_A82370 ≥ 3`
on entry (§5.1) then reads a `[u16 entityCount]` header and loops; every record is
zeroed (`memset(entity, 0, 0x2B4)`) before fill. Cross-witnessed against 437 records over
37 retail payloads in the 2026-06-16b loopback (`nw_ingame_pool_records_test`, body
consumed exactly on every payload).

| off (within record) | bytes | field | gate | landing |
|---|---|---|---|---|
| 0 | 2 | spawnFlags | always | (gates all conditional fields below) |
| 2 | 2 | entitySlotId (`pool<<12\|slot`) | always | pool resolve via `g_pool_list`; `0xFFFF` and `(s&0xF000)>=0x5000` end the batch |
| 4 | 2 | itemTypeId | always | `ItemList_FindIndexByTypeId @ 0x49E100` → entity+28 / `entity+32 = gItemDefs[idx]` |
| 6 | cstr | entityName | always | cstring read+skip; copied to `entity+244` later if AI-flagged |
| — | 4 | entityFlags | `spawnFlags & 0x0020` | entity+36 (bit 1 = movement gate; §5.6) |
| — | 4 | posX | always | entity+4 (i32 16.16 world) |
| — | 4 | posY | always | entity+8 |
| — | 4 | posZ | always | entity+12 |
| — | 4 | eulerZ (32-bit BAM, yaw heading) | `spawnFlags & 0x0001` | entity+16 |
| — | 4 | eulerX (32-bit BAM) | `spawnFlags & 0x0002` | entity+20 |
| — | 4 | eulerY (32-bit BAM) | `spawnFlags & 0x0004` | entity+24 |
| — | 4 | sectionMask | `spawnFlags & 0x0008` | entity+308 |
| — | 1 | **teamByte** | `spawnFlags & 0x0010` | entity+354 — BMS team (1=Blue/2=Red); D-NET-58 |
| — | 2 | parentHandle | `spawnFlags & 0x0100` | resolved → entity+368 (`occupantEntity` pool ptr) — a driver/occupant BACK-REFERENCE, not a transform parent (D-NET-195). Resolve nulls only `0xFFFF`/pool ≥ 5/capacity overflow — `0x0000` is a VALID pool-0 slot-0 ref `[orig: read @ 0x432e35..0x432e53, resolve+store @ 0x43326d..0x433289]`. Live AS witness: retail-vehicle-session carries a Dune Buggy (slot 0x1006, flags 0x1d77) with parent `0x0000` = "Player #1 (Multiplayer)"; the entity's client-side motion still rides its §5.13 compacts exclusively. Flag-clear default is `-1` (0xFFFF) |
| — | 2 | targetHandle | `spawnFlags & 0x0200` | resolved → entity+40 (pool ptr) |
| — | 1 | weaponSlotMask | `spawnFlags & 0x0400` | (the weapon block; reads u16 per set bit, 0xFFFF on a set bit skips storage but still consumes the wire u16) |
| — | 2 ea | weaponHandle\[bit\] | `mask & (1<<bit)` (bits 0..7) | entity+400+2·bit (capped at +414); 0xFFFF skips storage |
| — | 2 | extraHandle0 | inside `0x0400` block | entity+416 |
| — | 2 | extraHandle1 | inside `0x0400` block | entity+418 |
| — | 1 | **boneByte** | always | entity+290 (u16 zero-ext) — bone/other, NOT team; D-NET-58 |
| — | 4 | aiProfile1 | `spawnFlags & 0x0800` | trailer → aiSlot+16 |
| — | 4 | aiProfile2 | `spawnFlags & 0x0800` | trailer → aiSlot+20 |
| — | cstr | aiName | `spawnFlags & 0x0800` | trailer → aiSlot+156 |
| — | 1 | refNum | `spawnFlags & 0x0040` | entity+533 (D-NET-94; not an alert level) |
| — | 1 | subType | `spawnFlags & 0x0080` | entity+532 |
| — | 1 | weaponTypeByte | `spawnFlags & 0x1000` | entity+176 |
| — | 1 | zoneNumberRank (was "healthByte" — zone-object semantics, 2026-07-03) | `spawnFlags & 0x2000` | entity+538 — the packed **AS zone byte** `zoneNumber + 32·rank`, server source `ZoneSlotChain_GetZoneInfo @ 0x503eeb`; write gate = `entity+538 != 0` @ 0x503ecc. Golden ASH_I5A bunkers (type 0x054F): flags 0x20a1/0x20b1, byte 0x22 = zone 2 rank 1 |
| — | 2 | zoneRadius (was "healthShort") | `spawnFlags & 0x2000` (extra read) | entity+350 — the capture-zone/proximity radius (BMS record word 14; golden bunkers 70 = 0x46) |
| — | 2 | zoneRadius (alt) | `(spawnFlags & 0x8000) && !(0x2000)` | entity+350 — the un-numbered SpawnPoint-def path: write gate `ItemDef+84 & 0x40000` @ 0x503f29 |
| — | 1 | difficultyByte | `spawnFlags & 0x4000` | entity+624 — write gate = the def has a physics/damage handler @ 0x503f4c |

**Weapon block precise shape (corrected — D-NET-56):** the `0x400` branch reads the u8 mask;
when the mask is non-zero it walks bits 0..7 reading one u16 per set bit (`0xFFFF` on a set bit
skips storage but still consumes the wire u16); then it **always** consumes `extraHandle0` +
`extraHandle1` (2× u16). The mask==0 path (`goto LABEL_110 @ 0x4330b1`) skips the per-bit loop
but **still reads both extras** — an earlier note here claimed mask==0 ends the block with no
extras, which is wrong (it also misstated the "minimal block" size). So under `0x400`: minimal =
**5 B** (`u8 mask==0 + 2× u16 extras`), maximal = `1 + 2×8 + 4` = 21 B. When `0x400` is NOT set,
`entity+416/+418` are written `0xFFFF/0xFFFF` in-memory and nothing is read from the wire.
Note the **encode** side `serialize_entity_pool_to_packet_0 @ 0x503940` only sets `0x400` when its
mask (`itemDef+604`) is non-zero, so retail never emits the (0x400, mask==0) record — which is why
the byte-witness capture never exercised it; the client handler reads it regardless, so the decoder
must match.

**AI trailer correction (D-NET-52):** every conditional field of the trailer is a 4-byte
read (`cursor += 2` on a `uint16_t*` advances 4 bytes; hex-rays renders the value type as
`uint16_t*`, but the wire is u32). 195 of 437 records in the loopback carry the trailer;
all match this 4+4+cstring shape exactly.

**Team/bone label correction (D-NET-58, controlled capture 2026-06-17):** the
`spawnFlags & 0x0010`-gated byte at entity+354 is the **team** byte (1=Blue/2=Red), and the
unconditional post-weapon byte at entity+290 is a **bone/other** byte, NOT team. The earlier
labels (`orientByte`@+354 / `teamByte`@+290, from D-NET-54 trusting the handler-side Hex-Rays
variable name) are inverted. Witnessed on the encode side: `serialize_entity_pool_to_packet_0`
sources the gated byte from `entity+354` (Hex-Rays var `team_byte`; `if(team_byte) flags|=0x10`)
and the unconditional byte from `entity+290` (var `bone_byte`). entity+354 is the **unified team
landing** across both spawn paths (§5.12 already lands 0x20 team there under flag 0x08). The
controlled "ON RE Probe AS dvxi5" loopback confirms it empirically: the two trucks authored
team 1/2 carry +354 = 0x01/0x02 while +290 = 0x00 on both.
`[orig: serialize_entity_pool_to_packet_0 @ 0x503940 (team_byte @ 0x5039f1; bone_byte @ 0x503cda)]`

**Cross-witness numbers (`nw_ingame_pool_records_test` vs the 2026-06-16b loopback):**

- 37 payloads, sizes 226..550 B, modal 543 B; 437 entities total (max 20 per payload).
- Body consumed EXACTLY for every payload (`leftover_bytes = 0`).
- Flag bits observed: `0x3EF7` (every bit except 0x0008, 0x0100, 0x4000, 0x8000).
- AI trailer presence: 195/437 records (≈45%), matching §5.6's "always set on retail
  vehicle/AI templates" — Stage C's payloads include a mix of static-prop spawns (no
  trailer) and AI/vehicle spawns (trailer).

### 5.12 Tag 0x20 — bulk pool-3 entity sync (loopback capture 2026-06-16b)

`[orig: NapiNPClientMsg_0x020 @ 0x425C00]`. Bulk pool-3 sync (markers / waypoints /
nav-nodes; pool 3 in the engine primer's pool taxonomy). The handler raises
`dword_A82370 ≥ 5` (§5.1) then reads a `[u16 startIndex][u16 entityCount]` header. For
each i in `[0, entityCount)` it calls `Pool_GetEntryUnchecked(3, startIndex + i)` and
`memset(entitySlot, 0, 0x2B4)` (the pool-3 record stride is 692 B, but the handler only
clears the first 692 — wait, it clears 0x2B4 = 692 B exactly).

Cross-witnessed against 792 records over 29 retail payloads in the same loopback
(`nw_ingame_pool_records_test`, body consumed exactly on every payload).

| off | bytes | field | gate | landing |
|---|---|---|---|---|
| 0 | 2 | itemTypeId | always | `0` ⇒ empty-slot sentinel: record body ends here, advance to next slot |
| 2 | 1 | flagsByte | non-empty | (gates conditional fields below) |
| 3 | 4 | posX | non-empty | entitySlot+4 (i32 16.16 world) |
| 7 | 4 | posY | non-empty | entitySlot+8 |
| 11 | 4 | posZ | non-empty | entitySlot+12 |
| — | 4 | **movementVal** | `flags & 0x01` | entitySlot+16 — raw u32 (32-bit BAM heading for markers), NOT a `pool<<12\|slot` parent; D-NET-59 |
| — | 4 | orientationVal | `flags & 0x02` | entitySlot+0 |
| — | 2 | ammoCount | `flags & 0x04` | entitySlot+290 |
| — | 2 | netHandle | non-empty (ALWAYS) | entitySlot+124 (zero-ext to u32) |
| — | 1 | teamByte | `flags & 0x08` | entitySlot+354 |
| — | 2 | weaponType | `flags & 0x10` | entitySlot+640 |
| — | 1 | scoreByte | `flags & 0x20` | entitySlot+672 (zero-ext to u32) |

After per-record reads, `ItemList_FindIndexByTypeId` fills `entitySlot+28/+32/+452/+456`
(the def index, def pointer, modelFlags, second flag field — same shape as 0x0D). When the
loop exits, `Pool_UpdateMaxUsed(3, startIndex + entityCount)` finalizes the pool's high-water.

The `netHandle` field at +124 lines up with the AI-navigation marker references documented
in `docs/world/world-wac-ai-re.md:89-92` (target nodes resolved as
`Pool_GetEntryUnchecked(3, ·)`), and `+290` is the same ammo/team slot tags 0x10 and 0x0D
use (per-tag semantic — Hex-Rays auto-named).

**`movementVal` label correction (D-NET-59, controlled capture 2026-06-17):** the
`flags & 0x01` field at entitySlot+16 is the engine's `entry[4]` **`movement_val`**, written
RAW (no pool-resolve), NOT a `pool<<12|slot` parent handle. For pool-3 start markers it carries
a full 32-bit **BAM heading**: the controlled probe's Blue starts = `0x40000000` (90.00°), Red
starts = `0xc0000000` (270.00°) — values that are not valid pool handles and are strictly
team-correlated. The companion `flags & 0x02` field (`orientationVal` → entitySlot+0) is the
other angle slot. `[orig: serialize_entity_pool_to_packet @ 0x503460 (movement_val = entry[4] @ 0x50350c, written raw) / NapiNPClientMsg_0x020 @ 0x425C00]`

**Team byte == raw BMS team (controlled witness 2026-06-17):** first capture with authored-known
teams confirms the `flags & 0x08` byte at entitySlot+354 is the **raw BMS team integer, no remap**
— the two `0x1773` Blue start markers carry team `0x01`, the two `0x1774` Red markers carry
`0x02` (BMS team 1=Blue, 2=Red), at the exact authored cardinal positions.

**Cross-witness numbers:**

- 29 payloads, sizes 533..634 B, modal 625 B; 792 entities total (max 30 per payload).
- Body consumed EXACTLY for every payload.
- Flag bits observed: `0x1F` (all five gated bits 0x01..0x10 used; 0x20 score-byte
  NOT seen in this capture — only present on entities with non-default score state).
- Zero `itemTypeId==0` empty-slot sentinels in this load-phase capture (all records
  carry a body).

### 5.13 Vehicle compact record (S2C 0x0A trailing event)

`[orig: Entity_SerializeVehicleState @ 0x460560 — renamed 2026-07-04 from
Entity_SerializeMountedVehicleState]`. Used by every item
whose entity class tag is `CHel` / `cveh` / `cbot` / `cpln` / `ctrn` (per
§5.10b dispatch table). Both write (mode 1) and read (mode 2) paths handle
format type 11 only — the callback rejects modes 3/4 (extended), so vehicles
never appear in a C2S 0x0C body. They're host-pushed inside the S2C 0x0A
trailing event-loop `tag==1` record.

The write side branches first on whether the entity has an attached parent
(`entity+40`) — if so, position is vehicle-LOCAL (via
`Entity_TransformWorldToLocal`), otherwise anchored to the recipient eye
(`g_priority_ref_*`, the §5.9 header refs `[orig: @ 0x460c7d]`; the read adds
back the header-mirrored anchor `dword_A822E4..EC`). Then on `flagsByte & 4`:
if set, only a small orientation block follows; if clear, the full
weapon/turret block follows.

**THE FORM SEMANTICS (2026-07-04 — supersedes the "mounted form" reading):**
the `flags & 4` short form is the **DEAD/WRECK pose-only form**. The death
family sets `Flags |= 6` — bits 1+2 together — at every vehicle death site
`[orig: Entity_HandleDeathEvent @ 0x407118; Entity_ProcessVehicleDestruction
@ 0x466b7c; Entity_ProcessDestructibleDeath @ 0x43fbf6; Entity_InitDeathState
@ 0x48f96b; WeaponOverlay_HandleDamage @ 0x53c4f6]`, and the euler_y/euler_x
tail is the wreck's frozen full ORIENTATION (dead vehicles tumble; live ones
derive attitude from their own physics). Golden ASH_I5A proof: 16 parked dune
buggies flip to `flags=0x06` short-form records in ONE frame (f=237868 — a
mass-death event), while the joiner's RIDDEN vehicle (carrier 0x1034, boarded
at the deploy release f=240019 and driven for minutes) **streams the 21-B full
form the entire drive**. Riding/driving is not a wire form — see the
drive-authority subsection below. The serializer was renamed
`Entity_SerializeVehicleState` accordingly; the reimpl decoder's `is_mounted`
became `is_dead_pose` (nw_pp now prints `DEAD-POSE`/`live`; dumps predating
2026-07-04 show `MOUNTED`/`unmounted`).

| off | bytes | field (new name) | gate | write-source | read-dest |
|---|---|---|---|---|---|
| 0 | 2 | parentSlotHandle | always | `(pool<<12)\|slot` from entity+40 (`0xFFFF`=none) | resolves parent entity |
| 2 | 2 | posX compressed | always | entity+4 (vehicle-local if parent ≠ none) | entity+4 (local→world) |
| 4 | 2 | posY compressed | always | entity+8 | entity+8 |
| 6 | 2 | posZ compressed | always | entity+12 | entity+12 |
| 8 | 2 | eulerZ (i16 BAM `(v+0x8000)>>16`) | always | entity+16 | entity+576 |
| 10 | 1 | flagsByte | always | entity+36 (low byte) | entity+36 |
| 11 | 2 | eulerY (i16 BAM) | `flagsByte & 4` (dead-pose) | entity+24 | entity+584 |
| 13 | 2 | eulerX (i16 BAM) | `flagsByte & 4` (dead-pose) | entity+20 | entity+580 (dead-pose form ends here) |
| 11 | 2 | weaponX compressed | NOT `flagsByte & 4` | entity+160 | entity+160 |
| 13 | 2 | **healthWord raw u16** | NOT `flagsByte & 4` | entity+286 (the vehicle HEALTH word) | entity+286 (stored verbatim `@0x460aff`) |
| 15 | 2 | weaponAimY compressed | NOT `flagsByte & 4` | vehicleData[136] | vehicleData[177] |
| 17 | 2 | weaponAimZ compressed | NOT `flagsByte & 4` | vehicleData[135] | vehicleData[178] |
| 19 | 2 | weaponHeading (i16 BAM high) | NOT `flagsByte & 4` | vehicleData[132] | vehicleData[179] |

Total: **15 B** dead-pose (`flagsByte & 4`), **21 B** live.

`vehicleData` is `*(_DWORD **)(entity + 100)` — an auxiliary state buffer
attached to mounted vehicles for weapon-aim tracking. Note the write side reads
weapon-aim from `vehicleData[136/135/132]` while the read side lands the
decompressed values into a *different* slot triple `vehicleData[177/178/179]`
(write-source ≠ read-dest — the earlier single "landing" column conflated them).

**Read-side carrier semantics (2026-07-27, D-NET-195).** The off-0
`parentSlotHandle` is the CARRIER (deck/ground entity) and is consumed per
record, not latched: the reader resolves it (`0xFFFF`, pool ≥ 5, or a
capacity overflow → null `[orig: @ 0x46085d..0x46086c]`), and

- a resolving parent whose `entity+32` is still zero (not yet alive on this
  client) makes the reader QUEUE a C2S `0x0F` broken-entity repair request for
  the parent and BAIL the whole record `[orig: @ 0x4608ae..0x4608c1]`;
- a live parent composes THIS record's vehicle-local position against the
  parent's current pose `[orig: Entity_TransformLocalToWorld @ 0x4608ce]`
  while the Euler fields pass through UNTRANSFORMED (the wire eulerZ lands
  verbatim at entity+576 `[orig: @ 0x4607f5]` — vehicle orientation stays
  world-absolute even when the position is carrier-local);
- a null parent takes the anchor-relative leg `[orig: @ 0x4607b6..0x4607c9]`;
- either way the resolved pointer (or null) is RE-LANDED at `entity+40`
  `[orig: @ 0x460802]` — absence RELEASES the carrier; nothing persists from
  earlier records.

This is the same lift D-NET-67 witnessed for rider records. It is distinct
from the §5.11 spawn `parentHandle`, which lands at `entity+368`
(`occupantEntity`) and carries NO transform semantics — conflating the two
glued world vehicles to players (D-NET-195).

**Field labels corrected 2026-06-17 (D-NET-63); off-13 re-corrected 2026-07-02.** The
`eulerZ/eulerY/eulerX` triple (Z pre-branch always; X/Y mounted-only) feeds
`Math_BuildFixedPointMatrixFromEulerAngles`; the unmounted block is weaponX + the vehicle
HEALTH word + weapon-aim Y/Z + a weapon-heading BAM. **The off-13 u16 is NOT "turret pitch"
— that was an unwitnessed decode-era guess that survived into the IDB annotation**: the read
stores it verbatim to entity+286 (`@0x460aff`), the same +0x11E Health offset the §5.10
quantizer reads, and it drives the full damage model. Live-witnessed both ways on our host:
sending 0 killed every map vehicle each frame (retail-join v12); sending the world default 100
rendered them all burning (v13). Reimpl name: `VehicleCompactRecord::health_word`.

**Vehicle state machine driven by this record (witnessed 2026-07-02):**
- `flagsByte` bit 0x02 = DESTROYED state; wire transitions drive the client:
  wire set + local clear → pose snap + `Entity_KillBySlotId @ 0x42BCE0` if local health ≠ 0
  (`@0x460a25-0x460ad9`); wire clear + local set → `Entity_RespawnVehicle @ 0x45FF40`
  (`@0x460918-0x460964`).
- **Burn/damage visuals are pure functions of Health vs itemDef** [orig:
  `Entity_UpdateVehiclePhysics @ 0x48AF00` region 0x48afdf-0x48b0f7, cloned per vehicle-physics
  family]: destroyed = `!(Flags&2) && Health <= 0` (move-mode 21 wreck); **burning** =
  `0 < Health <= itemDef->criticalHp (+0x180)` (fire effect + authority self-drain
  `itemDef+0x182` per 64 ticks); **smoking** = `Health < healthMax>>2` (25%); regen =
  `itemDef+0x184` per 64 ticks up to healthMax.
- **Spawn health**: the 0x0D apply memsets the entity (Health 0) and the client lifts every
  pool-1/2 entity to `itemDef->healthMax` at `Game_StartMission`'s reload (`@0x522830`); the
  0x18 full spawn sets it explicitly (`@0x433780`), as does `Entity_InitFromItemDef @ 0x49E550`.
  So post-join vehicles sit at healthMax until 0x0A records say otherwise — the host MUST send
  real healthMax-scale values here.
- **The 0x0D record's optional 0x8000-gated u16 is NOT health** — it is the capture-zone/
  proximity radius, entity+0x15E, filled from .bms record word 14 (`@0x433206`; consumers
  `CaptureZone_*`, `render_minimap_slot_blip`, `Server_PositionPlayerForSpawn`
  (then still kong-misnamed `CMap_SetupSpawnCamera`; renamed 2026-07-03)). Reimpl renamed
  `zone_radius_short` and stopped populating it from Entity::health (golden ASH_I5A vehicle
  0x0D records carry no 0x8000 flag).

#### The vehicle DRIVE-AUTHORITY chain (witnessed 2026-07-04 — the v33 duplicate-model/can't-drive round)

**Vehicles have NO wire uplink.** The client's per-frame C2S 0x0C serializes
exactly ONE entity — `g_local_player_entity` `[orig: Client_ProcessNetworkFrame
@ 0x42c180, the single Player_BuildTag0CInputBody call @ 0x42c482]` — and this
callback returns −1 for the extended modes 3/4 `[orig: the mode switch
@ 0x460578/0x460580]`. Golden ASH_I5A: 344/344 C2S 0x0C bodies carry the
player's own handle, zero C2S 0x26 needed for the host player's own drive.

**Drive is host-side simulation off the driver's replicated input.** The
vehicle motor's input block gates on `itemDef->attrib & 0x40` (PlayerControl)
and the CONTROLLING occupant (`vehicle+0x170`, written by
`Entity_AttachToVehicleSlot @ 0x4946D0` types 2/5):
`(occupant->Flags & 0x100) && (occupant == g_local_player_entity ||
g_napi_np_ctx.is_authority)` `[orig: Entity_UpdateVehiclePhysics @ 0x48af00
gate @ 0x48b0ff]`. On the HOST that consumes the driver's REPLICATED
MoveOrder/heading/analog axes (all landed by the §5.10 0x0C apply): commanded
speed = `itemDef->playerSpeed` through the 8-way direction switch (turn-in-place
zeroes it, back halves and negates it), the steer target is the DRIVER's live
`Yaw` (mouse steer) or `yaw ± ramp` under key-steer (+2°/tick, cap 50°), stance
bits 0x100/0x200 shift the speed down, and the vehicle yaw turns by
`-speed × wheel-deflection` while grounded `[orig: the LABEL_123 modifier block
@ 0x48b490; the steer chase @ 0x48b9e9; yaw += modelPtr0 @ 0x48ef60]`. The
DRIVER's own client runs the same block as prediction (`occupant ==
g_local_player_entity`), corrected by the interp staging of the incoming full-form
records. There is no drive-ownership grant anywhere: **vehicle `+0x1CC`
("ownerSession") is the smoke/burn EFFECT-EMITTER handle** — spawned by the
physics when smoking (`submit_effect_descriptor` result store `@ 0x48b0f6`),
released at detach/respawn via `CEffectEmitter_ReleaseSafe @ 0x5f69f0` (renamed
2026-07-04 from the CNapiSession_FlushSendSafe kong misname; it guards on the
`CEffectSystem_Init` singleton).

The v33 defects reduce to the missing host motor: our host streamed the ridden
Super Puma pinned at its pad while the rider's client predicted motion — the
"second vehicle model" was the prediction-vs-wire fight, and "can't drive" was
the vehicle never responding server-side. Reimpl: `libs/world/vehicle_motor.{h,cpp}`
(the ground-family authority core) + the AiSystem vehicle pass + the items.def
physics-property parse (`libs/def`, scaled per `ItemDef_ParsePhysicsProperty
@ 0x49d870`) — D-NET-161.

### 5.14 Infantry / AI compact record (S2C 0x0A trailing event)

`[orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320]`. Used by items
whose entity class tag is `org0` / `org1` — AI infantry units and any other
"organic" pool-0 entity that isn't the player. Like §5.13, modes 1/2 only
(type 11), modes 3/4 rejected. Used in S2C 0x0A trailing `tag==1`.

The write side picks the parent vehicle from `entity+364` (mount slot) if set,
otherwise `entity+40` (general parent). Position is vehicle-local when a
parent exists, world-relative otherwise.

| off | bytes | field | landing |
|---|---|---|---|
| 0 | 1 | seatBoneIdx | entity+343 if mounted, else 0 |
| 1 | 2 | vehicleSlotHandle | `(pool<<12)|slot` resolved from `entity+364`/`+40`, `0xFFFF`=none |
| 3 | 2 | posX compressed | entity+4 (vehicle-local if parent set) |
| 5 | 2 | posY compressed | entity+8 |
| 7 | 2 | posZ compressed | entity+12 |
| 9 | 1 | yawByte (BAM high `(v+0x800000)>>24`) | entity+16 |
| 10 | 1 | flagsByte | entity+36 |
| 11 | 1 | pitchByte (clamped delta entity+748 vs entity+16, BAM high) | entity+748 |
| 12 | 1 | aimYawByte (BAM high of entity+720) | entity+720 |
| 13 | 1 | animByte | entity+696 if non-zero else entity+700 |

Total: **14 B** per record (fixed).

The read side runs an animation-state machine through a lookup table
`g_animStateFlagsTable[]` indexed by animState — non-zero bits 4 / 0x20 in the table
entry select whether to write +696 vs +700 — and handles a special path
through `Entity_TryAttachOrDetach` when `flagsByte & 2` flips. None of that
affects the wire layout.

### 5.15 Guided weapon record — per-(mode, field-group) codec

`[orig: Entity_SerializeGuidedMissileState @ 0x447C50]`. Used by item classes
`rokt` / `stng` / `hlfr` / `jvln` / `arty` / `arti` (rockets, Stinger / Hellfire /
Javelin / artillery). Structurally unlike §5.10 / §5.13 / §5.14: NOT one fixed
body keyed on format 11, but a **matrix of `mode` (`packetCtx[6]` ∈ {1..4}) ×
`field-group` (`packetCtx[7]` ∈ {1..6})** — each call serializes exactly one field
group of a projectile-in-flight's state.

**Modes** (`packetCtx[6]`): 1 = write-full, 2 = read-full (no-op when the receiver
is the authority), 3 = write-delta, 4 = read-apply (delta).

**Field groups** (`packetCtx[7]`) and per-mode payload sizes (bytes AFTER the
5-byte entity sub-header):

| group | landing | write-full(1) | read-full(2) | write-delta(3) | read-apply(4) |
|---|---|---|---|---|---|
| 1 status (launch) | entity+696\|=1, +276\|=0x1000 | 1 B (`0x00`) | 0 B | 1 B (`0x00`) | 0 B |
| 2 clear-target | entity+696&=~2, +724=0, +728=-1 | 1 B (`0x00`) | 0 B | 1 B (`0x00`) | 0 B |
| 3 target+pos | target entity+724, pos entity+700/704/708 | 14 | 14 | 2 (target only) | 2 (target only) |
| 4 target+type+pos | + weapon-type entity+698 | 18 (+target) | 18 (+target) | 16 (no target) | 16 (no target) |
| 5 pos | entity+700/704/708 (read clears target) | 12 | 12 | 12 | 12 |
| 6 attach-offsets | entity+740/744/748 | 12 | 12 | 12 | 12 |

The target handle is the 2-byte `(pool<<12)|slot`; the weapon-type is a 4-byte
field whose low u16 is the type id; position/attach are raw i32. The **delta modes
(3/4) drop the target handle** the full modes (1/2) carry — group 3 delta is just
the handle, group 4 delta omits it entirely.

**Group-selector framing (the previously-missing piece, now witnessed).** The
`(mode, group)` pair is set by the *caller*, not encoded in this function. On the
host C2S-receive path `[orig: dispatch_entity_packet_callback @ 0x4D6A80]` reads the
5-byte entity sub-header (`[u16 handle][u16 type_id][u8 sub_op]`, §5.10b), copies the
**`sub_op` byte into `packetCtx[7]` (the field group)** and hardwires
`packetCtx[6]=4` (read-apply). So for a guided entity the wire-carried `sub_op` IS
the field-group selector (1..6) — the same byte that is 10/11 (extended/compact
format) for the player/infantry/vehicle classes. Because the serializer rejects
format 11, guided entities never appear as a §5.10b 0x0A compact record, and
`decode_frame_update` correctly fails closed on `EntityClass::Guided`. The
write-side 1-byte `0x00` marker for groups 1/2 (read side reads 0 B) is a framing
byte the dispatcher owns — reproduced by the port but not round-trippable at the
serializer layer.

**Port + status.** `GuidedRecord` + `encode_guided_field_group` /
`decode_guided_field_group` (`ingame_encode.cpp` / `ingame_decode.cpp`) port the
write/read switches; `nw_ingame_guided_test` round-trips every (mode, group).
**Deferred** (D-NET-64): wiring the codec into the 0x0C entity-packet dispatch and
validating the per-group field semantics against the wire — no capture in hand
carries guided traffic (the 2026-06-16b loopback fired no rockets). Verdict:
**partial** (IDA-structural; round-trip-pinned; wire-unvalidated).

### 5.16 C2S 0x06 — client-fired-round (3-player loopback 2026-06-16d)

`[orig: NapiNPServerMsg_0x006_ClientFiredRound @ 0x513310]`. Fixed **45 B** body. The
joiner reports a discrete weapon-fire event: calculated fire pose, target, shot sequence,
and five u16 low-word deltas that let the host reconstruct that same pose against its live
shooter entity dwords before running `Server_ClientFiredRound @ 0x50BAA0`. The five words
are **not** an independent or weapon-specific muzzle-offset vector.
On success, when `fire_flags & 1 == 0` (primary fire), the host advances the shooter's
ammo-tick counter at `playerSlot+0x178D8` by `AdmDef_GetEntryByIndex(adm_index)[276]`
(reload-cooldown ticks).

| off | bytes | field | landing (host receiver) |
|---|---|---|---|
| 0 | 4 | `current_tick` (u32 LE) | client network-role `currentTick @ 0xA8229C`; compared against `playerSlot+0x178D8` for cooldown/freshness |
| 4 | 2 | `shooter_handle` (u16 LE, `pool<<12\|slot`) | pool resolve via `g_pool_list`; `0xFFFF` or `(h&0xF000)>=0x5000` rejects |
| 6 | 1 | `fire_flags` (u8) | bit 0 = alt fire (skips ammo decrement), bit 1 = AdmDef-indexed primary, bits 4-5 = the pre-consume magazine count's low two bits; → `dest[3]` |
| 7 | 1 | `adm_index` (u8) | `AdmDef_GetEntryByIndex @ 0x53FC80` key (action-descriptor — same index space as §5.9.1 weapon-hit `adm_index`) |
| 8 | 4 | `pos_x` (i32 LE, 16.16) | calculated fire-pose origin X; → `dest[4]` |
| 12 | 4 | `pos_y` | → `dest[5]` |
| 16 | 4 | `pos_z` | → `dest[6]` |
| 20 | 4 | `dir_x` (i32 LE) | fire YAW as a 16.16 TURN FRACTION — the MISSION BEARING directly, NOT the 0x0A euler_z heading frame (which is 90° − yaw): v29 wire-proof, two duel baselines within 1.4° (D-NET-153). Host applies `<< 16` (= BAM32, `dir_x_shifted`); → `dest[7]` |
| 24 | 4 | `dir_y` | fire PITCH, same encoding + `<< 16` shift; → `dest[8]` |
| 28 | 2 | `target_handle` (u16 LE) | hit entity; `0xFFFF` = no specific target; → `dest[16]` |
| 30 | 2 | `hit_part` (u16 LE) | **PACKED — `(roster slot << 9) \| (shot seq & 0x1FF)`**, NOT a bare counter (corrected 2026-07-26, see below); → `dest[17]` → `word_B7C670` → tag-2 `shot_seq` |
| 32 | 1 | `extra_byte1` (u8) | low byte of shooter `entity+352`; → `dest[18]` |
| 33 | 1 | `extra_byte2` (u8) | → `dword_C86FB4` (last-fire global) → `dest[19]` |
| 34 | 1 | `misc_byte` (u8) | → `LOBYTE(dest[20])` |
| 35 | 2 | `delta_x` (u16 LE) | `low16(fire X) − low16(shooter X)`; host adds to `shooter_entity[1]` → `dest[10]` |
| 37 | 2 | `delta_y` (u16 LE) | `low16(fire Y) − low16(shooter Y)`; host adds to `shooter_entity[2]` → `dest[11]` |
| 39 | 2 | `delta_z` (u16 LE) | `low16(fire Z) − low16(shooter Z)`; host adds to `shooter_entity[3]` → `dest[12]` |
| 41 | 2 | `delta_yaw` (u16 LE) | `low16(fire Yaw) − low16(shooter Yaw)`; host adds to `shooter_entity[4]` → `dest[13]` |
| 43 | 2 | `delta_pitch` (u16 LE) | `low16(fire Pitch) − low16(shooter Pitch)`; host adds to `shooter_entity[5]` → `dest[14]` |

**Exact client producer.** `WeaponAction_Fire @ 0x542B10` obtains a six-dword
`{X,Y,Z,Yaw,Pitch,Roll}` descriptor from `Entity_CalcWeaponFirePosition @ 0x4DC750`;
the ordinary on-foot leg is `{Position + CameraOffset, Yaw, Pitch + pitchBlend, Roll}`,
with separate mounted/scoped transform legs. `Entity_FireWeaponAndSendPacket @ 0x42BD80`
passes that descriptor to `NetPacket_WriteEntityPositionUpdate @ 0x42A610`. The writer
stores full X/Y/Z, rounded angle high words, and the five modulo-u16 differences above.
Grilled 2026-07-23, the remaining field sources: `target_handle` (off 28) is the writer's
pool-resolve of `entity->aiRuntime[3]` — the AI CURRENT-TARGET pointer — with null (a human
player has no aiRuntime) encoding `0xFFFF` [@0x542c15; @0x42a70d..0x42a7b1], so a human
shooter's fire always carries `0xFFFF`; `hit_part` (off 30) is the client-spawned round's
session-slot shot-seq word (`session_ctx[195*RoundData_SpawnRound(...)+30]` @0x42c030); and
the wire `fire_flags` byte is composed AT THE FIRE CALL SITE as
`(MountSlot "seatMask" & 3) << 4 | mode-bits` BEFORE `consume_weapon_ammo` [@0x542c11] —
confirming the bits-4-5 pre-consume-magazine reading (`seatMask` is a suspected IDB field
misnomer, unrenamed pending a full-use sweep).
The receiver at `@0x513310` keeps the claimed full fire pose in `dest[4..8]` and rebuilds
`dest[10..14]` from its current shooter pose plus those words; the latter feeds the
moving-carrier re-anchor paths in `Server_ClientFiredRound`.

**Cross-witness against `host_and_join_game_on_opennovaworld_loopback_threeplayers_more_gameplay.pcapng`:**
- f=2057 (adm=7, fire_flags=0x02): `tick=15532061 pos=(-444.8, -413.2, 14.5) hit_part=1025`.
- f=2061 (adm=7, +7 ticks ≈ 113 ms): `tick=15532068`, hit_part increments to 1026 — read at the
  time as "a monotonic fire-counter". **CORRECTED 2026-07-26: `hit_part` is a PACKED word,
  `(roster slot << 9) | (shot seq & 0x1FF)`.** 1025 = `0x0401` = roster slot **2**, seq 1; 1026 =
  `0x0402` = slot 2, seq 2. The counter reading was right about the low bits and blind to the high
  ones — the slot field was in this capture all along.
  The packing is witnessed on the HOST's own composition of the same word:
  `word_B7C670 = (*((WORD*)v91 + 10) << 9) | (packet & 0x1FF)` [orig: `Server_ClientFiredRound
  @0x50bda5`], where `v91 + 20` is the shooter's per-player record slot id — the value the S2C
  `0x04` hands the client in body byte 17 [orig: `NetPacket_WriteSlotAssignment @0x502b30`]. On the
  NETWORK arm the host does NOT recompose it: it copies our raw word into the global verbatim
  (`@0x50c2ba`, `@0x50c774`).
  Confirmed on two live captures the same day: a retail joiner at `mySlot=1` sent
  `0x0201/0x0202/0x0203` for shots 1/2/3
  (`.scratch/golden/retail-coop-playerinfo-join.pcapng`), while OUR joiner — also `mySlot=1` — sent
  `0x0001/0x0002/0x0003` (`.scratch/golden/opennova-joiner-profile-kit-verified.pcapng`). Zero slot
  bits name roster slot 0, which on a listen host is the HOST ITSELF, and the maintainer observed
  exactly that: the retail host's own first-person weapon reacted every time our joiner fired, and
  only when both held the same weapon. A retail↔retail pair on the same host never reproduced it.
  Builder + accessors: `opennova::pack_fired_round_hit_part` /
  `fired_round_hit_part_slot` / `_seq` (`libs/npwire/include/npwire/ingame_decode.h`), pinned by
  `tests/novaworld/nw_ingame_c2s_uplink_test::test_fired_round_hit_part_packing`.
- f=2278: weapon switch → `adm=61, fire_flags=0x32` (primary + pre-consume clip low bits 3), distinct pose-delta
  signature. The byte-witness pin is `tests/novaworld/nw_ingame_c2s_uplink_test::test_client_fired_round`.

**Weapon-variety witness (probe3_again, 2026-06-19).** The richer 2-shooter session exercises the
decoder across a broad slice of the `adm_index` space — **15 distinct weapon adm indices** in 260 fire
events, both human shooters: `0x0006` (FooPlayer, 203 fires) and `0x0007` (TestPlayer1, 57). Observed
indices (fire count): `11`×97, `82`×67, `9`×32, `34`×27, `39`×6, `13`×6, `1`×5, `59`×4, `55`×4, `14`×3,
`58`×2, `56`×2, `52`×2, `50`×2, `4`×1. `decode_client_fired_round` full-consumes every one (asserted by
`nw_probe3again_lifecycle_test`). All 15 are **ballistic** weapons — none drives the §5.15 guided codec
(D-NET-64 stays open; see below).

**AdmDef name resolution is deferred (runtime table, not a wire field).** The wire carries only the
`adm_index`; the human-readable weapon name lives in the runtime `AdmDefs` table (`@ 0x24E7FE0`, 1120 B
per entry, populated when the engine loads the `.adm` action-descriptor data — `AdmDef_GetEntryByIndex @
0x53FC80`). Resolving indices to names offline would need a `.adm`/weapon-def parser (a separate
game-data library), not a wire decoder; `nw_pp` therefore prints `adm` raw. Scoped as a future item —
this is the same AdmDef index space shared by §5.9.1 (weapon-hit) and §5.30 (0x5A loadout).

**The full host pipeline (witnessed 2026-07-03, D-NET-152 — closes the old follow-up).**
`NapiNPServerMsg_0x006_ClientFiredRound @ 0x513310` gates on authority, `!g_InCeaseFire`,
the connection's player slot (+352→+192), `!slot+100567`, and `CServerTick` phase == 3;
anti-spoofs the claimed shooter against the slot's OWN entity [orig: @0x51358d] and runs the
fire-rate/freshness gate `PlayerSlot_IsActive @ 0x4FC760` (with `slot+96480` armed, fire is
rejected until the uplink tick passes `slot+96472` — stamped on success as
`tick + AdmDef[276]` cooldown ticks [orig: @0x513740]; `dest[9]` = the zeroed third direction
component, `dest[2]` = the shooter entity ptr). Then `Server_ClientFiredRound @ 0x50BAA0`:

- **Guards**: grounded+moving reject (`Flags & 0x100 && Flags & 2` → −10); adm lookup
  (−3 "Tried to fire NULL wpn"); NULL dcb (−8); dcb ≠ shooter (−9 "bad addround owner");
  mounted-only weapon unmounted (`admFlags & 0x80` → −12/−18); the parachute-weapon anim
  gate (−14); the ammo check `WeaponSlot_CanFire @ 0x541BA0` (ex
  `should_send_entity_update` misnomer: busy weapon child, underwater-fire ban vs
  `Env_WaterHeightFixed`, **clip u16 slot+16** or the adm+220 pool, the adm+224 score-lock)
  → −15 `"server_ClientFiredRound: … (NO AMMO!)"`.
- **Warp compensation**: `savedLivePose − Position` for the shooter AND its `groundEntity`
  carrier [orig: @0x50bbac] — `savedLivePose (+0x80)` is Position saved at the TOP of every
  entity physics frame (all `Entity_*Physics/Movement` updates, e.g. @0x46e12d/@0x484054),
  so the delta is the current tick's not-yet-reported motion. A carrier that moved > 0xF
  units re-anchors the fire to the live pose (rounddef flags 0x400, or a camera raycast
  that hits the shooter's own carrier) [orig: @0x50bc41].
- **Side stamps**: `entity+352` = the wire extra_byte1 word; `entity+688` = adm on PRIMARY
  fire (the §5.10 `equipped_adm_index` mirror [orig: @0x50bd56]); `shooter+104→+12` = the
  claimed target (read LIVE by the §5.9.1 tag-2 serializer [orig: @0x50c2ad]);
  `word_B7C670` = `hit_part` (the per-shot sequence the tag-2 `shot_seq` echoes).
- **NET PRIMARY fire** does NOT call `RoundData_AddRound` directly: the validator resolves
  the per-player weapon slot (the 100-B array at `playerSlot+464`, index
  `adm[4] + 65*adm[0]` = rank + 65*category [orig: @0x50c0d7]; vehicle modes take the
  vehicle's slot), stamps the slot's +64..84 pose = the CLIENT's claimed origin/direction,
  latches slot+94 bit0, and invokes the adm **'fire' ACTION** — `*(admEntry+684)`, action
  slot 3 of the 12-action table at admEntry+676 (suffix/default table @0x830B94), default
  handler `WeaponAction_Fire @ 0x542B10`. The fire action re-checks can-fire, reads the
  latched pose (`Entity_CalcWeaponFirePosition @ 0x4DC750` returns slot+64..84 when the
  bit0 latch is set), calls `Entity_FireWeaponAndSendPacket @ 0x42BD80` — which on the
  authority builds a LOCAL-mode fire request and **re-enters `Server_ClientFiredRound`**
  (local path: ~2u origin-distance clamp vs the entity, scoring, then
  `RoundData_AddRound @ 0x4FDB40` → the §5.9.1 ring + `RoundData_SpawnRound`) — then
  decrements ammo (`consume_weapon_ammo @ 0x540850`: clip u16 slot+16 when adm+220 == 0,
  else the per-player pool `playerSlot+89176 + 4*adm220` / the global `data @ 0xB761E8`
  for AI), runs the 3-round-burst counter (`Flags & 0x20`), chains action 3 RECOIL, and
  plays the fire sound. **ALT fire** (bit 0) and the HOST'S OWN local fire call
  `RoundData_AddRound` directly; a 0x06 whose dcb is the host's local player returns 0
  untouched [orig: @0x50c18d] — the client-side of the same function is where a CLIENT
  calls `RoundData_SpawnRound` itself for its own fire and queues the C2S 0x06 (client
  prediction; why a shooter's own rounds are never tag-2-echoed back, §5.9.1).

### 5.17 C2S 0x21 — anti-cheat CRC reply (3-player loopback 2026-06-16d)

`[orig: handle_anti_cheat_crc_check @ 0x502050]`. Sent in response to S2C 0x30 (`0x5029B0`) /
S2C 0x31 (`0x5024A0`) anti-cheat challenges. Effective wire shape is **5 B** (`u8 player_index
+ u32 expected_crc`), but every observed reply has 4 trailing zero bytes the handler never
reads — `len=9 B` is the protocol layer's framing minimum, not a payload requirement.

| off | bytes | field | landing (host receiver) |
|---|---|---|---|
| 0 | 1 | `player_index` (u8) | record index into the 276-stride `dest[]` player array; out-of-range early-returns |
| 1 | 4 | `expected_crc` (u32 LE) | client's claim for `CRC_ComputeCustomTable(dest+idx*276, 276) ^ playerCtx[89924]` |
| 5 | 4 | echoed challenge key (u32 LE) | observed-zero here (the challenge carried a zero key); the handler does not advance the cursor past byte 5. **Corrected 2026-07-26** — the builder writes the echoed key there, it is not framing padding (§5.65, `NetPacket_WriteEntityCRCChecksum @ 0x42B020` key echo `@ 0x42B14D`) |

Host's validation [orig: 0x5020D5..0x5021CD]:
1. Snapshot 6 volatile fields (`+64/+68/+72/+76/+104/+112`) of `dest[player_index*276]`.
2. Zero them.
3. `CRC_ComputeCustomTable @ 0x53C820` over the 276-B record.
4. Restore the 6 fields.
5. Compare `(computed_crc XOR playerCtx[89924])` against `expected_crc`.
6. On mismatch — and only if `playerCtx[5] == 0 && playerCtx[96483] == 0 && playerCtx[89896]
   == 0 && dword_B4C698 == 0` — call `Server_WritePuntLog(playerCtx, "ACRC", ...)` and, if
   `playerCtx[96481] == 0`, send chat message `"PUNT ACRC"` via
   `CNapiNPConnection_SendChatMessage @ 0x4C7EF0` to disconnect the cheater.

**Cross-witness:** f=2101 (`player=18 expected_crc=0x42a13f29`), f=2388 / f=2694 (player=56,
both `0x82c31207` — the second is a re-issued challenge against the same record). Byte-witness:
`tests/novaworld/nw_ingame_c2s_uplink_test::test_client_checksum_reply`.

### 5.18 C2S 0x47 / 0x48 — request entity-state broadcast + stub (3-player loopback 2026-06-16d)

Resolved targets of the now-retired "Phase B sweep pending" TODO.

**C2S 0x47** `[orig: NapiNPServerMsg_0x047_SendEntityState @ 0x510ED0]`. **Header-only, 0 B
payload**. Acts as a request to host: "re-broadcast the sender's entity state to everyone".
Host pulls the sender's player struct (`connection+352 → +192`), seeds the global
`g_napi_msg_payload_buf` with `entityPtr` at offset 1128 and length-tag 32 at offset 1126,
calls `sub_510890` to serialize 4096 B, then `NapiNPServer_SendFiltered(..., 0x75u, 1, 0,
buf, len)` — i.e. **emits S2C 0x75 to every session** (filter=1, send_flag=0).

Confirmed in the loopback: `C f=715 0x47 → S f=716 0x75 (len=2: "00 02")` and similarly at
f=717 / f=719. The 4 C2S 0x47 events observed in the 3-player capture each triggered
exactly one S2C 0x75 broadcast.

**C2S 0x48** `[orig: NapiNPServerMsg_0x048 @ 0x510F30]`. Server-side **empty stub** — the
function body is `void f() {}`. The 4-byte payload observed in capture (`03 00 00 00`,
plausibly a client-side counter) is read off the wire by the protocol framing and discarded.

The mirror `NapiNPClientMsg_0x048 @ 0x4284B0` exists on the client side — S2C 0x48 has a
non-trivial handler — but no S2C 0x48 was emitted in the 3-player capture, so its semantics
remain TBD. **Open follow-up:** decompile `NapiNPClientMsg_0x048 @ 0x4284B0` if a future
capture surfaces an S2C 0x48 frame.

### Cross-witness append for §5.10 — 3-player loopback 2026-06-16d

The C2S 0x0C extended (type-10) field map decoded against the 2026-06-16b 2-player capture
is independently confirmed against the 3-player capture by
`tests/novaworld/nw_ingame_c2s_uplink_test::test_extended_uplink_stationary_on_foot`. Frame
1905 (joiner s2, on-foot, stationary near `(-441.7, 376.7, 11.9)`) hits every field
exactly: vehicle_handle = 0xFFFF, posXYZ = i32 16.16 LE world coords, heading = `0x382D`
(i16), pitch = 0, four `(weapon_id, fire_counter)` pairs all non-zero (mid-game state with
the player having fired all four loadout slots). The vehicle-mounted branch is exercised
from f=2053 onward when the joiner mounts handle `0x1033` (pool 1 / slot 51) and positions
flip to vehicle-LOCAL small-magnitude i32s — same wire shape, different host interpretation.

### 5.19 Tag 0x40 — minimap-overlay update / capture-zone state (controlled capture 2026-06-17)

`[orig: NapiNPClientMsg_0x040 @ 0x425A50 → sub_425A54 @ 0x425A54 (reads count) → MapOverlay_DecodeOverlayEntries @ 0x5BEBB0 (6-byte entry walker) → MapOverlay_UpdateOrCreateSlot @ 0x5BEA60]`. Refines the older
"capture-zone state (1 B)" note — the 1 byte it meant is the per-zone icon/color byte; the packet
itself is a general minimap-overlay update carrying N entries.

Wire shape: `[u8 count][count × 6-byte entry]`.

| off (within entry) | bytes | field | landing / meaning |
|---|---|---|---|
| 0 | 2 | handle (u16 LE) | `pool<<12\|slot`, resolved via `g_pool_list` |
| 2 | 1 | param | slot+2 (`param_size`); 0 for zones |
| 3 | 1 | iconColor | index into `g_minimap_overlay_color_table` @ 0x840A10 (LE `0xAARRGGBB`): **0x0c neutral/green** (0xFF208020), **0x09 Red** (0xFF802020), **0x0a Blue** (0xFF304080). Team↔color binding per the prior dvxi5-AS capture3 correlation (`replication_min.cpp build_tag_40_capture_zone_state`): **0x09 = Red (team 2), 0x0a = Blue (team 1)** — the per-zone capture state. (An initial RGBA-byte-order read had 0x09/0x0a swapped; the LE-dword + capture3 correlation agree on Red=0x09/Blue=0x0a.) |
| 4 | 1 | flags | **0x10 = persistent capture-zone marker**; bit **0x20 = clear slot** (writes handle 0xFFFF, zeroes lifetime) |
| 5 | 1 | source | slot+4 |

Overlay X/Y/Z is read from the **resolved pool entity**, not the wire — the 0x40 packet carries
no coordinates. The textual Under-Attack / Ready-for-Takeover HUD (`draw_capture_point_status_overlays
@ 0x5A2480`) derives contest state locally from per-team proximity counts; tag 0x40 is the
authoritative minimap **color** channel.

**The server producer (witnessed 2026-07-04):** `Server_BuildOverlayStateForPlayer
@ 0x517FC0` (called per active slot from `Server_UpdateAllActivePlayerSlots @ 0x5188d6`)
walks pools 2 → 1 → 0 with per-slot RESUME indices (a budgeted walk spread across frames),
classifies each entity through `Entity_ClassifyForMinimap @ 0x50FA70` — team 1 → color
0x0a, team 2 → 0x09, else 0x0c; `attrib & 0x20000` capture triggers and `attrib & 0x40000`
spawn points → icon 0; vehicles by `itemDef->unitType` (5..8 → 15, 3/4 → 11, 12 → 25, else
10, alive only); persons 3 alive / 8 dead (14 medic-revivable); armories 13; emplacements
4/12 color 8; supply crates 16/17/29 color 15; `attrib & 0x20000000` nohud skips — and
stages 6-byte entries into a 16-slot buffer flushed per-recipient as one 0x40
(`sub_50FEA0` stage → `Server_SendPendingOverlayState @ 0x50FE20`, send-mask 32 targeted
at that slot; the golden's `count=16` chunks). Reimpl (D-NET-162): the npruntime 1 Hz
block emits per-recipient zone entries (icon 0, flags 0x10) + vehicle blips (flags 0x00,
icon by the items.def `unit_type` now parsed into `world::VehicleTraits`); the
player/emplacement/crate entries and the resumable budget walk are deferred.

**Witness:** the A&S probe "ON RE Probe AS dvxi5" (two human players, 699 0x40 records) has two
capture points — Rebel HQ (handle 0x1000, bms(0,0)) and JO Tent (handle 0x1001, bms(0,-40)), both
authored neutral. Rebel HQ's iconColor evolves `0x0c→0x09→0x0c→0x0a` (neutral → Red captures →
neutral → Blue captures); JO Tent stays `0x0c` all game. Per-handle histogram: 0x1000 = 310
neutral / 217 Red (0x09) / 172 Blue (0x0a), 0x1001 = 699 neutral. Six `count==3` records append a transient truck blip (handle 0x1003,
flags `0x00` ≠ 0x10 — distinguishes a blip from a zone). Decoded by `decode_capture_zone_overlay`
in `libs/npwire/include/npwire/ingame_decode.h`.

### 5.20 Tag 0x16 — PLAYER-LIST / SCOREBOARD (controlled capture 2026-06-17; header/trailer + HUD-count semantics witnessed 2026-07-03)

`[orig: server NetPacket_SerializeScoreboard0x16 @ 0x504B80 (renamed from
NetPacket_SerializeScoreboard0x16); client NapiNPClientMsg_PlayerList @ 0x42FAE0]`. All 33 records in
the probe capture decode to a 2-byte trailer remainder.

```
[u8 flags][u8 row_count (clamp 252)]
row_count × { [u8 slot_id][u16 ping LE][u16 score LE][u16 deaths LE][u8 rowFlags] }     // 8 B/row
[u8 team_count]
(team_count+1) × { [u16 score LE][u16 deaths LE][u8 kothHold][u8 ctfFlag] }             // 6 B/row
[u8 inGameCount][u8 spectatorCount]                                                      // trailer
```

- **Byte 0 is a FLAGS byte, not max_players** (2026-07-03 correction): bit0 = team-mode, bit1 =
  timed-scores → `g_scoreboard_flags @ 0xA823B8` (renamed 2026-07-03 from the bare dword label).
- Row flags: **bit0 = SPECTATOR** (from slot+100567; the old "alive" reading was a decode-era
  guess), `team = flags >> 1`. Probe: host slot0 flags=0x02 (team1/Blue), joiner slot1 flags=0x04
  (team2/Red).
- **A row is ACCEPTED only when `PlayerSlotTable_GetActiveSlot(slot_id)` knows the slot** (slot
  byte+13 active — populated by the S2C 0x46 player-sync, §5.21/§5.33); an unknown slot's row is
  DROPPED and the client retries **C2S 0x22 `[slotId, 0x1CF7]`** — the repeated-0x22 churn a host
  that never join-broadcasts 0x46 provokes (D-NET-158). Rows exclude not-yet-in-game slots
  (byte+100579) server-side, so the list grows only when a joiner completes its load (the golden
  31 B → 39 B grow right before the joiner deploys).
- Trailer: `inGameCount → g_scoreboard_ingame_count @ 0xA85B3C`, `spectatorCount →
  g_scoreboard_spectator_count @ 0xA85B40` (renames 2026-07-03). **The HUD "Number of players" =
  accepted-row count (`g_scoreboard_row_count @ 0xA823C4`) − spectatorCount** `[orig:
  draw_hud_score_overlay @ 0x593E50; TAB header HUD_DrawGameScoreOverlay @ 0x423060; rows
  HUD_DrawKillList @ 0x423A30 over the 56-B score table @ 0xA823C8]` — a hardcoded trailer pins
  every client's count (the v31 HUD defect).
- Cadence: the server broadcast runs every **311 ticks (~5 s)**
  `[orig: Server_BuildAndBroadcastScoreboard @ 0x50D960]`.
- Team table: `team_count`=2 ⇒ **3 rows** (T0 neutral / T1 Blue / T2 Red); per-player deaths mirror
  into T1/T2, T0 stays 0. Probe deaths accrue with A&S play (T1 0→47, T2 0→57); ping=0 (loopback).
- The server may **re-sort the player rows between frames** — `slot_id` is authoritative, not row
  position.

### 5.21 Tag 0x46 — PLAYER-SYNC (controlled capture 2026-06-17)

`[orig: NapiNPClientMsg_PlayerSync @ 0x431370]`. All 51 records in the probe capture decode to the
byte (every set bit consumed).

```
[u8 slot_id][u16 fieldBitmask LE]
if (bitmask & 0x8000): removal — stop (no body)
else [u8 entity_slot_id]   // pool-0 slot; handle = (0<<12)|slot
then present fields IN SOURCE ORDER (NON-numeric — 0x10 before 0x04, 0x1000 before 0x40):
  0x0001 name   cstr (≤31 + NUL)
  0x0002 clan   cstr (≤15 + NUL)
  0x0010 id/label cstr (NUL-term)
  0x0004 u8 team
  0x0008 u8 type|subtype   (type=v&0x7F→slot+16, subtype=v>>7→slot+44)
  0x0020 u8 → slot+45
  0x1000 u8 → slot+46
  0x0040 u8 → slot+48
  0x0080 u8 → slot+49
  0x0400 u8 quality (clamp 4)
  0x0800 u32 entityRef
bit 0x4000 (no body byte) → client queues a C2S 0x22 ack
```

- `entity_slot_id` is a **pool-0** slot → handle `(0<<12)|slot`. Probe: host slot0 → entity_slot 4
  → handle **0x0004**; joiner "TestPlayer" slot1 → entity_slot 5 → handle **0x0005** — both
  cross-witnessed in the 0x0A stream as the only two type-`0x14b9` "Player #1, Multiplayer"
  entities. Ties the player table (slot) to the entity pool (handle).
- Team byte (bit 0x04): host=1 (Blue), joiner=2 (Red) — matches §5.20 `flags>>1` and the 0x04 path
  is gated by `g_GameType & 0x10000`; for the A&S probe (attrib 0x10000) the `|=0x200` branch is
  skipped, confirming `g_GameType` holds the gametype-attribute word `[orig: g_GameType @ 0x24D2128]`.
- **Residual follow-up:** the bit-0x08 byte is stored in the type/subtype slot per IDA, but in the
  probe its runtime content streams a respawn countdown (0x77→0x00 on the joiner). Layout solid;
  the runtime semantic of that byte needs a producer-side grill.

### Cross-note — pool-0 organic spawn path (controlled capture 2026-06-17)

Pool-0 AI organics (infantry) spawn via **S2C 0x0C** `[orig: _0x00C @ 0x42E730]` (full-entity spawn
batch; `[u16 count]` + per-record flat layout + inline name — full field map in §5.23), NOT via
0x0D (pool-1 items/vehicles) — the probe's single
0x0D batch carried only the 4 pool-1 items and the 0x10 static batch was empty, while the 4 AI
soldiers (type 0x0816) first appear in the 0x0C batch at the authored (-70,-15) and thereafter in
the per-frame 0x0A stream. 0x0A is live replication of already-spawned entities, never the spawn
mechanism. The compact 0x0A vehicle/infantry records carry no team byte — team lives only in the
spawn/sync packets (0x0D entity+354, 0x20 flag-0x08).

### 5.22 `/PROFILE` `.sph` server-log recording — independent value oracle (controlled capture 2026-06-17)

Launching with `/profile <file>` `[orig: Game_ParseCommandLineAndInit @ 0x4a7310 (sets
g_RunningWithProfile @ 0xb4c500 + filename g_ProfileLogPath @ 0xb4c504)]` makes the engine dump a
FOURCC-chunked "server-log" recording (`host.sph` / `client.sph`). It is the engine's own *decoded*
per-frame view of the session, so it cross-validates the in-game replication RE **without any
decryption** — and `host.sph` (authority) vs `client.sph` (replicated) is the round-trip itself.
The probe produced both for the SAME session as the §5.9–5.21 capture (dvxi5 / `mission.bms` /
`TestPlayer`+`FooPlayer`).

The recorder opens in `[orig: Game_StartMission @ 0x524360 (open path @ 0x524482; ctx recordCtx @
0xb79448)]` and closes via `[orig: CServerLog_CloseAndFree @ 0x4e1a10]` from mission teardown
`[orig: Game_TeardownMission @ 0x522350]`. The per-frame write is `[orig: Game_ProcessMainFrame @
0x5263f0 @ 0x526879]`: **every 8th engine tick** (`tick & 7 == 7`; 62 Hz → ~7.75 Hz) it iterates
**`g_pool_list[0]` — POOL 0 = players** — an independent witness that pool 0 is the player pool.

On-disk chunk = `[char[4] tag][u16 length][u16 pad][payload]`; `length` is the TOTAL size incl. the
8-byte header. Tags are the reversed mnemonic (the engine writes a u32 multichar constant LE, or
`strcpy`s the reversed literal with the low length byte folded into the comma/`\b`):

| on-disk | mnemonic | writer `[orig]` | len | payload |
|---|---|---|---|---|
| `NGEB` | BEGN | open path @ 0x524482 | 28 | `u32 ver=2`, `char[16]` mission basename |
| `FEDP` | PDEF | `CServerLog_WritePlayerNameRecord @ 0x4e1cc0` | 20+nameLen | `u32 netid`, `u32 team` (read from **entity+354**), `u32 nameLen`, `name` |
| `GEBF` | FBEG | `CServerLog_WriteTimestampRecord @ 0x4e1aa0` | 12 | `u32 frameIndex` (= tick>>3) |
| `TADP` | PDAT | `CServerLog_WritePositionRecord @ 0x4e1b00` | 44 | see field map below |
| `CPSP` | CDAT | `CServerLog_WriteEntityDataRecord @ 0x4e1bd0` | 168 | `u32 netid` + 154 B blob (not emitted in this capture) |
| `KRBP` | PBRK | `CServerLog_WriteDeathMarker @ 0x4e1e00` | 12 | `u32` player id — death event |
| `MERP` | PREM | `CServerLog_WriteDisconnectMarker @ 0x4e1c50` | 12 | `u32` player id — disconnect event |
| `DNE.` | .END | `CServerLog_CloseAndFree @ 0x4e1a10` | 8 | (none) |

`PDAT` field map (the 36-byte payload; entity-struct sources in parens):

```
+8  u32 net_id              entity[30] (bot) / entity[31]
+12 i32 -entity[2]          (negated on disk)   } reconstructed entity world
+16 i32  entity[3]                               } position = entity+4/+8/+12,
+20 i32  entity[1]                               } i.e. (entity[1], entity[2], entity[3])
+24 u32  entity[4]          32-bit BAM heading
+28 u32  entity[6]          2nd Euler angle
+32 u32  (unwritten / dead — stays 0 from the zero-init buffer)
+36 u32  entity[9]          entity flags
+40 u16  1 iff entity[91]   vehicle flag
+42 u16  playerSlot+0x15F78 a per-player STAT byte — NOT team. The decompiler
                            auto-labels it "team", but the source is parent[0x15F78]
                            (=0 in early frames); the authoritative team is in FEDP
                            (entity+354). Cross-checked: PDAT +42 = 0 while FEDP team = 1/2.
```

**Cross-validation against the wire capture** (`nw_pp host.sph` / `client.sph` vs `nw_pp <probe>.pcapng`):
FooPlayer (Red, roster id 3 = pool-0 handle `0x0005`) at spawn correlates **byte-for-byte across three
independent decodings**:

| source | position (16.16) | heading |
|---|---|---|
| `.sph` `PDAT` | `(70.0, 25.0, 56.306)` | `0xc0000000` = 270° |
| C2S **0x0C** extended uplink (§5.10) | `(70.0, 25.0, 56.3)` | `hdg=0xC000` → sign-ext ×0x10000 = `0xC0000000` |
| S2C **0x0A** header `refs` | `0x00460000,0x00190000,0x00384e68` = `(70.0, 25.0, 56.306)` | — |

This independently confirms (a) the `PlayerExtendedUplink` (0x0C) decoder and the `.sph` `PDAT`
decoder both yield the engine's true entity position+heading; (b) the **S2C 0x0A header carries the
subject player's raw world position** as 3×i32 16.16 `refs` — the decode-base for the compressed
per-entity records that follow (consistent with D-NET-50's "positions are compressed deltas"); and
(c) the coordinate reconstruction (un-negate +12, permute) is correct, since it produces the
identical `(X,Y,Z)` the wire uses. Team↔X-sign (Blue −X / Red +X), the id↔name↔team roster, and the
~7.75 Hz cadence (816 × 0x0A ≈ 789 client frames) all line up; the host's `PBRK`/`PREM` markers give
a labeled death/join/disconnect timeline.

**Limits.** `PDAT` is *decoded* state, so it validates VALUES, not wire byte-framing/encryption.
Only pool-0 (the two human players) is recorded — the AI/mission entities (pool-1/3, the §5.11/5.12
spawn batches) are not; the authored-mission cross-validation (§5.24,
`fixtures/novaworld/dvxi5_manifest.txt` + `nw_pool_groundtruth_test`) covers those. Sampling is 8-tick
(~7.75 Hz). Tooling: `apps/nw_pp` reads `.sph` natively (suffix-dispatched); the decoder is
`libs/npwire/serverlog_decode.{h,cpp}`; `tests/novaworld/nw_serverlog_decode_test` witnesses the
controlled knowns (gated on `NW_PROFILE_SPH_DIR`).

### 5.23 Tag 0x0C — pool-0 organic spawn batch (field map; D-NET-62)

`[orig: NapiNPClientMsg_0x00C @ 0x42E730]`. The route by which **pool-0 "organics"** — AI
infantry and human-player infantry — enter the world; NOT 0x0D (which handles pool 1/3 and
crashes on the player template type `0x14B9`, §5.6). On entry it raises `dword_A82370 ≥ 4`
(§5.1), reads a `[u16 entityCount]` header (no start-index, unlike 0x10/0x20), and for each
record resolves `(pool<<12)|slot` via `g_pool_list` (`0xFFFF` / `(s&0xF000)>=0x5000` / `slot ≥
pool.capacity` end the batch), then `memset(entity,0,904)` + `memset(entity,0,692)`. It calls
`Entity_AllocateAISlot` and parses the name cstring inline **for every record** — which is why
0x0C is crash-safe on `0x14B9` where 0x0D is not.

**The distinguishing structural fact: every field after `hasBody` is UNCONDITIONAL** — there are
no flag-gated optionals (0x0D has 16, 0x20 has 6). The record is `slotId`-first (0x0D is
flags-first).

| order | width | field | landing | gate |
|---|---|---|---|---|
| 1 | u16 | **slotId** `(pool<<12)\|slot` | pool resolve via `g_pool_list`; sentinels end batch | always `[@ 0x42e78f]` |
| 2 | u8 | **hasBody** | `0` ⇒ empty spawn, record ends here | always `[@ 0x42e813]` |
| 3 | u16 | **itemTypeId** | entity+28 (`ItemList_FindIndexByTypeId` → `Entity_InitFromItemDef`) | hasBody `[@ 0x42e83b]` |
| 4 | u32 | **entityFlags** | entity+120 (0x78) | hasBody `[@ 0x42e860]` |
| 5 | cstr | **entityName** | entity+244 (Name[16], capped) | hasBody `[@ 0x42e867]` |
| 6 | u16 | **minimapFlags** | entity+36 (Flags 0x24; bit 0x100 = minimap-register) | hasBody `[@ 0x42e912]` |
| 7 | i32 | **posX** (16.16) | entity+4 | hasBody `[@ 0x42e928]` |
| 8 | i32 | **posY** (16.16) | entity+8 | hasBody `[@ 0x42e93a]` |
| 9 | i32 | **posZ** (16.16) | entity+12 | hasBody `[@ 0x42e94c]` |
| 10 | i32 | **orientation** (32-bit BAM) | entity+16 (Yaw 0x10) | hasBody `[@ 0x42e95e]` |
| 11 | u8 | **team** | entity+354 (Team 0x162) — BMS 1=Blue/2=Red | hasBody `[@ 0x42e970]` |
| 12 | u8 | aiState | entity+692 (0x2B4) | hasBody |
| 13 | u8 | animSlot | entity+884 (0x374) | hasBody |
| 14 | u16 | netId | entity+348 (0x15C) | hasBody |
| 15 | u8 | weaponState | entity+660 (0x294) | hasBody |
| 16 | u8 | aiAction | `*(entity+104)+32` (AI sub-struct) | hasBody |
| 17 | u8 | *(skip)* | cursor advance only, discarded `[@ 0x42e9f5]` | hasBody |
| 18 | u8 | unusedByte | entity+340 (0x154) | hasBody |
| 19 | u8 | refNum (registration/group id, NOT alert level — D-NET-94) | entity+533 (0x215) | hasBody `[@ 0x42ea20]` |
| 20 | u8 | subType | entity+532 (0x214) | hasBody `[@ 0x42ea35]` |
| 21 | u8 | weaponType | entity+343 (0x157) | hasBody |
| 22 | u8 | parentSlot | entity+360 (0x168) | hasBody |
| 23 | u16 | parentHandle `(pool<<12)\|slot` | resolved → entity+364 (0x16C) | hasBody `[@ 0x42ea6e]` |

Decoder: `libs/npwire/ingame_decode.{h,cpp}` `decode_organic_spawn_batch` /
`OrganicSpawnRecord`. **Both spawn paths land team at entity+354** (the unified team landing,
D-NET-58); 0x0C orientation at entity+16 is the same 32-bit BAM as 0x20 `movement_val` (§5.12).

**Record field 15 (`weaponState`, entity+0x294) is the soldier `playerClass`** (D-NET-103, §5.2b). For a
JOINER'S OWN player spawn it MUST be ∈ 5-9: the client registers its body-anim channel (`animChannelB`,
entity+0x188 — the move/crouch/prone gate) at ROUND-LOAD only for `playerClass` ∈ 5-9, and this `0x0C`
handler registers no channels (it calls the leaf `Entity_InitFromItemDef @0x49e550`). See §5.38d for the
registration path and the consequence (`+0x188 == NULL` ⇒ the body motor bails ⇒ look works, locomotion
does not).

### 5.24 Authored-mission cross-validation — pools 1/2/3 (D-NET-62)

The `.sph` oracle (§5.22) sees **pool-0 only**. To validate the AI/vehicle/marker pools the
`.sph` cannot witness, the authored **dvxi5 probe mission** is the ground-truth oracle: the
host (retail `Jointops.exe`) serialized the *known* `mission.bms` onto the wire, so the decoded
spawn records can be checked field-for-field against the authored facts — the sibling of
D-NET-61 for pools 1/2/3.

Tooling (this commit): the authored `.bms` is reduced to `fixtures/novaworld/dvxi5_manifest.txt`
(via `opennova_mission_save_mis_path` → the libs/mission `.mis` writer); nw_pp's native pcap
reader is factored into the shared `apps/common/pcap_reader.{h,cpp}` (buffer-core + file wrapper
+ `build_pcap_udp` in-memory builder); `tests/novaworld/nw_pool_groundtruth_test` decodes the
real `.scratch` capture **directly** (no hexcap) and asserts every authored entity reproduces;
`tests/novaworld/nw_pool_decode_unit_test` round-trips the 0x0D/0x20 decoders through the full
S2C stack inside a crafted in-memory pcap (CI-runnable, no capture dependency).

Witnessed against the dvxi5 probe (2 trucks type `0x050E` → 0x0D; 2 objective markers `0x0576`/
`0x0575` → 0x0D pool-1; 4 start markers `0x1773`/`0x1774` → 0x20; 4 AI `0x0816` → 0x0C):

- **type_id, position, team all reproduce.** posX/posY are byte-exact i32 16.16 (lossless); the
  height (posZ) of vehicles and AI re-grounds onto the host's terrain (sub-unit delta, ≤1.0
  world unit) while markers keep their authored z. team byte gate behaves per the manifest
  (team 0 ⇒ gate clear; team 1/2 ⇒ gate set + value). 0x0C batches consume byte-exact.
- **Heading convention `wire_BAM = 90 - facing`** (the IDA-verified "90 − yaw" fix), pinned by
  the 0x0C AI authored at facing {0,90,180,270} → wire {90°,0°,270°,180°}. The 0x20 start
  markers alone (facing 0/180) could NOT distinguish "90 − yaw" from "yaw + 90"; the diverse
  asymmetric probe exposed it.
- **Team offset reconfirmed `entity+354`.** The onhook PoC's reads at `+146`/`+196` are inside
  `GamePlayerEntity.pad5` and carry at most a runtime/display mirror — NOT the BMS team (three
  engine-side witnesses pin +354: the 0x0C/0x0D spawn handlers, `serialize_entity_pool_to_packet_0
  @ 0x503940`, and the `.sph` `FEDP` writer `@ 0x4e1cc0`). Same decompiler-mislabel class as the
  PDAT `+42` STAT byte (§5.22). onhook is a useful lead source only; IDA is the source of truth.

### 5.25 Replay timeline — assembling a capture into per-entity tracks (tooling, 2026-06-17)

The pool decoders (§5.11/§5.12/§5.23) and the C2S `0x0C` uplink (§5.10) are composed into a
reusable **replay timeline** so a whole capture can be *seen*, not just byte-asserted. The
outer-decode pipeline (envelope → NWU → SCRK → `0x43`/`0x83` → reassembly → tag dispatch) — long
copy-pasted into `nw_pp` and each cross-validation test — is factored into the shared
`libs/npwire/wire_capture.{h,cpp}` (`decode_capture_to_messages` → `InGameMessage{frame, dir,
tag, payload}`). `libs/npwire/replay_timeline.{h,cpp}` then assembles those messages into an
entity table keyed by handle `(pool<<12)|slot`: spawns (`0x0D`/`0x20`/`0x0C`) lay down the static
world layout (type, name, team, initial pose); C2S `0x0C` extended uplinks append the joiner's own
per-frame track; and the **S2C `0x0A` event loop appends per-frame motion for every nearby entity**
(each compact record's compressed position decompressed + the message's header anchor, §5.10).
The same `ReplayTimeline` feeds the in-engine NovaWorld spectator straight from the wire — no JSON
intermediary: `nw_replay` streams the captured packets to `NovaNetClient` (godot/engine/network),
which decodes them and exposes the entity tracks (`sample_at`), the event stream (`get_events`), and
the env stream (`env_at`) to the Godot render path (`NetWorldView` + `NetEventView` + the spectator
kill-feed/env HUD). Walking `0x0A` needs items.def (the per-record width is class-dependent, §5.10b).

Two facts worth recording, both observed on the dvxi5 probe + a 3-player capture:

- **Multi-entity motion is decoded straight from the wire (no `.sph`, no hook).** Folding the
  `0x0A` compact records via the now-solved decompression + header anchor (§5.10) turns the replay
  from "static layout + the one uplink player" into every nearby entity moving: the dvxi5 probe
  yields 8 dynamic entities (2 vehicles, 4 AI, 2 players) with 663–816 per-frame samples each, and
  the 3-player capture yields 54 moving entities (51 vehicles + 3 players). Validation is
  **wire-only** (per the project rule that the viewer / reimplementation never depend on the `.sph`
  profiling recording): every dvxi5 entity's first `0x0A` sample lands on its `0x0D`/`0x0C` spawn
  position exactly (Δ = 0.00000 m). (Markers carry no `0x0A` motion.)
- **Mounted riders follow their vehicle (D-NET-67).** A mounted record's compressed position is
  vehicle-LOCAL (not world + anchor); it is lifted to world via the parent's pose
  (`network_transform_local_to_world`, a port of `Entity_TransformLocalToWorld @ 0x43BD00`), so a
  passenger/driver/gunner tracks its carrier instead of freezing at its last on-foot position.
  Mounts NEST (a rider on a weapon mount on a vehicle), so the lift is resolved bottom-up. On the
  medium loopback this places 3 riders (players `0x3`/`0x4`/`0x5`) on vehicles `0x1000`–`0x1002`
  through their motion (e.g. `0x4` rides `0x1002`→`0x1000`→`0x1001`); the unmounted vehicle record
  carries only the parent's yaw on the wire, so pitch/roll feed in as 0.
- **The unmounted C2S `0x0C` uplink lands in the spawn's WORLD frame.** §5.10 notes the unmounted uplink
  position is "world + map_origin"; on the dvxi5 capture the first uplink sample equals the
  player's `0x0C` spawn position exactly (Δx = Δy = 0), i.e. the map-origin contribution is zero
  (or pre-folded) here. The exporter emits raw values and tags each sample's source; the viewer's
  "anchor tracks to spawn" toggle reconciles the two frames for display and is a no-op when Δ = 0.
- **Death/respawn is a teleport, never motion (D-NET-66).** A killed entity does not glide from
  its death spot to its respawn — the engine snaps it (`Entity_ResetToSpawnState @ 0x4B9610` clears
  the dead flag `Flags & 2` and re-seats the position; it never interpolates). The timeline now
  models this from two wire signals the read path itself uses: the per-record dead bit (S2C `0x0A`
  compact **`flags & 0x02`** — set while the ragdoll is still being broadcast, cleared at the
  respawn record) and the kill stream (S2C `0x26`/`0x4E` → `Entity_KillBySlotId @ 0x42BCE0` — the
  only signal when a dying entity drops out of the `0x0A` set entirely). `mark_lifecycle` flags the
  dead→alive transition sample `respawn`; `interp_pos` / the viewer's `posAt` + trail never bridge
  it (the entity holds at the death spot, styled dead, then jumps). On the medium loopback this
  resolves 13 host-view respawn boundaries that previously dragged players across the map; before
  the fix a victim whose records stop (e.g. handle `0x5`: last seen f=1934, killed f=2344, reappears
  at spawn f=3651) interpolated a 97 m glide over 1717 frames. [orig:
  `NetPacket_SerializeInfantryEntityState @ 0x4C0320` (flagsByte & 2 branch) /
  `NetPacket_SerializePlayerState @ 0x4C09C0` (`test [entity+0x24], 2` → snap + reset) /
  `Entity_KillBySlotId @ 0x42BCE0` / `Entity_ResetToSpawnState @ 0x4B9610`]

CI coverage: `tests/novaworld/nw_replay_timeline_test` crafts inline captures (ServerAuth +
ClientAuth deliver SCRK; an `0x0D` spawn + two C2S `0x0C` uplinks; and an `0x0A` message with a
known header anchor + an unmounted infantry compact), drives them through
`decode_capture_to_messages` → `build_replay_timeline`, and asserts the assembled entities +
tracks — including `network_decompress_fixedpoint` vectors and that an `0x0A` sample equals
`decompress(compressed) + anchor`. No capture fixture; runs in CI. The real-capture cross-checks
(`nw_pool_groundtruth`, path-gated) were repointed onto the same shared helper.

### 5.26 Tags 0x1E / 0x26 / 0x4E — game events, kills, batch despawn (the kill feed; one-host/one-client capture 2026-06-17)

The client's death + announcement path, decoded field-for-field and validated against a fresh
one-host/one-client loopback capture (`apps/nw_pp` printers + `libs/npwire/ingame_decode`
decoders for all three).

**S2C 0x1E — game event (the kill feed proper).** Fixed 8-byte body.
[orig: `NetPacket_HandleGameEvent @ 0x426270`].

| Off | Field | Type | Meaning |
|---|---|---|---|
| 0 | `event_type` | u8 | 1–60; selects the canned message + side effects (see below) |
| 1 | `attacker_index` | u8 | pool-0 index → `[orig: Pool_GetEntryUnchecked @ 0x441FC0]`(0, idx); `0xFF`=none |
| 2 | `victim_index` | u8 | pool-0 index; `0xFF`=none |
| 3 | `aux_index` | u8 | pool-0 index — third actor / means; `0xFF`=none |
| 4 | `pos_x` | i16 | event world X in metres (handler does `<< 16` → 16.16) |
| 6 | `pos_y` | i16 | event world Y in metres |

The handler, in-session only (except `event_type==48`), switches `event_type` over ~60 cases:
each resolves a `"Canned Msg"`/`STRCNDnn` string via `[orig: GameText_GetString @ 0x51EBD0]`,
formats it with the resolved killer/victim/aux names via `[orig: HUD_FormatKillEventMessage @
0x422DA0]` (→ `[orig: Chat_FormatMessage @ 0x422C60]`, `$A`/`$B` token substitution; player names
from the slot table with `<ch>…<co>` clan-tag colouring), and posts it to the kill feed with a
colour via `[orig: Chat_AddDebugMessage @ 0x4987F0]`. Objective/zone cases additionally drive
`[orig: PlaySoundOnDedicatedServer @ 0x527BE0]`, `HUD_DrawDefaultProgressBar @ 0x527E60`, and
effect spawns. Cases that resolve both attacker AND victim (4–15, 24, 32–34, 38–39, 45, 49) are
**kills**; the flag/zone/camp/base cases (19–21, 41–44, 50–60) are **objectives**; the rest are
misc HUD lines. The full `event_type → STRCNDnn` table and the kind classification are ported in
`game_event_strcnd_key` / `game_event_kind` (libs/npwire/ingame_decode.cpp). **Wire-confirmed:**
the capture's three `0x1E` bodies were `04 05 04 ff 00 00 00 00` (type 4 = `STRCND04` kill,
attacker pool0/s5 killed victim pool0/s4), `2a 05 ff ff …` (type 42 = `STRCND_PSP_REDWARNING`),
and `02 05 00 00 …` (type 2). Pool-0 indices ARE pool-0 handles (`(0<<12)|slot`), so they key
straight into the organic-spawn (§5.23) entity table.

**S2C 0x26 — entity kill replication.** Fixed 4-byte body `[u16 victim_slot][u16 attacker]`.
[orig: `NapiNPClientMsg_0x026 @ 0x42EC30`] → `[orig: Entity_KillBySlotId @ 0x42BCE0]`(victim_slot,
attacker, 0). Client-only (skipped when `g_napi_np_ctx.is_authority`). The decompiler labels the
first word `killer_slot_id`, but `Entity_KillBySlotId`'s arg0 is the entity that **dies** (it
resolves the slot, validates it is alive, records arg1 as the attacker on the hit record, and
invokes the entity's death callback) — so the first word is the **victim** slot, the second the
attacker. The handler is defensive: it reads the victim if ≥2 B are present and the attacker if a
further 2 B follow, then always kills.

**S2C 0x4E — batch despawn/kill.** `[u16 count][count × u16 slot]`. [orig:
`NapiNPClientMsg_HandleBatchKill @ 0x431870`] — Kong labeled it "HandleBatchSpawn", but it kills every u16 slot
after the count word via `Entity_KillBySlotId(slot, 0, 1)` up to the buffer end (the leading
`count` is echoed in the reply, not a read limit), then queues the C2S `0x28` ack.

### 5.27 Replay event + environment streams (tooling, 2026-06-17)

Two refinements turn the replay timeline (§5.25) from "where is everything" into "what is
happening", and consolidate the `0x0A` decode to a single source.

- **`decode_frame_update` is now the whole `0x0A` decode.** The env sub-block (header case 2:
  fog / time-of-day / clouds / quake), the conditional vehicle-passenger record, and every
  trailing **weapon-hit** (event-loop `tag==2`, §5.9.1) — previously walked only by `nw_pp`'s
  printer — are captured into the `FrameUpdate` struct. `nw_pp::print_tag_0a` is now a thin
  renderer over that struct, so advancing `0x0A` knowledge means editing one walker.
- **An event stream + an environment stream** ride alongside the per-entity tracks in
  `ReplayTimeline`: **fire** (C2S `0x06`, §5.16 — world origin + direction + shooter + `adm_index`),
  **hit** (`0x0A` `tag==2` — impact world pos = `network_decompress_fixedpoint(compressed)` + the
  message anchor, + target / weapon / `adm_index`), **kill** + **game-event** (§5.26 `0x1E`/`0x26`),
  and **capture-zone** state (§5.19 `0x40`, emitted only on change — the sync repeats every frame).
  The env stream is the `0x0A` case-2 snapshots over time. The in-engine NovaWorld spectator renders
  both straight from the wire (no JSON): `NovaNetClient.get_events()` / `env_at()` feed `NetEventView`
  (fire-ray tracers + hit bursts, kill marks, live capture-zone rings drawn over the 3D world) and the
  spectator HUD (a time-synced kill feed + an environment readout).

**Wire-confirmed** on the one-host/one-client capture (957 datagrams): 151 events — 19 fire
(pool0/s5, `adm 24`), 125 hit (→pool0/s3, weapon pool0/s4, `adm 18`, positions decompressed), 1
kill (s5 ✖ s4, `STRCND04` — matching the `0x1E` byte-witness above), 2 game-events, 4 capture-zone
state changes (deduped from 336 `0x40` syncs) — plus 98 env snapshots. CI: `nw_replay_timeline`
gains `test_event_stream`, which crafts an inline `0x1E` kill + C2S `0x06` fire + `0x0A` env+hit
through the shared pipeline and asserts the assembled events (kill source/target/`STRCND04`, fire
origin/dir/adm, hit world pos = decompress + anchor) and the env snapshot.

### 5.28 Mission delivery to a joiner — a chunked file transfer (probe2, 2026-06-18; header corrected 2026-06-18b)

When a client joins a hosted mission it does NOT have locally, the host streams it as part of the
initial game state. The probe2 capture (Team Deathmatch; the joiner ran from a separate install
lacking `probe2.bms`, so a real download was forced) shows the joiner receiving, in order:

- **S2C `0x0B`** — the literal 616-byte BMS header (`42 4D 53 13` … mission name … designer), §5.4.
- **S2C `0x60` / `0x64`** — two **chunked file transfers** (corrected below).
- **S2C `0x0F`** world-state-load (§5.29), then the entity **spawn batches** (`0x10` statics, `0x0D`
  pool-1, `0x0C` organics, `0x20` markers) — the actual world contents.

**`0x60` and `0x64` are identical chunked-file-transfer handlers**, not a structured "announce +
opaque chunk". Both read a 12-byte header then RAW file bytes, reassembled by offset:

| off | type | field |
|---|---|---|
| 0 | u32 | `transferId` / checksum — echoed in the re-request; probe2 = `1` |
| 4 | u32 | `totalSize` — full transfer size across all chunks |
| 8 | u32 | `chunkOffset` — where this chunk's bytes land in the reassembly buffer |
| 12 | … | `len − 12` raw file bytes |

When `chunkOffset + chunkSize >= totalSize` the transfer completes; otherwise the client re-requests
the next chunk — `0x60` → **C2S `0x33`**, `0x64` → **C2S `0x37`**, payload `[transferId][nextOffset]`
(8 B). There is **no compression codec** — the payload is literal file content. `0x60` reassembles
into a `CDataStream` (`stru_A86920.pad9[24]`); `0x64` into a raw buffer (`unk_24D1E08`) whose
completion extracts three 32-byte mission-name strings (buffer +0x34/+0x54/+0x74).
[orig: `NapiNPClientMsg_HandleFileTransferChunk @ 0x432350` (0x60) /
`NapiNPClientMsg_HandleMissionDataChunk @ 0x432410` (0x64)]

**Correction (D-NET-74, supersedes the original D-NET-69 framing).** The first pass read the header
as `[u32 type=1][u32 bodyLen][u32 reserved=0]` and called `0x60` a parsed `SERVERNAME`/`MISSIONNAME`
VarList. That was a **single-chunk artifact**: probe2's transfers each fit in ONE chunk, so
`transferId` read as `1` (looked like `type=1`), `totalSize` equalled the remaining bytes (looked
like `bodyLen`), and `chunkOffset` was `0` (looked like `reserved=0`). The C2S `0x33`/`0x37`
re-requests never fired because each transfer **completed in one chunk** — NOT because the protocol
is re-request-free. The `SERVERNAME`/`MISSIONNAME` text is the *content* of the file `0x60` carries
(itself a `{ cstr key, u32 len, value }` VarList parsed downstream of reassembly), not a field layout
the `0x60` handler imposes — the handler only `memcpy`s raw bytes into a stream.

**Witness:** `.scratch/probe2.pcapng` — `decode_file_transfer_chunk` (shared by both tags) decodes:
- `0x60` @ f896: `id=1 total=163 offset=0 chunk=163 [FINAL]` — content
  `SERVERNAME\0 [u32 6] "biggy"\0 MISSIONNAME\0 [u32 22] "ON RE Probe TDM Dvxi3"\0 …`.
- `0x64` @ f899: `id=1 total=180 offset=0 chunk=180 [FINAL]` — a binary mission-metadata blob.
Both consume to the byte; this matches the host emit order in §5.2a
(`Server_SendInitialGameStateToPlayer @ 0x51bba0`).

**Correction (2026-07-27 — the joiner needs NO mission file; supersedes this section's opening
framing).** The 0x60/0x64 transfers are SMALL metadata blobs (163/180 B in probe2), not the
mission file, and nothing else delivers one: a retail joiner never opens a `.bms` at all.
`Game_StartMission @ 0x524360` authority-gates every mission-file leg — the early exists-check
@ 0x524751 and both `Mission_LoadBMSFile @ 0x40F4E0` call sites (@ 0x524b5d / @ 0x524ffa) — and
the in-session non-authority arm (@ 0x524d8f → @ 0x524df1) drives terrain/env entirely from the
wire 0x0B header (`g_BmsHeaderBlock @ 0xA761D0`: map basename +0x44 = the env/TOD-config key,
env name +0xDC, tile-set +0x118 → `.TGA`/`.TSD`), then waits for game start
(`NapiClient_WaitForGameStart @ 0x42cc10`) while the world contents arrive as the spawn
batches listed above. "The host streams it" was probe2's inference from the joiner lacking
`probe2.bms` and still joining — true, but what the host streams is the HEADER + metadata +
entities, never the file. Custom missions reference stock terrain/tile-set/env assets by
name, so nothing else must exist client-side. Full witness chain + our divergence: D-NET-194.

### 5.29 Tag 0x0F — world-state-load (joiner spawn + scores + location names; probe2, 2026-06-18; server writer + field roles witnessed 2026-07-03)

After the mission transfer (§5.28) the host sends the joiner its spawn pose, the game flags, the
per-slot-type score table, and the waypoint / location-name lists. ~624 B.
[orig: client `NapiNPClientMsg_0x00F @ 0x42E200`; server writer
`NetPacket_WriteWorldStateLoad0x0F @ 0x502D10` — renamed 2026-07-03 from
NetPacket_WriteWorldStateLoad0x0F (it serializes THIS body, not generic player state). Server
layout: `[u32 GetTickCount][3×i32 spawn pos][3×i16 yaw/pitch/roll][u8 flags][128×i32 from
player+88664][u16 pool3Count + {u16 id, u16 val, u8}× when player+354 == 1][u16 nameCount]
[cstring × count]`]:

| off | type | field | notes |
|---|---|---|---|
| 0 | i32 | sessionTick | → `dword_A82368` |
| 4 | i32 ×3 | posX/Y/Z | local-player spawn (16.16); → entity+4/8/12 when `!is_authority` |
| 16 | i16 ×3 | yaw/pitch/roll | each `<< 16` to 16.16 → entity Yaw/Pitch/Roll |
| 22 | u8 | gameFlags | bit0 = spawn zones exist (server: `SpawnZoneList_GetCount() != 0` @ 0x502da7) → sets `g_deploy_screen_active @ 0xA860DC` ONCE (gated on `g_death_screen_active @ 0xA860EC == 0`) — momentary without the 0x0A flags1-bit1 hold (§5.9/D-NET-156); bit1 = `g_respawn_requires_team_dead && in_session` → `A860DD`; bit2 → `A860DE`; bit3 = ceasefire |
| 23 | i32 ×128 | slotTypeScores | **FIXED 128-entry block** — the **per-slot-type SCORE table** (client outTable @ 0xB75FE8; readers `Entity_GetScoreValueBySlotType` / `WeaponSlot_*`; server source player+88664), NOT zone data — zeros are benign for the deploy picker (2026-07-03 correction of the "teamScores" reading). Fills `[outTable, data)` @ 0x42e324 (512 B; the bulk of the body) |
| 535 | u16 | waypointCount | |
| 537 | … | waypointRecords | `{ u16 slotId, u16 nameId, u8 pad }` × waypointCount — **present ONLY for a waypoint gametype** `(g_GameType & 0xFFFDFFFF) == 0x10020`; that gate is **not on the wire** (off-wire, like the §5.9 0x0A objective block), so the decoder takes the `is_waypoint_gametype` hint. **First witnessed in probe3** (Co-op `g_GameType 0x30020`): `waypointCount=4` (slots p3/6-9), `teamNameCount=0`; byte-exact once the hint is supplied (D-NET-75). TDM/A&S send count 0 |
| … | u16 | nameCount | |
| … | cstring × nameCount | locationNames | **the deploy-map "Location" labels, NOT team names** (2026-07-03 correction): registered at BMS spawn of **def-type 2044 markers**, in spawn order, from the mission text `Locations/LOCATION%03i` (fallback = the key string), appended at `g_location_names @ 0xA2ED10` `[64 * g_location_name_count @ 0xA77644 ++]` [orig: `Entity_SpawnFromBMSRecord @ 0x40E9F0 @ 0x40f182-0x40f221`]. The client handler OVERWRITES its local copies (each copied 3-bytes-at-a-time into a 64-byte slot; wire advance = `strlen+1`). Golden ASH_I5A: 6 names ("North Sea Village", "Southside Jungle", "Rocky Point", "Point Doom", "Ash Village", "Katulus' Mound") |

**The deploy PICKER rows are CLIENT-LOCAL** (witness 2026-07-03): `Entity_BuildSpawnZoneList
@ 0x43EAE0` (called only from `Game_StartMission @ 0x525e01`) scans pools 2+1 for
`ItemDef(+84) & 0x40000`, sorted by `(entity+538 & 0x1F) + (def+406 << 8) + (typePrio << 16)`
(typePrio: def type 1 → 2, 32 → 1) — the client builds zones/names/radii from its LOCAL BMS,
so the 0x0F body re-binds/refreshes labels but never gates the picker's existence.

Because the waypoint gate is off-wire, an off-wire decoder takes an `is_waypoint_gametype` hint
(default false); for TDM/DM the host sends `waypointCount = 0` / no records, so the default is
byte-exact. A non-authority client then queues the C2S burst replies
`0x28`/`0x29`/`0x2D`/`0x32`/`0x22`/`0x23` (§5.33). **Witness:** probe2 `0x0F` — `decode_world_state_load`
consumes the 539-byte body to the byte: `tick=501013671 spawn=(85.0, 0.0, 27.6) yaw=0xC000 flags=0x01
scores[7 nz] waypoints=0 teamNames=0` (spawn x=85 matches an authored Red start; `waypoints=0`
confirms the TDM gate-off default and the 128-entry score-block count).

### 5.30 Tag 0x5A — weapon-loadout sync (probe2, 2026-06-18)

[orig: `NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290E0`]. `[u8 avatarClass]` then a slot chain
`{ u8 typeId, u8 ammoPrimary, u8 ammoSecondary, u8 ammoAlt }` repeated, terminated by `typeId == 0xFF`
(the terminator replaces the next typeId; ≤ 40 raw slots @ 0x429155). Each `typeId` is an
`AdmDef_GetEntryByIndex` (weapon / action-descriptor) index, **not** an items.def type. The retail
handler drops slots that fail the AdmDef lookup, but that is runtime validation, not a wire field — the
decoder keeps every slot (a deliberate non-divergence). Resets `dword_81474C = 0` (the
heartbeat/input gate) on completion. **Witness:** probe2 — 8× `0x5A`, e.g. `avatarClass=8 slots=8`;
full-consume via `decode_weapon_loadout`.

**The full apply chain (witnessed 2026-07-04 — the "no spare magazines" round):** the ammo
bytes are **SIGNED clip counts**: clamp to `adm[83]` (maxClips); a NEGATIVE value (wire
`0xFF`) falls back to `adm[23]` (the def default); the surviving count multiplies by
`adm[22]` (clip size) into TOTAL ROUNDS handed to `WeaponSlot_SetAmmoCount(total,
adm_byte216 ammo bucket)` `[orig: @ 0x4295c4..0x429613]`. Around the ammo loop the handler
REBUILDS the local player's weapon slots from the granted set (`WeaponSlotPool_ResetAllEntries`
→ `WeaponSlotTable_LoadAllFromDefs` → `WeaponSlots_RecalculateAmmoFromCapacity`, re-run
after the loop), re-resolves the body model from `avatarClass` (`AnimMap_GetSlotPropertyInt`
props 10/11/12 by graphics detail), and re-selects/mounts the equipped slot — all gated on
the client game-state dword (mislabeled `lod_level`) being 1/2. **The in-game magazine
counter therefore reads the ENTITY weapon slots, which byte-equal grants fill identically;
the §5.47 phase-0 weapon sub-block instead lands in HUD MIRROR GLOBALS** — `dword_A85B64` =
the preround timer (`HUD_DrawTimerOverlay`), `dword_A85B5C` = death/deploy-screen content
(`UI_UpdateDeathScreenContent` / `draw_death_screen_overlay`), `word_A85B7C` =
`HUD_DrawCaptureProgressBar` — so a host emitting ZEROED phase-0 fields (ours, D-NET-134's
weapon-block gap) blanks the DEATH/DEPLOY-SCREEN loadout panel + preround timer + capture
bar, while the live mag counter stays pool-driven. The v33 "no spare magazines" report needs
one discriminating observation (v35): missing on the death/deploy screen ⇒ author the
phase-0 fields from the recipient's granted loadout; missing on the in-game counter ⇒ an
entity-pool leak still unwitnessed.

### 5.31 Tag 0x6E — team/squad roster sync (probe2, 2026-06-18)

[orig: `NapiNPClientMsg_HandleSquadRosterSync @ 0x429880`]. `[u8 teamCount]` then per team:

| type | field | notes |
|---|---|---|
| u16 | teamEntityHandle | (pool<<12)\|slot; `0xFFFF` ⇒ skip the entity-slot write (fields still read) |
| u16 | teamSlotIndex | index into the roster arrays (`unk_A85CC4 + 16*idx`, `dword_A85BC4[idx]`) |
| u8 | memberCount | → entity+550 |
| u16 | teamSlotHandle | → entity+548 |
| u16 × memberCount | members | each a (pool<<12)\|slot; a member matching the local player sets `word_A85BC0 = teamEntityHandle` |

**Witness:** probe2 — 43× `0x6E` (mostly `teams=0` early in the join); full-consume via
`decode_roster_sync`.

**Producer refinement (2026-07-03, §5.61):** the server side is the SPAWN-WAVE status
(`NetPacket_WriteSpawnWaveStatus @ 0x507490` over `g_spawn_wave_list`) — "team" entries are
per-zone wave groups for the receiving player's team, `teamSlotIndex` is the spawn-zone-list
index, and `teamSlotHandle` is the wave COUNTDOWN in seconds (not a handle); members are the
queued players. The client's roster-array landing is the deploy-screen consumer.

### 5.32 Tag 0x7B — full player/session info (probe2 + loopbacks, 2026-06-18)

[orig: `NapiNPClientMsg_HandlePlayerInfoFull @ 0x429BB0`]. Five NUL-terminated strings, then
`[u32 extra]`, then two more NUL-terminated strings. The handler caps the dest buffers (32 / 512) but
advances the wire by `strlen+1` — the caps are dest sizes, not wire widths.

**Field roles are witnessed from the landing globals, NOT the Hex-Rays "clan/squad/label/rank"
auto-comment** (which is wrong on every field). Two independent witnesses pin the meanings:
`PunkBuster_GetCvarValue @ 0x4D96A0` maps three strings to named cvars (`name` → string 1,
`sv_hostname` → string 3, `mapname` → string 5, `gamename` → string 7), and string 2 lands in the
exact slot the **S2C 0x7A** player-name handler also writes (`stru_A86920.pad9[196]`, `@ 0x429B40`).

| # | wire field | dest | cvar | observed |
|---|---|---|---|---|
| 1 | playerName | `pad9[164]` (0xA86C60) | `name` | `"FooPlayer"` / `"cdouglass"` — local/LAN display name |
| 2 | **playerId** | `pad9[196]` (0xA86C80) | — | `"00000003"` / `"00000005"` — NovaWorld player/account ID (**not a clan tag**) |
| 3 | serverName | `pad9[228]` (0xA86CA0) | `sv_hostname` | `"biggy"` |
| 4 | missionName | `dst` (0xA86CC0) | — | `"ON RE Probe TDM Dvxi3"` |
| 5 | mapFile | `byte_A86CE0` | `mapname` | `"probe2.bms"` / `"mission.bms"` |
| — | **extra (u32) = `g_GameType`** | `dword_A86D00` | — | probe2 `0x00010000` (TDM) / probe3 `0x00030020` (Co-op) |
| 6 | motd | `byte_A86D04` | — | (empty in every capture — "MOTD" guess unconfirmed) |
| 7 | gameName | `byte_A86D24` | `gamename` | probe3 `"jox01"` (the loaded expansion id); empty in probe2 |

**The `extra` dword is the session `g_GameType` (D-NET-75).** Across both probes it equals the
IDA-derived gametype exactly — probe2 TDM `0x10000`, probe3 Co-op `0x30020` (= `AI_GetTaskTypeFromFlags
@ 0x40DAE0` → `Game_StartMission @ 0x524360`). This makes 0x7B the on-wire source for the two
off-wire gametype gates the receiver otherwise can't see: 0x0F waypoint records `(g & 0xFFFDFFFF)==0x10020`
(§5.29) and the 0x0A objective sub-block 3 `(g & 0x20000)` (§5.9).

**String 2 is a player ID, not a clan tag** — cross-capture: it is a persistent per-player
zero-padded number (`FooPlayer = "00000003"` across probe2 / medium / reconnects loopbacks; a second
player = `"00000005"`), populated **instead of** the display name on a NovaWorld account join, and
**empty on a LAN/local join** (the LAN `cdouglass` capture has `name="cdouglass"`, id `""`). So strings
1 and 2 are the two faces of player identity — local display name vs online account ID — exactly the
mutual exclusion the join auth path produces. **Witness:** `decode_full_player_info` full-consumes
every `0x7B` across all five `.scratch` captures (`0x7B` ×2 in probe2).

### 5.33 C2S burst replies 0x22 / 0x23 / 0x28 / 0x29 / 0x4C (probe2, 2026-06-18)

The small client→server requests a joiner queues in response to S2C load/sync messages (the 0x0F
world-state reply burst, the 0x46 `0x4000`-ack, the 0x4D spawn-slot). Field-mapped from the authority
**SERVER** read-handlers (the canonical body); each serializes a reply back to the requester.

| tag | body | server reply | handler |
|---|---|---|---|
| `0x22` | `[u8 slot][u16 fieldFlags]` (3 B) | S2C `0x46` player-sync for `slot` with `fieldFlags` | `NapiNPServerMsg_0x022 @ 0x514C90` |
| `0x23` | empty (0 B) | S2C `0x4C` visible-players snapshot | `@ 0x514D50` |
| `0x28` | `[u32 loadoutFilter][u32 flags][u16 extra]` (10 B) | S2C `0x4E` | `NapiNPServerMsg_HandleWeaponLoadoutRequest @ 0x51A550` |
| `0x29` | `[u16 bufferIndex]` (2 B) | S2C `0x51` entity packet | `NapiNPServerMsg_0x029 @ 0x514F10` |
| `0x4C` | `[u8 value]` (1 B; server clamps 0..4) | — (sets player connection-quality) | `NapiNPServerMsg_0x04C @ 0x5111B0` |

These confirm §5.8's hypothesis that `0x22`/`0x23`/`0x28`/`0x29` are the 0x0F/0x4D reply burst, with
exact bodies. **Witness:** probe2 — `0x22` ×11 (`slot=0x00/0x01 fieldFlags=0x5cf7/0x1cf7`), `0x23` ×1,
`0x28` ×1 (`filter=0x1ddcc5d4 flags=0x1ddcdca7`), `0x29` ×1, `0x4C` ×60; all full-consume via the
`decode_burst_*` family.

**0x22→0x46 ack-walk mechanics (grill 2026-07-01) — the walk is CLIENT-driven.** The server handler
passes the request's `fieldFlags` straight into the 0x46 serializer; the serializer ECHOES bit `0x4000`
(`NetPacket_SerializePlayerSync0x46 @ 0x505e80` — renamed 2026-07-03 from
NetPacket_SerializePlayerSync0x46: `adjusted |= 0x4000` iff the request had it,
@ 0x505f05) and passes the low field-selection bits through (`adjusted = fieldFlags & 0x7FFF` when the
slot is live) — **the reply answers EXACTLY the requested fieldFlags**. An inactive slot / NULL entity
forces `adjusted = 0x8000` (a 3-byte removal reply — the early return @ 0x505f37 writes only
`[u8 slot][u16 flags]`, `|0x4000` when the request asked for the ack) — walk termination depends on
this answer. The CLIENT terminates the walk: its 0x46 handler tail (`NapiNPClientMsg_PlayerSync
@ 0x431370`) re-requests `[u8 slot+1][u16 0x5CF7]` only while `slot+1 < g_max_player_slots`
(`g_max_player_slots` — fed by **the S2C 0x04 SESSION-SLOT-CONFIG byte 18**, §5.53, NOT the 0x08
block as previously noted; a hardcoded small capacity there caps every client's walk, D-NET-158) —
the server never decides when to stop. Walk kickoff: the client's 0x0F handler sends 0x22
`[0, 0x5CF7]` (@ 0x42E659). probe2's `0x5cf7` = `0x4000 | 0x1cf7`, the same `0x1CF7` field set
`Server_PlayerAdd @ 0x51cbc0` uses for its unsolicited 0x46 broadcast (`push 7415` @ 0x51d2bf).

**Roster lifecycle contract (witness 2026-07-03, D-NET-158):** JOIN — `Server_PlayerAdd` broadcasts
the 0x32 name record then a 0x46 (fieldFlags `0x1CF7`, no ack) to every in-game connection
(@ 0x51D296), so existing clients bind the new slot and ACCEPT its 0x16 row without the unknown-slot
0x22 retry churn. LEAVE — `Server_HandlePlayerDisconnect @ 0x51B5C0` memsets the slot then broadcasts
the 0x46 `0x8000` removal (@ 0x51B882 → the client's `PlayerSlot_ClearAndUnlink @ 0x431420`). Deltas —
`Server_TickUpdate` re-sends 0x46 with masks `0x400`/`0x8` on dirty quality/state.

The reimpl (`dispatch_session_replies case 0x22`) previously computed the ack bit from `max_players`
server-side (FIXED 2026-07-01 to echo the request bit) and answered a fixed field set (FIXED
2026-07-03 to serialize the requested mask verbatim, `encode_player_sync(rep, field_flags)`); the
join broadcast is `broadcast_player_sync_on_join` and the live 0x04 capacity byte is threaded from
`GameConfig.max_players` (both 2026-07-03). 0x46 body field semantics witnessed the same
session: bit 1 = name (VARIABLE-length strlen+1 string — all three strings are, never fixed-width),
2 = team-string (retail always writes ""), 0x10 = vehicle name, 4 = team byte (slot+416),
8 = damage|alert<<7, 0x20 = vehicle score byte (vehicle+156), 0x1000 = late-join flag
(slot+100567 && !slot+100579), 0x40 = squad (slot+100576, init -1), 0x80 = side (slot+100577),
0x400 = quality (slot+418; client clamps <=4), 0x800 = vehicle timer dword (vehicle_data+420).

### 5.34 Session/transport control pings — RTT 0x57/0x2C + request trio 0x68/0x43/0x39 (probe3_again, 2026-06-19)

The NAPI transport / anti-cheat keepalives — distinct from gameplay replication: each carries a single
scalar. The request trio and RTT messages with `echoFlag != 0` trigger fixed replies; RTT messages with
`echoFlag == 0` terminate the exchange and update the sample ring. They dominate the wire by volume
(the RTT pair alone is **~10.7 K each**
in a multi-minute session), so they're the bulk of an in-game capture's datagram count and were the largest
remaining hex-only hole in the §4 catalog.

**RTT ping/pong — S2C `0x57` ⇄ C2S `0x2C`.** Identical 5-B body `[u32 timestamp][u8 echoFlag]`. The two
handlers mirror each other: when `echoFlag != 0` the receiver bounces the timestamp straight back
(`0x57`→C2S `0x2C`, `0x2C`→S2C `0x57`) with `echoFlag` cleared; when `echoFlag == 0` the receiver computes
`rtt = GetTickCount() - timestamp` into a 10-sample ring. The server side (`0x2C`) additionally enforces
`g_MinPing` / `g_MaxPing`, incrementing a strike counter and disconnecting players who violate >20× in a
row. The probe was **bidirectional** — both host and client ping each other; the timestamp pairs across the
two directions (e.g. `ts=0x2233240a`: C2S `0x2C` flag=1 → S2C `0x57` flag=0).
[orig: `NapiNPClientMsg_0x057_RTT @ 0x432210` (S2C); `NapiNPServerMsg_HandlePingResponse @ 0x515070` (C2S 0x2C)].
**Note `0x2C` is direction-overloaded** — S2C `0x2C` (`@ 0x427E10`) is an unrelated chat-history entry, NOT
RTT; only the C2S direction is the ping reply.

**Server emit LANDED (2026-06-27, D-NET Wave 5).** `dispatch_session_replies` (`server_message_dispatch.cpp`)
now handles a C2S `0x2C`: when `echoFlag != 0` it bounces an S2C `0x57` = `[u32 timestamp][u8 0]`
(`build_tag57_pong`, the faithful port of `NetPacket_WriteInt32AndByte_0 @0x5070c0`); an `echoFlag == 0`
return leg is the server-internal RTT/min-max-ping path (no reply). Was previously consumed with no reply.
The `g_MinPing`/`g_MaxPing` strike-disconnect is a server-internal stat path not yet modeled (no wire
output beyond the kick chat message — deferred). Tested: `npruntime_handshake_server` (0x2C echo→0x57 body
+ echoFlag=0→no reply).

**Periodic request trio — S2C `0x68` / `0x43` / `0x39`.** Each parses a single `[u32]` (4 B) and queues a
different fixed reply built from local state; the inbound parse is structurally identical, so one reader
(`decode_u32_scalar`) serves all three. They fire together on a coarse period (~every 335 frames in probe3).

| S2C tag | body | meaning | reply | handler |
|---|---|---|---|---|
| `0x68` | `[u32 start_index]` | frozen loaded-model-definition page cursor (observed paging 50, 100, 150…) | C2S `0x3D` (`[cursor]` + ≤50 renderer-cache dwords) | `NapiNPClientMsg_0x068 @ 0x42DAA0` |
| `0x43` | `[u32 server_timestamp]` | time-sync / anti-speedhack stamp | C2S `0x08` (`[u32 server_ts][u32 GetTickCount]`) | `NapiNPClientMsg_0x043 @ 0x42FA90` |
| `0x39` | `[u32 challenge_seed]` | anti-cheat charattr CHARACTER-row CRC seed (constant `0x3D5D` in probe3) | C2S `0x1C` (seed XOR row CRC, or zero) | `NapiNPClientMsg_HandleChecksumChallenge @ 0x42E6D0` |

**Witness:** probe3_again — `0x57`/`0x2C` ×10,679 each (every body 5 B, timestamps pair across the two
directions); `0x68`/`0x43`/`0x39` ×141 each (`0x39` seed constant `0x3D5D`, `0x43` serverTs a slow coarse
tick, `0x68` startIdx paging 50/100/150…); all full-consume via `decode_rtt_sample` / `decode_u32_scalar`.

**Client reply implementation (2026-07-24, D-NET-175 closure).**
`JoinerConnection` now queues the complete trio as semantic replies; `ClientRuntime` holds them behind
the field-3 send gate and frames them in as few MTU-bounded session packets as possible at the shared
send boundary. `0x43` produces C2S `0x08`
`[server_timestamp][monotonic_milliseconds]`; `0x68` produces C2S `0x3D`
`[start_index]` plus at most fifty verbatim rows from the renderer-finalized
loaded-`.3DI` snapshot; and `0x39` produces C2S `0x1C` from the process-scoped
`charattr.def` table. The IDB name `AnimMap_GetSlotChecksum @0x412AA0` is wrong:
the function selects one `g_CharAttr[16]` CHARACTER record with
`uint8_t(class_id - 1) & 0x0F`, rejects it unless its +0 active dword is nonzero
and its +4 class byte matches, then returns
`challenge_seed ^ crc32_napi(row, 124)`. An absent, inactive, or mismatched row
returns **zero**.

`CharAttr_LoadFromDef @0x412140` clears all `0x7C0` table bytes, then loads
`CHARACTER1` through `CHARACTER16`, stopping at the first missing section. Each
124-byte row is: +0 active u32; +4 class u8 (+5..7 zero); +8/+12/+16
STEALTH/HPBONUS/MANABONUS floats; +20/+24/+28/+32/+36
RECOIL_MUTE/RELOAD_MUTE/XHAIR_MUTE/XHAIRDX_MUTE/SCOPE_MUTE floats; +40
ATTRIBUTES flags; +44/+48/+52 JUNGLE/DESERT/ARCTIC_CAMMO u32; +56
RUN_MODIFIER i32; +60..123 zero. `ConfigFile_ReadKeyValue @0x75FC90` selects the
first case-insensitive key occurrence, tokenizes values on comma/space, stores
type 2 through `atof`→float and other numeric types through `atol`.
ATTRIBUTES maps AutoScope/SpreadBonus/KnifeBonus/Medic/WaterGirl to
`1/2/4/8/0x20`.

The production provider loads that boot-soft resource before the first join
receive and carries it through a pre-load runtime rebuild. S2C `0x41`
(`NapiNPClientMsg_ClearAnimSlot @0x4254C0`, also misnamed) mutates the retained
table in dispatch order. The setter it calls (`@0x412890`, IDB
`AnimMap_SetSlotProperty`) is a NINE-case switch, not the four originally
recorded: ids 0/2/3/4/5/6/7/8/9 zero +40 ATTRIBUTES / +8 STEALTH / +12 HPBONUS /
+16 MANABONUS / +20 RECOIL_MUTE / +28 XHAIR_MUTE / +36 SCOPE_MUTE /
+32 XHAIRDX_MUTE / +24 RELOAD_MUTE across all sixteen rows (per-case arms
`@0x4128ef/@0x41290b/@0x41291e/@0x412931/@0x412944/@0x412957/@0x41297d/@0x41296a/@0x412990`
over the 124-byte row stride); id 1 and ids ≥10 fall to the `default: return` and
are the ONLY no-ops. A missing payload byte means 0 and extra bytes are ignored.
Two retail riders are deliberately omitted because they are unobservable through
the `0x1C` checksum: the per-row ACTIVE gate `@0x4128b3` (an inactive row is
all-zero here and the checksum's own row select rejects it) and the per-property
disable latch `AnimMap_SetSlotDisabled @0x4125c0` set from `@0x42550d` — every
original caller of the setter passes `0.0`, so neither changes a byte the reply
hashes.
Missing/empty `charattr.def` stays all-zero and does not abort the join, matching
retail's boot error-and-continue behavior.

The authoritative JO class-8 row has CRC `0x22A25E01`. Two independent retail
captures agree: seed `0x0000F7ED` replies `0x22A2A9EC`, and seed `0x0000B380`
replies `0x22A2ED81`. The native regression pins the full 124 bytes, both
capture equations, wrapped class rejection, first-missing-section behavior, and
same-packet `0x41`/class-change ordering.

The `0x3D` name is misleading: its source is **not** `ClientState` or any live entity
enumeration. `NetPacket_WriteEntityIndexList @0x42D950` reads the frozen pointer/count
returned by `@0x5B1560`; the normal mission path resets the shared `.3DI` model-def
cache, loads entity/celestial/HUD render definitions, and calls `sub_5B3A80 @0x5871CF`
once to snapshot unique definitions whose foliage marker (`node+0x3D4`) is clear.
OpenNova models that lifecycle at the common `NovaObjectData.open_from_resource_root`
boundary: mission load resets the registry before celestial/terrain work, successful
mounted loads register by case-folded filename, foliage loads mark their shared
definition excluded, and `GameWorld` prewarms the joiner's player avatar plus the
current FP gun/arms definitions (the late `LocalPlayerPresenter` builders then hit the same
placer cache) before freezing the page after mission runtime/model setup but before
`world_loaded`. A same-packet S2C spawn + `0x68` regression proves network
entities cannot enter or reorder the page. The writer adds the two model-row fields
`node+0x3CC` and `node+0x3D0`; the complete xref set in this binary leaves both zero
after resolution, so production contributes one zero dword per included definition
while the npruntime seam preserves arbitrary witnessed values, order, duplicates, and
the exact fifty-row paging rule.

The reverse RTT leg also landed: S2C `0x57` with `echoFlag!=0` queues C2S `0x2C` with
the same timestamp and flag zero. Client-originated `0x2C` continues to fire every
deployed frame with a real monotonic-millisecond stamp; the 62-count variable remains
vestigial as proved by the xref set. Per-frame `0x2C`, `0x0C`, and other queued records
share one MTU-aware send-boundary batch instead of forcing one UDP datagram per message.
On the authority side, a reactive S2C `0x57` is likewise deferred into the same send
boundary as that tick's `0x0A`, up to the 1300-byte ceiling. This preserves the retail
per-frame message cadence without producing a separate reply datagram every frame.

### 5.35 Minimap overlays, weapon reload, second death path, checksum + misc scalars (probe3_again, 2026-06-19)

The per-entity HUD / lifecycle notifications the host streams alongside the 0x0A frame. All were
dispatch-table one-liners (no field map) until probe3_again carried enough of each to witness.

**S2C `0x6B` — minimap overlay batch.** `[u8 count]` + `count × 12-B records`. The handler reads only the
`[u16 handle]` at each record's offset 0 (resolved via the pool table) and **rebuilds that entity's
minimap blip from its own engine-side state** — position, type, and team @ `entity+354` (the same team
byte as D-NET-58) → icon + team color. The 10 trailing bytes per record are *not* consumed by the handler,
so the decoder keeps them raw. [orig: `NapiNPClientMsg_0x06B @ 0x425520` → `update_minimap_overlay_entity @ 0x5BEC10`].
(The census guess "objective/HUD countdown" was wrong — the `1e→1d` byte is inside a per-record blob the
handler ignores, not a global timer.)

**S2C `0x49` — weapon-reload notification.** `[u16 entityHandle][u16 reloadParam]` (4 B). Resolves the
entity, then takes ONE of three arms. The **local player** is tested FIRST `[orig: @0x42c0f2 / @0x42c0f8]` and goes straight to the refill, before the item type is ever read. Otherwise the handler reads the ITEM TYPE: a remote **PERSON** gets `entity+0x371 = 80` (the arms-dip window, world-wac-ai-re §14.8.5) and RETURNS immediately `[orig: @0x42c105 / @0x42c10b / @0x42c113]` — it never reaches `WeaponSlot_ReloadAmmo`, whose `@0x54173c` is the only site that SEEDS the `entity+0x372` third-person reload-clip pose window. Every **NON-person** (vehicle / emplaced) weapon falls through to the real refill `[orig: @0x42c109 -> @0x42c116]`. **Corrected 2026-07-27: the earlier "a vehicle entity instead arms an 80-tick timer" had the two arms INVERTED** — it is the remote PERSON that gets the timer and the vehicle that gets the refill. Consequence: a pure client never plays a peer's reload clip; only a host does, via its own-copy refill `@0x514f03`. See D-NET-189.
**The IDB name `handle_camera_sync_packet_0x049` is wrong** — there is no camera code; it reloads ammo.
[orig: `handle_camera_sync_packet_0x049 @ 0x42C0A0` (misnamed) → `WeaponSlot_ReloadAmmo @ 0x541720`].

**S2C `0x13` — entity death (the SECOND death path, beside `0x26`).** `[u16 entityHandle][i16 killerSource]`
(4 B). Sets the entity `Health=0`, stores `killerSource` at `entity+pad9[36]`, clears `entity+pad8[86]`,
fires the death callback `(entity, 4, 0)`; if the local player died it stamps the respawn tick + toggles
the weapon scope. Unlike `0x26` (which routes through `Entity_KillBySlotId`), this acts directly on the
entity. [orig: `NapiNPClientMsg_EntityDeath @ 0x42EB50`].

**S2C `0x30` — entity-checksum request.** `[u8 entityId][u16 checksum]` (3 B) → builds
`NetPacket_WriteEntityChecksum(entityId, checksum)` and replies **C2S `0x20`** (an entity-checksum reply,
distinct from the S2C `0x20` pool-3 sync). [orig: `NapiNPClientMsg_HandleChecksumRequest @ 0x431170`].
The reply builder itself — the `entityId == 0xFF` literal-42 arm and the two real checksum sources —
is §5.65, together with its `0x31` sibling; both are answered by our joiner as of 2026-07-26.

**Misc client scalars.** `0x42` input/state-flags `[u16]` → `Input_UnpackStateFlags`
([orig: `NapiNPClientMsg_0x042 @ 0x4281A0`]); `0x79` spectator-mode flag `[u8]` → `dword_82BEE4`
([orig: `NapiNPClientMsg_0x079 @ 0x429B00`]); `0x2A` chat-history entry `[i32 a][i32 b][i16 c]` (10 B) →
`Chat_AddToHistory` ([orig: `NapiNPClientMsg_0x02A @ 0x425BA0`]).

**Witness:** probe3_again — `0x6B` ×266 (`count=1`, blip handle = the active player), `0x49` ×84
(`reloadParam=195` on both player handles `0x0006`/`0x0007`), `0x13` ×18 (`killerSource=0`), `0x30` ×174
(`entityId=0xff checksum=0`, ⇄ C2S `0x20` ×174), `0x42` ×143 (`flags=0`), `0x79` ×355 (`flag=1`), `0x2A`
×12; all full-consume via the `decode_*` family. `nw_pp` decodes the whole capture with **zero decode
failures**.

### 5.36 Deployed-item spawn 0x59 + entity-routed sub-packet 0x44 (probe3_again, 2026-06-19)

**S2C `0x59` — deployed-item / weapon-overlay spawn-or-update.** Fixed 32-B record. The host streams the
placeable / weapon-overlay entities a player drops (ammo / supply crates, mines, beacons, satchels,
deployed guns…). The handler searches 512 weapon-overlay slots for a matching entity and either updates
its transform or allocates a new pool entry from the item def. It reads 15 u16s (30 B); the trailing 2
bytes are unread.

| off | field | type | notes |
|---|---|---|---|
| 0 | itemId | u16 | base / fallback item id (used if owner/items invalid) |
| 2 | ownerHandle | u16 | the placing entity `(pool<<12)|slot` |
| 4 | friendlyItemId | u16 | model shown to the owner's team |
| 6 | enemyItemId | u16 | model shown to the other team |
| 8 | slotHandle | u16 | the spawned entity `(pool<<12)|slot` |
| 10 | parentHandle | u16 | attach parent (`0xFFFF` = none) |
| 12 | posX/Y/Z | 3×i32 | world position (16.16) |
| 24 | angX/Y/Z | 3×u16 | Euler; engine shifts `<< 16` |
| 30 | reserved | u16 | not read by the handler |

The friend/foe item pair lets one deployable look different to each side, selected by the owner's team @
`+354` vs the local player (`enemyItemId` also chosen under the `dword_24D1E34 & 0x8000` no-friendly-fire
flag). [orig: `NapiNPClientMsg_0x059 @ 0x4228E0` → `Entity_SpawnOrUpdateFromSlotPacket @ 0x546770`].
**Witness:** probe3_again ×12 — a `Rifle-sized Crate` (`itemId=0x0362`) dropped by player slot 5 at world
`(53.5, -27.9, 11.6)`, `parent=none`; byte-exact full-consume via `decode_deployed_item_spawn`.
That is **codec coverage, not production client consumption**: the reimpl
`NetClientView` does not fold decoded 0x59 rows into live placed entities,
and the throwable host path emits neither 0x59 spawn nor 0x12 removal. Together
with that missing placed-device path, remote clients do not receive the
persisted-device replacement. The tag-2 round-event → visual-only client
`RoundSim` fold now presents the flying throwable itself (D-THROW-7).

**S2C `0x44` — entity-routed sub-packet.** A 5-B sub-header `[u16 field0][i16 netId][u8 subtype]` then a
class-dependent body the dispatcher routes to the target entity's per-class serialize callback (`entity
def+356`, `source_type=2`) — the **same per-class path** the C2S `0x0C` entity-uplink uses (§5.10b). We
decode the sub-header + expose the body slice; the body's field layout is class-specific and is **not yet
fully mapped** (PARTIAL — same deferral as the §5.15 guided record; the `subtype` here plays the field-group
role the C2S 0x0C `sub_op` does). [orig: `NapiNPClientMsg_0x044 @ 0x422710` →
`NetPacket_DispatchToEntityByNetId @ 0x4D6960`]. **Witness:** probe3_again ×12 — `subtype` 1/2/3 with body
sizes 1/1/14 B; sub-header consumes, body left raw.

### 5.37 Terrain-tile load batch 0x45 (operation_whitenoise stock Co-op, 2026-06-19)

**S2C `0x45` — multiplayer terrain-tile load batch.** The host streams the per-mission terrain-tile array
to a JOINING client as **phase 5** of the initial-state load sequence (`Server_SendInitialGameStateToPlayer
@ 0x51BBA0`, between the `0x20` pool-3 sync and the `0x7E`/`0x1A` finishers), repeating until the serializer
returns 0. It is **LOAD-ONLY** — there is no gameplay-tick caller. The §4 dispatch row historically read
"empty payload", which is **wrong** (D-NET-83): the handler `NapiNPClientMsg_0x045 @ 0x422890` forwards the
message body to `PolyTrn_LoadTileData @ 0x6081D0` (the Hex-Rays render aliases the body argument as the
separate-looking `buffera`, but both are `[ebp+8]` — the same incoming pointer). The body is a **paged**
stream witnessed byte-exact from BOTH the writer (`serialize_terrain_tiles @ 0x6080F0`) and the reader:

| off | field | type | notes |
|---|---|---|---|
| 0 | startWord | u16 | `0xFFFF` ⇒ first chunk (header follows; start_index = 0); else = `start_index` |
| 2 | endIndex | u16 | one past the last tile index in this chunk |
| — | *first chunk only:* | | (present iff startWord == `0xFFFF`) |
| 4 | magic | u32 | `'til0'` = `0x74696C30` (reader returns without loading on mismatch) |
| 8 | tileCount | u32 | total tiles in the full terrain set (drives the client's tile-array alloc) |
| 12 | hdr2 / hdr3 | 2×u32 | copied from `g_TerrainTileData[2]/[3]` |
| 20 / 4 | tiles | (endIndex−startIndex) × 12 B | each a 3-dword **opaque** tile record |

Tile entries start at byte **20** in the header chunk, byte **4** otherwise. Each 12-B entry is copied
verbatim into `g_TerrainTileData+16+12*idx`; the network layer never interprets the 3 dwords (the terrain
renderer does, later), so the decoder exposes them at that copy granularity rather than inventing field
names. [orig: `serialize_terrain_tiles @ 0x6080F0` (writer) / `PolyTrn_LoadTileData @ 0x6081D0` (reader) /
`NapiNPClientMsg_0x045 @ 0x422890` (handler) / `Server_SendInitialGameStateToPlayer @ 0x51BBA0` (sender,
phase 5)]. **Witness:** operation_whitenoise ×8 — a header chunk (`tileCount=381`, tiles `[0,52)` → 20 +
52×12 = **644 B**) then 7 pages (`[52,105)`/`[105,158)`/… → 4 + 53×12 = **640 B**) walking the full 381-tile
set; byte-exact full-consume via `decode_terrain_load_batch` (→ Decoded, **39 Decoded tags**), pinned by
`nw_whitenoise_coverage_test`. **IDB renames this session:** `dword_319F7A0 → g_TerrainTileCount` (live
loaded-tile count) and `dword_319F7A4 → g_TerrainTileArray` (the 12-B entry array base = `g_TerrainTileData
+ 16`); `NapiNPClientMsg_0x045` / `serialize_terrain_tiles` / `PolyTrn_LoadTileData` carry clarifying
comments noting the §5.37 role + the "not empty payload" correction.

### 5.38 Local-player input→pose locomotion — the player simulates, never interpolates (Phase 2, 2026-06-20)

Witnessed to drive the SP-as-listen-server "moving player" (libs/netsim Phase 2). All anchored
(exact disasm read this session). **Corrects a planning premise** that had the motor's
simulate-vs-interpolate branch inverted — the premise was never landed in this doc, and the
smooth-target was already documented as the §5.10 *receive* side, so §5.38 only adds the
local-player side and the branch direction.

**The motor's simulate-vs-interpolate gate `[orig: Entity_UpdateInfantryAI @ 0x4b9910 @ 0x4b9a74]`.**
Per entity the motor branches (`ebp == 0` here):
```
cmp is_authority, 0     ; jnz loc_4B9C3E          ; jump if IS authority
cmp entity, g_local_player_entity ; jz loc_4B9C3E ; jump if entity IS the local player
; fall-through (0x4b9a8c) reached only when (!is_authority AND entity != local)
```
- **`loc_4B9C3E` = authoritative / local-player SIMULATION.** Taken when
  `is_authority || entity == g_local_player_entity`. Runs the anim-driven ground locomotion that
  integrates the live `Position` (entity+4/+8/+0xC): heading→velocity (speed scale `0x5800`),
  restriction/avoidance probes (`[orig: Entity_RaycastGroundHeight @ 0x4142C0]`), the anim flag table
  `g_animStateFlagsTable` gating velocity application. This is the SAME mover the AI uses (OpenNova
  `AiSystem::tick_infantry`, `libs/world/src/infantry.cpp`, verdict MATCHING) — the only
  difference is the move-order source.
- **Fall-through (0x4b9a8c) = network INTERPOLATION (remote entities on a client only).** Reads
  the smooth-target entity+0x234/+0x238/+0x23C minus the saved live pose (+0x80/+0x84/+0x88),
  computes a 3D distance → step count **{3,4,5,8,16}** (refined in §5.38a — NOT "2..16"; exact
  thresholds `0x2AAA/0x4000/0x5555/0x8000` plus the `>0x20000` snap / `<0x2000` ignore edges) at
  +0x27E, applies one per-step delta to the live pose, progress counter at +0x27C. The smooth-target
  is staged by the C2S 0x0C read-apply
  `[orig: dispatch_entity_packet_callback @ 0x4D6A80]` (ctx_flags=4, §5.10) — the RECEIVE side of a
  remote player's reported pose.

**Verdict — the local player NEVER interpolates.** On the host it takes the branch via
`is_authority`; on a client via `entity == g_local_player_entity`. Either way it locomotes
directly through the motor from its own input. The host does NOT stage the smooth-target for its
own local player; interpolation toward +0x234 is exclusively the receive path for *remote* peers
(a Phase-4 MP concern, not Phase-2 SP). This reconciles D-NET-68 (the host read-applies a *remote*
player's reported pose and never re-simulates it) with the fact that every machine simulates *its
own* player locally.

**Input → pose front end** (client-frame order from `[orig: Game_ProcessMainFrame @ 0x5263f0]`:
`Input_ProcessFrame` → `Client_ProcessNetworkFrame` [pack input + build C2S 0x0C] →
`Server_TickUpdate` → `Entity_UpdateAllEntities` [the motor]):

| Stage | Function | What it does |
|---|---|---|
| Look | `[orig: Input_ProcessMouseAxisBindings @ 0x499680]` (via `Input_ProcessPlayerFrame @ 0x49d4c0`) | mouse deltas `dword_3342E54`(X)/`dword_3342E58`(Y) (Y negated unless invert-Y `dword_24D2078`) × sensitivity `dword_24D207C << 11` (reduced by weapon zoom), fixed-point `(delta*sens + 0x8000) >> 16`, dispatched via `Input_TryTriggerMouseAxisBinding(bindIdx, entity, dX, dY)` to the entity's look-axis bindings → `Yaw` (entity+0x10) / `Pitch` (entity+0x14) |
| Move | `[orig: Player_PackInputStateToEntity @ 0x4df450]` (via `Client_ProcessNetworkFrame @ 0x42c180`, call @ 0x42c3e9, every frame) | `g_inputFlags` (`dword_B3B728`) → 4 direction bits (F/B/L/R) → 8-way `move_direction_index` (0..7) via switch → DWORD at `entity->pad7[12]` (= **entity+0x12C**): low = move index, `\|0x8` = is_moving, plus fire `0x10` / scope `0x100,0x200` / lean `0x1000,0x2000` / grenade `0x4000,0x8000`; analog axes → pad7[16..19] (entity+0x130..0x133) |
| Simulate | `Entity_UpdateInfantryAI` `loc_4B9C3E` (above) | consumes the move order at entity+0x12C — the player's analog of the AI think order — plus the look-set `Yaw`, producing the new live pose |
| Serialize | `[orig: Player_BuildTag0CInputBody @ 0x42A550]` | gate `entity+286 (healthMax) != 0 && (entity+36 & 2) == 0`; → `Pool_SerializeEntityViaVTable` → `NetPacket_SerializePlayerState @ 0x4C09C0` writes the live pose into C2S 0x0C |

The 8-way move map (`direction_bits` F/B/L/R combo → index): F→0, F+L→1, L→2, B+L→3, B→4, B+R→5,
R→6, F+R→7; opposing pairs (F+B, L+R, all-four) → not moving.

**Phase-2 implication.** The SP listen-server player is a pool-0 infantry entity run through the
already-ported motor (`AiSystem::tick_infantry`) with its move order sourced from input
(entity+0x12C) instead of `infantry_think`. **No smooth-target staging and no interpolation branch
are needed for SP** — those belong to the Phase-4 remote-peer receive path. The C2S 0x0C is still
built and looped back for the replicate-to-own-client-view keystone (ADR 0011), but the local
player's *movement* is the motor simulation, not a self-read-apply.

**Follow-ups (unwitnessed):**
- The exact `Yaw`/`Pitch` write inside `Input_TryTriggerMouseAxisBinding` (the per-binding apply)
  — only the sensitivity scaling (`dword_24D207C << 11`, 16.16) and the dispatch are witnessed.
- The precise `move_direction_index` (0..7) → move-mode → anim-state mapping inside the motor
  (largely covered by the existing `infantry.cpp` port; grill if the player's strafe/back speed
  scales need pinning).

**IDB changes this session (comments only; no renames — all functions already curated):**
`set_comments` at 0x4b9a74 (simulate-vs-interpolate gate; corrects the inverted note), 0x4b9a80
(gate 2nd half), 0x4b9a8c (remote-interpolation detail), 0x4df68f (move-order write to
entity+0x12C), 0x499680 (mouse look→Yaw/Pitch front end); `idb_save`.

#### 5.38a Host-side remote-peer disposition RESOLVED — the host SNAPS, never interpolates (Phase 4 grill, 2026-06-23)

§5.38 deferred "the host-side remote-peer receive path" to Phase 4. Grilled this session to gate the
libs/netsim co-op host mover. All anchored (exact disasm; IDB comments saved at 0x4c2000 / 0x4b9a03 /
0x4b9a8c). It settles the two-hypothesis question — **H1** the host interpolates a remote peer locally
vs **H2** the host snaps and only clients interpolate — decisively in favour of **H2**.

- **`is_authority` is GLOBAL** (`g_napi_np_ctx.is_authority`), read directly at the
  simulate-vs-interpolate gate `[orig: Entity_UpdateInfantryAI @ 0x4b9a74]`. A listen-server host
  (`is_authority != 0`) therefore takes the SIMULATE branch (`loc_4B9C3E`) for EVERY entity — local
  player, AI, and remote peers alike — and **never reaches the interpolation fall-through @0x4b9a8c**.
  Interpolation toward the smooth-target is **client-only** (reached only when `!is_authority &&
  entity != g_local_player_entity`). [D-NET-89]
- **The host mover is the read-apply SNAP, not the motor.** `[orig: NetPacket_SerializePlayerState
  case 4 tail @ 0x4c2000-0x4c20a9]`: after four gates — `(entity+0x24 & 2)==0` (movement/spawn gate),
  `g_spawn_success_gate (dword_24C1928)==0`, `playerSlot(packetCtx+0x20)+0x20 == 6` (in-game
  connection state), `dword_C8D824==0` — it STAGES the smooth-target (+0x234/+0x238/+0x23C pos,
  +0x240 heading, +0x244 pitch, +0x248), ALWAYS mirrors the LIVE orientation to +0x10 (heading) /
  +0x14 (pitch), SNAPS the LIVE position +4/+8/+0xC **iff `(entity+0x24 & 1)`** (the net-snap flag),
  and resets the interp progress +0x27C = 0. If the entity is the local player it also caches heading
  into `dword_B75FCC`. [D-NET-90]
- **`entity+0x24 bit0` = the network-snapped / motor-skip flag.** The motor full-skips a net-snapped
  entity `[orig: Entity_UpdateInfantryAI @ 0x4b9a03 (test [esi+24h],1; jnz loc_4BFC8B)]` — it never
  re-simulates a read-applied peer; the same bit gates the live-pos snap @0x4c207e. It is cleared
  @0x4b99ff when `is_authority && entity+0x354 owner-ptr valid && team bytes (+0x162) match &&
  owner+0x21C >= 0x10000`. [D-NET-89]
- **Heading/pitch receive framing — pure widen, no 90° offset (confirms §5.10).** Case 4 reads the
  i16 and does `movsx; shl 16` straight into +0x240/+0x10 (heading @0x4c1da6/0x4c1da9) and
  +0x244/+0x14 (pitch @0x4c1dc7/0x4c1dca) — NO `(90 − yaw)` framing and NO `kBamPerDegree` multiply
  on receive (the wire is already engine-frame BAM; the (90−yaw) conversion is the
  mission-degrees↔BAM boundary only, applied by `snapshot_of` on the FORWARD path).
- **Extended (type-10) position is ABSOLUTE WORLD on receive.** Case 4 stores the raw i32 with NO
  map-origin (`dword_C867A4/8/AC`) add; only the mounted branch lifts vehicle-local via `[orig:
  Entity_TransformLocalToWorld @ 0x43BD00]`. Corrects the §5.10 case-4 table note "(+ map origin if
  no vehicle)" — that add does not exist on the extended receive path. [D-NET-91]
- **Interp math refinement** of the @0x4b9a8c note in §5.38: the saved-live pose +0x80/+0x84/+0x88 is
  recaptured from the live pose +4/+8/+0xC EVERY tick (top of func @0x4b9a5f); the step bucket is
  **{3,4,5,8,16}, not "2..16"** — `dist > 0x20000` SNAPS live to the smooth-target, `dist < 0x2000`
  zeroes it (steps 0), else thresholds 0x2AAA/0x4000/0x5555/0x8000 → 3/4/5/8 else 16; the per-step
  delta `(d + N/2)/N` (signed round) is re-stored into +0x234/+0x238/+0x23C and one step added to the
  live pose each tick; progress +0x27C caps at 512 (@0x4b9c09).
- **Receive dispatch gate** `[orig: dispatch_entity_packet_callback @ 0x4D6A80]`: only
  `g_napi_np_ctx.is_authority` + `owner_ctx` present + `entity == *owner_ctx` (the wire handle
  resolves to the sender's owned entity) + entity_def + the +356 callback; sets ctx mode = 4. **No**
  `entity+286`/`entity+36` health gate on receive — that gate is SEND-side only, confirmed at `[orig:
  Player_BuildTag0CInputBody @ 0x42A550]` (`!entity || !healthMax(+286) || (entity+36 & 2)`).

**Port (libs/netsim + libs/world, this session; verdict MATCHING, unit-tested by
`netsim_loopback_identity`):**
- `EntityWireBridge::apply_player_intent` (`libs/netsim/src/entity_wire_bridge.cpp`) — the host
  read-apply/snap as a two-store wire-boundary write (the inverse of `snapshot_of`): snaps the
  registry `Entity.position/yaw` (the store the S2C 0x0A frame re-broadcasts), mirrors the
  engine-frame `AiEntity` (live pos + heading/pitch BAM), stages the `AiEntity` smooth-target, resets
  interp progress, marks the entity net-snapped; gated on `Entity.flags` bit1 clear; REJECTS the local
  player (the host never read-applies its own pose).
- `drain_connection_c2s` (`libs/netsim/src/connection_fan.cpp`) — the per-connection C2S 0x0C drain
  (authority-gated by the host driver Server_TickUpdate): decode sub-header + extended uplink →
  `PlayerIntent` → `apply_player_intent`. (The legacy `NetSystem::tick` wrapper was retired at P8.)
- `world::AiEntity` (`libs/world/include/world/ai.h`) — `net_smooth_target/heading/pitch`,
  `net_interp_progress/steps`, `net_saved_live_pose`, `net_is_remote_peer`.
- `AiSystem::tick_infantry` (`libs/world/src/infantry.cpp`) — the net-peer skip-guard
  (`if (e.net_is_remote_peer) return;`), the OpenNova analog of the @0x4b9a03 bit-0 full-exit.

The motor interpolation branch (@0x4b9a8c) is **client-only and intentionally NOT ported** — a
deferred client-side smoothing concern (OpenNova's SP-as-listen-server host renders its own view from
the decoded client-view, ADR 0011; no non-authority World motor exists yet). [follow-up: client-side
smooth-target interpolation — the @0x4b9a8c math is fully witnessed above, ready to port when a
non-authority client path exists.]

#### 5.38b Joiner-side self-identification — the name-match in the S2C 0x0C organic-spawn stream (D.0 witness, 2026-06-23)

§5.38a settled the HOST disposition (snap, never interpolate). This is its CLIENT-side counterpart: how a
JOINER (`is_authority == 0`) learns WHICH wire entity is its own player, so it can simulate that player
locally (the §5.38 motor) and stamp the right handle in its C2S `0x0C` uplink. Witnessed from
`.scratch/host_and_join_lan.pcapng` (joiner "cdouglass" → host "biggy", mission dvxi5) cross-read with
Jointops.exe; behavioral, read-only (no IDB writes).

- **Self-ID is a NAME-MATCH in the S2C `0x0C` organic-spawn stream, NOT a slot-assignment packet. [D-NET-92
  — CORRECTED 2026-06-25, see below]** The joiner's own player is the type-`0x14B9` organic in the host's S2C
  `0x0C` batch (§5.23) whose `entity_name` equals the joiner's own player name; matching it sets
  `g_local_player_entity @0xB75FC8`, and that record's `slot_id` IS the joiner's wire handle **H**. In the
  capture, record 5 of the f=516 `0x0C` batch = `slot=0x0005 pool0/s5 type=0x14B9 name="cdouglass"
  pos=(70,25,55.4) yaw=270° team=2` — the joiner's own entity. H (`0x0005`) is fixed at this named spawn
  (f=516), BEFORE the game-start bundle (f=559) and BEFORE the first C2S `0x0C hdl=0x0005` (f=561). It is NOT
  learned from `0x51` (absent in the capture) nor from `0x46` PlayerSync (whose `slot=1 entity=0x0005` arrived
  f=564, AFTER the first uplink). `[orig: NapiNPClientMsg_0x00C @ 0x42E730]`
  - **CORRECTION (D-NET-92):** the RETAIL client's self-ID is **NUMERIC, not a name-match.**
    `Player_FindLocalPlayerEntity @0x4e0090` scans pool 0 for `(entity.miniFlags+0x36 & 0x100) &&
    (entity+0x78 == local_session_id)`, where `local_session_id` is the client's own
    `NapiNPConnection.unk_18` (its ConnectionId / dcb, via `NapiNP_GetLocalConnectionId`). In `host_and_join_lan.pcapng` the
    matched record happened to ALSO carry `name="cdouglass"`, so the name-match was coincidental — the
    `entity+0x78` (eFlags) numeric match is the real mechanism (confirmed by the F3 dcb work,
    `project_dcb_join_mechanism`; the crash is `Player_InitPlayer @0x4e15f0` →
    `Player_BuildNetIdLookupOrFatalError @0x4dff60` when the scan returns NULL). **Host requirement:** for a
    retail joiner the host MUST stamp the joiner's own `0x0C` `entity_flags (entity+0x78)` = the joiner's
    ConnectionId (learned from its in-match `0x48` ack) AND `minimap_flags (entity+0x36)` bit `0x100` — a name
    alone is insufficient. Our `JoinerSession` name-match (`entity_name == player_name`) is a separate
    opennova-side decode convenience for the opennova↔opennova path; it does not reflect the retail client's
    self-ID path. `[orig: Player_FindLocalPlayerEntity @0x4e0090]`
  - **Naming (2026-06-26, §5.41):** `entity+0x78` is now `GamePlayerEntity.ownerConnectionId` (was
    `entityFlags`; D-NET-101); the local-id getter `sub_4C6D40` is now `NapiNP_GetLocalConnectionId`
    returning `NapiNPConnection.connection_id` (D-NET-100); the NULL-abort `0x4dff60` is now
    `Player_FatalPlayerDcbNotFound` (D-NET-102).
- **`NapiNPClientMsg_0x00F` (WORLD-STATE-LOAD, §5.29) drives the post-load client burst when `!is_authority`.**
  It applies the spawn pos/yaw to the already-identified `g_local_player_entity` and clears the §5.6
  movement gate (`Flags & 1`); on a non-authority client it additionally caches the spawn at
  `dword_A87068/6C/70` and QUEUES the witnessed reply burst `0x28 / 0x29 / 0x2D / 0x32` (then `0x22 / 0x23`).
  `[orig: NapiNPClientMsg_0x00F @ 0x42E200]`
- **`NapiNPClientMsg_PlayerSync` (`0x046`) is a SECONDARY slot↔handle channel, not the primary self-ID.** It
  binds a player-table slot to an entity (`entity_slot_id → Pool_GetEntryUnchecked(0, id)`; playerTable
  `slot+36` = entity, `slot+15` = entity_slot_id) and arrives AFTER the joiner already self-identified via
  the `0x0C` name-match. `[orig: NapiNPClientMsg_PlayerSync @ 0x431370]` (full layout §5.21)
- **The witnessed joiner C2S admission sequence is server-reactive, with packet boundaries that
  matter (§5.0d):** exact `0x00` JOIN → `0x01 {00}` → `0x02` (256 B), then one grouped
  `0x4E/0x03/0x48/0x47/0x33`, standalone `0x47`, standalone eight-zero `0x37`, and one grouped
  `0x09/0x22`. The client ACKs terminal S2C `0x11` and loads locally before its empty C2S `0x0A`
  releases the world stream; only S2C `0x1A` triggers grouped `0x2F ×2 / 0x0B`, and the ensuing
  initial S2C `0x5A` pair grants the loadouts. With spawn zones (`0x0F gameFlags & 1`) the
  joiner then sends one C2S `0x0E {FF FF}` and waits for the later post-pick `0x5A` release;
  without spawn zones the initial grant is also the release. The joiner sends no C2S `0x0C`
  before it knows H or before that applicable release—there is no pre-deploy pose uplink.
- **Unpinned (low-risk):** the exact store that writes `g_local_player_entity` on the name-match — the
  `0x42E730` handler exceeds a clean single decompile — is not byte-anchored; the mechanism is empirically
  certain from the wire (H == the named record's `slot_id`, set before any C2S `0x0C`).

**Port (`libs/npruntime` + `godot/engine`, this session; verdict MATCHING, unit-tested by
`npruntime_client_runtime` + `npruntime_two_endpoint_socket` + `netsim_build_player_uplink`):**
- `JoinerConnection` (`libs/npruntime/src/joiner_connection.cpp`) — the CLIENT MIRROR of the game
  host: ClientHello/Auth handshake (the joiner's player name rides game `ClientAuth.NA`, the
  free/unvalidated field the host echoes into the organic-spawn `entity_name`), then a
  server-driven admission FSM emits the §5.0d replies at their witnessed S2C triggers. `pump()`
  flushes pending ACKs, active-send probes, and the held C2S `0x0A` when the binding reports
  `world_ready`; it does not invent a fixed admission burst. An early terminal `0x11` is latched
  like the early `0x16` (see §5.0d). The S2C `0x0C` handler name-matches
  `entity_name == player_name` → adopts `slot_id` as wire handle **H**; a second same-name record
  with a DIFFERENT slot pre-release fails the join with a duplicate-callsign error (post-release
  the latched handle is kept — D-NET-169). It does NOT compose `ClientSession` (whose
  post-`0x82` path is the matchmaking lobby-verify flow, the wrong channel for the in-match game
  connection). The Godot binding surfaces `post_auth_stage_name()`; the shell watches the
  post-load admission tail against the retail 60 s window and aborts to the menu with a
  stage-named error (the reachable analog of `NapiClient_WaitForGameStart @0x42cc10`'s failure
  legs) instead of holding a hidden player forever.
- Deployment is a separate gate from self-identification. With spawn zones, the joiner answers
  the initial loadout grant with one C2S `0x0E {FF FF}` and waits for the post-pick S2C `0x5A`
  before entering InMatch and enabling its `0x0C` uplink. With no spawn zones, the initial
  `0x5A` is the applicable release.
- Two-handle reconciliation: the joiner simulates its OWN local player (handle L, motor-driven per §5.38)
  and stamps **H** (the wire identity) in its C2S `0x0C` sub-header so the host's `apply_player_intent`
  (§5.38a) resolves the right peer; the wire present is self-filtered on H (render local L, not the host's
  SNAP of self). The joiner-side `NovaSimulation` mode is the next increment.
- Host side: `NovaSimulation::announce_joiner_organic_spawn` builds a 1-record `OrganicSpawnBatch
  {slot_id = the admitted handle, entity_name = the joiner's `ClientAuth.NA`, type 0x14B9, pose}` →
  `encode_organic_spawn_batch` → `HostSessionAccept::frame_in_match_s2c(peer, 0x0C, …)`, so the joiner can
  name-match. The game host captures `ClientAuth.NA` into `NapiNPConnection.player_name` and surfaces it
  on the `PeerSpawned` event.
- ctests: `tests/npruntime/client_runtime_test` + `tests/npruntime/two_endpoint_socket_test`
  (the promoted `np::JoinerConnection` driven against the real np server legs on the World path:
  handshake → name-match + applicable deployment release → InMatch with H → C2S `0x0C`
  uplink → host apply; the old
  `joiner_session_test`, which drove the retired `novaworld::JoinerSession` against the retired
  `HostSessionAccept`, was deleted with those classes at P8);
  `tests/netsim/build_player_uplink_test` (the joiner-side uplink body builder).

**Port status — D.2 (joiner `NovaSimulation` mode + wire-direct present, 2026-06-23).** The joiner-side
runtime is built and green (`godot/tests/net/coop_two_sim_test` — a host listen server + a joiner in one
process, each on a real loopback `NovaUdpPump`, free-running their own `step`; asserts the joiner
reaches InMatch, the host admits it, and the two-handle present resolves both ways):
- `NovaSimulation::enable_join(host_ip, port, name)` mirrors `enable_host_listen`: dial a pump, drive a
  `JoinerSession`, feed the host's S2C `0x0A` into the SAME `NetClientView`/`ClientState` the listen server
  uses (via an identity-framed `UdpSessionTransport` conduit). The joiner runs `run_logic_tick(false)`,
  never emits S2C, never registers `NetSystem`; after the name-match and applicable deployment release
  it `spawn_player`s **L** at the H-learned pose and per-frame builds the C2S `0x0C` uplink stamped
  with **H**.
- **Remote entities render WIRE-DIRECT** (`wire_present_pass.gd`), the faithful client model (§5.23/§5.25):
  a non-authority client cannot resolve the host's entities through its local `MissionEntityRegistry` (the
  wire handles live in the host's handle space), so it builds one model per wire handle, keyed by the wire
  `type_id`, posed from the decoded `0x0A` position + coarse yaw. The host keeps the registry-resolved
  `MissionPresentPass` for its placed NPCs and adds the wire pass ONLY for un-placed spawned players (an
  admitted joiner has no `.bms` node) — so co-op is bidirectional on both sides. Each side excludes its own
  local player from the wire pass (drawn by `LocalPlayerPresenter`); the joiner keys that exclusion on **H**,
  not L (L collides with a host-side slot). The present buffer gained `PF_TYPE_ID`/`PF_WIRE_HANDLE`.
- **Refinement [D-NET-96]:** the host surfaces `PeerSpawned` REACTIVELY, from `handle_datagram` on an
  incoming SESSION packet (`host_session_accept.cpp` — not from `tick_handshakes`), so once the
  late-spawn gate opens (which takes ~tens of server ticks of world streaming) there must be an inbound
  `0x43` to surface it on. `JoinerSession::pump` therefore keeps streaming a per-frame keepalive (the
  witnessed `0x22` player-sync) after the spawn-gate burst instead of going silent — the real client never
  goes quiet mid-join. Equivalent in effect to the original server proactively pushing the organic-spawn
  when its gate opens.

**Automated confirmation + open issues (updated 2026-07-22).** The real-socket
`coop_two_sim_test` proves a host and joiner reach InMatch and present both sides in one process;
the focused menu/GameWorld tests prove discovery handoff, retained-session mission loading, and
visible bind failure. A final two-GUI smoke with installed retail assets remains a manual acceptance
check for this delta, not an automated claim. The host listens on the requested retail-range port;
the direct launch contract is `NW_LAN_HOST=<m.bms>` for the host and
`NW_LAN_JOIN=<ip>:<port>` for the joiner. The joiner learns the mission from the retained game
session's post-auth S2C `0x7B`, as retail does; `NW_LAN_MISSION` remains only an explicit
compatibility/debug override for the older preloaded path.
Tracked client-fidelity items:
1. **[CLOSED 2026-06-25 — stream the DYNAMIC set during load]** *(was: the joiner saw only the host player,
   not the NPCs.)* The host streams the networked/dynamic set during the joiner's world-load via
   `NovaSimulation::stream_world_state_to_peer` (fired on the F3 `PeerEnteredWorldStreaming` event, after the
   joiner's own dcb-bearing `0x0C`): an **empty `0x10`** phase marker, the **pool-0 organics (host player +
   AI) in `0x0C`** (paged, remote peers excluded — their dcb record streams separately), then an **empty
   `0x20`** phase marker (the empty 0x10/0x20 drive the witnessed `g_loading_progress` 2/5 phases without
   data). **(SUPERSEDED: this is the original MINIMAL stream — dynamic-only, empty 0x10/0x20 — which stalled
a live retail join at 7%. The host now streams ALL four pools paged in the witnessed phase order
0x10→0x0D→0x0C→0x20 [orig: Server_SendInitialGameStateToPlayer @0x51bba0]; see the D-NET-98 UPDATE.)** Built: the
   faithful `encode_static_entity_batch` (`0x10`, byte-exact inverse of `decode_static_entity_batch`,
   replacing the simplified `build_tag_10_entity_batch` stub); libs/netsim `build_pool{0..3}_*` extractors
   (routed by `handle.pool()`); joiner reception (`JoinerSession` surfaces spawn bodies + `NetClientView::pump`
   decodes `0x10`/`0x0C`/`0x20` into the single `ClientState`). Verified by `coop_two_sim_test` (the joiner's
   present carries the host's two AI organics AND the pool-2 building — which IS wire-streamed under 0x10,
   matching @0x51bba0 phase 1; D-NET-98 UPDATE) + lib round-trips (`nw_ingame_encode`,
   `netsim_world_stream_extractors`). Pool-1 (`0x0D`) is now streamed, AI-trailer crash fixed (D-NET-97).
2. **Remote body phase + acceptance arbitration — FIXED for the received compact channel
   2026-07-21.** Both compact organic forms carry the body-state byte. The player form additionally
   carries the authority's elapsed half-frame tick byte (off 15); the infantry form intentionally
   does not. A newly accepted player state seeds that elapsed tick once and then free-runs locally;
   repeated same-state packets do not rescrub the clip, and infantry no longer pins to frame zero.
   `NovaObjectModel` owns the retail current/pending channel: current flags `0x04` queue an incoming
   state; current `0x20` queues unless the incoming state carries `0x01`; otherwise the state
   interrupts immediately. Pending states promote at the current clip's completion boundary and begin
   at tick zero. Concrete skeletal tests pin seeding, free-run, same-state stability, both flag gates,
   and completion-time promotion. The packed present row explicitly distinguishes a joiner's raw compact
   request from the host's already-accepted authority state, so the listen host keeps its direct phase pose
   instead of re-arbitrating a transition. Raw player/infantry lifecycle flags now drive remote hidden/dead
   state; dead non-hidden corpses stay visible, and a decoded dead→alive revision resets the body channel
   before the first respawn animation request even when several wire frames folded in one render pump.
   D-NET-159 remains open only for its upstream authority body-motor
   stand-ins (gait/lean/death selection), not this receive-side phase/arbitration path. [orig: player
   write @0x4c0cf2; remote accept @0x4c11a6; infantry compact apply @0x4c0859;
   AnimChannel_AdvancePlayback @0x40b140]
3. **The joiner's local player uses the NPC motor.** On a non-authority client, L does not route through the
   `is_local_player` infantry-motor branch (§5.38) the SP host uses, so the joiner's own movement reads as
   NPC locomotion. Investigate the joiner `spawn_player` + motor gating under `run_logic_tick(false)`.
4. **Joiner authoritative health/death bridge — PARTIALLY FIXED 2026-07-21.** NetClientView already
   decoded the recipient-specific `0x0A` tail health, but ClientRuntime never consumed it and the Godot HUD
   continued reading local predicted entity L. A fresh decoded zero now closes the deployed/uplink gate in
   the same client frame, and NovaSimulation applies the scalar to L's registry + infantry-motor stores
   without resolving host-space identity H or disturbing L's predicted pose. The fresh-frame guard prevents
   the pre-`0x0A` default zero from killing L during handshake; once dead, L also rejects a later positive
   tail until a real deploy edge can restore it. This ports the observable death outcome, but infers the gate
   from tail health rather than retail's separate state/flags edge. The initial-join C2S `0x0E`
   deployment pick is PLAYER-PACED as of 2026-07-24: the DEATH deploy screen (death.mnu, §5.61's
   screen witness) parks a pick-required join at the player's zone pick — re-picks included — with
   the headless auto parameter-0 default preserved for ctest/replay callers. POST-DEATH
   respawn/deploy (re-opening the same screen on the death edge), spectator, and the deploy-map
   window's terrain draw (D-HUD-19) remain open. [orig: tail health read @0x430428; local Health
   store @0x4305df]

**D-NET-97 [TRACKED SIMPLIFICATION + POOL-1 CRASH] — pool routing is by `EntityKind`, not item-def
capability flags; pool-1 (`0x0D`) deferred.** The original routes an entity to a wire pool (and thus its
stream tag) by item-def capability flags read off the live engine entity (`itemDef+604`,
`itemDef+84 & 0x100000/0x40000`, `entity+100/+104`) — a purely-static structure goes to pool-2 (`0x10`), a
destructible/AI-bearing object to pool-1 (`0x0D`). Our `pool_for_kind` (`libs/mission/src/promote.cpp`)
assigns the pool at promotion by BMS `EntityKind`: `Organic→0`, `Item→1`, `Building→2`, `Marker→3`. The
per-pool wire BYTES stay §5.x-faithful (each `encode_*_batch` round-trips its witnessed decoder); only the
static-vs-destructible split heuristic differs.

**Pool-1 `0x0D` AI-trailer — now FAITHFULLY gated (was a live crash, RESOLVED 2026-06-25):** the retail
`0x0D` decoder `[orig: NapiNPClientMsg_0x00D @0x432c40]` enters an AI-name `strcpy` **whenever the record's
item def is AI-capable** (`if (itemDef->attrib & 0x100000)` `@0x433327`), reading the `0x0800` AI-trailer name
pointer `aiNameStr`. That pointer is **NULL unless the record set spawn-flag `0x0800`** — and the original
serializer GUARANTEES `0x0800` for any AI-capable item def. The crash window is therefore exactly:
`0x0800`-clear **and** AI-capable → `aiNameStr==NULL` → `strcpy[NULL]` at **`0x433370`** (`mov cl,[edx]` with
`edx==0` → access violation; SYSDUMP confirmed 2026-06-25, last packet `#13`=`0x0D`). **Fix (landed):**
`build_pool1_spawn_batch` now emits the `0x0800` AI-trailer **iff the entity is AI-capable**
(`Entity::is_ai_capable`), which is resolved from `items.def ItemDefAttrib & 0x100000` (the `AIData` token) —
parsed into `DefItemDef.attrib` (`libs/def`), surfaced as `NovaItemDatabase::is_ai_capable`, and stamped onto
every live entity by the host's `NovaSimulation::resolve_item_traits` post-load pass (called from
`MissionRuntime` alongside `resolve_infantry_adm_ids`). Because our emit gate is now the SAME predicate as the
decoder's own gate (`attrib & 0x100000`), an AI-capable record ALWAYS carries the `0x0800` flag + a valid
in-packet NUL-terminated name → byte-faithful (retail emits the trailer iff AI-capable) AND crash-safe. The
earlier dc90f64f stopgap (force `0x0800` on EVERY pool-1 record) is removed — no remaining divergence on the
trailer. The pool-0 (`0x0C`), pool-2 (`0x10`) and pool-3 (`0x20`) handlers are crash-safe (`0x0C` reads its
name inline/always-present; `0x10`/`0x20` have no name/AI branch). Byte-matching a specific retail mission
also wants the item-def flag routing (the residual D-NET-97 simplification, still by `EntityKind` here).
**Routing-fn clarification (witnessed 2026-07-05):** `serialize_entity_pool_to_packet_0 @0x503940` is the
pool-1 SERIALIZER (`Pool_GetEntryUnchecked(1, …)`), not the routing decision — it READS the flag gates
during emission (`itemDef+84 & 0x100000` → the name + `0x0800` AI-trailer; `& 0x40000` → the `0x8000`
zone-slot branch; `itemDef+604` → the `0x400` weapon-seat block), but pool MEMBERSHIP is assigned at
spawn-time registration into `g_pool_list` (a separate fn, still to hunt for the port). So the D-NET-97
routing slice must: (a) find the spawn-time entity→pool assignment that keys on `itemDef+84 & 0x100000`
(AI) / `& 0x40000` (destructible) → pool-1 vs purely-static → pool-2, and (b) port it into
`pool_for_kind` (`promote.cpp`), which today keys on `EntityKind` at promotion — noting the ordering
gap that item traits are resolved post-load (`NovaSimulation::resolve_item_traits`), after promotion.
**Router-hunt narrowing (2026-07-05):** `Pool_GetEntryUnchecked @ 0x441fc0` is a bare
`g_pool_list[poolIndex].base + idx*stride` accessor, and each wire serializer reads ONE
`g_pool_list` pool (pool-1 = `Pool_GetEntryUnchecked(1,…)` @ 0x503940, no flag filter in the
serializer) — so **an entity's wire pool IS its `g_pool_list` membership**, set at allocation, not a
serialize-time re-classification. `Entity_InitFromItemDef @ 0x49e550` is only the item-def→entity
field copy (callbacks/models/health), NOT the pool selection. The router is therefore the entity
ALLOCATION's `g_pool_list` pool pick (the `Pool_Alloc(poolIndex)` caller that reads `itemDef+0x54`);
that is the remaining hunt. NB the PORT is golden-gated regardless: changing pool membership changes an
entity's wire TAG (0x0D↔0x10), which alters byte-exact golden captures — a tier-2 golden-harness slice.

**D-NET-98 [SCOPE — the load-time world-stream is the DYNAMIC set, not the full static mission].** The
golden retail capture `.scratch/host_and_join_lan.pcapng` (a real host+join, mission dvxi5) shows the host's
load-time world-stream is SMALL: `0x0b` BMS-header → `0x10` **empty (len=4)** → `0x0d` (192 B) → `0x0c`
(278 B, ~5 organics) → `0x20` (92 B, a few markers) → game-start bundle (`0x42`/`0x0a`/`0x0f`/`0x61`). The
host does NOT stream the hundreds of static buildings or nav markers — in the original, static mission
geometry is CLIENT-LOCAL `.bms` data; only the server-authoritative DYNAMIC entities (players, AI, a few
gameplay markers/destructibles) ride the wire. Our `promote_mission` over-populates the networked World pools
with the ENTIRE static mission (00TRg/dvxi5: 816 statics → pool-2, 497 markers → pool-3), so a first cut that
streamed pools 2/3 in full flooded the client and walked the retail `Pool_GetEntryUnchecked(2/3, start+i)`
(`@0x4334a8` / `@0x425c77`, UNCHECKED) past pool capacity → memory corruption → the observed "load finishes,
resets, reloads, kicked to login". FIX: `stream_world_state_to_peer` streams ONLY pool-0 organics (`0x0C`),
with empty `0x10`/`0x20` as the witnessed load-progress phase markers (`g_loading_progress` 2/5). Open
follow-ups: (a) the JOINER should load static geometry locally (like retail) rather than rely on the wire —
`game_world.gd` currently skips ALL placement on a joiner, so opennova joiners won't see buildings until they
place statics locally; (b) stream the small NETWORKED marker subset (spawn volumes / objectives) via `0x20`
rather than all 497 — needs the witnessed "is this marker networked" gate. [net-re §5.2a world-stream pacing.]

**D-NET-98 UPDATE (2026-06-29) — the original DOES stream pool-2 statics; the divergence is PROMOTION,
not streaming. The "stream only pool-0 / empty 0x10" FIX above is SUPERSEDED.** Witnessed in
`[orig: Server_SendInitialGameStateToPlayer @0x51bba0]`: the per-joiner initial-state is a frame-paced
state machine whose **sync-state 4 walks ALL FOUR pools in a fixed phase order**, each under its own tag,
each bounded by `[orig: Pool_GetUsedCount @0x441f80]` (= `g_pool_list[idx].used`, arg is a pool index 0..3):
phase 1 **pool-2 statics → `0x10`** `[orig: loc_5042F0 @0x5042F0]`, phase 2 pool-1 → `0x0D`
`[orig: serialize_entity_pool_to_packet_0 @0x503940]`, phase 3 pool-0 dynamics → `0x0C`
`[orig: serialize_entity_states_to_buffer @0x5030a0]`, phase 4 pool-3 markers → `0x20`
`[orig: serialize_entity_pool_to_packet @0x503460]`. The `0x10` body is `[WORD cursorStart][WORD count]`
then per-entity `[orig: Pool_GetEntryUnchecked @0x441fc0]` records of `handle + XYZ(int32×3) + flag-gated
optionals`, re-emitted each tick (~650 B) advancing the per-player cursor `playerSlot+0x15F1C` until
`Pool_GetUsedCount(2)` — i.e. the **full pool-2 set IS streamed**, as paced `0x10` batches. So the host
does NOT skip statics. The golden's `0x10` **empty (len=4)** = `[cursorStart=0][count=0]` is simply
retail's **pool 2 being EMPTY in that session**: retail keeps client-local BMS *map geometry* out of
entity pool 2 (only networked statics are pool-2 entities), so its `0x10` is header-only — it is NOT
evidence the host skips statics. **The actual opennova divergence is PROMOTION:** our `promote_mission`
puts the ENTIRE static mission into pool 2 (816 statics), so our (correct, paged) `0x10` carries them
while a retail host's is near-empty. Current code (`server_initial_state.cpp` sync_state 4,
`emit_paged_pool` per pool) already streams all four pools in this phase order (the comment "reverses the
D-NET-97/98 shortcut") — wire-FORMAT-faithful and crash-safe (paged, bounded), which fixed the 7% stall;
a stock client accepts it. Residual divergence = the pool-2 **contents** (promotion should exclude
client-local map geometry to be byte-equal to retail). `coop_two_sim_test` now asserts the pool-2 building
IS streamed under `0x10` (matching the binary), not absent.

#### 5.38c The deploy gate — why a retail joiner stalls at spawn-select and auto-kicks (2026-06-25)

Witnessed by diffing two captures of the SAME mission (AS – Dormant Volcano Isle / `ASH_I5A.BMS`):
`.scratch/retail_nw_host_and_join_jored_host_ljim_client.pcapng` (a working NovaWorld retail-host →
retail-client join) against `.scratch/capture5.pcapng` (our listen-host, a stock retail client joining and
**failing**), cross-read with Jointops.exe (read-only, no IDB writes). In capture5 the joiner clears the
"Could not find player dcb" crash (the F3 value fix holds), **reaches spawn-select, sends 0 C2S `0x0C`
deploy uplinks, floods the host with ~378 C2S `0x0F` (2 B each), then auto-kicks**. Retail sends 31 C2S
`0x0C` and 0 C2S `0x0F`.

- **The C2S `0x0C` deploy uplink has TWO gates: `!dword_81474C && !g_spawn_success_gate (0x24C1928)`. [D-NET-99]**
  `[orig: Client_ProcessNetworkFrame @0x42c46d / @0x42c4a3]`.
  - `dword_81474C` (loading-wait): set at load start, cleared by the `0x0F` world-state-load handler
    `[orig: @0x42E391]` (and `0x5A` loadout-sync `@0x429713`). **In capture5 this gate WAS satisfied** — the
    client processed our `0x0F` (it sent the `0x0F`-triggered C2S burst `0x28/0x29/0x2D/0x32` at f=231, AFTER
    the `0x0F` at f=224). So `dword_81474C` was clear when the flood began. **The `0x0F`-vs-`0x0C` load ORDER
    is therefore NOT the deploy blocker** — an early hypothesis this session that was REFUTED by the capture,
    and harmful: deferring the joiner's own `0x0C` past the game-start bundle re-triggers the dcb crash
    (`Player_FindLocalPlayerEntity @0x4e0090` returns NULL → `Player_BuildNetIdLookupOrFatalError @0x4dff60`,
    `__noreturn` MessageBox), because `Player_InitPlayer @0x4e15f0` runs the pool-0 self-scan during load,
    BEFORE the late `0x0C` arrives. **The joiner's `0x0C` must stay EARLY** (streamed during world-load on
    `PeerEnteredWorldStreaming`), as the F3 fix already had it.
  - `g_spawn_success_gate (0x24C1928)` — **the actual deploy blocker. RESOLVED.** Complete writer set
    (every encoding byte-searched at the global's address): **four SETTERS → 1** —
    `NapiNPClientMsg_GameReset` (opcode **0x25**) `[orig: @0x422849]`, `NapiNPClientMsg_0x01D` (opcode 0x1D
    round-end scoreboard, `mov …,1`) `[orig: @0x430858]`, `Server_ProcessRoundEnd` `[orig: @0x5168e4]`,
    `Cine_StartPlayback` `[orig: @0x577848]` — and **one CLEARER → 0**: `Game_StartMission`
    `[orig: @0x524a1f]` (mission-load; ebx held 0 from the entry `xor ebx,ebx @0x524381`). The setters all
    mean "between rounds / pending / spectating"; the lone clearer means "actively playing." `Game_StartMission`
    runs ONCE per mission/round at the client's **Game-Loop mode-enter**: the join FSM
    `MultiPlayer_JoinSessionStateMachine` (`dword_25E58A0`, states 0-9) reaches **state 7** and does
    `ui_nav_history_push("Game Loop")` `[orig: @0x56a91d]`, whose mode descriptor's enter-callback `loc_526370`
    `[orig: @0x526370/@0x526377]` calls `Game_StartMission`. While the gate stays SET, the deploy uplink is
    blocked AND `dword_C8D820` counts down to `reason = 4` `[orig: @0x42c3c1]` = the auto-kick.
    **WIRE PROOF (`.scratch/host_and_join_lan.pcapng`, the full working retail LAN join):** the joiner sends
    `C2S 0x03` at f=470 (the state→7 transition = `Game_StartMission`, `NetPacket_WriteSessionTick @0x56a70a`),
    DEPLOYS at f=561 (`C2S 0x0C hdl=0x0005`, gate=0), and the capture's **only `0x25` is at f=895** — 334
    frames AFTER deploy (a later round-reset). So a working initial join sends **no gate-setter before deploy**;
    `Game_StartMission` leaves the gate clear and the joiner deploys. **Our bug:** capture5's joiner runs
    `Game_StartMission` early (`C2S 0x03` at f=176 → gate=0), then our game-start bundle emits a `0x25` at f=224
    → re-SETS the gate with nothing left to clear it → stuck + kicked. **FIX (landed):** drop the `0x25`
    `RESET_AND_START` from the joiner's bundle (`game_session.cpp` `add_game_start_bundle`). Unit-proven by
    `game_session_test` / `host_session_accept_test` ("bundle does NOT carry 0x25"); opennova↔opennova
    `coop_two_sim` still green. Live retail re-test pending.
- **The C2S `0x0F` flood is an entity item-resync request, NOT spawn-point enumeration.** Emitted inside the
  per-frame `0x0A` handler `[orig: NapiNPClientMsg_0x00A @0x4307C6]` — a 2-byte "resend this entity, my
  weapon-slot copy doesn't match the snapshot" request, one per mismatched slot. An undeployed joiner's local
  entity has empty/default weapon slots, so every incoming `0x0A` keeps mismatching → the flood; it stops the
  instant a real deploy populates the slots. (Corrects the prior "0x0F = capture-zone/spawn query" guess in
  `project_dcb_join_mechanism`.) It is a SYMPTOM of being undeployed, not a cause.
- **The spawn-select↔deploy VISUAL toggle is `g_death_screen_active`, driven per-frame by `0x0A` flags1 bit0 — not by
  the `0x0F` handler.** `NapiNPClientMsg_0x00A` sets `g_death_screen_active=1` (enter spawn-select) when flags1 bit0 = 1
  `[orig: @0x42ffa0]` and clears it to 0 the first frame flags1 bit0 = 0 `[orig: @0x43001e]`. The `0x0F`
  handler only READS `g_death_screen_active`. (Corrects the prior note pinning the gate to the `0x0F` handler.) Note
  this is the camera/UI toggle; the deploy UPLINK is separately gated on `g_spawn_success_gate` above.
- **In-match S2C `0x1A` is a secondary liability.** `NapiNPClientMsg_0x01A @0x425EB0` feeds
  `NapiClient_WaitForGameStart @0x42cc10`, which re-sets `dword_81474C = 1` `[orig: @0x42cc14]`. Retail does
  not send `0x1A` post-load; our `GameSession` leaks it (`game_session.cpp:442/458/483`). Hygiene, not the
  blocker.
- **Ruled out (IDA-verified red herrings):** `0x4E FF FF` is a zero-iteration no-op kill loop
  `[orig: NapiNPClientMsg_HandleBatchKill @0x431891]`; the joiner's pool-0 slot index is irrelevant (self-ID
  is `(miniFlags&0x100) && (eFlags==local_session_id)`, `[orig: @0x4B1124]`); pool 0 capacity is 256
  `[orig: EntityPool_Allocate @0x4421CD]` so a 56-entity mission does not overflow.

**Verdict / D-NET-99 status: FIX LANDED, live-validation pending.** The deploy blocker is
`g_spawn_success_gate`, re-armed by the bundle's `0x25` GameReset after the joiner's `Game_StartMission`
already cleared it — NOT the `0x0F` load ORDER (a hypothesis REFUTED this session). Fix: the joiner's
game-start bundle no longer emits `0x25` (`game_session.cpp` `add_game_start_bundle`), matching the working
retail initial join which sends no gate-setter before deploy. The joiner's own `0x0C` stays streamed EARLY
during world-load (`PeerEnteredWorldStreaming`) — an earlier attempt to defer it to `PeerSpawned` was
reverted because it re-triggered the `Player_BuildNetIdLookupOrFatalError` dcb crash (`Player_InitPlayer
@0x4e15f0` scans pool 0 before the late `0x0C` arrives). Verified: `game_session` / `host_session_accept` /
`game_server_runtime` ctests + `coop_two_sim` GUT green; the bundle is unit-asserted to omit `0x25`. Open:
end-to-end confirmation with a stock retail client (joiner should now send C2S `0x0C` deploy uplinks and
not flood `0x0F`). **Process lesson recorded:** two wrong fixes this session (the `0x0F`-ordering mechanism
and the `PeerSpawned` deferral) both came from trusting wire-order intuition over the gate witnesses; the
fix only converged after enumerating every writer of `g_spawn_success_gate` and reading the full LAN-join
capture. Witness the gate, then diff the working capture, before editing emit code.

#### 5.38d Local-player body motor — fall-damage tolerance, anim-channel gate, spectator divert (retail-join grills, 2026-06-28)

`[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]` is the local-player/infantry BODY motor (look,
stance, body anim, ground collision, fall-damage) — distinct from the AI/infantry locomotion-integration
motor `Entity_UpdateInfantryAI @ 0x4b9910` (§5.38/§5.38a); the exact division of labour between the two is
a follow-up. Witnessed this session debugging a retail joiner that could look but not move and took constant
damage; cross-checked against `.scratch/golden/retail-lan-host-join.pcapng`. All anchored (exact disasm,
imagebase `0x400000`); read-only.

**Early gates (in entry order):**
- **Spectator / spawn-select divert.** `cmp g_death_screen_active, 0` @0x4b40f8: while `g_death_screen_active` is SET, if
  `entity == g_local_player_entity` the motor calls `Camera_UpdateFreeFly @ 0x4b2980` and RETURNS @0x4b410d-
  0x4b411a — look-only, no body simulation. `g_death_screen_active` is the spawn-select / spectator flag, set by S2C
  `0x075` `[orig: NapiNPClientMsg_SetSpectatorMode @ 0x4259e0]` and per-frame by `0x0A` flags1 bit0 (set
  @0x42ffa0 / clear @0x43001e, §5.38c). Our build sends flags1 bit0 = 0, so this is NOT a blocker for us —
  it is the witnessed look-only path, confirming the spawn-select camera is this divert.
- **Flags bit 0 gate.** `mov edx,[esi+24h]; test dl,1; jnz loc_4B83A9` @0x4b411b-0x4b4127 — bail (no
  locomotion) if `Flags` (entity+0x24) bit 0 is set.
- **Anim-channel gate (the move/look split).** `mov eax,[esi+188h]; cmp eax,ebp(=0); jz loc_4B83A9`
  @0x4b412d-0x4b4135 — if `animChannelB` (entity+0x188, §5.2b) is NULL the motor BAILS before any
  walk/crouch/prone (the 8-way move order is read immediately after: `mov eax,[esi+12Ch]; and ecx,7`
  @0x4b414b-0x4b4153). So a local player with `+0x188 == NULL` can still LOOK (the separate
  `Camera_UpdateFreeFly` path) but cannot LOCOMOTE. `animChannelB` is written ONLY by
  `[orig: AnimMap_RegisterEntity @ 0x40bb60]`.

**Where the local player's `animChannelB` gets registered (the move gate's source).** At ROUND-LOAD,
`[orig: Game_ReloadEntityModelsAndCallbacks @ 0x522830]` walks pool 0; for each player with `Flags & 0x100`
(@0x522c59) it resolves the soldier model from `[orig: AnimMap_GetSlotPropertyInt(entity->playerClass
/*+0x294, §5.2b/D-NET-103*/, lod_level) @ 0x4127b0]` (@0x522c91 / 0x522c85 / 0x522c79 per LOD) →
`ItemList_FindIndexByTypeId` → entity+28 → `EntityDef_LoadModelsAndCallbacks`; then IFF the resolved
item-def's ADM filename (`ItemDef+192`) is non-empty (@0x522d07) it loads the ADM
(`AnimMap_LoadAdmFile @0x522d14`) and calls `AnimMap_RegisterEntity @0x522d38` (→ writes `+0x188`). An MP
session (`g_napi_np_ctx.is_in_session`) preloads EXACTLY soldier classes 5-9 @0x522872-0x5228e5. A player
whose `playerClass` is outside 5-9 resolves to a slot with no soldier ADM → no register → `+0x188` stays
NULL → the body motor bails. ⇒ **the joiner's own §5.23 `0x0C` player spawn MUST carry a valid `playerClass`
∈ 5-9** (the byte after `NetId` in the `0x0C` record, stored @0x42e9d5, §5.2b), **and the spawn must reach
the client BEFORE its round-load.** The `0x0C` handler `[orig: NapiNPClientMsg_0x00C @ 0x42e730]` itself
calls `Entity_InitFromItemDef @0x49e550` (a leaf, §5.2b) and registers NO channels — registration is the
client-local round-load path above.

**Landing-impact / fall-damage (the constant-damage root).** After ground/collision resolution
(`[orig: movement collision resolver @ 0x4b2bd0]`, returns the ground delta) @0x4b7cf4, when
GROUNDED (delta ≤ 0; `jg` skips otherwise @0x4b7d04) the motor compares vertical velocity `velZ`
(entity+0xA0) to `dword_C6EAE4 × -1057` (`imul eax,0FFFFFBDFh` @0x4b7d15; `cmp [esi+0A0h],eax; jg` skip
@0x4b7d1b): if `velZ ≤ threshold` AND `entity == g_local_player_entity` it calls
`[orig: Player_OnDamageReceived @ 0x4dd880]` @0x4b7d2d — screen-red `dword_B764B4 += 120` (cap 255),
camera-shake `dword_B764B0 += 10` (cap 255), and (self-attacker) `Radar_AddBlip(…, 255)` = minimap-red. The
HEALTH reduction in the sibling block @0x4b7d3b is `g_napi_np_ctx.is_authority`-gated (only the server lowers
`Health`), so a client shows the feedback but never dies from it. The death site (`Health ≤ 0`) is the
sibling branch @0x4b61f5. With **C6EAE4 = 13** (init default, @0x4f638b) the threshold is −13741 (only a real
hard fall trips it); with **C6EAE4 = 0** the threshold is 0, so any downward micro-velocity trips it EVERY
grounded frame → constant red/shake/minimap-red while `Health` stays full (authority-gated). ⇒ a server MUST
stream the `0x0A` sub-block 1 (§5.9) carrying `C6EAE4 ≠ 0` (retail 13); a server that only ever sends
sub-block 0 leaves the client's tolerance at 0 and inflicts per-frame fall damage. This is the REAL
constant-damage path (minimap-red ⇒ `Radar_AddBlip`), separate from the §5.9 0x0A tail-health decrease
detector (cosmetic red/shake only, no `Radar_AddBlip`).

(`Entity_GetMaxHealthWithDifficulty @ 0x43b8a0` and its heal-to-max clamp `sub_43C290 @ 0x43c290` (raise
`Health` up to max, store @0x43c2aa) run on the SPAWN/respawn paths — `Entity_ResetToSpawnState @0x4b9610`,
`PlayerClass_InitEntity @0x4b1060`, `Server_ProcessPlayerDeath @0x517740` (xrefs) — NOT per-frame, so they
are not part of the 0x0A loop.)

### 5.39 First/third-person player camera (Phase 2.5, 2026-06-20)

The moving player's view, witnessed for a faithful first-person camera (the §5.38 player). All
anchored (decompiled this session); read-only, no IDB writes.

**Mode flag `dword_A890C8`** (set by `[orig: Camera_SetTrackedEntity @ 0x4391d0]`; tracked entity =
`dword_A890CC`): **0 = first-person on-foot** (primary), 1 = vehicle/mounted (3P), 3 = spectator,
4 = lerp transition. The original toggles 1P/3P on foot with **F4**.

**First person (mode 0)** `[orig: Camera_ComputeThirdPersonView @ 0x437d10]` — despite the name this
is the master view placement; the mode-0 branch is FP:
- `g_view_pos {x,y,z}` ← entity `Position` (+4/+8/+12); the base rotation
  `g_view_rot {yaw,pitch,roll}` ← entity `Yaw/Pitch/Roll` (+16/+20/+24, 32-bit BAM) `[orig: @ 0x437d92]`.
- **Correction (2026-07-13): the `+0x10000` (+1.0) bump `@ 0x437e8f` is the NON-person leg only**
  (itemDef+0x5C != 3). An on-foot PERSON takes the type-3 leg `@ 0x437f9c`:
  `g_view_pos += CameraOffset(+0x6C)`, `pitch = entPitch + 2·pitchBlend(+0x380)`,
  `roll = torsoRoll(+0x2DC) + leanAngle(+0xB0)/4` (the FP lean tilt; pitch/roll writes
  `@ 0x437fc7/@ 0x437fe6`), and the eye then pulls BACK 0.1875u along the FULL view rotation
  (roll included): `eye += R·(−0x3000, 0, 0)` `@ 0x438001..0x438031` (witnessed 2026-07-13,
  ported as `PLAYER_EYE_PULLBACK`). The local
  player's `CameraOffset` is produced by the body updater's bone path `@ 0x4b6bb3`:
  `Entity_BuildBoneTransformMatrices` → the POSED HEAD BONE world position, floored to the max of
  4 terrain samples (±0x4000 x/y) + 0x1000 unless Flags & 0x800000 `@ 0x4b6c1c`, stored as
  `head − Position`; remote players get a cheaper trig approximation from the capsule height
  (clamped 0xD000) rotated by −lean/body pitch/roll with a 0x2000 z floor `@ 0x4b6984-0x4b6ba5`.
  So the FP eye FOLLOWS THE ANIMATION — stand/crouch/prone/jump and the walk/run bob all move it.

The final 1P view matrix `g_view_matrix @ 0xB764E0` is built by `[orig: Player_UpdateFirstPersonCamera
@ 0x4dd380]` = `g_view_pos`/`g_view_rot` + weapon view-bias + weapon bone offset + clamped velocity
lead + prone Z-drop (−0x500); those refinements are deferred.

**Third person** `[orig: ThirdPersonCamera_Update @ 0x437af0]`: `target = Position + CameraOffset@+0x6C`
(SpecialVec3), plus smoothing + distance `dword_A8910C` + bone collision (deferred).

**Look apply** `[orig: Input_HandleActionBinding_0 @ 0x4e1330]`, `analog` = the sensitivity-scaled
mouse delta: `Yaw ±= analog<<16` (**wraps, no clamp**); `Pitch = clamp(Pitch ± analog<<16,
±954437120)` = **±80°** (turret variant +40° when entity flag 0x100); center-view → `Pitch = 0`.

**Sensitivity / FOV** `[orig: Input_ProcessMouseAxisBindings @ 0x499680]`: `scaled = (raw_delta ·
(dword_24D207C<<11) + 0x8000) >> 16`; `dword_24D207C ∈ [1,511]`; Y inverted unless `dword_24D2078`.
FOV = `dword_A7839C / 65536` degrees (16.16; base `dword_26C6844`, scope-modified)
`[orig: Render_ProcessMainSceneFrame @ 0x5ca0f0 @ 0x5ca601]`.
Completed 2026-07-13: the raw deltas are **center-lock cursor PIXELS per frame**
(`Input_PumpAndCenterCursor @ 0x7616e0` pins the cursor at (320,240) and reads the offset);
the sens setting is profile+0x590, **default 128** (`PlayerProfile_InitDefaults @ 0x54bbc0`),
stepped ±0x10 and clamped [1,0x1FF] by the `mousescale` adjust (`case 17 @ 0x49b18b`); the
invert is profile+0x594 (`flipmouse` toggles it, case 9 @ 0x49afbb; default OFF = push-forward
looks up). **The scoped reduction** `@ 0x499706-0x499714`: `sens /= Player_GetClampedWeaponElevation()
@ 0x4dc6b0` — the CURRENT zoom level (slot+0xC, seeded from and clamped to the def's
`scope_max_mag` @ +0x90) — while `CanFire && (weapon scoped || vehicle gunner scoped) &&
!g_binocularsViewActive (0xB76538)`. Apply: `yaw −= scaledX << 16` (case 166 @ 0x4e109d,
mouse-right = heading negative); `pitch += scaledY << 16` (case 164 @ 0x4e0fed) clamped ±80°
with the **up-limit +40° while PRONE** (`MoveOrder & 0x100` @ 0x4e0ff7 — not a turret variant);
AbsorbPitch (0x10000) weapons route Y into the turret elevation accumulator instead
(`dword_B79008 @ 0x4e0fe2`). Ported: `libs/world/player_look.{h,cpp}` +
`NovaSimulation::add_local_player_look` (sim-owned look state; the host feeds raw pixels).

**OpenNova port (Phase 2.5).** The host first-person camera places the `Camera3D` at the player's
Godot position + 1.0u eye, oriented by the player's authoritative Yaw/Pitch
(`NovaSimulation::get_local_player_yaw_deg`/`get_local_player_pitch_deg`); **F4** swaps to a
behind+above third person; the mouse drives Yaw + Pitch (clamped ±80°). The "AI in the ground" symptom
was a two-store bug (the motor's grounded `AiEntity.pos` was mirrored to the registry `Entity` only
for the local player) — now every motor entity mirrors. **Tracked deferrals:** 3P follow
smoothing/collision (`@0x437af0`), the weapon view-bias/bone/velocity-lead/prone-drop (`@0x4dd380`),
the exact `CameraOffset@+0x6C`, and the FOV source. (The FP arms viewmodel placement is now §5.40.)

**2026-07-08 addendum (controller grill) — the 3P deferrals witnessed.** All camera-system globals
renamed in the IDB this session (`g_camera_*`; world-wac-ai-re §14.7 lists them). Corrections to the
2026-06-20 rows: the ±40° pitch clamp variant keys on **`MoveOrder & 0x100`** (+0x12c), not entity
`Flags`; `Input_HandleActionBinding_0`'s function start is `0x4e0420` (0x4e1330 is an inner site);
`Camera_SetTrackedEntity` takes **(entity, mode)** — the old 1-arg type hid the mode param.

- **Follow anchor** `[orig: ThirdPersonCamera_Update @ 0x437af0]`, called per 62 Hz tick from
  `Game_ProcessMainFrame @ 0x5263f0`: on foot `anchor_target = Position + CameraOffset(+0x6c)`;
  seats 2/5 use the **parent vehicle's** Position with Z + max(1.0, 0.375·boundRadius). Anchor eases
  ¼-step per tick on foot, 1/16 (xy) + 1/32 (z) seated; a (pos − savedLivePose)<<8 velocity-lead
  pair smooths 1/32. View bits 0x10/0x40 (`dword_B3B738`) orbit yaw ±0x1000000 (≈1.4°)/tick. Dead
  tracked entity: chase distance auto-reels >7.0 → −1.0/tick, then 1/16-step to exactly 3.0.
- **Eye/orientation** `[orig: Camera_ComputeThirdPersonView @ 0x437d10]` mode 1: on foot
  `eye = anchor + R(entYaw+orbitYaw, entPitch+orbitPitch)·(−dist,0,0)` with defaults dist=3.0,
  orbit pitch=0x4000000 (**5.625°** — corrected 2026-07-13; the earlier 22.5° was a 4× BAM
  misconversion) (reset in `Camera_SetTrackedEntity @ 0x4391d0`); orbit-pitch keys
  ±0x800000 (actions 407/408). Seats 2/5: yaw = vehYaw + (lookYaw−vehYaw)/4, pitch fixed −0x8000000
  (11.25° down), **dist = 1.0 + 1.5·boundRadius**; plus ground-slope follow (max ground slope
  toward the anchor × 0.333 raises the eye), a smoothed (1/32) look-ahead point 6.0u along the
  vehicle's `orientationMatrix(+0xb4)`, and model types 3/4 (helo/plane byte @ model+406) drop the
  eye by boundRadius/2. Final rotation = atan2 look-at from eye to anchor(+R·(0.125,0.125,0.125)),
  roll 0 (+ explosion-shake sway from `dword_B764B0` filtered `Env_WeatherPrng` noise).
- **Collision** `[orig: Camera_RaycastCollisionOffset @ 0x4378b0]` + inline in the view fn
  `@ 0x438213..0x43832e`: 0.25u-step march along the offset ray (skipped when dist ≥ 8.0),
  stepIndex 1..numSteps−1 with numSteps = dist>>14 — **the no-collision landing is the LAST
  step, (numSteps−1)·0.25, never the full distance** (witnessed 2026-07-13; at the reset
  dist 1.0 the on-foot camera sits 0.75u behind the nudged pivot — ported as
  `tp_effective_distance`; numSteps ≤ 1 skips the march, eye = pivot), bone-collision force
  (`Entity_ComputeCollisionForceFromBones @ 0x4afff0`) against a per-entity collider list at
  entity+0x1bc (ptr)/+0x1c0 (count) filtered to defless/type-5 entities, radius 0.25; terrain
  clearance via `Terrain_SampleHeightBilinear @ 0x6067b0`; eye clamped ≥ water+0.25 (when entity
  above water) and ≥ ground+0.25 (unless entity flag 0x800000). Min pull-in 0.25.
- **View actions** `[orig: Input_HandleActionBinding @ 0x49ad40, cases @ 0x49c073]`: 400 = first
  person (view bit 0x4000000), 401 = cockpit (0x10000000), 402 = third person (0x8000000 +
  `g_camera_third_person_selected=1`), 412 = toggle FP→3P→cockpit, 405/406 orbit yaw keys, 407/408
  orbit pitch. The view bits are the same `dword_B3B738` triple the BMS runtime doc lists.
- **Mode arbiter** (per frame, `[orig: Render_ProcessMainSceneFrame @ 0x5ca1d2..0x5ca267]`):
  desired = 0; 3P-selected && parentSlot∈{2,5} → 1; death screen sub-mode 1 → 1, sub-mode 2 →
  0 **with tracking swapped to the killer** (kill-cam POV, `@ 0x4391f7`); dead or
  awaiting-spawn-on-foot → 4 (overview lerp over 128 ticks, from/to in `g_camera_lerp_*`) unless
  option `dword_24D1E34` bit 1; in-session + `dword_24D1E34 & 0x40` → forced 0 (the server
  "force first person" rule). Session setting +0x5CC==2 → `g_cfg_default_camera_mode=1`
  (`[orig: apply_session_settings_to_globals @ 0x5521a8]`), applied via
  `Camera_ResetToLocalPlayer @ 0x4a3d30`. **Resolved 2026-07-13:** stock 1.7.5.7 has NO on-foot
  third person — the arbiter only selects mode 1 when 3P-selected AND parentSlot ∈ {2,5}.
  The onhook debug tool (opennova-int `debug/camera_control.c`) patches exactly this block
  (its `PATTERN_CAMERA_DEFAULT_MODE` bytes are the `@ 0x5ca1d2` sequence) plus the mode-1 HUD
  mount gate to force on-foot 3P; the on-foot chase math itself is fully present (above).
  Our F4 toggle is that same debug affordance, not stock behavior.
- **FP mounted refinements** (mode 0): seat-bone eye (`Entity_GetBoneWorldPosition @ 0x545e60`)
  when the parent def sets +84 bit 0x20 (and not 0x40); a per-model camera callback (vtable +372)
  for cockpit-type parents; itemDef type-3 entities add `CameraOffset` to the eye with
  pitch += 2·pitchBlend and roll = torsoRoll + lean/4 (the FP lean tilt).

**Port (2026-07-08 controller train; corrected 2026-07-13 ×2):** `local_player_presenter.gd` uses
the IN-PLAY chase state — distance **1.0**, orbit yaw/pitch **0**, the round-start reset
(`Camera_ResetToLocalPlayer @ 0x4a3d30` ← `Game_StartMission @ 0x525c54` /
`Game_InitNewRound @ 0x4227a2`; the tight over-the-shoulder view). The 3.0 / 5.625°
`Camera_SetTrackedEntity` pair (the 07-08 pass also carried a 22.5° misconversion of the
0x4000000 seed) applies only on tracked-entity CHANGES — kill-cam/spectate retargets, incl.
a respawn retarget (no ResetToLocalPlayer runs then; only round/mission starts call it).
Zoom keys (view actions 409/410): `dist −/+= max(dist>>6, 0x800)` clamped [0.5, 512]
`@ 0x49c1c5..0x49c23f`. Anchor: quarter-step toward the HOST-SAMPLED head-bone eye
(Position + CameraOffset `@ 0x437b70`; the sim receives the sample per frame via
`set_local_player_eye`); orientation = the seed angles (equal to the witnessed look-at
while the collision march is unported); the third-person body renders with the §14 aim
overlay (world-wac-ai-re §14.6, D-INF-11 partial). Still deferred: the collision march,
orbit/zoom keys, the 0.125u look-at nudge, vehicle mode-1 (no local mounting), the
kill-cam distance reel, the weather/impact shake.

**2026-07-13 addendum (the controller-parity pass) — lean, stance keys, input bits, the eye.**

- **The lean-angle producer (entity+0xB0, BAM32), witnessed end to end** — the old audit's
  "@0x483fe0/@0x46e100" guess was wrong; every writer lives in the body updater:
  decay `lean −= (lean+8)>>4` EVERY body tick `@ 0x4b5c97` (before the weapon-channel block);
  the on-foot ramp `@ 0x4b7dbf/@ 0x4b7dd6` — MoveOrder bit 6 (left) −0x3000000/tick, bit 7
  (right) +0x3000000/tick, gated `!(Flags & 0x100020)`, not prone, and `!(Flags & 2)`; a seated
  (`+0x168 == 1`) ±0x1400000 variant `@ 0x4b66b5/@ 0x4b66c3`. Ramp-vs-decay equilibrium
  ≈ ±0x30000000 (67.5°). **The ORDER within that one body pass is load-bearing**: decay runs
  BEFORE the ramp, so a held key settles at ±0x30000000; ramp-first would settle at
  ≈ ±0x2D000000. The remote/decoded integrator `netsim::NetClientView::tick_lean` folds the same
  two `move_input` bits for wire peers and was corrected to that order 2026-07-25 (its gate legs
  stay unmodeled — the decoded row carries no honest stance/seat state; D-INF-17). Consumers:
  the FP camera roll (`torsoRoll + lean/4` @ 0x437fe6 — torsoRoll's own producer
  `@ 0x4b5cff..6d` + `@ 0x4b700c..25` is witnessed+ported 2026-07-13: chase of the slope
  roll with the ±20° LAG clamp; prone idle 48 decays to level; the combat rolls 41/42 RAMP
  it −/+0x4000000 (5.625°) per tick — the FP barrel-roll view, snapped back by the clamp
  when the clip ends), the §14 aim-overlay lean term, the remote CameraOffset trig (reads
  −lean `@ 0x4b699f`), and the prone roll anims 41/42.
- **`g_inputFlags` bit map (the key handlers `Input_HandleActionBinding_0 @ 0x4e0420`)**:
  0x2 forward / 0x4 back / 0x8 strafe-left / 0x10 strafe-right (cases 152/151/156/157);
  0x20 look-up (155) / 0x40 look-down; 0x100/0x200 keyboard turn L/R (158/159); 0x1000 jump
  (153); **0x2000 lean-left (case 148, catalog id 6 Q) / 0x4000 lean-right (case 147, id 7 E)**;
  0x8000 fire. The packer `@ 0x4df68f-0x4df79a` maps them onto MoveOrder: dir index | 8·moving,
  jump → 0x20, fire → 0x10, lean → **0x40/0x80**, keyboard-turn → 0x1000/0x2000,
  look-up/down → 0x4000/0x8000, prone/crouch latches (`dword_B76484/dword_B76480`) → 0x100/0x200.
- **Stance keys are a 3-key SELECT, not toggles** (catalog ids 9/10/11 = Z prone, X crouch,
  C stand): cases 169/170/172 `@ 0x4e0d77/@ 0x4e0df3/@ 0x4e0e3e` send **C2S 0x1D** with the
  action id (0xA9/0xAA/0xAC), refused while the equipped def has ForceCrouch (0x40000) or in a
  seat-kind-3 mount; the local player's own stance rides the same loopback. The apply
  (`NapiNPServerMsg_HandleStanceChange @ 0x501c60`) is pure SELECT with mutual exclusion:
  169 → crouch (0x200, prone cleared), 170 → prone (0x100, crouch cleared), 172 → clear both;
  local latches `dword_B76484` (prone) / `dword_B76480` (crouch). The function's old header
  comment mislabeled 170/172 (corrected in the IDB 2026-07-13).
- **`byte_B76538` identified** → renamed `g_binocularsViewActive`: the per-frame effective
  binocular-VIEW flag (`Player_UpdatePerFrame @ 0x4de382` copies `g_binocularsToggle`, forced 0
  when dead / spawn-gated / any move key (`g_inputFlags & 0x1E`) / camera mode 1). It gates the
  crosshair, the FP-model draw, several HUD overlays, and the scoped mouse reduction.
- **Ported this pass** (libs/world + NovaSimulation + LocalPlayerPresenter): the shared
  `player_body_select` (run promotion + idle_mortar + prone rolls + the 4th-tick cadence for the
  LOCAL player too), `infantry_lean_tick`, the primary channel's clip-end pending promotion
  [orig: @ 0x40b77b], the full mouse pipeline (`player_look_apply`), stance SELECT requests with
  the ForceCrouch refusal, the head-bone eye (host-side skeleton sampling — the structural
  translation of the @ 0x4b6bb3 bone path), the FP lean roll (lean/4), and weapon.def `run_anim`
  parsing through the def → database → sim seam. The lean bits ride the wire byte 19 (bits 6/7)
  both directions. **Unported tails**: the seated lean ramp variant, the Flags 0x20/0x100000
  ramp gates, the 4-sample terrain eye clamp + the remote trig CameraOffset, torsoRoll(+0x2DC)
  and pitchBlend(+0x380) camera terms, the zoom-adjust keys (slot+0xC beyond the scope_max_mag
  seed), analog axes, and keyboard look/turn keys.
- **Binoculars/NVG ported 2026-07-21.** Binoculars is input action 26 (catalog id 103,
  default B), not action 220: the raw request (0xB76539) survives movement/death/round/3P
  suppression; raised pose (0xB7653A) survives 3P; effective optics (0xB76538) is FP-only,
  fixes horizontal FOV at 20 degrees, hides the FP model/crosshair/SIGHTS card, disables scoped
  mouse reduction and weapon/category/cycle input, and adds the persistent 0x02000000-BAM
  random aim displacement. The hosted HUD loads `Binoculr.tga`, `BinoCH.tga`, and
  `BNumbers.tga` from VFS and ports the exact four-digit range easing. NVG is action 41
  (catalog id 104, default N), independent of mission `EnableNVG`; actions 56/57 (OEM +/−)
  clamp gain 0..4 even while off. `StartWithNVGOn 0x400000` reseeds every player init,
  first-person-only environment gain uses the exact hemisphere formula, Inset sights drop/
  restore through the normal scope toggle, and the post/mask/scale presentation is hosted.
  Binocular activation also refuses while the PowerThrow fire-charge tick is live, preserving
  the held windup instead of converting optics input suppression into a release. Bounded
  residuals: the NVG post collapses the retail four-frame temporal history to the current
  frame, the NVG style-8 laser and raw-active death-screen exception remain unported, and
  Binoculars still lacks its capture-point detail overlay.

**§5.40 viewmodel correction (2026-07-08, same train):** the FP viewmodel hardcode named a
model that does not exist in the JO assets ("AKM_1st"), so the gun never loaded and the arms
had no skeleton — an invisible viewmodel with every failure silent. Fixed to the witnessed
WPN_AK47AUTO def names (`animadm ak47_1st`, `gfx1 ak47_1st`, `gfx1a armsG`, `gfx1b armsGb`
unused), load failures now warn, `pos` corrected to the def line (−19.46, 21.19, −161.31)
(the old (10, 0, −201) matched no def line), and the facing derives from
`bms_to_godot_basis` on the camera's engine orientation (model space == view space
`[orig: Player_RenderFirstPersonViewModel @ 0x4ded60]`) instead of a hand-tuned rotation.
**Open (the §5.40 refinement grill):** the FP rig is its OWN pre-posed skeleton —
`ak47_RST.bad` is 39 bones (BN01 Pelvis, arms, 26 fingers, gun bones; BN## tags do NOT match
the body rig) — and the rendered hold pose is still visibly mis-framed. Pinning the rig's
authored frame + the view-bias chain (and D-RORD-4's near-Z depth trick) is the follow-up;
the arms/gun at least load, render, and warn on failure now.

**§5.40 FP rig runtime semantics — RESOLVED (2026-07-09 grill).** The mis-framed hold traced to
three stacked misreadings, each now witnessed and ported:

1. **Bone pivots come from the MODEL, not the `.bad`.** Every composed bone-matrix builder reads
   per-bone rest positions from the model's bone table (`modelDef+56`, stride 64, pivot float3
   @+0x24 — the runtime form of the .3di ROBJ chunk); `BadBone.position` is never consumed at
   runtime. That field is a lossy export — 257/477 retail `.bad`s triplicate X into all three
   slots (ak47_RST 100%) — and the engine simply doesn't care.
   `[orig: BoneAnim_BuildWorldMatrices @0x40c400 (FP), build_world_bone_matrices @0x40c770
   (world, fixed-point pivots @+56/60/64 of the 108-byte entity bone table).]`
2. **Channels are BIND-RELATIVE deltas, and the bind is a pure translation.** The shared channel
   evaluator multiplies `Transpose(bind 3x3) × sampled channel matrix` per bone — the stored bind
   3x3 (the 100-byte `.bad` bone record) is only the zero-reference the channels are measured
   against, and it is stored TRANSPOSED relative to the channel quaternions (verified on
   ak47_RST: `mat3(bind) × channel ≈ identity` through reset AND idle; the reload clip carries
   real 139° mid-clip motion). The composed builders then apply `T(−pivot) · delta` with a
   translation-only hierarchy — the skinning bind-inverse is a translation, bind world rotations
   are identity by construction, and the authored mesh renders verbatim at the reset clip no
   matter how degenerate the stored bind matrices look in isolation.
   `[orig: BoneAnim_TransformBones @0x410360 (slerp → matB scratch) →
   AnimChannel_ComputeBoneMatrices @0x410da0 (Transpose(state bind) × matB; state = the .bad
   bone records at boneData+24) → @0x40c400/@0x40c770 (T(−p), pivot chain via the parent's FULL
   matrix + channel translation).]`
3. **The FP rig is authored IN VIEW SPACE and both builders' X-negation is a frame map, not
   posing.** ak47_1st/armsG live in x = downrange (muzzle at +1.5, the arms' upper ends at −0.9
   behind the eye plane), y = up (content ~0.6 above the waist origin — cancelling the def `pos`
   −0.63 drop to land at eye level), z = lateral. The `S·Aᵀ·S` copy loops + x-negated
   pivots/translations in both builders are the engine's model→render frame conversion — the
   same job our world pipeline does with the (−x,y,z) mesh import flip.
   `[orig: the copy loops @0x40c4d8..0x40c57c / @0x40c84c..0x40c8f5; pivot negate @0x40c953.]`

**Port** (PR #213): `libs/anim sample_clip(…, model_bind)` implements (2) natively (bind-relative
deltas, identity rest, model pivots via `NovaObjectData.get_bone_origins`); the FP meshes build in
the NATIVE frame (`build_lod_submeshes(…, native_frame)` — no import flip, winding re-reversed for
Godot's CCW cull) so mesh, skeleton, and Skin (`T(−abs pivot)` from identity rests) share one frame;
`LocalPlayerPresenter` maps rig→camera with yaw +90 plus the witnessed per-weapon biases (`pos`/256 in
view axes; `rot` degrees added about the eye `[orig: Player_UpdateFirstPersonCamera @0x4dd444]`).
Verified: reset/idle identity oracle in ctest (`anim_sample`) + on-asset probes
(`godot/tests/fp_clean_probe.gd`, `vm_mesh_probe.gd`). **Still open:** the dedicated FP render pass
(weapon `renderfov` @Def+0x148, near-Z 0.05 swap + viewport depth [0, 0.1] — D-RORD-4) which gives
retail its close-up framing; def `rot` bias sign confirmation against retail footage; the delta
sense final pin (D-INF-14); body-rig unification onto the same semantics (D-INF-13).

**§5.40 bind-source correction — the T-pose freeze (2026-07-09 grill, second pass).** Point (2)
above misread WHOSE bind the channels compose against, and the port froze the FP rig at its
authored T-pose (idle composed to identity → the mesh rendered verbatim: splayed arms, gun parked
on the outstretched right hand). The witnessed mechanism:

1. **The bind operand is the `channel+44` override, pinned once to the `.adm` slot-0 `.bad`.**
   `AnimChannel_ComputeBoneMatrices @0x410da0` resolves its bone records as
   `boneData = *(channel+44) ? *(channel+44) : *(channel+0)` (`@0x410dd8`/`@0x410de3`).
   `AnimMap_RegisterEntity @0x40bb60` writes `channel+44` exactly once at entity registration —
   to the slot-0 node's `.bad` (`@0x40bbe3`), i.e. the `.adm`'s reset/skeleton animation
   (ak47_RST). Clip switches re-init only the playing channel: `AnimMap_PlayAnimBySlot @0x40bda0`
   and `AnimMap_UpdateEntity @0x40b5f0` write `channel+0..+12` and the slot bookkeeping, never
   `+44`. The null-override fallback (compose against the playing clip's own records) is real but
   reaches only standalone channels (the menu profile preview's global channel `@0x560cde`).
2. **Consequence:** every clip of a rig is measured against the ONE skeleton bind. A clip composed
   against its *own* bind self-cancels at its start frame by construction — that was the
   2026-07-08 oracle's blind spot: `mat3(stored bind) × channel ≈ I` at reset AND idle is true
   *per-file* (each `.bad`'s channels start at its own bind) but says nothing about the runtime
   pairing. Against the skeleton bind, `anim_wpn_idle` composes to the HOLD (~180° bone rotations
   folding the T-pose arms onto the weapon), and the reset clip still composes to identity.
3. **The rig, re-read:** ak47_1st is a 39-bone T-posed character skeleton (BN01 Pelvis at the
   origin, R/L arm chains along ±X, 26 finger bones, gun bones) with a 39-entry model part table
   matching bones 1:1 (part[1] rel x 0.1564 ↔ "BN02 R UpperArm"); armsG is the same skeleton's
   skin with a 38-part table of its own (unused — the placer passes the GUN's origins). The wpn
   clips pose it into the view hold facing +Z model space; the §5.40 "authored in view space"
   reading described the T-pose bind, not the runtime hold.
4. **`.bad` positions, closed:** retail derives `BadBone.position` at export
   (≈ `inv(parent bind world) ⊗ swizzled model rel` — maintainer's relation; the EXACT form is
   pinned in the model-table correction below: `F_parent⁻¹ · P · d` with `F` = the Max bone
   frame, not the bind) and never reads it at runtime; pivots come
   from the model bone table (`modelDef+56` rel float3 @+0x24, parent @+20, stride 64
   `[orig: BoneAnim_BuildWorldMatrices @0x40c400, pivot reads @0x40c5f2]`). Pipelines that consume
   `BadBone.position` directly (the pre-repo oscarmike port) work exactly on the healthy subset
   and break on the triplicated one; the model-pivot path works on both — matching retail.

**Port correction** (same PR): `sample_clip` gained a `bind_source` parameter (`nullptr` = the
faithful no-override fallback); `NovaSkeletalAnim` keeps the parsed reset `.bad` alive through
Pass 2 and passes it for every clip; the `NOVA_VM_DELTA` experiment knob is deleted — composition
is `q(stored skeleton bind) ⊗ channel`, operand order pinned visually on the ak47 rig (the
conjugate order collapses the rig; oscarmike's `pose × inverse-bind` shape differs legitimately
because its Skeleton3D rest carries the bind rotations, ours bakes the whole composition into the
pose over identity rests). The rig→camera container map corrected from yaw +90 to the yaw-180
Z-flip (model forward +Z → camera forward −Z, Y up; the +90 was tuned against the misread pose).
Verified: `anim_sample` ctest (self-bind fallback + skeleton-bind override cases) +
`fp_clean_probe` captures on 05TR — both camo arms gripping the AK, mag hanging −Y, muzzle
downrange. **Same train, the FP render pass (D-RORD-4) ported:** the viewmodel now composites
through a dedicated shared-world SubViewport whose camera draws only the viewmodel layer at the
weapon `renderfov` — HORIZONTAL degrees converted to vertical through the live aspect, with the
witnessed near-plane swap; JO's weapon.def never sets the key, so every weapon renders at the
record default 80.0 `[orig: fov read @0x4dee71 → h→v @0x58d900; near 0.05 swap @0x4dee29 /
restore 0.2 @0x4df0aa; depth remap @0x58a7b0; default flt_7D1898=80.0 @0x53ff31; parser key
'renderfov' @0x54482a]`. A four-way container-yaw sweep (`fp_clean_probe` `NOVA_VM_SWEEP=1`)
confirmed yaw-180 is the only map that places the rig in frame — 0 puts it behind the eye,
±90 off-frame laterally. The stored ak47_RST binds are all proper rotations (det +1 across the
39 bones), so the quat composition is exact — no reflection caveat. **Still open:** def `rot`
bias signs + reload direction vs retail footage (the D-INF-14 tail); left-hand/finger pose
fidelity vs retail footage (retail's hip idle is a low-ready — compare before judging); per-weapon
`renderfov`/`pos`/`tpos` def plumbing *(landed — the fifth pass below)*; D-INF-13 (bodies onto
model_bind — the part↔bone question
is resolved by the model-table correction below: rows pair by index, no matcher needed).

**§5.40 model-table correction — the rig IS the model's bone table (2026-07-09 grill, third
pass).** Point (4) above said "pivots come from the model bone table"; the data + a re-read of
`@0x40c400` show the table's role is total, and the port still keyed the rig off the `.bad` with
a silent `BadBone.position` fallback that point (4) should have killed:

1. **Corpus proof that `BadBone.position` is dead data.** Sweeping every `*1st.3di + .adm` pair:
   **12 of 43 JO (JOX) viewmodel rigs ship broken positions** — the recurring signature is 24
   zeroed bones + ~14 stale values (ak47, M4, M4GL, 357, 47GL, 74GL, AK74, PKM, RPG7, SR25, L115,
   Mach, Mort, Dgnv, FM92) — and retail renders every one correctly. The REVVY-SKU `ak47_RST.bad`
   is **byte-identical** to JO's (`max|Δpos| = 0`, same rot/length), so "healthy vs broken" is a
   per-rig export accident shipped unchanged across SKUs, not a data revision. Several rigs flip
   health across SKUs from different export batches (M4/PKM/RPG7/SR25 broken in JO, healthy in
   REVVY; G17 the reverse).
2. **The witnessed rig source (`@0x40c400`, re-read).** The FK loop is bounded by `modelDef+52`
   — the MODEL's row count, never the `.bad`'s — and takes BOTH the parent index (row `+0x14`)
   and the float pivot (row `+0x24`) from the `modelDef+56` table. The `.bad` contributes
   rotations only, channel *i* → row *i* by index. When the anim has FEWER bones than the model,
   a padding loop (`@0x40c5a1`) pre-fills the extra rows with bone 0's composed matrix; when it
   has MORE (AKM_1st: 46 bones, 45 parts), the surplus channels are simply never read. On flag-2
   (translated) clips the padded rows sum an UNINITIALIZED `bone_translations` stack slot
   (`@0x40c6e9..0x40c71d`) — ported as zero, ledgered **D-INF-15** (class D, ADR 0003).
3. **The export relation, closed exactly.** On healthy rigs,
   `BadBone.position = F_parent⁻¹ · P · d` **exactly** (residuals ≤ 6e-5 on AKM_1st, M4AC_1st,
   m4_1st — M4AC/m4 share identical solved frames, i.e. one skeleton, swappable attachment
   models), where `d` = the model-space pivot offset parent→child, `P` = the y↔z SWAP (an
   improper axis exchange, det −1), and `F_parent` = the parent's **3ds Max bone frame**
   (X-down-the-bone; root = identity; mirrored L/R). `F` matches NEITHER the stored bind 3×3 nor
   the frame-0 channel under any constant conjugation (48-perm × 48-perm × transpose sweeps) nor
   any accumulated relative-channel chain — it is DCC-side authoring data lost at export, which
   is exactly why the field can rot without anyone noticing: **`BadBone.position` is not
   reconstructible from shipped data and nothing at runtime wants it.** *(Superseded — the
   position-derivation correction below: that sweep's frame-matching metric false-negatived;
   `F` IS the bind under one constant map and the field is reconstructible.)* (`BadBone.length`
   is the Max bone length — equals the child distance on chain bones only.) The stored bind 3×3
   ≡ the RST frame-0 channel transposed, confirming bind = reset-pose channel in matrix form.
4. **Port.** `sample_clip` gained `model_parents` (paired with the origins): in model-table mode
   the rig's count/hierarchy/pivots come from the model, rows past the `.bad`'s channels take row
   0's composed rotation, self-parent roots normalize to −1, and the equality-gated fallback to
   `BadBone.position` is gone from the runtime path (it survives only as the no-model
   menu-preview fallback, the original's own `@0x410de3` shape). `NovaObjectData.get_bone_parents`
   exposes the table; `NovaSkeletalAnim`/the placer pass origins+parents (the FP arms still use
   the GUN's table). Verified: `anim_sample` ctest (model-table count/hierarchy/pivots, bone-0
   padding, AKM-shape shrink, garbage-`.bad`-pos immunity) + `vm_mesh_probe` on REVVY — ak47
   (broken pos, 39=39) and AKM_1st (healthy pos, 46 bones/45 parts, previously reachable only
   through the fallback) both rest exactly on their model pivots, compose reset→identity, and
   pose the idle hold (barrel → +Z view space).
5. **For D-INF-13 (bodies):** the world builder `@0x40c770` has the same composed math but its
   own table shape — count `skeletonData+104`, 108-byte rows at `skeletonData+108`, parent
   `@+40`, pivots 16.16 fixed `@+56/60/64` consumed in (z,x,y) order with x negated, bind-inverse
   `T(−parent pivot)`, and NO bone-0 padding loop. Bodies close by porting that table, not by
   inventing a part↔bone matcher.

**§5.40 position-derivation correction — `BadBone.position` IS reconstructible from shipped
data (2026-07-09 derivation experiment, fourth pass).** Point (3) above closed the export
relation through a per-parent solved Max frame `F` and declared `F` unrecoverable. Re-solving
with a *position-residual* metric instead of frame matching refutes the unrecoverability half:

1. **The relation, restated without the DCC detour:**
   `BadBone.position[i] = bind_rows[parent(i)] · (−d.x, d.y, d.z)` — the parent bone's stored
   bind 3×3 (row-major as parsed; ≡ the reset frame-0 channel transposed, numerically identical
   residuals either way) applied to the child's x-negated model `rel` pivot, the engine's own
   model→render negation (the same map the composed builders apply — pivot negate `@0x40c953`).
   Equivalently `F_parent = L·bindᵀ` with the single constant signed perm `L = (−x, z, y)`;
   folded against point (3)'s `P`, `L·P = diag(−1,1,1)` — the mystery "Max bone frame" was the
   bind all along under one constant two-sided map.
2. **Why the third-pass sweep false-negatived.** It matched solved *frames*: (a) the only
   two-child solvable parent (the gun-assembly bone) is det-ambiguous at k=2 — the solver's
   arbitrary improper branch sits 90° from the true frame while fitting positions exactly;
   (b) position prediction is blind to frame error about the bone axis, so frame matching
   demands agreement the data never pins (every single-child chain). Under the
   position-residual metric the constant map is unique — the runner-up (L, R, transpose)
   combo is 37× worse.
3. **Corpus validation** (94 rigs: 42 JOX + 52 REVX02-archive `*_1st` model+`.adm` pairs;
   M4AC_1st shares m4's skeleton; 26 scrambled REVX02 `.adm`s resolved via the
   `<stem>_RST.bad` convention; FM92/uzi lack loose RSTs). Of 2753 norm-consistent bones,
   2463 reconstruct within 2e-4 and 61 more within 5e-4 (the float32 floor). Every larger
   deviation clusters by bone-name × export batch — JOX: the shared `BN38 BONE` gun bone on
   12 rigs (≤7.8e-3); REVX02: the LEFT-hand finger chain on exactly the 8-rig MG/shotgun
   batch (G36/m60/P90/R870/M240/M249/PKM/RPK, ≤1e-2) plus AKM_1st's L-forearm/L-hand pair
   (1.4e-3) — pos/bind pairs exported from different rig states (stale), the same per-batch
   export rot as the broken-12 catalog above, not rule failures. Negative control: on the
   X-triplicated rigs (JOX ak47/M4) the reconstruction disagrees with 38/38 broken bones and
   leaks 0 healthy ones. The per-parent orthogonal solve reproduced the third pass first
   (≤1.02e-4 across the four solvable parents) before any rule work.
4. **Port.** `bad_positions_from_model` in `pyopennova/bad_build.py` (byte-identical mirror
   in `blender/opennova/bad_build.py`): index-paired to the model part table, root bones
   (parent < 0/self) take the x-negated rel unrotated (zero on every shipped rig), surplus
   `.bad` bones past the part count excluded by construction. Evidence: pytest
   `tests/test_bad_pos_derivation.py` — synthetic exactness runs unconditionally;
   `OPENNOVA_JO_ASSETS`-gated legs reconstruct M16/M24/M21/Frag within 5e-4 per bone and
   prove the ak47 triplication control (see docs/asset-gated-tests.md).

Consequence: DCC import of the broken-12 no longer depends on the shipped field — armatures
can derive it from the model + bind; anything that consumes `BadBone.position` (the pre-repo
oscarmike path, our exporters' round-trips) has a corpus-exact reconstruction. The runtime is
untouched — the model-pivot path above remains the witnessed-faithful rig source.

**§5.40 per-weapon def plumbing + position-source unification (2026-07-09, fifth pass).**
Two loose ends of the series closed together, validated end to end on both SKUs:

1. **The viewmodel def slice is data-driven.** `libs/def` parses `renderfov` (record
   default **80.0** seeded per weapon block `[orig: AdmDef_InitEntryDefaults @ 0x53ff31;
   parser key @ 0x54482a]`; REVX-era defs only ever comment the key out, reaffirming the
   default-80 witness), `NovaWeaponDatabase` exposes the viewmodel slice —
   `animadm`/`gfx1`/`gfx1a`/`gfx1b`/`gfx3`, the `pos`/`tpos` rows (xyz raw units +
   yaw/pitch/roll degrees), `renderfov` — plus a case-insensitive `find_weapon`
   `[orig: WeaponDef_ParseProperty @ 0x54d730; pos/tpos handlers @ 0x54476b/@ 0x54471f]`.
   `GameWorld.build_local_player_viewmodel` resolves the fixed default weapon
   (`WPN_AK47AUTO`; `NOVA_VM_WEAPON` overrides the name for rig A/B checks) from the
   MOUNTED root's weapon.def and `LocalPlayerPresenter` applies the resolved
   `pos`/`rot`/`tpos`/`renderfov`; the witnessed JOX AK-47 constants survive only as the
   no-def fallback. Equipped-weapon resolution (the def per the player's actual weapon)
   remains the follow-up — the plumbing no longer cares which weapon it is.
2. **The `(10, 0, −201)` archaeology, closed.** The pre-2026-07-08 hardcode that "matched
   no def line" is the REVX-era `WPN_AK47AUTO` `pos` row verbatim — and in that SKU the
   AK-47's viewmodel IS `AKM_1st` (`ANIMADM AKM_1ST`, `GFX1 AKM_1st`, `GFX1A ARMSG`,
   `pos 10.0 0.0 -201.0 / rot 0 0 1`). Both old constants (the model name and the offset)
   came from the same REVX def; neither matched JO because JO re-modeled the rig
   (`ak47_1st`, `pos −19.46 21.19 −161.31 / rot 5 3.75 353`). One weapon name resolving
   per-SKU data is exactly the original's shape.
3. **`BadBone.position` has no remaining preview/runtime consumer.** The ONED object
   preview's Anims workflow now binds the `.adm`'s MODEL bone table — the .adm basename's
   `.3di`, falling back to the open model; the FP arms therefore ride the gun's table, the
   same rule as `build_model_from_graphic` — with the bind-relative channel semantics and
   native-frame meshes; the net-replay model resolver passes model origins like the mission
   placer's body path. The only reader left anywhere is `NovaSkeletalAnim`'s no-model
   fallback, the original's own no-override shape `[orig: @ 0x410de3]`.
4. **Historical evidence (before ADR 0025 retired PIE).** The then-current Play-in-Editor
   on both SKUs exercised the def path: REVX root (00TRa) draws the AKM_1st/ARMSG composed
   hold, JOX root (05TR) the ak47_1st/armsG one. Preview↔lib
   numeric parity: the posed AKM idle (the 46-channel/45-row rig) dumps 0/45 joint
   mismatches against the plain-file evaluator, and the ak47 preview's canonical-camera
   idle matches `vm_mesh_probe`'s j37 discriminator (barrel → +Z, belly → −Y). A US01 body
   previews through the same semantics. ctest `def_parse_weapons` pins the renderfov
   default + override; GUT `anims_inspector`/`local_player_presenter`/`game_world`/
   `object_editor` green in isolation. The drive surface at the time was curated ONED MCP tools
   (`object_load_anims`/`object_play_clip`/`object_rig_state` rest+posed joint dumps;
   `mission_play` for the now-retired Play-in-Editor path). Current live mission validation
   launches the exact saved loose `.bms` in the standalone game with F6 and attaches runtime
   MCP to that child.
5. **Still open** (unchanged): the D-INF-14 tail — def `rot` bias signs + reload direction
   + left-hand/finger pose vs retail footage; the `pos`→`tpos` ADS swap (value plumbed,
   swap unwired) *(landed — the §5.62 FSM/ADS pass)*; velocity lead + prone drop; D-INF-13 (bodies onto the world table
   @ 0x40c770); D-INF-15.

**§5.40 frame correction — rig positions COMPUTED from the model; the native+container
realization was X-mirrored (2026-07-09, sixth pass).** The model-table port's Godot
realization (native-frame meshes + identity rests + the yaw-180 container) rendered the FP
viewmodel **left/right mirrored against retail** — stock anchored bottom-LEFT where retail
(and the pre-train build, retail-confirmed side-by-side this session) anchors it
bottom-RIGHT. Root cause: the original's model→render frame map is IMPROPER
(`S = diag(−1,1,1)`) and lives INSIDE its composed builders — the S·Aᵀ·S conjugation
loops and the x-negated pivots/translations (`[orig: @ 0x40c4d8..0x40c57c; pivot negate
@ 0x40c953]`) — so no proper container rotation can realize it after the fact; yaw-180
covers only the D3D→Godot forward flip's share and leaves one mirror unpaid.

The shipped realization now factors the mirror where the data already carries it:
`NovaSkeletalAnim`'s model-table mode RECONSTRUCTS the skeleton's rest positions from the
model bone table + the reset `.bad`'s bind rotations via the corpus-exact export relation
(`opennova::anim::positions_from_model`, the fourth-pass derivation
`pos[i] = bind_rows[parent]·(−dx, dy, dz)`) and runs the SAME rest-carrying composition +
import-flipped meshes as every body rig. On rigs whose shipped `BadBone.position` is
healthy this is bit-for-bit the `.bad`-driven pipeline (the pre-train look); on the
broken twelve it substitutes exact reconstructed values — the model computes what the
`.bad` should have said, and nothing trusts the shipped field. `sample_clip`'s
`model_bind` mode survives as the witnessed-composition reference implementation
(ctest `anim_sample`, incl. a `positions_from_model` exactness case); the knob left the
Godot binding (all callers updated). Historical validation used the then-current ONED
Play-in-Editor on both SKUs: REVX `AKM_1st` and JOX `ak47_1st` (fully broken shipped
positions) render the master-identical close-up hold, user-confirmed live against retail
memory. [ADR 0025](../adr/0025-standalone-game-is-the-only-live-mission-runtime.md) later
retired PIE; the current equivalent runs the exact saved loose mission through F6 and the
standalone `MainGame`/`GameWorld` lifecycle.

### 5.40 First-person weapon viewmodel placement — weapon.def `pos`/`tpos` (2026-06-21)

How the original places the first-person arms+weapon, from the user's lead that it "has to do with
`pos` and `tpos` in weapon.def". All anchored (decompiled this session). The viewmodel is **drawn at
the biased view root**, not as a separately-positioned model: `pos`/`tpos` bias the *camera*, and the
gun+arms are rendered with that same transform.

**The two weapon.def fields** (`WeaponDef`, size 296):
- **`pos`** → `WeaponDef.Bone` (`BoneTransform` @0xF4 = `{float pos[3]; int rot[3]}`) — the **hip**
  first-person offset.
- **`tpos`** → `WeaponDef.AltCamOffset` (@0x10C) — the **ADS / sighted** offset (the alternate camera
  position used when aiming down sights). Corroborated by the values: `tpos` pulls the weapon toward
  the centreline and up vs `pos` (MP5SD `pos 9.07 20.74 -183` vs `tpos -44.98 44.05 -162`).

**Units / scale** [orig: weapon.def `tpos` handler @ 0x54471f; `pos` mirror just above]:
- POSITION: each value is `atof(str) × 256.0` (`flt_7D1D70` @0x544770) and stored as a float. The
  camera `ftol`s it to an int and adds it **straight onto `g_view_pos`** (16.16 world fixed) — so the
  stored float is already a 16.16 world coordinate, and the **net world offset = `file_value / 256`
  world units**. (MP5SD `pos` → `(0.035, 0.081, −0.715)` world units.)
- ROTATION: `Math_ParseFixedPoint16` (→16.16 degrees) `× 0x0B60B60` (= 2³²/360) → **32-bit BAM**.
  Stored as `Bone.rot = [yaw, pitch, roll]` (file columns 4/5/6). MP5SD `pos` rot = `2.0 / −0.5 / 0.0°`.

**How it is consumed** [orig: `Player_UpdateFirstPersonCamera` @ 0x4dd380]:
1. Read the offset `cam_offset = ftol(Bone.pos)` (the `pos`, 16.16 world), `+ g_view_pos_bias`.
2. Build the view rotation `BuildRotationYXZ(g_view_rot_bias + Bone.rot)` — the small per-weapon
   `Bone.rot` is added to the look angles (Z·X·Y order, 10.22 fixed `@0x615400`).
3. Add a **clamped velocity lead** (`g_view_velocity >> 7`, ±1024 xy / ±4096 z) and a **prone Z-drop**
   (`−0x500` when `3·dword_A78394 ≤ 4·dword_A78398`).
4. **ADS switch**: if `entity Flags & 2` ‖ `dword_24C1970`, *overwrite* `cam_offset` with
   `AltCamOffset` (the `tpos`) — an **instant** swap in this function (any ADS-in easing is the
   separate scopeup/scopedown weapon state, see [[project_fp_weapon_fsm]]).
5. Rotate the offset by the view matrix (`Math_FixedPointTransformPoint22` @0x615810, 10.22) and add
   `g_view_pos`; emit `g_view_euler_translation_out`.

**Render** [orig: `Player_RenderFirstPersonViewModel` @ 0x4ded60]: builds `root_matrix` from
`g_view_euler_translation_out` (`Math_BuildFixedPointToFloatMatrix4x4` @0x612200 — translation ÷65536,
**Y negated**, rotations Z·X·Y/BAM) and renders the gfx1 gun (`WeaponDef[1].pad_10[52]`) plus the
character arms (`g_local_player_entity->CharacterEntity`) with it. So **`pos`/`tpos` move the gun AND
the arms together** (one unit at the view root); they enter via the camera, never here.

**OpenNova port (2026-06-21).** `main_game._update_player_camera` places the host viewmodel at
`camera.global_transform × Transform3D(model_facing, offset)` where `offset = (x, z, −y) / 256` from
the weapon.def `pos` units (`_viewmodel_offset`), replacing an eyeballed constant. The view-local frame
is **(x = right, y = forward, z = up)** — derived from the camera adding `ftol(Bone.pos)` straight onto
`g_view_pos` (world Z up) under an identity view matrix at a level look, so component *i* lands on world
axis *i*. Hence **`pos[2]` is the grip's DOWN offset (the dominant −183 → ~0.7u below the eye; the
barrel reaches forward via the model), NOT depth.** Godot camera-local is (x right, y up, −z forward),
so file `x→x`, `y→−z`, `z→y`. (A first cut mistakenly sent `pos[2]` into forward depth, producing a
gun floating ~0.7u in front of the camera — the screensnapr.io/s/8e9d030 symptom; corrected here.
oscarmike `WeaponManager._jo_to_godot_position` independently agrees on `/256` + `pos[2]→up/down`.)
Hardcoded to WPN_MP5SD until a weapon.def Godot binding resolves the equipped weapon. **Deferrals:**
per-weapon `pos`/`tpos` from a weapon.def binding *(landed — the fifth-pass def plumbing above)*; the
`pos`→`tpos` ADS swap (entity `Flags & 2`); the small per-weapon `Bone.rot` *(landed, same pass)*;
velocity lead + prone drop; the model-facing basis and the two small
lateral/forward signs are dialed by drive (the `pos[2]→down` term is the certain one).

### 5.41 `Player_*` family — naming validation + decomp cleanup grill (2026-06-26)

A full read-only grill of the **32 `Player_*` functions** (the local-player input / weapon / camera /
net-identity cluster, `0x42a550`–`0x5cf780`) plus their player-subsystem neighbors. Method: per-function
decompile / disasm / xref + struct-field witnessing, with every proposed rename adversarially re-derived
from its address by two independent skeptic passes (read-only multi-agent refutation). Verdict at the
time: **MATCHING (read-only grill)**. A 2026-07-19 card-selector re-witness later
disproved the `0x4dcd30` vehicle/gunner interpretation recorded below: flags bit
2 is **Sighted**, and the compared `7` is `MountSlot.currentAction ==
SWITCHFROM`, not a seat/type. The current IDB name remains misleading; the later
pass was read-only. This is a naming/typing grill of original engine code, not a
reimpl-equivalence claim. IDB names/types/comments updated in the original
session are retained in its historical log.

**Naming corrections (the misnomers the grill caught; each was UPHELD by that
session's adversarial pass, with the later `0x4dcd30` correction flagged inline):**

| Addr | Old name | → New name | Why (witness) |
|---|---|---|---|
| `0x4c6d40` | `Player_MaybeGetLocalSessionId` | `NapiNP_GetLocalConnectionId` | returns `NapiNPConnection.connection_id` (@+0x18) — the ConnectionId/dcb, an int; return type was wrongly `NapiNPConnection*` (D-NET-100) |
| `0x4dff60` | `Player_BuildNetIdLookupOrFatalError` | `Player_FatalPlayerDcbNotFound` | `__noreturn`; loop never matches, always `MessageBoxA("Could not find player dcb…")`+crash; the "lookup" tables are dead (D-NET-102) |
| `0x4b1060` | `Player_InitLocalPlayer` | `PlayerClass_InitEntity` | sole xref = the `"plyr"` entity-class descriptor table @`0x813054`; inits the passed entity, not specifically "local" |
| `0x4a3d30` | `Player_ResetTerrainPosition` | `Camera_ResetToLocalPlayer` | `Camera_ClearViewState` + `Camera_SetTrackedEntity(local)` + cam-height/offset globals; nothing terrain |
| `0x4dc6b0` | `Player_GetCurrentWeaponAmmoCapacity` | `Player_GetClampedWeaponElevation` | reads/clamps `MountSlot.Elevation` → `WeaponDef.MaxElevation`; feeds the FOV zoom divisor; no ammo |
| `0x4dcc80` | `Player_GetVehicleAutoAimRange` | `Player_IsEquippedWeaponScoped` | returns `g_weaponScopeActive` gated on `Def->Flags&1`; not a range |
| `0x4dcd30` | `Player_IsGunnerInVehicle` | `Player_IsVehicleGunnerScoped` *(2026-06-26 IDB name; semantics disproven 2026-07-19)* | settled Sighted standard-card predicate: `Flags&2`, `g_weaponScopeActive`, and `MountSlot.currentAction != SWITCHFROM (7)`; no vehicle/gunner test |
| `0x51cbc0` | `player_ServerAdd` | `Server_PlayerAdd` | own string `"server_PlayerAdd():"`; server subsystem |
| `0x59b280` | `sub_59B280` | `Radar_AddBlip` | bearing(atan2) + compass-edge marker + 128-slot blip array (pos/type/lifetime 62/color); `OnDamageReceived` uses it for damage direction |
| `0x541690` | `WeaponSlots_SeedAmmoPoolsFromDefs` | `WeaponOverlay_BuildTypeLookup` | memset 0x200; iterate 780 slots; index by slot-type byte +216; action-specific overlays (state 5-9) |

**Signature corrections:** `Player_FindLocalPlayerEntity @0x4e0090` → `GamePlayerEntity* __cdecl(void)`
(the decompiled `stream`/`playerData` params are spurious; returns the matched entity); `Player_InitPlayer
@0x4e15f0` → `int __cdecl(int isRestore)` (3 phantom trailing params; the caller pushes a single `1`; the
body uses only `isRestore`); `NapiNP_GetLocalConnectionId @0x4c6d40` → `unsigned int __thiscall(NapiNPServerCtx*)`.

**Names VALIDATED correct (confirm-only, no change):** the equipped-slot getters
`Player_IsVehicleHasAutoAim` / `…HasAttackCapability` / `IsDriverInVehicle` / `IsVehicleSeatHasFlag4` /
`…Flag8` (all read `EquippedSlot->Def` capability bits — "Vehicle*" is contextual: the equipped slot is
the held weapon for infantry, a seat when mounted; `Field0C&0x200` = alternate scope-camera, NOT provably
"auto-aim"); the weapon-slot family `Select` / `Mount` / `Cycle` / `SwitchToWeaponByHandle` /
`EquipWeaponByEntity` / `ToggleWeaponScope`; `AdjustWeaponZoomLevel` (scope zoom level, `MountSlot[1]+0x24`)
vs `AdjustWeaponElevation` (`MountSlot.Elevation@0xC`) — distinct fields, both correct;
`UpdateFirstPersonCamera`, `RenderFirstPersonViewModel`, `OnDamageReceived`, `StartRoundEndTransition`,
`PackInputStateToEntity`, `CanFireWeapon`, `UpdatePerFrame`. The neighbor `calculate_kill_score @0x5407e0`
is correctly named (sums entity-type score + weapon pass value); a recon claim that it was "only a
slot-eligibility predicate" was REFUTED — it IS reused as an eligibility predicate by Cycle/Switch, but
its identity is kill-scoring.

**The weapon scope / ADS state machine (globals named this pass).** `g_scopeEngaged @0x82CE94` is the
master scoped flag (set/cleared by `Player_ToggleWeaponScope`); `Player_UpdatePerFrame` mirrors it each
frame into `g_weaponScopeActive @0xB76478` (`= g_scopeEngaged != 0`) once the scope-camera interp settles.
`g_weaponScopeActive` is the effective flag read by `CanFireWeapon` + the equipped-slot getters + the
unscope-on-move / leave-FP / round-reset paths; `g_scopeHipfire @0x82CE98` is the complement.
`g_cameraFovDeg @0x26C6848` (16.16°, default `0x500000` = 80.0; scope recomputes `80.0 / elevation`) is
the current camera FOV, env-interpolated (target `g_cameraFovDegTarget @0x26C684C`). `g_currentWeaponSlot
@0xB76474` is the current equipped weapon-slot flat index (group×65 + offset into the 780-entry
`weaponSlotArrayBase` pool). `g_inputFlags @0xB3B728` is the raw per-frame input bitfield
(`Player_PackInputStateToEntity` packs it into `entity->MoveOrder@0x12C`, saves to `g_inputFlagsPrev`).

**The local-player camera/view block `0x82CE40..0x82CEF8` — two `CNetPlayerInterp` instances (Phase 3).**
`CNetPlayerInterp_Setup @0x4ddfd0` proves `0x82CE40` and `0x82CEA0` are `CNetPlayerInterp` (84 B:
stepCount, per-step pos/angle velocity, current pos/angles, target pos/angles, `entitySlotPtr@0x4C`,
`activeFlag@0x50`). Typed + named **`g_fpCameraInterp @0x82CE40`** (first-person / scope camera;
`activeFlag != 0` gates scope-toggle/fire) and **`g_roundEndCameraInterp @0x82CEA0`** (round-end / death
camera; `Player_StartRoundEndTransition` target = weapon bone, Z −1.0). This collapsed ~20 stray
`dword_82CExx` globals into two named interp instances + the scope scalars above.

**Divergence catalog (new IDs, stable).**
- **D-NET-100** [naming, FIXED] `Player_MaybeGetLocalSessionId @0x4c6d40` → **`NapiNP_GetLocalConnectionId`**:
  returns `NapiNPConnection.connection_id` (@+0x18, renamed from `unk_18`) — the local ConnectionId / join
  "dcb" — as an `unsigned int`. The decompiled `NapiNPConnection*` return type was wrong (the value is an
  int id; all 5 callers treat it as an integer, none derefs). The dcb chain now reads cleanly:
  `Player_FindLocalPlayerEntity` stores it and compares `entity->ownerConnectionId == it`. `[orig:
  NapiNP_GetLocalConnectionId @0x4c6d40]`
- **D-NET-101** [naming, FIXED] `GamePlayerEntity.entityFlags @0x78` → **`ownerConnectionId`**: it is NOT
  flags (the real flags are `Flags@0x24`). It holds the entity's owner ConnectionId — the join "dcb" — the
  field `Player_FindLocalPlayerEntity @0x4e0090` self-matches against the local ConnectionId (D-NET-92),
  written by `Server_PlayerAdd @0x51cbc0` (`entity+0x78 = joinEvent+76`, the same ConnectionId
  `PlayerSession_InitFromProfile` stashes at `playerCtx+0x18`) and by the wire spawn handlers (`[orig:
  NapiNPClientMsg_0x04F @0x4288bb / @0x4288cd]` writes +0x78 and +0x7C from consecutive independent wire
  dwords). It is **distinct from `DcbId @0x7C`** (the BMS/.dcb-script id keyed by `Entity_*ByNetId`): the
  "Could not find player dcb" abort searches `@0x78`, not `@0x7C`. So §5.2b's "three id fields" is really
  FOUR — `ownerConnectionId@0x78` (join/self-match dcb = ConnectionId), `DcbId@0x7C` (BMS script id),
  `NetId@0x15c` (streaming id), `Ssn@0x2e` (authority id). `[orig: Player_FindLocalPlayerEntity @0x4e0090]`
- **D-NET-102** [naming, FIXED] `Player_BuildNetIdLookupOrFatalError @0x4dff60` →
  **`Player_FatalPlayerDcbNotFound`**: `__noreturn`; the pool loop has no break-on-match and always falls
  through to the "Could not find player dcb" MessageBox + crash; the two stack lookup tables it fills
  (`entry+0x24`, `entry+0x78`) are never read. Reached from `Player_InitPlayer` when
  `Player_FindLocalPlayerEntity` returns NULL. `[orig: Player_FatalPlayerDcbNotFound @0x4dff60]`
- **D-NET-103** [naming, FIXED] `GamePlayerEntity.weaponState @0x294` → **`playerClass`** — the soldier
  CLASS (5-9) from the char-select `g_charSelClass`; `Player_InitPlayer` copies the per-team
  `g_charClassTeam1/2`; indexed by class in `WeaponOverlay_BuildTypeLookup` + `Entity_GetHealthClassification`
  and gated `== 6` in `Player_AdjustWeaponElevation` — never a runtime weapon state (kong `weaponState`
  was an auto-guess; NPCs get it from `Entity_SetHealthFromDifficultyByte`). Session globals renamed:
  `dword_24D20E0` / `pool` → `g_charClassTeam1` / `g_charClassTeam2` and `byte_24D4DFE` / `…DFF` →
  `g_avatarTeam1` / `g_avatarTeam2` (sourced in `apply_session_settings_to_globals`; team split {1,3} vs
  {2,4}). **`animSlot @0x374` was investigated and KEPT** (an interim `avatarIndex` rename was REVERTED):
  it is the general character-model / anim-set selector that `Entity_SpawnFromAnimSlotProperty @0x43c522`
  (the BMS `AnimSlot` property), the player's avatar (`g_avatarTeam1/2` via `Player_InitPlayer`), and the
  wire spawn all write — so the engine's own `animSlot` term is faithful and the avatar is only the
  player's source. `PlayerSession_InitFromProfile`'s `weaponClassA/B` params are the avatar bytes (`avatarA/B`).
  `[orig: Player_InitPlayer @0x4e15f0 / apply_session_settings_to_globals @0x551500 / Entity_SpawnFromAnimSlotProperty @0x43c522]`

**IDB changes made during the session (2026-06-26).**
- Functions renamed: `0x4a3d30 Camera_ResetToLocalPlayer`, `0x4dc6b0 Player_GetClampedWeaponElevation`,
  `0x4c6d40 NapiNP_GetLocalConnectionId`, `0x4dff60 Player_FatalPlayerDcbNotFound`, `0x4b1060
  PlayerClass_InitEntity`, `0x4dcc80 Player_IsEquippedWeaponScoped`, `0x4dcd30 Player_IsVehicleGunnerScoped`,
  `0x51cbc0 Server_PlayerAdd`, `0x59b280 Radar_AddBlip`, `0x541690 WeaponOverlay_BuildTypeLookup`.
  The `0x4dcd30` rename is the historical 2026-06-26 mutation; its semantics
  were disproven by the read-only 2026-07-19 pass and no replacement rename was
  applied.
- Signatures: `0x4e0090` `GamePlayerEntity*(void)`; `0x4e15f0` `int(int isRestore)`; `0x4c6d40`
  `unsigned int(NapiNPServerCtx*)`.
- Globals named/typed: `g_cameraFovDeg` + `g_cameraFovDegTarget` (`0x26C6848/4C`), `g_weaponScopeActive`
  (`0xB76478`), `g_currentWeaponSlot` (`0xB76474`), `g_inputFlags` + `g_inputFlagsPrev` (`0xB3B728/2C`),
  `g_scopeEngaged` + `g_scopeHipfire` (`0x82CE94/98`); `0x82CE40` + `0x82CEA0` typed `CNetPlayerInterp` →
  `g_fpCameraInterp` / `g_roundEndCameraInterp`.
- Struct members: `NapiNPConnection.unk_18` → `connection_id`; `GamePlayerEntity.entityFlags@0x78` →
  `ownerConnectionId`.
- Comments added at the net-identity, scope-state, camera-interp, and `calculate_kill_score` sites.
- Follow-up (the `Player_InitPlayer` team-block dig, D-NET-103): struct `GamePlayerEntity.weaponState@0x294`
  → `playerClass` (`animSlot@0x374` kept — an interim `avatarIndex` rename was reverted, D-NET-103); session globals `g_charClassTeam1/2` (`0x24D20E0/E4`,
  was `dword_24D20E0`/`pool`) + `g_avatarTeam1/2` (`0x24D4DFE/DFF`); `Player_InitPlayer` local
  `teamByte`→`avatarByte`; `PlayerSession_InitFromProfile` params `weaponClassA/B`→`avatarA/B`. Final `idb_save`.

### 5.42 `Server_*` family — naming validation + decomp cleanup grill (2026-06-26)

A full grill of the **120 `Server_*` functions** (the authoritative-server per-tick lifecycle:
tick orchestration, player join/leave, teams/balance, scoring/rounds/win, capture zones, weapon
validation, anti-cheat/admin, gate metrics, the `Server_Send*`/`Server_Broadcast*` packet
wrappers, and the dedicated-server status screen; `0x4243a0`–`0x563af0`). Method: per-function
decompile + xref + struct-field witnessing across 11 behavioral clusters, every rename/retype
adversarially re-derived from behavior before landing, signatures cross-checked at call sites and
(for `__stdcall` candidates) at the `retn` instruction. Verdict: **MATCHING (read-only grill)** —
the family is faithfully named after the corrections below; this is a naming/typing/cleanup grill
of original engine code, not a reimpl-equivalence claim. The server entry point is
`Server_TickUpdate @0x51d7e0` (gates ~14 cadence timers; the per-second block at
`g_periodic_second_timer == 0` drives capture/win/violation/timeout work).

**Naming policy (user decision).** Several functions carry their own `__FUNCTION__` log strings
proving the original module used lowercase `server_*` names (`server_PlayerAdd`,
`server_ProcessClientRequestRespawn`, `server_ClientFiredRound`, …). Per the maintainer's call the
whole family is normalized to the PascalCase `Server_*` house style; where a self-string proves an
exact original *suffix*, that suffix is adopted (e.g. `ClientFiredRound`) in PascalCase. Lowercase
functions renamed up: `server_handle_entity_sync`→`Server_HandleEntitySync`,
`server_ProcessClientRequestRespawn`/`…SpectatorRespawn` (kept suffix),
`server_broadcast_entity_kill`→ see D-NET-108, `server_handle_client_crc_validation`→
`Server_HandleClientCRCValidation`, `server_PlayerPuntCRCMisMatch`→`Server_PlayerPuntCRCMisMatch`.

**Naming corrections (the misnomers the grill caught):**

| Addr | Old name | → New name | Why (witness) |
|---|---|---|---|
| `0x5008b0` | `Server_ProcessTeamChanges` | `Server_FindPlayerSlotByNetKeys` | body is a pure slot lookup matching `slot[7]→+184→+48/+52 == (k1,k2)`; changes/sends nothing; old name + a stale disasm comment both wrong (D-NET-107) |
| `0x515390` | `server_broadcast_entity_kill` | `Server_BroadcastMedicRequest` | fetches `GameText("Server","STRSRV_MEDREQ")` (medic request), broadcasts msg 0x54+0x14, sets a once-only "notified" flag; no kill (D-NET-108) |
| `0x50baa0` | `Server_ValidateAndFireRound` | `Server_ClientFiredRound` | own string `"server_ClientFiredRound: Player:%s Type:%d Ammo left:%d (NO AMMO!)"` (D-NET-109) |
| `0x541ba0` | `should_send_entity_update` | `WeaponSlot_CanFire` | the "NO AMMO!" gate: reads the slot's weapon child, the underwater-fire ban, the clip u16 slot+16 / adm+220 pool, and the adm+224 score-lock; no entity update anywhere (D-NET-152) |
| `0x4ffee0` | `compute_entity_angular_priority` | `Server_BuildRoundEventListForPlayer` | walks `g_round_ring` since the recipient's `playerSlot+97544` watermark, skips own rounds, scores by line-of-fire proximity into `g_round_event_refs` — round events, not entity priorities (D-NET-152) |
| `0x504820` | `serialize_projectile_to_packet` | `NetPacket_SerializeRoundEvent` | writes one §5.9.1 tag-2 ROUND-EVENT record (fire origin + direction) from a ring record; no projectile entity involved (D-NET-152) |
| `0x42f270` | `NetPacket_DeserializeWeaponHit` | `NetPacket_DeserializeRoundEvent` | the same record's client read side — a round FIRED event the client re-simulates; nothing about it is a hit (D-NET-152) |
| `0x4ec0d0` | `RoundData_ProcessHit` | `RoundData_SpawnRound` | SPAWNS the round from a fire request (projectile-pool entity, spread, velocity, tracer/guided/burst dispatch); no hit is processed at fire time (D-NET-152) |

**Signature corrections — the wrong-prototype cascade (D-NET-110).** Many callees carried IDA-inferred
prototypes with phantom params; their garbage flowed up as uninitialised `v*` args in
`Server_TickUpdate` and elsewhere. Two sub-classes, both fixed:
- *Extra cdecl params, body uses none/fewer:* `Server_BuildEntitySlotLists @0x4f97a0` (was
  `__thiscall(char*,const char*)`) and `Server_UpdateEntityIdleTimers @0x50d770` (was `(int,int,char)`)
  → `void(void)`; `Server_CheckWinConditions @0x51ad40` `void(void)`; `Server_UpdateBotMovement @0x51b960`
  `void(void)`; `Server_SendEntityStateToPlayer @0x517ba0` `(int playerSlot)`;
  `Server_SendMissionMetrics @0x4fb3a0` `void(void)`; `Server_BroadcastWeaponOverlayUpdate @0x509fc0`
  `(int,int)`; `Server_SetNetworkDelay @0x50cbc0` `(int delayTicks)`; `Server_HandleBanPuntCommand @0x50b380`
  `(uint,int)`; `Server_BuildEndOfRoundScoreboard @0x508f30` `(int enable,int winningTeam)`;
  `Server_SendEntityStatePacket @0x509d70` had the opposite defect — a *dropped* 2nd param, restored to
  `(int entityPtr,int param)` (caller pushes 2, verified at the call site).
- *Bogus inherited `__stdcall` + WndProc/display arg names on a genuinely cdecl fn:*
  `Server_SendWeaponSlotListToPlayer @0x502550` (args `msg/wParam/lParam`) and
  `Server_UpdateCaptureZoneEntities @0x519690` (args `resolutionId/outWidth/outHeight`) — both end in a
  **plain `retn`** (not `retn N`), proving caller-cleans = cdecl, so trimmed to `int(void*)` / `void(void)`.
  `Server_UpdateCaptureZones @0x53b8f0` was `__usercall(...@<ecx>)` with a spurious `eax` input → `__fastcall(_DWORD*captureCtx)`.
- *Net message-handler signature (dispatch table `@0x82b5d8`)* restored to `(int connectionCtx, data*, int dataLen)`:
  `Server_HandleEntitySync @0x510990`, `Server_ProcessClientRequestRespawn @0x519af0`,
  `Server_ProcessClientRequestSpectatorRespawn @0x51c840`, `Server_ValidateWeaponCRC @0x501f70`,
  `Server_BroadcastMedicRequest @0x515390` — each had been declared `()` yet read `connectionCtx`/`data`/`len`
  off the stack uninitialised.

**Names VALIDATED correct (confirm-only, no change).** The bulk of the family was already accurate; the
opcode of every `Server_Send*`/`Server_Broadcast*` wrapper was confirmed against its
`NapiNPServer_SendFiltered(&g_napi_np_ctx, <op>, …)` call — e.g. entity-state 0x26/0x28/0x0A, chat 0x14,
weapon overlay 0x46, player-info 0x7B, scoreboard 0x16/0x1D, round-end 0x61, kill/event 0x1E, medic 0x54,
random-seed 0x61, despawn 0x12. `Server_PlayerAdd @0x51cbc0` (D-NET / §5.41) and `Server_ClientFiredRound`
are the two C2S handlers with self-name strings. The two priority-list builders are correctly distinct:
`Server_BuildEntityPriorityList @0x50e590` sends despawns (msg 0x12) + returns the `CPairList`;
`…ForPlayer @0x50df20` writes caller out-arrays.

**Server globals named/typed this pass (~45).** Tick cadence timers (witnessed by reset constant / action):
`g_dirtyflag_clear_timer` (`0xC8D810`, 744 → `EntityPool_ClearDirtyFlags`), `g_botmove_timer` (`0xC8D814`, 15),
`g_quarter_roundrobin_counter` (`0xC8D808`), `g_scoreboard_broadcast_timer` (`0xC8D80C`, 310),
`g_serverinfo_update_timer` (`0xC8D818`, 1860), `g_mission_metrics_timer` (`0xC8D820`),
`g_periodic_second_timer` (`0xC8D83C`, 62), `g_preround_delay_timer` (`0xC8D824`, = `dword_24D2160` at round
start), `g_playerslot_broadcast_timer` (`0xC8D838`, 310), `g_spectator_broadcast_timer` (`0xC8D840`, 310),
`g_weapon_broadcast_slot_cursor` (`0xC8D844`), `g_weapon_resend_timer` (`0xC947A0`, 62, KOTH).
Replication: `g_priority_ref_x/y/z` (`0xC867A4/A8/AC`, the broadcast-origin eye position),
`g_priority_pairlist` (`0xC86FE0`), `g_entity_send_budget` (`0xC8FC50`, BANDWIDTH cmd sets it 100-1600,
default 600), `g_entity_action_queue` (`0xC86FDC`). Rules/config:
`g_score_limit`/`g_kill_limit`/`g_time_limit_minutes`/`g_respawn_time` (`0x24D2134/38/44/40`),
`g_autobalance_enabled`/`_min_diff`/`_trigger_diff` (`0x24D2190/94/98`), `g_team_change_entity_list`
(`0xC947C8`), `g_team1_name`/`g_team2_name` (`0x24D1FF5`/`0x24D2006`), `g_num_teams_config` (`0x24D2150`),
`g_capture_duration` (`0x24D2248`), `g_round_wins_team1..4` (`0xC8FF0C/10/14/18`), `g_total_rounds_played`
(`0xC8FF1C`), `g_round_winning_team` (`0x24C1924`), `g_mission_time_ticks` (`0x24C1944`). Join/moderation:
`g_expansion_checksum` (`0xB4C5A4`), `g_banned_name_count`/`g_banned_name_list`/`g_banned_id_list`
(`0xC8FF20`/`0xC90728`/`0xC8FF28`), `g_squad_max_players`/`_password_required`/`_required_tag`
(`0x2550924`/`0xC9478C`/`0x2550928`), `g_pcid_dupe_reject` (`0x25509E8`), `g_weapon_violation_limit`
(`0x24D2178`), `g_votekick_enabled`/`_min_players`/`_percent` (`0x24D226C/70/74`), `g_network_delay_ticks`
(`0xB4C29C`), `g_punt_log_enabled`/`g_cheat_log_enabled` (`0xC86FBC`/`0xC86FC0`), `g_gate_address`/
`g_server_label` (`0xB5F4DC`/`0xB5F4BC`).

**`g_napi_np_ctx` access note.** `Server_*` stage outgoing messages by writing past the typed
`NapiNPServerCtx` end via `*((_DWORD*)&g_napi_np_ctx + 1126/1127/1128/1129)` — these reach the adjacent
msg-staging globals `g_napi_msg_payload_buf @0xB5BBB0` / `g_napi_msg_payload_len @0xB5CBB0` (filter
mode / target conn / target slot / target team); not a bug, an artifact of the contiguous staging block.

**Divergence catalog (new IDs, stable).**
- **D-NET-107** [naming, FIXED] `Server_ProcessTeamChanges @0x5008b0` → **`Server_FindPlayerSlotByNetKeys`**:
  iterates player slots and returns the one whose net-player (`slot[7]→+184`) key fields `+48`/`+52` equal
  the two args; it is a pure read (no state change, no packet). Old name and a stale "processes pending team
  change… sends packets" disasm comment were both wrong. The exact meaning of the two key ints (`+48`/`+52`
  of the inner net object) is **not yet witnessed** — follow-up. `[orig: Server_FindPlayerSlotByNetKeys @0x5008b0]`
- **D-NET-108** [naming, FIXED] `server_broadcast_entity_kill @0x515390` → **`Server_BroadcastMedicRequest`**:
  net msg handler (table `@0x82b5d8`) for a wounded player's medic call — formats `STRSRV_MEDREQ` with the
  player name, broadcasts msg 0x54 (entity handle) + msg 0x14 (chat), sets `slot+89856` so it fires once, and
  plays the help sound. No kill is involved. Signature restored to `(int connectionCtx, u8 *data, int dataLen)`.
  `[orig: Server_BroadcastMedicRequest @0x515390]`
- **D-NET-109** [naming, FIXED] `Server_ValidateAndFireRound @0x50baa0` → **`Server_ClientFiredRound`**: own
  log string `"server_ClientFiredRound: …"` is the original name; the function validates a client fired-round
  request (ammo, distance, ownership, weapon CRC). Signature `()` →
  `(int fireRequest)` (the body's `teamIndex` local was a misnamed pointer to the fire-request descriptor).
  (2026-07-03 correction: only the ALT-fire / AI / local-host paths queue via `RoundData_AddRound`
  directly — a NET primary fire goes through the adm 'fire' action and re-enters this function in
  LOCAL mode before reaching the ring; the full pipeline is §5.16 / D-NET-152.)
  `[orig: Server_ClientFiredRound @0x50baa0]`
- **D-NET-110** [signature, FIXED] **Wrong-prototype cascade across `Server_*`.** ~15 functions carried
  IDA-inferred prototypes (extra phantom params, dropped params, or a bogus `__stdcall`+WndProc/display
  prototype on a cdecl fn); the synthesized garbage surfaced as uninitialised `v*` call args in
  `Server_TickUpdate`. All corrected (list above); `__stdcall` candidates were disambiguated by the actual
  `retn` (plain `retn` = caller-cleans = cdecl). Re-decompiling the affected callers after a Hex-Rays cache
  flush (`mark_cfunc_dirty`) clears the phantom args. `[orig: Server_TickUpdate @0x51d7e0]`
- **D-NET-111** [type, FOLLOW-UP] **`CServerTick` is an undefined struct.** The telemetry instances
  `dword_C87020` and `dword_C87598` (stride `0x578`) are accessed as raw dwords; layout partly witnessed —
  `+0` phase enum (0..4), `+8` last `GetTickCount`, `+C/+10/+14/+18` per-phase accumulators, plus
  player-count maxes, entity-type counts and the mission-metric fields (`g_mission_time_ticks` base
  `0xC8759C`; phase durations `0xC875A4/A8/AC/B0`; max/most players `0xC8761C/0xC87620`; mission filename
  `byte_C875DC`). Methods: `CServerTick_Construct @0x5017d0`, `_Reset @0x4f9f40`,
  `_UpdateMaxPlayerCounts @0x4fa130`, `_AccumulateEntityTypeCounts @0x4fa1f0`,
  `_SetState @0x4fa090`. **Not minted this pass** (conservative scope); grill the four methods to define
  it, then type both globals. Other deferred items: `Server_UpdateCaptureZoneEntities` retn-verified cdecl
  (FIXED); the `param4 @0x24D1DCC` server-terrain-CRC misnomer global in `HandleClientCRCValidation`; the
  `entityIndex @0x252DCD0` misnomer (it is the 16×8 connection/slot table used by `BroadcastChatToAllPlayers`
  + `Server_DestroyBatchedEntities`, not an index); `Server_ToggleDedicatedFlag @0x4dc9c0` name suspect
  (toggles render-flags bit `0x100` of `dword_24C1930` + refreshes the LOCAL player's weapon overlay — reads
  like a client view toggle, needs a `0x100`-consumer witness). `[orig: CServerTick_Construct @0x5017d0]`

**IDB changes made during the session (2026-06-26).** Functions renamed (10): the 3 above + the 5 lowercase
`server_*`→`Server_*` normalizations + `Server_PlayerAdd` (already done in §5.41). Signatures corrected on
~18 functions (D-NET-110 list). ~45 server globals named/typed (list above). Comments added at the
`FindPlayerSlotByNetKeys`, `BroadcastMedicRequest`, and `ClientFiredRound` sites documenting the rename
rationale. No new local types were declared (zero duplicate-type risk). Per-cluster `idb_save`; final
cache flush + `idb_save`.

### 5.43 Player add → spawn: id allocation, team, burst cadence, placement (2026-06-26)

Grill in support of the npruntime P3/P4 spawn + §5.2a burst remediation. Verdict: **MATCHING
(read-only grill)** for the witnessed engine behaviour below; two reimpl divergences catalogued
(D-NET-112, D-NET-114-reimpl). Anchors: `Server_BuildPlayerInfoAndAdd @0x51d560` →
`Server_PlayerAdd @0x51cbc0` → `Entity_SpawnFromAnimSlotProperty @0x43c390`; the per-join burst
producer `Server_OnPlayerJoin @0x51a680`; `Server_AssignPlayerTeam @0x4fe310`;
`Entity_FindBestSpawnPoint @0x50ccc0`; lookup `EntityPool_FindByNetId @0x4f0a20`; authority-id
reader `Entity_GetNetIdIfAuthority @0x4e4010`.

**The four player id fields (confirm-only, already typed in `GamePlayerEntity`).** Distinct, not
interchangeable:
- `ownerConnectionId @0x78` (uint32) — the **join dcb = ConnectionId** (D-NET-105 join-order
  counter). Written by `Server_PlayerAdd`/`Entity_SpawnFromAnimSlotProperty` as
  `entity+0x78 = event+76 = conn->connection_id`; this is the self-match field
  (`Player_FindLocalPlayerEntity @0x4e0090`) and the wire `0x0C` `entity_flags`.
- `DcbId @0x7c` (uint32) — the **`find_by_net_id` lookup key**: `EntityPool_FindByNetId @0x4f0a20`
  matches `*(entity+124) & 0xFFFF` across pools 0 then 1-3 (mask 0xF), returns `pool<<12 | slot`.
- `Ssn @0x2e` (uint16) — the **authority id** returned by `Entity_GetNetIdIfAuthority @0x4e4010`
  (`return is_in_session && !is_authority ? 0 : entity->Ssn`).
- `NetId @0x15c` (uint16) — a separate streaming id field; left 0 by the player spawn path.

**D-NET-112** [reimpl divergence, **FIXED 2026-06-27**] **No high-band player net-id allocator exists in the
original.** **FIX EXECUTED:** `allocate_player_net_id` + `kPlayerNetIdBase` deleted; a player now spawns with
`net_id = 0` (faithful — `Entity_SpawnFromAnimSlotProperty @0x43c390` leaves Ssn/DcbId/NetId at 0, writing
only ownerConnectionId@0x78). A player is identified by its pool HANDLE (wire) + `owner_connection_id` (dcb,
the client self-match), never an SSN — so it stays out of the WAC/BMS `find_by_net_id` space (mission
entities own that). Production rendering was already handle-based; the present-pass/avatar tracking + the
3 GUT tests (`nova_listen_server`, `host_session_accept_gut`, `coop_two_sim`) + `server_spawn_test` were
migrated from net_id-keying to handle/`owner_connection_id` (new `get_entity_owner_connection_id` /
`get_entity_wire_handle` getters). Verified: 31 net+world+wac+mission ctest + 25 GUT green, zero regressions.
The residual Ssn@0x2e-vs-DcbId@0x7c field LABEL nicety (find_by_net_id keys our single field, which plays the
WAC-address role regardless of name) is a cosmetic follow-up, not a behavior gap. **(Original DOCUMENTED note,
for history:)** `Server_PlayerAdd` finds an empty *player slot* (`sub_4FD7B0`/`sub_500A50`) and spawns
the entity; the per-frame entity stream (`serialize_entity_states_to_packet @0x50f070`, sent by
`Server_SendEntityStateToPlayer @0x517ba0`) identifies every entity on the wire by its **handle
(`pool<<12 | slot`)**, not by an allocated 16-bit net id. There is no count-based or
downward-scan allocation of a reserved-band SSN anywhere in the add/spawn path. The opennova
reimpl collapses `DcbId`/`NetId`/`Ssn` into a single `world::Entity::net_id` and mints player ids
in a high reserved band (`allocate_player_net_id`, 0xFFF0 downward, skipping live ids and
0/0xFFFF) to avoid colliding with the small authored mission ids that share that one field. This
is a deliberate divergence pending a faithful multi-field id model; the prior count-based variant
(uint16 overflow past 16 players + reuse-after-disconnect) is fixed by the downward collision-
checked scan. `[orig: Server_PlayerAdd @0x51cbc0; serialize_entity_states_to_packet @0x50f070]`

**Wave 4 grill (2026-06-27) — the faithful fix, fully scoped.** Re-grilled `Server_PlayerAdd @0x51cbc0`
+ `find_by_net_id` usage. The original NEVER registers a player in the SSN / `find_by_net_id` space:
that space is the WAC/BMS mission-entity addressing (our `world.cpp` `set_ssn_*`/`kill_ssn` all
`find_by_net_id(ssn)`); authored mission entities have a real SSN, players do not. A player is
identified three ways, none of which is our collapsed `net_id`: (1) **wire** = the pool HANDLE
`pool<<12|slot` (the 0x0C/0x0A bridge already uses this); (2) **client self-match** =
`entity+0x78 ownerConnectionId/dcb` (`Player_FindLocalPlayerEntity @0x4e0090` vs
`NapiNP_GetLocalConnectionId`), already modeled as `Entity.owner_connection_id`; (3) the separate
`NetId@0x15c` for the 0x51 body. Our `allocate_player_net_id` exists ONLY because the **present pass /
nova_simulation keys the local player on `Entity.net_id == 0xFFF0`**. **Faithful fix (DEEP, dedicated
test-driven session):** migrate the present-pass / local-player identification from `net_id` to
`owner_connection_id` (+ handle), then free `Entity.net_id` to its real value (0 for a player, so it
stays out of the WAC SSN space) and DELETE `allocate_player_net_id`. Touches libs/world spawn +
libs/netsim bridge + nova_simulation present + the GUT listen-server/present tests — hence not done
inline (no-regression discipline). The current allocator is WIRE-CORRECT (handle-based wire identity),
so this is internal-fidelity debt, not a wire bug.

**Deeper grill (2026-06-27, the field-identity reconciliation the fix must do first).** Two more
witnesses make the model precise: (a) `Entity_SpawnFromAnimSlotProperty @0x43c390` (the player entity
spawn) memsets the entity and writes ONLY `entity+0x78` (ownerConnectionId/dcb = spawnData[6]) as an id —
it never writes Ssn@0x2e, DcbId@0x7c, or NetId@0x15c, so a player's Ssn/DcbId/NetId are all 0. (b)
`EntityPool_FindByNetId @0x4f0a20` keys on **`entity+0x7C` (DcbId)**, NOT Ssn@0x2e (and has no netId==0
guard). So the WAC/BMS `set_ssn_*` addressing is actually by **DcbId**. The reimpl conflates this: our
`EntityRegistry::find_by_net_id` matches `Entity.net_id` (entity.h labels it "SSN"), while the original's
key is DcbId@0x7c (= our `AiEntity.net_id`, ai.h). **The faithful net-id model must therefore (1) split
the conflated field into Ssn@0x2e / DcbId@0x7c (the find key) / NetId@0x15c, (2) repoint `find_by_net_id`
at DcbId, (3) leave a player's DcbId/Ssn at 0 and identify it by handle + ownerConnectionId (as the
present already does via `cached.local_player`), (4) delete `allocate_player_net_id`.** This is a
world/ai/wac/mission-wide reconciliation with its own test surface — a dedicated, test-driven session.

**Why the fix is genuinely multi-site (final scope, 2026-06-27).** The reimpl's player net_id is NOT
purely internal: the present surfaces it as `PF_NET_ID` (`nova_simulation.cpp:1662`) and — because a
player has no BMS `(kind,index)` origin — the **player AVATAR is host-managed BY net_id** (the stable
per-player key across frames; `nova_listen_server_test.gd:88-99`). Four GUT tests assert the high-band
ids directly (`nova_listen_server` 0xFFF0, `host_session_accept_gut`/`coop_two_sim` 0xFFEF, plus
`nova_simulation_test`). So step (3) above must ALSO migrate the present/avatar stable-key + those GUT
tests from net_id to ownerConnectionId(dcb)+handle. Setting player net_id=0 naively breaks the avatar
tracking + those tests — which is precisely why this is a dedicated session, not an inline edit.

**D-NET-113** [behavior, MATCHING] **Player team assignment.** `Server_AssignPlayerTeam @0x4fe310`
(called from `Server_PlayerAdd`, writes `playerSlot+416`):
- spectator (`+100567 && is_in_session`) → team **0**;
- co-op / non-MP (`(g_GameType & 0xFFFDFFFF) == 0x10020` **or** `!is_in_session`) → team **1**;
- otherwise (DM/TDM, and the 4-team gametypes `0x10000/65537/65544` when `g_num_teams_config==4`):
  requested team name (`g_team1_name`/`g_team2_name`, case-insensitive), then a team preference
  (`teamPref`), then **autobalance** to the least-populated team — 2-team: `(team1Count >
  team2Count) + 1`; 4-team: count per team over the player slots, shell-sort, pick the
  least-populated *existing* (named) team; `rand()`-free, deterministic by count.

  ⇒ The reimpl's `spawn.team = 1` is **faithful for the current co-op/SP target** (the
  `!is_in_session`/co-op branch); the MP team path is the autobalance/name-match logic above,
  to be ported when a DM/TDM gametype is wired. `[orig: Server_AssignPlayerTeam @0x4fe310]`

**D-NET-114** [behavior, MATCHING — reimpl divergence FIXED here] **The §5.2a initial-state burst
is one-shot per join.** `Server_OnPlayerJoin @0x51a680` emits the *entire* sequence — 0x42 input
flags, the entity-state world-stream (`Server_SendEntityStateToPlayer`, only when `is_in_session`),
0x0F player state, 0x4D player index, the random seed(s) (`Server_SendRandomSeedToPlayer`), the
0x1D weapon overlay + 0x14 cease-fire when applicable, and the 0x3E terminator — **synchronously in
one call (one engine tick)**. There is no per-frame cursor that paces one phase per tick. The P3
reimpl advanced exactly one §5.2a phase per `tick_connections` pass (~20 ticks / ~6 datagrams per
join); the faithful behaviour is to drain the whole track in one call.
`[orig: Server_OnPlayerJoin @0x51a680]`

**D-NET-115** [behavior, MATCHING] **Spawn placement + the rand tiebreak + the player avoid-set.**
`Entity_FindBestSpawnPoint @0x50ccc0`: for each pool-3 marker matching the start-family type,
score = min 2D distance (`sqrt(dx²+dy²)`, mission x/y) to any **pool-0 entity with `Flags & 0x100`
that is not the spawning entity** — and that flagged set **includes already-spawned players**, so
the second player to spawn scores the first player's marker low and a *different* marker wins
(this is how players spread across markers). `CPairList_AddEntry` collects (score, marker);
`CPairList_ShellSortByValue` orders; the farthest-from-enemy marker wins. **When the markers are
equidistant (a tie), each score is overwritten with `rand() >> 8 & 0xFFFF` before the sort**, so
ties break randomly. A mission authored with exactly one start marker places every player at that
single marker — i.e. **single-marker stacking is faithful** (the original has no further push-
apart); spread relies on multiple markers + the player avoid-set. The reimpl's `select_player_spawn`
already scores farthest-from-enemy over the start family but (a) its avoid-set must include spawned
players and (b) the `rand()` tiebreak was deferred. `[orig: Entity_FindBestSpawnPoint @0x50ccc0]`

**D-NET-116** [behavior, DOCUMENTED] **The pending-spawn loop gates on the mission-load flag.**
`CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0` runs only when `is_authority && !g_net_spawn_suspended &&
!g_spawn_success_gate`. `g_net_spawn_suspended` (0x24D1DE0, formerly `dword_24D1DE0`; renamed in the
2026-06-27 grill, D-NET-117) is the mission-LOADING-in-progress flag (written throughout
`Game_StartMission @0x524360`); the server does not process pending spawns until load completes and
the pool-3 start markers are promoted — so the placement scan always has markers to choose from. (The
same function also holds the spawn-time team-BALANCE gate — `CNapiServerConfig_BuildFlags & 0xF0`,
`Server_CountPlayersOnTeam`, the ratio test — skipped for co-op, `& 0xF0 == 0`.) The reimpl maps
`g_spawn_success_gate -> spawn_success_gate` but has no `dword_24D1DE0` equivalent; unhit today because
every caller (the npruntime tests and the not-yet-wired host driver) wires `ctx.world` AFTER the world
is loaded with its markers. A production driver that wires `ctx.world` DURING load must add a
load-complete gate, else the idempotent origin fallback latches the player at (0,0,0).
`[orig: CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0]`

**D-NET-117** [reimpl divergence, **FIXED 2026-07-22**] **World-path pose look-pitch is sourced from
the bound `AiEntity`.** The legacy `pose_from_session` fills `HostJoinerPose.pitch` from
`gss.client_pitch`, the signed HIGH 16 bits of the BAM32 look-pitch (the wire/0x0C likewise uses
`ae.pitch >> 16`). The World path now resolves `ctx.world->ai->for_handle(owned_entity)` and copies
that same high word. It deliberately does not read `world::Entity::pitch`: that field is unset for a
net-snapped remote peer and narrowed to the LOW 16 bits for the local player. When the World entity
has no AI peer, the pose retains its prior zero pitch; when the live World/entity path is unavailable,
the existing pre-spawn/session fallback is unchanged. This field rides only the in-process
PeerSpawned/F3 event and does not change the 0x0C/0x20/0x0A wire. The
`npruntime_initial_state_burst` regression pins both lifecycle events to a nonzero `AiEntity` pitch
high word. `[orig: pose_from_session gss.client_pitch; entity_wire_bridge ae.pitch >> 16]`

No IDB changes this grill (all fields/functions already named in §5.41/§5.42); read-only.

### P4 `Server_TickUpdate` host-loop review (2026-06-27)

A correctness review of the P4 commit (`f61ee3fd`) cross-checked the reimpl host loop against the
witnessed frame (`Server_TickUpdate @0x51d7e0`, gated at its call site by `Game_ProcessMainFrame
@0x5263f0 @0x5266b4`). Two load-bearing gaps were FIXED in the follow-up; five lower-severity
divergences are tracked as deferrals. IDB-only note: the host tick is gated by `is_authority` (+0x60)
at its CALL site (not `is_in_session`), and the raw recv/send pumps (`CNapiNetwork_PumpServerProtocol
Recv/Send`) are NOT `is_in_session`-gated — only the replicate/broadcast blocks inside the tick are.

**D-NET-119** [behavior, reimpl divergence FIXED] **The C2S 0x0C drain enforces the per-connection
owner gate.** The original resolves the wire handle (`pool<<12 | slot`) to an entity and verifies
`entity == *owner_ctx` (@0x4d6b08) — `owner_ctx` is the connection's authorized entity (`connCtx+0x160
-> +0xC0 -> *`, built + null-checked by `NapiNPServerMsg_0x00C @0x501c30`) — before invoking the
`entity_def+356` read-apply callback. On mismatch it falls through to `return 0` (@0x4d6b7e): silent
no-op, no apply/reject/disconnect. The reimpl `drain_connection_c2s` previously applied the wire handle
with only a local-player refusal, so any peer could SNAP another peer's entity by naming its handle.
Fixed: `drain_connection_c2s` now takes the `Connection` and skips any uplink whose handle
`!= conn.owned_entity` (an invalid owner matches nothing, mirroring the original's `owner_ctx != null`
guard @0x4d6ad3). `[orig: dispatch_entity_packet_callback @0x4D6A80]`

**D-NET-120** [behavior, reimpl divergence FIXED] **The S2C 0x0A replicate fan is is_in_session-gated.**
Inside `Server_TickUpdate` the replicate/broadcast blocks each read `is_in_session` (+0x58) as an inner
gate (@0x51d9ab..0x51e3f3); the raw recv/send pumps are not — they run on active-connection only. The
reimpl gated nothing on `is_in_session`, so a World kept alive past match-end (is_in_session flips to 0,
world stays non-null) would keep fanning 0x0A. Fixed: step (3) (snapshot + emit) is wrapped in
`if (ctx.is_in_session)`; the C2S drain + logic tick stay unconditional, faithful to the witnessed
structure. The whole tick is gated at its call site by `is_authority`, not `is_in_session`. `[orig:
Server_TickUpdate @0x51d7e0; Game_ProcessMainFrame @0x5266b4]`

**D-NET-121** [reimpl deferral, DOCUMENTED] **`fallback_anchor = {}` is a non-zero (dvxi5) anchor.**
`PlayerReplicationState` default-constructs `spawn_x/y/z` to the hardcoded dvxi5 map-center coords
(`0xfe56f854/0x0049f5f0/0x003a5e6a`), team=1, mi=0x3CDE — not origin. A spawned connection that reaches
the emit step with no resolvable owned entity (the host's own loopback, or a peer despawned mid-match)
anchors its 0x0A there, so a receiver decompresses every entity offset by the gap to the real local
position. Today the only caller is the golden test (which binds an owned entity); revisit before a
production driver fans to an owned-entity-less connection. `[orig: replication_model.h dvxi5 defaults]`
**RESOLVED (P5, §5.44):** the host's own loopback binds `owned_entity` to the host player
(`Server_BuildPlayerInfoAndAdd`), so it never anchors on the dvxi5 fallback; the fallback now bites only
the no-owned-entity edge (a despawn mid-match), still deferred.

**D-NET-122** [reimpl divergence, DOCUMENTED] **The 0x0A fan is gated on `burst.spawned`.** This narrows
the legacy `NetSystem::emit_s2c`, which emits to every transport-bearing connection (its host loopback
has no burst field). When `Server_TickUpdate` replaces `NetSystem` as the host/SP driver (P5), a
loopback whose `burst.spawned` never latches would be starved of its 0x0A (frozen local view). Revisit
the in-match predicate (shared verbatim with the drain loop — a single `is_in_match(conn)` helper) at
P5. `[orig: NapiNPServer_SendFiltered @0x4C87E0]`
**RESOLVED (P5, §5.44):** the drain + emit loops now share the single `is_in_match(conn)` predicate, and
the host loopback latches `burst.spawned` via its §5.2a burst completion (D-NET-114) like any joiner, so
it gets its per-frame 0x0A.

**D-NET-123** [reimpl deferral, DOCUMENTED] **`Server_TickUpdate` owns the logic tick.** It calls
`world.run_logic_tick(true)` itself — the inverse of the legacy seam, where the C2S drain ran INSIDE
`run_logic_tick` (NetSystem as a World ISystem driven by the host's existing `run_logic_tick` call). A
P7 binding that migrates to `Server_TickUpdate` but keeps its own `run_logic_tick()` advances the sim
(and drains the C2S queue) twice per frame. Enforced only by the header guardrail comment today
(D-NET-125). `[orig: net-before-logic, Game_ProcessMainFrame @0x5263f0]`

**D-NET-124** [reimpl deferral, DOCUMENTED] **The drain/emit fan assumes type-1 nodes stay resident.**
`Server_TickUpdate` walks `np_protocol.connection_list` for both the drain and the emit; a mid-match
`configure_session_runtime()` erases every type-1 (remote-joiner) node, which would silently drop those
peers from replication for the rest of the round. No reconfigure path calls it mid-match today; revisit
when round-restart / re-invoke lands. `[orig: tick_connections connection_list residency]`

**D-NET-125** [reimpl guardrail, DOCUMENTED] **The single-drain / single-tick invariant is comment-only.**
Nothing in code prevents a binding from both registering a `netsim::NetSystem` ISystem and calling
`Server_TickUpdate` (`ctx.net` stays a settable `NetSystem*`); whichever runs first drains the C2S queue
and the other sees nothing, with no compile- or run-time signal. P7 folds the tables onto one transport
and removes `ctx.net`; until then the guardrail is the header comment on `Server_TickUpdate`. `[orig:
ADR 0011 single-owner connection table]`

### 5.44 `Client_ProcessNetworkFrame` — the per-frame client net role (P5, 2026-06-27)

The client counterpart of `Server_TickUpdate` (§5.42 / P4). Witnessed `[orig: Client_ProcessNetworkFrame
@0x42c180]`, called by `[orig: Game_ProcessMainFrame @0x5263f0 @0x526692]` — **no args, NOT
authority-gated** (it runs on every machine: the SP listen-server host-as-client AND a remote client),
positioned **after `Input_ProcessFrame @0x52661d` and before `Server_TickUpdate @0x5266b6`**. The raw
recv into the FIFO happens earlier in the frame via `[orig: CNapiNetwork_PumpManagerReceive @0x4c4d10
@0x526528]`. Drives `libs/npruntime`'s `client_runtime.{h,cpp}` (`np::ClientRuntime`, P5).

**Frame order (the witnessed structure):**
1. `[orig: Player_UpdatePerFrame @0x42c18e]` (skipped when `dword_A87050`, the cinematic/pause flag).
2. **recv pump** `[orig: CNapiNetwork_PumpClientProtocolRecv @0x42c228]` → `[orig: NapiNPProtocol_Pump
   @0x62a650]` with **flags 26**, 250 ms — drains the recv FIFO and dispatches each S2C by opcode to the
   per-tag `NapiNPClientMsg_*` handlers (e.g. the `0x0A` fold, §5.9).
3. periodic housekeeping: `0x34` keepalive (~29760 ticks), `0x4C` anti-cheat (310 ticks,
   `is_mp_session_peer`), `PlayerSlot` type/subtype sync (62 ticks), terrain colour ramps.
4. **send block**, gated `if (np_connection)` and `if (!np_connection->send_holdoff_countdown)`:
   - `[orig: Player_PackInputStateToEntity @0x42c3e9]` — writes raw input to `entity->pad7[12]`; runs
     for **everyone, the host included** (the ADR-0012-R1 motor-from-raw-input path, §5.38).
   - if `is_in_session && !is_authority && !dword_81474C && !g_spawn_success_gate`: a `0x2C` RTT
     timestamp ping (`GetTickCount`; body `[u32 ts][u8 0x01]` via `NetPacket_WriteInt32AndByte`, the
     `0x01` echoFlag requests the S2C `0x57` pong) **and** the C2S `0x0C` uplink — `[orig:
     Player_BuildTag0CInputBody @0x42a550]` (`sub_op = 0x0A` extended) → `[orig:
     CNapiNetwork_QueueReliableMessage @0x4c4fa0]` `(0x0C, …)`. **Correction (P6 grill 2026-06-27):** the
     `0x2C` is **NOT** 62-tick throttled — `g_tag2CSendCooldown` (`dword_A860D8`) is set to 62 here and
     self-decremented at `@0x42c386`, but the complete
     `Client_ProcessNetworkFrame @0x42C180` control-flow audit shows no compare
     against it. After the field-3 holdoff gate `@0x42C3DD`, the
     deployed/non-authority branch unconditionally writes 62 `@0x42C412`, calls
     `GetTickCount`, and queues tag `0x2C @0x42C44A` immediately before building
     the per-frame `0x0C`. The only xrefs are the decrement and set. Thus `0x2C`
     fires **every deployed send frame**; the 62 counter is vestigial/telemetry
     state in this binary, not a cooldown gate. Consecutive-frame runtime
     coverage pins this behavior.
   - **flush** `[orig: CNapiNetwork_PumpClientProtocolSend @0x42c4bc]` → `NapiNPProtocol_Pump` with
     **flags 738**, 250 ms.

**The decisive gate — the `0x0C` uplink is `!is_authority`.** The SP listen-server host (mode 3 =
host + client, §5.0) is `is_authority == 1`, so it **never uplinks its own player**: its player is a
server-side entity driven by `Player_PackInputStateToEntity` + the motor under `Server_TickUpdate`
(ADR 0011/0012). Only a non-authority client layers the `0x0C` pose uplink on top. So the frame
invariant is **recv/fold first, then raw-input pack, then send-C2S, then flush** — and the reimpl
`ClientRuntime` mirrors exactly that order.

**Gate polarity (resolved).** `g_spawn_success_gate` (`dword_24C1928`) is **SET** on death/spectator
(the per-frame `0x0A` `flags1 & 0x01`, §5.9) and at spawn-select (`0x1D`, §5.2), and **CLEARED on
deploy** — so `!g_spawn_success_gate` means **deployed/alive**, and the `0x0C`/`0x2C` sends flow only
while deployed. `dword_81474C` is the companion respawn/loading-wait latch (set by `[orig:
Game_InitNewRound @0x422740]` and the `0x0F` world-state-load handler `[orig: NapiNPClientMsg_0x00F
@0x42e200]`; cleared as the round comes up). `[orig: reads @0x42c3bb/0x42c40a/0x42c467 +
0x42c402/0x42c45f/0x42c4a8]`

**Inner-message framing (the `0x0C` byte format).** `QueueReliableMessage` drops its `flags` arg and
calls `[orig: CNapiNPConnection_QueueMessage @0x628640]` → `[orig: NapiNPMessage_Create @0x627fc0]` with
`msg_flags = 0`; the latter computes `len_field_size` = **3 for payloads 1..255** (the LEN8 inner-message
flag `0x20`), 4 for >255 (LEN16), 2 for empty — and `msg_flags = 0` means **no SKIP1/SKIP2 reliability
bytes** in the inner wire framing. So a 48-byte `0x0C` (5-byte sub-header + 43-byte extended body, §5.10)
serializes as `[0x20][0x0C][48][payload]` — exactly what the reimpl's generic
`make_protocol_message(0x0C, payload)` produces (§3 inner-message layout). This is the byte-witness that
makes full client-emission parity achievable.

**Reimpl shape (P5).** `np::ClientRuntime` composes the connect-leg state machine (`np::JoinerConnection`,
P2: Idle → Hello → Auth → Driving spawn-gate burst → InMatch; the lobby GATE → VERIFY → READY → PLAY
legs are the SEPARATE ADR-0010 `ClientSession` matchmaking flow, owned by the binding, NOT this runtime)
with the S2C → `ClientState` fold (`netsim::NetClientView`). It is role-aware, mirroring the
not-authority-gated original: **Joiner** (a remote client — drives the legs + per-frame folds S2C and
emits the `0x0C`) and **HostClient** (the SP host's own loopback view — handshake-less, `is_authority`,
recv-fold only, `0x0C` suppressed). The two framing layers are kept strictly separate (the P5 design
review): a Joiner's traffic is whole NWU-framed datagrams (`JoinerConnection` does the `0x83`/SCRK decode
and surfaces inner bodies, folded via the new public `netsim::NetClientView::apply(tag,body)`); a
HostClient reads inner `{tag,body}` off the in-process loopback via `NetClientView::pump` (the ADR-0011
§3 SP crypto bypass). The witnessed housekeeping (the `0x34`/`0x4C`/`0x2C`-RTT sends and the
`send_holdoff_countdown` send-block gate) is **PORTED at P6** onto the Joiner role (a new public
`JoinerConnection::frame_inner(tag,body)` rides the same `0x43`/SCRK envelope/seq as the `0x0C`):
`0x34` (29760-tick, both roles), `0x4C` (310-tick, Joiner in-match), `0x2C` (every deployed frame, per the
correction above). The `0x34`/`0x4C` producers execute before the holdoff test and queue their
messages, while `0x2C` is produced inside the send block; the actual
`PumpClientProtocolSend @0x42c4bc` transmission boundary is gated by
`send_holdoff_countdown_` (default 0 = open). A due `0x34`/`0x4C` therefore remains queued across
held frames and flushes at the first open boundary—it is neither transmitted through the holdoff
nor discarded. The client now consumes
the high-table CS-config direction-1 field-3 update into that countdown and decrements it while the
whole semantic send block is closed, matching `NapiNPConnection+0x648` /
`GetSendHoldoffTicks @0x4C4AB0`. Queued semantic messages flush through MTU-bounded
`frame_messages` batches at the send boundary, so the every-frame RTT ping no longer forces its own
datagram beside the `0x0C`. `seed_session` sets a
`replay_mode_` that suppresses the housekeeping so a seeded golden replay reproduces only the captured
`0x0C` byte-for-byte. HostClient's own-loopback housekeeping stays deferred-and-logged (recv-only, no
outbound seam). `Player_PackInputStateToEntity` (step 4a) is the host-side motor seam (ADR 0012 R1), not
part of the headless client (which takes the already-built `0x0C` body as its per-frame input).

**D-NET-126** [behavior, reimpl divergence FIXED] **The production `PeerC2SInMatch` consumer routes a
joiner's C2S `0x0C` into the host drain.** `handle_client_session` only *surfaces* `PeerC2SInMatch`
(P4 left an inline apply as a dead-code / double-apply trap, D-NET-125); P5 adds `np::apply_in_match_c2s`
at the owner boundary, which `deliver_c2s`-injects each decoded in-match `0x0C` onto its owning
connection's transport so the NEXT `Server_TickUpdate` `drain_connection_c2s` read-applies it (the single
drain). A new `netsim::ISessionTransport::deliver_c2s(tag,body)` (impl on `LoopbackChannel` +
`UdpSessionTransport`) is the uniform inbound-inject the consumer needs (a host endpoint's `client_send`
stages OUTBOUND and never reaches `host_recv`). `[orig: NapiNPServerMsg_0x00C @0x501c30 →
dispatch_entity_packet_callback @0x4d6a80]`

**D-NET-121** and **D-NET-122 — RESOLVED (P5).** The two P4 deferrals about the host's own loopback are
closed together. `Server_TickUpdate`'s C2S drain and S2C `0x0A` fan now both gate on the single
`is_in_match(conn)` predicate (`napi_np_connection.h`; == `burst.spawned`) instead of an inline
`burst.spawned` in each (the D-NET-122 ask). The host's own type-2 loopback latches `burst.spawned` the
SAME way a remote joiner does — `tick_connections` drives its §5.2a initial-state burst to completion
(D-NET-114) — so it is no longer starved of its per-frame `0x0A`. Its `0x0A` anchors to its
`owned_entity` (the host player, bound by `Server_BuildPlayerInfoAndAdd @0x51d560`, §5.43), NOT the
D-NET-121 dvxi5 `fallback_anchor` (which is reached only by an in-match connection with no resolvable
owned entity — a deferred edge that no longer includes the host loopback). Verified by the host-as-client
section of `npruntime_client_runtime` (anchor == host player position, explicitly `!=` the dvxi5
fallback).

**Evidence.** `npruntime_client_runtime` (always-on): the full in-process round-trip — `ClientRuntime`
↔ the real np server legs ↔ `Server_TickUpdate` + `apply_in_match_c2s` + the `NetClientView` fold
(handshake → spawn-gate burst → name-match → per-frame `0x0C` → drain/SNAP → `0x0A` → `ClientState`),
plus the host-as-client D-NET-121/122 anchor path. `npruntime_golden_client` (env-gated
`NW_GOLDEN_GAMEPLAY`, skip-clean): against `retail-gameplay-session.pcapng`, the real client emission
path (`frame_c2s_uplink`) reproduces the captured `0x0C` **inner message byte-for-byte** (flags +
sub-header + 43-B body), our framing re-frames the captured 9-message bundle into a **byte-identical**
datagram (`0x43` header + SCRK + NWU/CRC), and the first S2C `0x0A` anchor lands **0.87 world units**
from the nearest world-stream spawn (a non-circular cross-decoder oracle). No IDB renames this session
(symbols already named); a summary witness comment was added at `Client_ProcessNetworkFrame @0x42c180`
and `@0x42c482`.

### 5.45 P8 reactive-reply gate — the `game_session.cpp` reply machine vs the witnessed serializers (2026-06-27)

"Grill-the-gate" pass before retiring the legacy in-match glue (`libs/novaworld/game_session.cpp`
+ `game_server_runtime.cpp`, npruntime P8): confirm the *structure* of the gameplay-layer reactive
reply handlers the reimpl drives through `ctx.game_runtime` so they can be ported onto `npruntime`
off `game_runtime`. Verdict: **reply structure (recv-tag → reply-tag) MATCHING; reply BODIES are
captured-from-observation fixtures** that diverge from the witnessed serializers (catalogued below
as the deferred body-grill-wave targets). Read-only; no IDB renames (handlers already named).

**Witnessed reactive-reply serializers** (the bodies a faithful port emits; the reimpl carries its
captured-from-observation fixtures verbatim through P8, a tracked divergence — never invented bytes):

| recv | handler | reply | witnessed serializer(s) | reimpl fixture (`game_session.cpp`) |
|---|---|---|---|---|
| 0x02 | `NapiNPServerMsg_0x002 @0x512FD0` | 0x01, 0x7A, 0x7B, 0x03 | `0x01`=dword `1`; `0x7A`=`NetPacket_WritePCID @0x5076e0` (player+0x250 PCID); `0x7B`=`NapiNPMsg_0x7B_BuildPayload @0x507740` (name, PCID, serverName, title, mapFile, gametype u32, "", expansion); `0x03`=`NetPacket_WriteWeaponRestrictionFlag @0x502ac0` (sets game-state 7) | **0x7A/0x7B now FAITHFUL ports (D-NET Wave 1.5, 2026-06-27)** — `build_tag7a_pcid`/`build_tag7b_session_summary` source the PCID field (was: 0x7A wrote player_name; 0x7B's PCID slot held an invented `"DEV-A02-0001"` literal). reimpl still emits extra `0x00`×2 + `0x05` + `0x04` not from THIS handler (owner-attribution TODO) |
| 0x22 | `NapiNPServerMsg_0x022 @0x514C90` | 0x46 | `NetPacket_SerializePlayerSync0x46 @0x505e80` (reads `[u8 slot][u16 fieldFlags]`, `slotPtr[25146*slot]`) | `build_tag_46_player_sync` |
| 0x29 | `NapiNPServerMsg_0x029 @0x514F10` | 0x51 | `write_entity_packet @0x506bb0` (`CBufferList_GetAtIndex(g_team_change_entity_list, idx)`; gated `is_authority && !g_net_spawn_suspended && !g_spawn_success_gate`) | `build_tag_51_player_spawn` (8 B) |

**D-NET-127** [reimpl divergence, DOCUMENTED] **The reactive §5.1/spawn-confirm reply bodies are
captured-from-observation, structurally faithful but byte-divergent from the witnessed serializers.**
`game_session.cpp`'s `build_tag02_push`/`build_tag7b_session_summary`/`build_tag60_server_info`/
`build_tag64_mission_metadata` and the reply-builder `build_tag_46`/`build_tag_51`/`build_tag_5a`
fixtures, plus the `0x0E`→`add_game_start_bundle` (`0x5A×2/0x42/0x0A/0x0F/0x4D/0x61/0x3E/0x40…/0x6F…/
0x6E/0x57/0x4E/0x58/0x5D/0x4C`), reproduce the *expected reply tags in the witnessed order* but with
captured bytes. The witnessed join burst is **leaner** — `Server_OnPlayerJoin @0x51a680` (§5.43,
D-NET-114) emits only `0x42`(`NetPacket_WriteInputStateFlags @0x505ba0`) → world-stream
(`Server_SendEntityStateToPlayer @0x517ba0`) → `0x0F`(`NetPacket_WriteWorldStateLoad0x0F @0x502d10`) →
`0x4D`(player index byte) → seed (`Server_SendRandomSeedToPlayer @0x5101a0`, `0x61`) → `[0x14`
cease-fire `NetPacket_WriteTwoBytesAndCString @0x5047a0]` → `[0x1D` weapon overlay
`WeaponOverlay_SerializeToBuffer @0x505280` + game-state 11, when in-progress`]` → `0x3E`(empty
terminator). **P8 moves the reply machine to `npruntime` carrying these fixture bodies verbatim (zero
wire regression); the faithful per-body port — emitting the serializers cited above — is the deferred
grill wave** (mirrors the §5.2a serializer wave that P3–P6 left deferred). `[orig: Server_OnPlayerJoin
@0x51a680; NapiNPServerMsg_0x002 @0x512FD0; NapiNPServerMsg_0x022 @0x514C90; NapiNPServerMsg_0x029
@0x514F10]`

**D-NET-127 UPDATE (2026-06-27, D-NET Wave 2 partial).** The §5.2a serializer wave this entry cross-references is CLOSED (Wave 1 / §5.2a serializer-grill, MATCHING). For the reactive-reply bodies: the `0x7A` PCID and `0x7B` session-info bodies are now FAITHFUL ports (the invented `"DEV-A02-0001"` literal removed; `0x7A` was wrongly writing the player name instead of the PCID — golden frame 134 proves len 1 / empty). The witnessed field maps for `0x46` (`NetPacket_SerializePlayerSync0x46 @0x505e80` — `[u8 type][u16 fieldFlags]` then per-bit fields: 0x1=name, 0x2=team-string, 0x4=score@slot+416, 0x8=damage/alert, 0x10=vehicle-name, 0x20=score2, 0x40=squad, 0x80=side, 0x400=weaponType, 0x800=timer-dword, 0x1000=alert2) and `0x51` (`write_entity_packet @0x506bb0` — `[u16 header][u16 handle=pool<<12|slot][u8 team][u16 NetId-if-Flags&0x100-else-0][u8 animSlot-if-Flags&0x100-else-0]`) are now landed (no longer "unwitnessed"); they carry approximations pending slot-state / entity-handle modeling (the `0x46` flag-driven body reads ~10 slot/entity fields the headless host does not yet model; tracked, not invented). **0x51 layout FIXED (2026-06-27):** `build_reply_tag_51` now emits the witnessed `[u16 requested_index (echoed from C2S 0x29)][u16 handle][u8 team][u16 NetId][u8 animSlot]` (was wrongly `[team][handle][team][0][0]`); the index is sourced from the 0x29 payload. NetId/animSlot still default 0 pending the spawned entity's net_id/anim threaded into the reply binding. **0x46 confirmed structurally FAITHFUL (2026-06-27):** `build_reply_tag_46` already emits the witnessed flag-driven format — `[u8 slot][u16 fieldFlags][u8 entitySlot]` then the bit-gated fields in source order — and **round-trips through `decode_player_sync` (@0x431370, the client inverse)**. The invented `"A-A02-000000"` literal in field 0x10 (vehicle-name) is removed (empty for an on-foot player). The remaining gap is only per-field slot-state VALUES (score/squad/side/timer) for fields the headless host doesn't model — defaults, the wire SHAPE is faithful. Remaining D-NET-127 work (all LOW/cosmetic): the `0x46` per-field slot-state values, the `0x51` NetId/anim binding plumbing, and the `0x02`-handler extra `0x00`/`0x05`/`0x04` owner attribution.

**D-NET-127 UPDATE (2026-07-01) — the post-handshake `0x00`/`0x03`/`0x05`/`0x04` bodies and owner
attribution are now FULLY WITNESSED; the observation-carry for this burst is closed.** Owners: the
`0x03`/`0x05`/`0x04` trio is emitted by the spawn pump `CNapiServer_ProcessPendingPlayerSpawns
@ 0x4c8dc0` (not the 0x02 handler), in the witnessed order `0x03 → (Server_BuildPlayerInfoAndAdd
@ 0x51d560 → Server_PlayerAdd) → 0x05 → 0x04 → 0x7B`; the settings-flagged `0x00` pair is the NAPI
CS-config update `CNapiNPConnection_SendConfigUpdate @ 0x6286e0` (`[u8 direction==0][u32 bit
mask][u32 cs_dir value per set bit]` — ours sets field index 3 = 12 for both directions, matching the
golden). Bodies: `0x03` = `NetPacket_WriteWeaponRestrictionFlag @ 0x502ac0` (`[u8 1][u16 list-node
count][u16 weapon mask (inner+76)]` when restriction data exists, else `[u8 0]`); `0x05` =
`NetPacket_WriteBoolTrue @ 0x502c00` (exactly `{0x01}`); `0x04` = `NetPacket_WriteSlotAssignment
@ 0x502b30` (24 B: `[4×u32 netPlayer+60..72 stat dwords][u8 g_mode dword_24D2110][u8 player_slot
(slot+20)][u8 slot capacity @ 0x24c0ca4][u32 0][u8 team (slot+416)]`) — every byte of the former
24-byte fixture decodes field-for-field (slot 1, capacity 2, team 2 on the golden join). The reimpl's
`emit_post_handshake_burst` now builds `0x04` via `build_tag04_slot_assignment` (witnessed encoder;
slot/capacity/team parameters) instead of the opaque fixture. What is STILL open under D-NET-127:
the `0x46` per-field slot-state values and the `0x51` NetId/anim binding (see the `0x51` note under
D-NET-137 — the "NetId 0 is faithful" justification is superseded by the minimap-id witness).

### 5.46 C2S 0x0F entity-info query → S2C 0x18 FULL-ENTITY-SPAWN — the self-heal path (2026-07-01)

**The repair loop the retail-join DBuggy/0x0F-flood investigation surfaced.** When the client's per-frame
0x0A tail entity-chain cross-check finds a live entity whose local resolve disagrees with the wire —
`!itemDef || itemDef.id != wireType || ItemTypeIndex != ItemList_FindIndexByTypeId(wireType)` [orig:
`NapiNPClientMsg_0x00A @ 0x42FEC0`, cross-check @ 0x4307c4] — it queues **C2S 0x0F `[u16 handle]`** (one
per offending 0x0A frame). The server handler [orig: `NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180`]
gates on authority + a live session, reads the u16 handle (0 if len < 2), validates `pool <= 1 && slot <
g_pool_list[pool].capacity` (organics + vehicles only), resolves the POOL SLOT POINTER (no liveness check
— an empty in-capacity slot serializes as zeros), serializes it via `serialize_object_to_buffer @ 0x504d10`
and replies **S2C 0x18, msgClass 1, send_mask 0x20** (the requester only). The client handler [orig:
`NapiNPClientMsg_FullEntitySpawn @ 0x433780`] `Entity_Destroy`s + `memset(entity, 0, 0x2B4)`s the slot,
then — gated on wire `item_type != 0` @ 0x433b5a — FULLY REBUILDS it: `ItemTypeIndex =
FindIndexByTypeId(type_id)`, itemDef + death/update callbacks, `EntityDef_LoadModelsAndCallbacks`,
graphic/husk models, health/armor, the three entity links, and for `ItemType_Person` (3) the ADM load +
`AnimMap_InitBoneTypes` + `AnimMap_RegisterEntity @ 0x40bb60` + minimap slot allocation (Flags & 0x100,
`MinimapSlot_HasEntity`/`MinimapSlot_FindOrAllocByEntityId`), ending in `Entity_InitFromModel` + the
itemDef initCallback. A type-0 record (empty slot) therefore CLEARS the client's stale entity — destroy
semantics, observably identical to retail serializing a zeroed slot.

Field order (offsets float after the name; all LE), server source → client target:

| # | field | type | server source | client store |
|---|---|---|---|---|
| 1 | slot_id | u16 | recomputed from the entity ptr (pool scan) | `0xFFFF` ⇒ immediate return |
| 2 | item_type_id | u16 | itemDef+0x50 low16 (wire type id) | `FindIndexByTypeId` → ItemTypeIndex/itemDef |
| 3 | item_type | u8 | itemDef+0x5C (ItemDefType; 1=vehicle, 3=person) | 0 ⇒ stop after destroy+memset |
| 4 | team | u8 | entity+354 | Team low byte |
| 5 | minimap_flags | u16 | entity+36 low16 | Flags (bit 0x100 gates minimap registration) |
| 6 | entity_flags | u32 | entity+120 | ownerConnectionId |
| 7 | entity_name | cstr | entity+244 iff itemDef attrib & 0x100000, else `""` | copied iff LOCAL def attrib & 0x100000 (+ `Entity_AllocateAISlot`) |
| 8–10 | parent_vehicle / ground_entity / parent_entity | 3×u16 | entity+368 / +40 / +364 ptrs → handles (0xFFFF null) | parentVehicle / groundEntity / parentEntity |
| 11 | seat_mask + occupants | u8 + N×u16 | itemDef+604; entity+400+2i per set bit | mountHandles[0..7] (prefilled 0xFFFF) |
| 12–13 | mount8 / mount9 | 2×u16 | entity+416 / +418 | mountHandles[8]/[9] |
| 14–16 | pos x/y/z | 3×i32 | entity+4/+8/+12 (16.16 world) | Position |
| 17–18 | yaw_hi / pitch_hi | 2×u16 | entity+18 / +22 (BAM high words) | Yaw/Pitch = (i16)<<16 |
| 19–20 | ai_state / anim_slot | 2×u8 | entity+692 / +884 | aiState / animSlot |
| 21 | net_id | u16 | entity+348 (minimap id) | NetId (minimap alloc iff Flags&0x100 && !HasEntity) |
| 22 | player_class | u8 | entity+660 | playerClass |
| 23 | (zero) | u8 | hard 0 | discarded (cursor advance) |
| 24–26 | tail bytes | 3×u8 | entity+340 / +533 / +532 | +0x154 / refNum / subType |

**Why every healthy capture has zero 0x0F/0x18:** the loop only runs when an entity is broken; the
same-map retail↔retail golden (retail-ashi5a) carries none — which is why the early §5.7 read "does not
fire in normal multiplayer". It is nonetheless MANDATORY host surface: in the retail-join defect
(DBuggy-host-player + ~1000/session C 0x0F flood, 2026-07-01) the joiner's round-load re-resolve garbages
the REMOTE host player's itemDef (`Game_ReloadEntityModelsAndCallbacks @ 0x522830` re-resolves
`FindIndexByTypeId(AnimMap_GetSlotPropertyInt(playerClass@+0x294, camoProp))`; the camoProp switch has NO
default for lod_level ∉ {0,1,2} and class 0 reads unloaded charattr slot 15 → garbage → an unstable
pool-1 vehicle index) and the client then asks the host to repair it EVERY 0x0A frame — a host that
consumes the query silently strands the vehicle-resolved player entity forever. The flood starts exactly
at the deploy bundle (C 0x28/0x29/0x22/0x23 + the first 0x0F burst, capture ov-til45c f=65791), placing
the break at `Game_StartMission`'s reload, not at 0x0C application.

**Root cause CLOSED (2026-07-02): the sustaining loop was our field-17 byte (D-NET-138), witnessed
end-to-end.** Why each S2C 0x18 repair did not stick: the client's 0x0A person deserializer (case 2 of
`NetPacket_SerializePlayerState`) applies `Entity_SetHealthFromDifficultyByte @ 0x4AD580` for every
REMOTE player (`@ 0x4c11ba`; local skip `@ 0x4c11ac`), which writes `entity->playerClass = field17 &
0xF`, and then IMMEDIATELY re-resolves the entity's item from playerClass (`@ 0x4c1248-0x4c12be`:
`AnimMap_GetSlotPropertyInt(playerClass, lod-prop 0x0B/0x0A/0x0C)` → `ItemList_FindIndexByTypeId @
0x49E100` → overwrites `ItemTypeIndex` (+0x1C) and `itemDef` (+0x20) — the exact inputs of the
`@ 0x4307c4` cross-check). Our pre-fix raw-health byte `0x64` decodes as playerClass 4 (real class 8), so
the cycle was: 0x18 repairs (class 8, itemDef 0x14B9) → next applied 0x0A stamps class 4 and re-resolves
a wrong itemDef → the following 0x0A frame fails the cross-check → C2S 0x0F → S2C 0x18 → repeat (~2-frame
oscillation, 1,526 C 0x0F in the v10 session). The 0x18 record itself was exonerated: it fully
re-establishes `ItemTypeIndex`/`itemDef` from its u16 item_type_id (`@ 0x433bc7`/`@ 0x433bdf`); its
item_type byte is a nonzero gate only (`@ 0x433b5a`) and its net_id feeds only the minimap path
(`@ 0x433ddd`) — see D-NET-137/D-NET-138. With the packed byte (D-NET-138 fix), live retail-join v11
(full join+deploy+move, 4,774 S 0x0A / 395 C 0x0C) carries **zero** C2S 0x0F — matching the golden.
**Open lead:** a SECOND C2S 0x0F trigger sits inside the person read path itself
(`@ 0x4c107b-0x4c10ae`): if the record's mounted-vehicle handle resolves to an entity whose itemDef is
null, the client queues 0x0F for the VEHICLE handle and abandons the record — relevant once vehicle
mounting replicates.

**Re-grill 2026-07-01 (full-field verify vs fresh decompilation): MATCHING both directions.** Three
additions: (1) `serialize_object_to_buffer @ 0x504d10` takes FOUR args — the IDB's 3-arg prototype was
wrong (all 3 callsites clean up 0x10; fixed in the IDB); arg 4 is the serialized entity, arg 3 (the
requester's slot) is unused. (2) **S2C 0x18 has a SECOND emitter**: the C2S 0x40 vehicle-spawn handler
`[orig: NapiNPServerMsg_HandleVehicleSpawnRequest @ 0x51C4C0]` broadcasts the record (send_mask 0x90 =
alive + exclude-host) for the freshly spawned deployable AND every pool-1 entity sharing its refNum
(+0x215) — 0x18 is "full entity (re)spawn", not exclusively the 0x0F reply. (3) The client tests the
wire `item_type` byte ONLY against zero (@ 0x433b5a); the rebuild's person-vs-vehicle behavior comes from
the LOCAL itemDef's type (@ 0x433d6e), so any nonzero value is observably equivalent — D-NET-133's
former pool-derived approximation was provably safe before the exact def source landed. Also witnessed:
the 0x0F handler additionally gates on the
requester having a session player (`session+192` non-null) — our host replies regardless (safe direction;
noted in D-NET-133).

**Reimpl (2026-07-01, all 46 net+world ctests green):** `encode_full_entity_spawn` /
`decode_full_entity_spawn` (libs/npwire/ingame_{encode,decode}); `netsim::build_full_entity_spawn`
(entity_wire_bridge — shares the player wire rules with the 0x0C builder: per-recipient flags, minimap
net_id, playerClass clamp; D-NET-133 documents the narrow +340/dispatch residuals); npruntime dispatch `case 0x0F`
(server_message_dispatch.cpp — in-capacity empty slots reply the type-0 record;
`EntityRegistry::pool_capacity` mirrors the capacity gate); nw_pp `print_tag_18` + catalog row
`S 0x18 full-entity-spawn` (Decoded) and the C 0x0F rename `spawn-query → entity-info-query` (the old
name was the §5.9-era guess). Tests: `nw_ingame_encode` (55-B player-record layout / sparse seat block /
type-0 empty slot), `netsim_world_stream_extractors` (player wire rules + roundtrip),
`nw_message_coverage` (S 0x18 Decoded drift guard).

### 5.47 Server per-frame S2C 0x0A emit — phase counter + sub-block cycle + priority/budget entity loop (2026-07-01)

The authoritative host builds every recipient's `0x0A` in `Server_SendEntityStateToPlayer @0x517ba0`
(one call per connected player per frame): gate on the recipient's player-slot being active and
`state(+0x20) == 6` (deployed); set the priority reference `g_priority_ref_{x,y,z}` to the recipient's
EYE position (`entity.pos + camera_offset`, `entity[1..3] + entity[27..29]`); build the distance-sorted
priority list `Server_BuildEntityPriorityList @0x50e590`; write the header (`NetPacket_WritePlayerState`)
then the entity loop (`serialize_entity_states_to_packet`); send via `NapiNPServer_SendFiltered`
(mask `0xA0`, tag `0x0A`). New/stale recipients (`uptime > 2000` ticks) get `g_entity_send_budget >> 1`
for that frame — a ramp-up.

**Header + sub-block cycle** `[orig: NetPacket_WritePlayerState @0x4ff6b0]`. The header is
`[i32 ref_x][i32 ref_y][i32 ref_z][u8 state_flags][u8 phase_byte]`, where `phase_byte = playerSlot+100566`
is a per-player-slot counter incremented once per send (`++` at `@0x517be8`). `phase_byte & 3` selects
the header sub-block, so a free-running counter cycles all four evenly; `phase_byte & 0xF == 8` gates an
additional mounted-vehicle/turret tail every 16th frame:

| `phase & 3` | sub-block | 11/6/16/0-byte body |
|---|---|---|
| 0 | weapon/reload/uniform | `[u8 preround_timer][u8 slot360][u8 slot368][u8 slot364][u8 slot356][u8 slot460][u8 reload_seconds][u32 ZoneSlotChain_GetOwnedZoneMask @0x4a2620 (ex-CWeaponSlotManager_GetUniformTeamMask) — the deploy-map owned-zone mask, §5.61]` (11 B). slot+360/368 gated on entity+36 bit 1 (else 0). reload_seconds = `dword_25510F4 − (ticks since fire)/62` clamped to 0xFE, 0xFF = belt-fed special, 0 = idle (`@0x4ff8f0..0x4ff992`). The reimpl decode struct was renamed `FrameAimBlock` → **`FrameWeaponBlock`** (fields `preround_timer`/`slot_state360..460`/`reload_seconds`/`uniform_team_mask`) 2026-07-01 — nothing in this block is aim state. |
| 1 | server-status | `[u8 C6EAE0][u8 C6EAE4 fall-dmg tol][u8 g_serverFps][u8 g_serverCpuPct][i16 dword_24C1958/62]` (6 B) |
| 2 | environment | `[u16 word_26C6822 fog][u16 (FogDistAccelClamp+255)>>8][u16 (CurTimeFixed24+4096)>>13 tod][u8 quake][u8 cloud>>10][u8 dword_26C6880>>8][u8 OvercastBlend>>8][u8 dword_2C059D0]` (11 B) |
| 3 | gametype | 4×`i32` scores, **only if `g_GameType & 0x20000`** (`@0x4ffc2d`) — else 0 B |

Env scales witnessed against the decoder (`NapiNPClientMsg_0x00A @0x430244` case 2) and the golden:
`CurTimeFixed24 = hours × 2^24` (the day spans `0x18000000 = 24 × 0x1000000`,
`Environment_ComputeTimeOfDayColors @0x57de40`), so wire `tod = CurTimeFixed24 >> 13` and, for our
`EnvState.time_of_day` (hours × 2^16), `wire_tod = time_of_day >> 5` — verified by the golden ASH_I5A
value `todFixed=0x7905` (= 15.13 h). Fog is `wire_fog = fog_dist(16.16) >> 16` (client re-`<< 16`s it into
`Env_FogDistTarget`). Then the always-tail (non-local recipient): `[u8 health][u16 target_handle]
[u16 entity+286][u16 entity+288]` and, when `phase & 0xF == 8`, the mounted vehicle handle + turret words.

**Entity loop** `[orig: serialize_entity_states_to_packet @0x50f070]`. Walks the priority pairlist and, for
EVERY entity whose `itemDef` has a serialize callback (`itemDef+0x164` — players, vehicles, and AI alike),
emits `[u8 1][u16 handle][u16 type = *(itemDef+0x50)][compact body]`, then the projectile chain as
`[u8 2]…`, then a `[u8 0]` terminator. It stops once the packet reaches `g_entity_send_budget` bytes, so
each frame emits a distance-prioritized SUBSET and the priority cursor round-robins across frames — the
mechanism by which a retail host replicates dozens of vehicles/AI a few per frame rather than all at once.

**Reimpl status.** The header phase-counter + sub-block dispatch is ported
(`netsim::Connection::s2c_phase` = `playerSlot+100566`; `emit_connection_s2c` advances it;
`build_0a_frame` dispatches on `flags2 & 3`, `libs/netsim/src/connection_fan.cpp`). We cycle the SAFE
subset `{1 server-status, 0 weapon, 3 gametype}`. Phase 3 now faithfully emits the four authoritative
`World::subgoals` masks for objective game types and zero bytes otherwise; joiners learn the required
off-wire game-type gate from S2C `0x08`/`0x7B`. **Sub-block 2 (env) remains DEFERRED
(D-NET-134)** because our headless host does not yet author `world.env` (env is set only by WAC
`TOD`/`fogdist` commands, which ASH_I5A drives from its `.env` file, not on the netsim world) —
emitting it would OVERWRITE the client's correct mission-loaded sky. The passenger block
(`phase & 0xF == 8`) is likewise deferred pending vehicle-mount modeling. Entity-loop class dispatch,
priority, aging, budget, and round-robin are implemented and tracked under their own residuals; they are
no longer part of this phase-counter divergence. Verified by the objective codec/fanout/runtime tests
named in §5.9, `netsim_two_peer_fanout` (`run_0a_subblock_phase_cycle`), and
`scripts/net/diff_0a.py`.

**Player-record health byte is PACKED, not raw health (grill 2026-07-01; pack witnessed + ported
2026-07-02, D-NET-138 FIXED).** The §5.10 player compact record's field-17 byte decodes client-side
via `Entity_SetHealthFromDifficultyByte @ 0x4AD580`: low nibble → `entity->playerClass` (+660), bits
4–5 → a health TIER scaled off `itemDef->healthMax` (tier 0 ≈ 21.9 %, 1 ≈ 59.4 %, 2 ≈ 87.5 %; 16.16
multiplies 28671/49152 with 0x8000 rounding). The LOCAL player skips the apply (`@ 0x4c11ac`), so a
wrong byte only affects how REMOTE players render — and, worse, re-breaks them: the read path
re-resolves the item FROM playerClass right after the apply (`@ 0x4c1248`; the §5.46 0x0F-flood
re-break). The server-side pack is `Entity_GetHealthClassification @ 0x4AD4E0` (case-1 write call
`@ 0x4c0d71`): `(tier << 4) | (playerClass & 0xF)`, tier boundaries `ratio > 49152` / `> 28671` in
16.16 of `Health / max(healthMax, 1)` — ported as `netsim::health_classification_byte`
(`build_0a_frame` now sends the packed byte; boundary tests in `netsim_two_peer_fanout`).

### 5.48–5.56 The 2026-07-01 wire-coverage sweep — session/HUD state channel (decoded)

One grill wave took every tag the retail↔retail goldens carry that `nw_pp` could not yet name or decode
— after it, **all three goldens decode with zero unnamed tags in both directions** (`nw_pp --histogram`).
Reimpl: `libs/npwire/ingame_decode.{h,cpp}` (`decode_session_status` / `decode_zone_timer_value` /
`decode_zone_timer_window` / `decode_play_sound` / `decode_mission_map_names` / `decode_chat_uplink` /
`decode_chat_broadcast` / `decode_session_slot_config` / `decode_session_config` /
`decode_join_padding_probe` / `decode_loadout_submit`), nw_pp printers + catalog rows, pinned by
`nw_message_coverage` (crafted-body consumption + Decoded-set drift guard).

**§5.48 S2C 0x58 SESSION-STATUS** `[orig: NapiNPClientMsg_SessionStatus @ 0x4228C0 →
SessionStatus_ParseFromBuffer @ 0x530ED0]` — the old "texture loader" naming was wrong (functions renamed
in the IDB). Wire: `[cstr serverName (≤31 kept)][cstr missionTitle (≤63 kept)][u8×3][u32 uptimeMs]
[39×i32 statPointValues][u8 kvCount][(u8 key≤9, u32 val)×N]`. Parsed into the single global
`g_session_status @ 0x24E3E88`: serverName feeds the end-game STROVER_SERVERNAME line, uptimeMs is
elapsed-at-send (client stamps GetTickCount at parse; `CSessionTimer_GetElapsedMS` = wire − parseTick +
now — the admin-status "UP-TIME"), the 39 i32s are the STROVER_STATVAR00..38 per-stat score rules read by
`Overlay_BuildEndGameStatsText @ 0x54A240` via `SessionStatus_GetStatPointValue @ 0x52D5D0` (index ≤ 0x26,
reads block[30+i]). Wire tolerances witnessed from the golden: kvCount may exceed the pairs present
(reader is bounds-tolerant, missing pairs read as zeros @ 0x531055), and the body may carry trailing
bytes the parser never reads (golden: 5 zero bytes; the dispatcher never requires full consumption).
Golden: server="Untitled", mission="TD - Dormant Volcano Isle", uptime=45063 ms, 8 nonzero stat values.

**§5.49 S2C 0x6F / 0x53 ZONE TIMERS** — NOT cinematic-camera messages (that §4 label and the kong
"CTerrainRenderer color ramp / CColorGradient" names were wrong; renamed `ZoneTimerList_*` in the IDB).
Both program per-zone-entity timer entries in `g_zone_timer_list @ 0x24E41B0` (13-dword entries keyed by
entity ptr, count at +0x1A00), advanced every client frame by `ZoneTimerList_AdvancePerTick @ 0x537D60`
(value += rate, clamp [0, limit]) and consumed by the capture-point HUD — `HUD_DrawTakeoverStatus
@ 0x59B630` draws the entry returned by `find_nearest_entity_in_proximity_list(g_zone_timer_list,
&player.Position, …)`; `draw_capture_point_detail_panel` / `render_capture_point_labels` / the
death-screen overlays read the same list.
- **0x6F ZONE-TIMER VALUE** (15 B) `[orig: NapiNPClientMsg_ZoneTimerValue @ 0x428D60 →
  ZoneTimerList_SetEntryValue @ 0x537EC0]`: `[u16 zoneHandle][u8 mode][i32 value][i32 limit][i16 rate]
  [u8 → entity+544][u8 → entity+545]`. value/limit are 16.16-fixed SECONDS, scaled ×62 into tick-fixed
  by the handler (golden: 1.0 s for owned zones, rate 0, targets pool-1 zone entities). Also maintains
  the tracked nearest-zone accumulator cluster `dword_A85BA4..BB8` (advanced per frame in
  `Client_ProcessNetworkFrame @ 0x42C34F`).
- **0x53 ZONE-TIMER WINDOW** (9 B) `[orig: NapiNPClientMsg_ZoneTimerWindow @ 0x428AE0 →
  ZoneTimerList_SetEntryWindow @ 0x537DE0]`: `[u16 zoneHandle][u8 modeA][u8 modeB → entity+547]
  [u16 start_s][u16 end_s][u8 rate]`, start/end ×62 s→ticks; tracked cluster `dword_A85B88..BA0`; a new
  nearest zone is only adopted within 20.0 world units (1310720 in 16.16, @ 0x428cf5).
  `ZoneTimers_ResetState @ 0x4244A0` (ex-"CineEditor_ResetState") zeroes the list count + sync globals
  at mission start.

**Runtime fold ported 2026-07-29.** `ClientRuntime` now retains the shared
13-dword semantic entry rather than only the latest decoded packet. A new
0x6F entry seeds value-current from wrapping `62 × value`; an existing entry
keeps its current, while both forms replace value-target/limit/rate, activate
the value channel, and disable the window channel. A new 0x53 entry seeds the
window current; an existing entry resets it only when `modeB` changes. It
activates the window channel, disables the value channel, and clears the value
target/limit without clearing its current. Mixed 0x53/0x6F records stay in wire
order for a joiner, the HostClient loopback uses the same semantic fold, and
the list advances exactly once after the complete receive pump with wrapping
ADD followed by signed high-then-low clamps
`[orig: Client_ProcessNetworkFrame @ 0x42C2E1..0x42C2E6;
ZoneTimerList_AdvancePerTick @ 0x537D60]`.

The same entry now feeds the numbered-zone model callback's
`LFP_CAMPPERCENT`: no entry means no register store; a zero limit writes
`0x10000`; otherwise the callback truncates
`signed current / signed limit × 65536`
`[orig: BoneCallback_gnrc_World @ 0x4E2860; store @ 0x4E28C4]`.

**§5.50 S2C 0x34 PLAY-SOUND** `[orig: NapiNPClientMsg_PlaySoundByName @ 0x4283A0]` — the kong name
"GotoTeleport" was a misnomer (renamed). Wire: `[u8 flag][cstr soundProfileName]` + (flag==1 only)
`3×i16 pos`, each `<<16` into 16.16 world space. flag 0 → flat play; flag 1 → positioned 3D one-shot at
full volume (`Entity_PlaySound3D_FullVolume @ 0x528E20` against a zeroed temp entity). Gated
`is_mp_session_peer`. Resolver: `SoundProfile_FindLoadedByName @ 0x5274F0`.

**§5.51 S2C 0x2C SESSION + MISSION-FILE NAMES** `[orig: NapiNPClientMsg_MissionMapNames @ 0x427E10]` —
the old "chat entry" note was wrong (chat-history is 0x2A). Wire: `[cstr sessionName → byte_A82378]
[cstr bmsFileName → g_map_file_name @ 0x24D1F3E]`; bumps `g_loading_progress` to ≥ 1; gated
`!is_authority`. Golden: "Untitled" + "TDH_I5A.BMS".

**§5.52 CHAT (C2S 0x0D → S2C 0x14)** — the C2S table's "replication frame ACK" note for 0x0D was WRONG:
`[orig: NapiNPServer_HandleChatMessage @ 0x513760]` is the chat uplink `[u8 channel][cstr text]`. The
server strips `<...>` tags, rate-limits 1000 ms/player (+100360), prepends the player name (+ squad tag
via CLinkedList_FindByTag), and fans the formatted line out as **S2C 0x14** `[u8][u8][cstr]`
(`NetPacket_WriteTwoBytesAndCString @ 0x5047A0`, send_mask 0x20 per recipient) with channel routing:
2 = team (+354 match; also appends the nearest pool-3 type-2044 marker name as a `:[<location>]` tag),
4/5 = side 2/1, 11/12 = squad/commander, 13 = proximity ≤ 0x640000 (100.0 world units) per axis,
default = all. Client receive: `[orig: NapiNPClientMsg_ChatMessage @ 0x42F240]` →
`Chat_DispatchToChannel @ 0x42B910`.
**§5.52a** C2S 0x0A SPAWN-MENU REQUEST (len 0) `[orig: NapiNPServerMsg_HandlePlayerSpawnRequest
@ 0x513260]`: game state → 9 (spawning), session+32 = 4, replies **S2C 0x19** = `[u32 timestamp]`
(NetPacket_WriteTimestampB) to the requester only → client stores it in `dword_A82360` (read by the
0x0F world-state-load and batch-spawn handlers).

**§5.53 S2C 0x04 SESSION SLOT CONFIG** (24 B) `[orig: NapiNPClientMsg_SessionSlotConfig @ 0x425410]`:
`[4×i32 skipped][u8 sessionConfig → dword_24D2110][u8 teamMode → g_local_player_slot_id][u8 maxPlayers →
g_max_player_slots + PlayerSlotTable_Reallocate @ 0x434B90][i32 skipped][u8 → byte_A85B48]`.

**§5.54 S2C 0x08 SESSION CONFIG** (fixed 51 B) `[orig: NapiNPClientMsg_HandleSessionConfig @ 0x4281D0]`
— the old "game-state snapshot / delta entity updates (~2 KB)" table note was wrong. Wire:
`[10×i32 → dword_A821BC..A821E0]` (fields[3] = gameType → `g_GameType`), `[7×u8 → byte_A821E8..ED +
dword_24D2110]`, `[u32 bitflags → dword_A821E4]` with bits 13/15/16 latched into byte_A821EE/EF/F0.
Bumps `g_loading_progress` to ≥ 1.

**§5.55 S2C 0x02 JOIN POSITION-ACK + PADDING PROBE** `[orig: NapiNPClientMsg_HandleJoinResponse
@ 0x42E0F0]`: the handler reads only `[i32 posX][i32 posY][i32 paddingLen]`; the rest of the ~512-B body
is ignored filler. The client replies **C2S 0x02** and resets its send-holdoff counter. The reply body
is THREE dwords plus filler, not "position + paddingLen bytes": `[i32 posX][i32 posY][u32
clientNetFrameCounter]` (`@ 0x42a396` / `@ 0x42a3a9` / `@ 0x42a3ba`) followed by
`max(0, clamp(paddingLen, 0, 512) - 12)` `rand() >> 3` filler bytes (clamp `@ 0x42a374..0x42a382`,
pad gate `> 12` `@ 0x42a3c2`), so the total is `max(12, clamped)`
`[orig: NetPacket_WritePositionWithPadding @ 0x42A360]`. The first two dwords are the two the
server sent, echoed verbatim (`@ 0x42e10a` / `@ 0x42e119`); the third is the client's own
`dword_A822A4`. That is why the server reads dword 0 (as the round-trip stamp for its latency
sample) and dword 2, skipping dword 1 (`NapiNPServerMsg_0x002 @ 0x512fd0`). Golden: S 0x02
512 B ⇄ C 0x02 256 B (paddingLen=256).

**§5.56 C2S 0x2F LOADOUT SUBMIT** `[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790]` — the
spawn-menu accept, and the uplink that sets the wire-visible playerClass. Wire: `[u8 team 1..4]
[u8 playerClass 5..9][u32 weaponSlotIdx]` then `[u8 admIndex][u8 ammoPri][u8 ammoSec][u8 variant]`
entries until an `0xFF` admIndex terminator (the same slot vocabulary as the §5.30 S2C 0x5A downlink).
The header bytes were originally mapped "class 1..4 / soldierType 5..9" from the server's local
names; the CLIENT builder witness (2026-07-24, below) pins byte 0 as the server-assigned TEAM
(`byte_A85B48`) and byte 1 as the profile's character CLASS — the server's own 1/3→blue 2/4→red
side-mask switch on byte 0 (`@ 0x515943..0x515957`, identical to the builder's) confirms it. Server
validation, both header bytes read SIGNED (`char`):
- **team** must be `>= 1`, and `> 4` only when `g_GameType == 0` — otherwise the handler applies
  nothing and just re-sends the current slot list (`@ 0x5158a9` → `Server_SendWeaponSlotListToPlayer
  @ 0x502550`).
- a **NONZERO class outside 5..9 takes the SAME abort** (`@ 0x5158b1` → `@ 0x515fa5`) — as does an
  in-range class that arrives outside the armory window (`player[89] <= 0 || g_preround_delay_timer`
  `@ 0x5158d0`, unmodeled here). The in-range path then runs the `g_hostClassAllowMask @ 0x24D59FC`
  remap (masked-off → first allowed), and ONLY that path's tail coerces a still-out-of-range value
  to **8** (`@ 0x515913`) — the same default our §5.23/§5.46 playerClass clamp mirrors.
- **class 0** is NOT an abort: it skips the gate entirely, falls to the `type_mask = 0` default
  (`@ 0x5159af`) so every submitted ADM entry fails the type test, and applies an EMPTY loadout with
  `entity+660 = 0` (`@ 0x515ab0`).

An accepted submit sets class → entity+660, rebuilds the avatar display list + weapon slots and
replies S2C 0x5A. Golden: team=2 class=8 + 7 ADM entries. Note for a future pass: retail's two
pre-apply memsets cover `player+78464` (0x27D8) and `player+94408` (0x800) only — the per-ammo
damage-class table at `player+89688` is NEVER wholesale-cleared, just overwritten entry by entry
(`@ 0x515e92`), where our accept path re-zeroes the whole vector first, so an accepted grant
covering fewer ammo types than its predecessor clears the rest. Pre-existing, unrowed.

**The client-side 0x2F builder (witness 2026-07-24, the D-NET-168 close).**
`[orig: NetPacket_SendLoadoutSubmit @ 0x42cdc0]` (renamed from the
`NetPacket_SendWeaponRestrictionMask` misnomer — the restriction MASK is S2C 0x66, §5.63) takes
`(team, playerClass, kitTupleBuffer, weaponSlotIndex)` and emits the header + one 4-byte row per
kit tuple: the 2048-B `{name\0 ammoPri\0 ammoSec\0 flags\0}*` buffer walks through
`AvatarDef_FindIndexByName` (a name that misses the catalog is skipped WHOLE, no row `@ 0x42cf0b`)
with the three parameter strings `atol`-truncated to their low byte (`"-1"` → `0xFF`). The slot
argument is re-resolved at send time against the local player's team side: keep it when its slot
def's `+128` side mask matches (team 1/3 → 2, 2/4 → 1, else 3), else the first side-legal def in
the same 65-slot category, else the raw argument (`@ 0x42ce2d..0x42ce8b` — an EMPTY slot table
falls through raw). Wire-team source: `byte_A85B48` = the S2C 0x04 slot-assignment TAIL byte
(`NapiNPClientMsg_0x004 @ 0x425410 → @ 0x425499`; frame 16 of the §5.0d golden), re-latched by
S2C 0x50 team assign and the death-screen close leg of the 0x0A handler. Four call sites:
- `Game_StartMission @ 0x525836` — the FIRST golden submission: pre-`Player_InitPlayer`, slot
  argument the fixed Primary key **195** (the empty slot table leaves it unresolved → raw `0xC3`),
  kit = the per-side per-class profile tuple buffer (`g_charSelClass @ 0x2551130` for teams 1/3,
  `+0x8006` for 2/4; class kits at `+6+2048*(class-5)`) copied into `restrictionData @ 0x24D4E00`.
- `Game_StartMission @ 0x525c2e` — the SECOND golden submission: post-`Player_InitPlayer`, slot
  argument `g_currentWeaponSlot` (the live equipped combo — golden `0xD4` = 212 = category 3
  rank 17), SAME kit buffer. The pair is therefore profile-automatic, not player-paced; the
  spawn-menu/armory pages RE-send on change (below).
- `NapiNPClientMsg_TeamAssign @ 0x431aad` — a mid-session S2C 0x50 team change re-copies the NEW
  side's profile kit and re-submits with slot 195, then runs `Player_InitPlayer(1)`.
- `WeaponLoadout_ApplyFromBuffer @ 0x565d94` — the armory ACCEPT re-send: live `entity->Team` +
  the armory-selected class + `g_armoryLoadoutBufferByClass` + `g_currentWeaponSlot`; a
  non-authority client resets its slot pool first (the S2C 0x5A grant refills it).
Reimpl: `encode_loadout_submit` (npwire), the `JoinerConnection::set_loadout_kit` seam + the
S2C 0x04 team latch (assigned_team_), `NovaSimulation::push_joiner_loadout_kit` (the shell's
applied kit → ADM rows + class + equipped combo). Headless callers keep the capture-default kit
byte-for-byte (`npruntime_client_runtime` pins both: the canned pair under the 0x04 team, and an
injected kit riding the zones e2e).
Related C2S rows witnessed in the same wave: 0x03 (`[i32] → entity+372`), 0x0E respawn/deploy request
(`[i16 spawnHandle]`, 0xFFFE = auto team spawn, `Server_ProcessClientRequestRespawn @ 0x519AF0`),
0x26/0x27 vehicle attach/detach (@ 0x502390 / @ 0x4FC980 — attach overwrites wire word0 with the
requester's own handle, an anti-spoof), and S2C 0x50 team-assign / 0x81 score-delta-sound (§4 rows).

**IDB changes (2026-07-01 wire-coverage grill).** Function renames:
`TerrainTexDef_ParseFromBuffer → SessionStatus_ParseFromBuffer @ 0x530ED0`,
`sub_52D5D0 → SessionStatus_GetStatPointValue`, `NapiNPClientMsg_0x058 → _SessionStatus @ 0x4228C0`,
`CTerrainRenderer_AdvanceColorRamps → ZoneTimerList_AdvancePerTick @ 0x537D60`,
`CColorGradient_AddOrUpdateRamp → ZoneTimerList_SetEntryWindow @ 0x537DE0`,
`CTerrainRenderer_AddOrUpdateColorRamp → ZoneTimerList_SetEntryValue @ 0x537EC0`,
`CineEditor_ResetState → ZoneTimers_ResetState @ 0x4244A0`,
`NapiNPClientMsg_0x06F → _ZoneTimerValue @ 0x428D60`, `NapiNPClientMsg_0x053 → _ZoneTimerWindow
@ 0x428AE0`, `NapiNPClientMsg_GotoTeleport → _PlaySoundByName @ 0x4283A0`, `NapiNPClientMsg_0x02C →
_MissionMapNames @ 0x427E10`, `NapiNPClientMsg_0x014 → _ChatMessage @ 0x42F240`,
`NapiNPClientMsg_0x004 → _SessionSlotConfig @ 0x425410`, `NapiNPServerMsg_0x026 →
_HandleVehicleAttach @ 0x502390`, `NapiNPServerMsg_0x027 → _HandleVehicleDetach @ 0x4FC980`,
`NapiNPClientMsg_0x050 → _TeamAssign @ 0x431910`, `NapiNPClientMsg_0x081 → _ScoreDeltaSound
@ 0x42A0B0`. Data renames: `dword_24E3E88 → g_session_status`, `dword_24E41B0 → g_zone_timer_list`.
Type fix: `serialize_object_to_buffer @ 0x504D10` re-prototyped to its true FOUR-arg form
`(char *buf, int cap, void *requester_unused, GamePlayerEntity *entity)` — the 3-arg prototype made
Hex-Rays mis-render all three callsites. Plus ~34 local renames across @ 0x433780 / @ 0x504D10 /
@ 0x514180 / @ 0x5030A0 (wrong Hex-Rays inferences: "texture name" → entity name, "minimapWidth" →
mountHandles[8], "cameraByte" → playerClass, position dwords mis-named as damage/health, …) and
reimpl reverse-link comments on every grilled handler.

### 5.57 The weapon.def loadout pipeline — AdmDef table, C2S 0x2F → S2C 0x5A derivation, ammo semantics (2026-07-02)

**The "AdmDef" table IS the weapon-definition table** (the IDB's `AdmDef_*`/`AvatarDef_*` helpers
all operate on it; the real avatars.def system is separate — `CAvatarDefs_ParseConfigLine
@ 0x57A3F0`). Table `AdmDefs @ 0x24E7FE0`: 255 entries × 1120 B (0x460); an entry is live iff
`name[0] != 0` at entry+20 (`AdmDef_GetEntryByIndex @ 0x53FC80`; `AdmDef_FindFreeSlot @ 0x53FC50`
walks for the first free slot).

**Load chain (per mission start):** `Game_StartMission @ 0x524360` → `AnimDef_InitAll @ 0x5435C0`
(memset + per-entry defaults via `AdmDef_InitEntryDefaults @ 0x53FEF0`, creates the `"null"`
entry @ index 0) → parse **`weapon.def`** (string @ 0x7D0448, call @ 0x5254B8) via
`WeaponDefs_LoadFile @ 0x5450A0` → `File_ParseASCIIFile @ 0x53D810` (optionally SCR-encrypted,
sniffed by header; CRLF lines; tokens split on space/comma/tab, `"..."` quotes, `//`/`;`
comments, ≤30 tokens/line) with line callback `WeaponDefs_ParseLineCallback @ 0x543680` →
post-pass `AdmDefs_PostParseRecompute @ 0x53FEA0`; loaded flag `dword_252DB80`. (Functions
renamed from `sub_*` in the 2026-07-02 grill; `WeaponDefs_ResetParseState @ 0x53FF90` is the
shared block-close/parse-state reset.)

**Block grammar:** `weapon "name"` … `end` (name → entry+0x14, strncpy 32 @ 0x543737); nested
`action <name>` … `end` (12 actions in `g_weaponActionTable @ 0x830B90`: idle, emptyidle, fire,
recoil, reload, empty, switchto, switchfrom, switchrank, scopeup, scopedown, overheated; bodies →
`ActionDef_ParseScriptLine @ 0x4023C0`). Top-level `ammoclass_max_carry <class> <n>` → the
per-class carry-cap table `dword_24E7DE0 @ 0x543873`.

Key per-entry fields (dword index / byte offset / keyword / handler):
`[0]+0x00 category @0x5439C6` (0..11) · `[4]+0x10 rank @0x543A1B` (0..64; slot combo =
`category*65 + rank`, consumer `WeaponSlotTable_LoadAllFromDefs @ 0x5414E0` slot =
`table + 100*(def[4] + 65*def[0])` @0x5415D3, 780 slots) · `[22]+0x58 clipsize @0x543A79`
(rounds/mag, default 1) · `[23]+0x5C startrounds @0x543AA8` (raw atol, **default −1** — the
shipped norm for most entries, set in `AdmDef_InitEntryDefaults @ 0x53FF19`) · `[31]+0x7C charfilter @0x543F6E`
(medic=1, sniper=2, gunner=4, rifleman=8, engineer=0x10 — table @0x830EB0) · `[32]+0x80
teamfilter @0x543FE3` (red=1, blue=2 — a TEAM mask) · `[83]+0x14C maxclips @0x5440A9` ·
byte+0xD8 `ammoclass <name> <n>` @0x5441CB (ammo-class id byte; builtins @0x830F10; second param →
[56]+0xE0 pool-units/round) · `[235]+0x3AC loadout_subclasses @0x544E43` · `weapon_class
primary|secondary|grenade|accessory` → +0x3A4 · `loadout_selectable` +0x3A8 · `sameas <name>` →
+0x34. (The full keyword sweep — sights/hud/sounds/heat/etc. — was witnessed and lives in the
session record; the fields above are the loadout-pipeline set.)

**C2S 0x2F → S2C 0x5A derivation** [orig: `NapiNPServerMsg_HandlePlayerLoadout @ 0x515790` →
`Server_SendWeaponSlotListToPlayer @ 0x502550`]: parse `[u8 playerClass][u8 soldierType]
[u32 weaponSlotIndex]` + `{[u8 admIdx][u8 ammoP][u8 ammoS][u8 variant]}*` until admIdx 0xFF;
per entry validate `class_mask & entry[32]` and `type_mask & entry[31]` (masks from
playerClass/soldierType @0x51593c/@0x51596c) plus the armory-enable table `unused6 @ 0x24D5600`
(loader unwitnessed); clamp soldierType to [5,9]-else-8 (@0x515913, restricted by the
allowed-class mask `dword_24D59FC`); stamp `entity+660 = soldierType` (@0x515ab0) and re-resolve
the player model; load accepted entries into the 780-slot weapon table
(`WeaponSlotPool_ResetAllEntries @ 0x53F240` + `WeaponSlotTable_LoadAllFromDefs @ 0x5414E0`);
ammo per entry: `req >= 0 ? min(req, entry[83]) * entry[22] : entry[23]` →
`WeaponSlot_SetAmmoCount @ 0x540B50` (@0x515e58-0x515e86). The fourth request byte is
normalized (`1` = ×0.9, `2` = ×1.1, every other value = `0`) and stored at
`player+89688[entry AmmoDef index]`. This is a **per-ammo table**, not a per-weapon field:
accepted entries are processed in wire order and the last entry using an AmmoDef wins. Repeating
the same `category*65+rank` replaces that 780-table slot, so it is emitted only once by the reply
walk (with the last entry's ammo fill).

**The reply walk (grilled 2026-07-02):** `Server_SendWeaponSlotListToPlayer` walks the player's
780-slot table **ascending by weapon-slot combo `category*65 + rank`** (@0x5026e5..@0x5028a0 —
NOT AdmDef-index order; the two coincide for the golden kit), filters each populated slot by
`team_side_mask & def+0x80` and `slot_category_mask & def+0x7C` (@0x502716; team mask from
entity+416: 1/3 → blue 2, 2/4 → red 1, else 3 @0x502666; char mask from playerType: 5..9 →
`1<<(t-5)`, 1..3 → all @0x502693), and emits per slot: `[u8 admIdx =
AvatarDef_FindIndexByName(def name) @0x50273b]` `[u8 ammoPrimary = WeaponSlot_GetTotalClips
@0x502794]` `[u8 ammoSecondary = the same count for the FIRST different-ammoclass sub-variant
in parent+1..parent+LSC (@0x5027c8, class-byte compare @0x5027f8), else 0xFF]`
`[u8 damageClass = player+89688[slot AmmoDef index] @0x50288a]`, then skips the LSC slots
(@0x50284e); 0xFF terminator after the leading `[u8 avatarClass]`.
Consequently, every emitted weapon sharing one AmmoDef serializes the same final normalized class,
including an earlier weapon whose request byte was different. The 0x2F read side is witnessed
(2026-07-20): every field goes through a bounds-guarded cursor that substitutes ZERO past the end
(`@0x515853`-style guards throughout), the entry loop exits ONLY on the 0xFF terminator
(`@0x515a99`), and there is no end-of-body check after that exit — TRAILING bytes are accepted and
processed. An UNTERMINATED list never exits retail's loop (the zero-filled reads can't produce
0xFF — a hang/overflow on hostile input). The reimplementation matches the accept side (trailing
bytes accepted, 2026-07-20 fix) and rejects unterminated/truncated bodies as the crash-safe
divergence, without emitting 0x5A or opening the loadout gate. An invalid class/type in retail
answers with `Server_SendWeaponSlotListToPlayer` rather than silence (`@0x5158a9`); ours stays
silent on those — a bounded residual.
**`WeaponSlot_GetTotalClips @ 0x5425F0`** (ex-`sub_5425F0`; the auto "kill score" comment was a
misnomer) = the slot's TOTAL AMMO IN CLIPS: (entity ammo pool for the def's ammoclass [+ the
bucket value @0x542651 / loaded rounds slot+16 @0x54265b]) ÷ clipsize (@0x542673); clipsize −1
→ returns −1 = wire 255, the knife/no-clip sentinel (@0x542669); clamp 127 (@0x542678). At
loadout time the pool is the §5.57 fill, so a default-request (0xFF) byte ==
`startrounds/clipsize` and an explicit request == `min(req, maxclips)` — verified against the
golden ASH_I5A reply `{2:255, 3:10, 21:10, 76:1, 77:2, 78:3, 83:3}` field-for-field. The CLIENT
apply (`NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290E0`) re-runs the same clamp/fallback on
its own table — echoed bytes converge on apply EXCEPT when the fallback `entry[23]` is negative
(the shipped default), where the count degenerates (the v15 "ammo issues"). Reimpl:
`build_tag_5a_weapon_loadout` (libs/npruntime/server_message_dispatch.cpp) + the witnessed
rules in libs/npruntime/weapon_table_build.cpp over `world::WeaponTable` (D-NET-141).

**Index numbering (CLOSED 2026-07-02):** `AnimDef_InitAll @ 0x5435C0` wipes the table and
creates exactly ONE built-in — `"null"` @ index 0 (strcpy @0x543615) — immediately before
`weapon.def` parses (`Game_StartMission @0x5254b3/@0x5254bd`). The `weapon "NAME"` open handler
resolves an EXISTING same-name entry first (`AvatarDef_FindIndexByName @0x5436e1`, the re-parse
override) else takes the LOWEST free slot (`AdmDef_FindFreeSlot @0x53FC50`), inits defaults
(`AdmDef_InitEntryDefaults @0x53FEF0`: clipsize 1 @0x53ff13, startrounds −1 @0x53ff19) and
copies the name (@0x543737). `loadout_subclasses` (@0x544E43 → +0x3AC) and `loadout_selectable`
(@0x544e11 → +0x3A8) are plain scalar stores — **nothing reserves slots**; a parent's
sub-variant blocks directly follow it in the file and allocate their own consecutive indices
(the 0x5A alt-ammo walk depends on parent+1..parent+LSC being the variants @0x5027c8).
`AdmDef_FindFreeSlot` has exactly ONE caller (the weapon.def callback), so one parse into a
fresh table is **pure file order, 1-based**. The prior 94-weapon anchor contradiction was the
FIXTURE, not the rule: a live install's VFS-resolved weapon.def differs from the
fixtures/def/weapon.def extract (the JO:CA host root resolves 126 weapons — loose files shadow
archives, patch PFFs override), and under file order every golden anchor lands (KNIFE2@2,
M4AUTO@9, AK47AUTO@21, GRENADEFB/HE/SM@76/77/78 with startrounds 1/2/3, SATCHEL_CHARGE@83; the
gunner kit's PKM@41 + DESIGNATOR@89). The parse callback (ex-`loc_543680`) is now the defined
function `WeaponDefs_ParseLineCallback @ 0x543680` (0x543680–0x545098; two fallthrough
artifacts sub_544148/sub_544D5F deleted, the shared warning helper `WeaponDefs_ParseWarning
@ 0x53FFC0` defined after a data-in-instruction repair @0x540047).

### 5.58 The reload round-trip — C2S 0x25 → S2C 0x49 (2026-07-02)

Reload on an MP client is NOT local: **the clip only refills when the server's S2C 0x49
arrives.** Witnessed chain:
1. R key → `Input_HandleActionBinding_0 @ 0x4E12E9` → `WeaponSlot_RequestReload @ 0x53F110`
   (local gates: no pending reload — slot+90 sign bit — and FSM in IDLE/EMPTYIDLE/OVERHEATED)
   → next_action = RELOAD. Auto-reload: `WeaponAction_Idle @ 0x5429AF` /
   `WeaponAction_EmptyIdle @ 0x542AA3`.
2. FSM `WeaponAction_Reload @ 0x5430B0`: first tick sends reliable **C2S 0x25**
   `[u16 packed entity handle][u16 weaponSlotCombo = category*65+rank]` (via the misnomered
   `NetPacket_SendEntityDeathNotification @ 0x432930`, combo @0x5430E7) and sets the slot's
   **0x80 reload-pending flag** (@0x543108). This is an entry-time guard, not an ACK latch:
   the shared `ActionSlot_BeginActivePhase` shim overwrites the phase with 2 on that same first
   handler tick (§5.62).
3. Server: `NapiNPServerMsg_HandleReloadRequest @ 0x514DF0` (dispatch @0x82B5D8) validates and
   **broadcasts S2C 0x49** with the same payload (two filtered sends @0x4C87E0), applying
   `WeaponSlot_ReloadAmmo @ 0x541720` on its own copy for remote requesters. The gate requires
   a live, non-dead requester and a live payload-addressed pool entity; it deliberately does not
   require those handles to be equal because mounted reloads may address a vehicle weapon.
4. Client: `NapiNPClientMsg_WeaponReload_0x049 @ 0x42C0A0` (dispatch @0x82AE28): local player →
   `WeaponSlot_ReloadAmmo` — **the only place a client's clip refills** (clears the 0x80 flag
   if still present @0x5417A2; also stamps `entity+0x372 = 80`, the 3P body reload-clip
   window — ANIMNUM 65/66,
   world-wac-ai-re.md §14.8.5); other players → +0x371 = 80 (CORRECTED 2026-07-09: not a clip
   window — the arms-dip `pitchKickAccum` feed, §14.8.5; a pure client shows remote reloads as
   the dip only, the host shows the full clip via its own-copy refill); vehicle weapons →
   direct refill.

Reload ammo math: `WeaponSlot_ReloadAmmo` transfers `def[22] (clipsize) × def[56]
(pool-units/round)` from the per-ammo-class carried pool (`Entity_GetScoreValueBySlotType
@ 0x5406E0`, class byte def+0xD8; pool cap = `ammoclass_max_carry` @ 0x24E7DE0). If a host
ignores C2S 0x25, the joiner's clip stays empty; when the reload action returns to idle, the
empty-magazine logic queues another reload, producing repeated reload animations/requests rather
than a permanent 0x80 wedge. The S2C 0x49 refill is what ends that cycle — D-NET-142.

Host-side bookkeeping (witnessed in full 2026-07-03): after the two relayed sends, the host
calls `WeaponSlot_ReloadAmmo @ 0x541720` on its OWN copy iff the requester is REMOTE
[orig: @0x514f03 `g_local_player_entity != *player`], keyed by the wire slot combo
(`slot = playerSlot+464 + 100*combo` @0x54176d): clears the slot+90 0x80 flag, REFUNDS the
remaining clip into the adm+216-typed ammo pool, and refills `clip u16 slot+16` to
`capacity(adm+88)` clamped by what the pool affords [orig: @0x541811/@0x541850]. This is the
same clip the C2S 0x06 fire pipeline decrements (§5.16) — a host that tracks fire without
tracking reload wedges its OWN ammo authority one clip in. REIMPL (D-NET-152): the dispatch
0x25 case refills the tracked clip to capacity (pool refund/clamp deferred with the pool
model).

### 5.59 The character-slot binding family — C2S 0x29, S2C 0x29/0x50/0x51, and the registry/blip structures (2026-07-02)

The client binds every `Flags & 0x100` (player-flagged) entity to a CHARACTER DESCRIPTOR via
`entity->CharacterEntity` (+0x3C). Getting any message of this family wrong re-binds a player to
the wrong archetype — the live symptom is a wrong minimap/shadow descriptor under a correct mesh
(D-NET-148's DBuggy1 shadow).

**The two structures** (one global blob, `count_and_entries @ 0x26A7748`):

- **Character-archetype registry** — `[i32 count]` + 288-byte-stride entries: `+4` type,
  `+8` subtype, `+12` index, `+280` SIDE (0 = A, nonzero = B), `+284` avatar byte, plus three
  88-byte sub-blocks (byte triples at +44/+132/+220, floats at +96/+184/+272 — the descriptor
  payload). Seeded client-locally (`Game_StartMission @ 0x524360`,
  `Entity_SpawnFromAnimSlotProperty @ 0x43c390`, `PlayerProfile_InitDefaults @ 0x54bb40`) and
  browsed by the player-info avatar UI (`populate_avatar_combo_list @ 0x560210` et al).
- **Blip table** — 256 × 36-byte entries inside the same blob (entity ptr at +28, active flag
  +32, 23-byte descriptor at +0): per-LIVE-entity binding records.
  `[orig: MinimapSlot_FindOrAllocByEntityId @ 0x57b1e0]` finds-or-allocs by ENTITY POINTER, then
  `[orig: MinimapSlot_InitBlipFromPackedId @ 0x57b080]` (renamed this session from the kong
  `HUD_DrawAllMinimapEntities`) resolves the packed char id via
  `MinimapSlot_FindByPackedId @ 0x57a270` and COPIES the registry entry's descriptor into the
  blip; a failed resolve zeroes it with avatar = 1.

**The packed char id** (`lookup_entity_slot_and_pack_entry @ 0x57ad40`, pack @ 0x57ae47):
`type(bits 0-4) | subtype(5-8) | index(9-14) | SIDE(15)`. Bit 15 is the SIDE-B bit — matched
against registry entry+280 — **not** an "alive" bit (corrects the original D-NET-137 reading).
The lookup's second parameter is the side (`team != 1`), and a side with no registry entry falls
back to packing entry 0. Golden ids: host `0x0200` = side A/type 0/index 1, joiner `0x8207` =
side B/type 7/index 1 (the CI1 u16 truncation, §5.0b/D-NET-146).

**The messages** (client handlers in the 16-byte-entry dispatch table `@ 0x82AE28` —
`{tag, name, handler, 0}` — which pins every tag↔handler pairing):

| msg | handler | payload / behavior |
|---|---|---|
| S2C 0x29 | `[orig: NapiNPClientMsg_CharMinimapUpdate @ 0x427D00]` (renamed from `handle_entity_minimap_update`) | `[u8 pool0Idx][u8 team → entity+354][u8 flags7 → entity+692][u16 packedCharId → NetId +0x15C]`; self-heals an unresolvable id by side, then REBINDS CharacterEntity. Client-only (`is_authority` gate). |
| S2C 0x50 | `[orig: NapiNPClientMsg_TeamAssign @ 0x431910]` | team assign (§4 row); also touches the registry. |
| S2C 0x51 | `[orig: NapiNPClientMsg_HandlePlayerSpawn @ 0x431BB0]` | **FIELD-PARSED** (refuting the D-NET-137-era "spawn signal only" claim): `[u16 ackSeed][u16 entityHandle][u8 team → entity+354 (client only)][u16 packedCharId → NetId @ 0x431cad][u8 → entity+884]`; acks C2S 0x29 with `ackSeed+1` (`@ 0x431c99`); for `Flags & 0x100` entities REBINDS CharacterEntity (`@ 0x431cf3`). |
| C2S 0x29 | server `[orig: NapiNPServerMsg_0x029 @ 0x514F10]` | `[u16 team_change_index]` — sent by the client's 0x51 apply (team+1) and at deploy/team pick. The server replies S2C 0x51 ONLY when the index resolves to a pending `g_team_change_entity_list @ 0xC947C8` entry (gated `!g_net_spawn_suspended && !g_spawn_success_gate`), and the reply body is a real `write_entity_packet @ 0x506bb0` record. **A plain join-deploy C 0x29 draws NO reply** — golden retail-ashi5a has zero S2C 0x51 in the whole session. |

The deploy gate is unrelated: retail drops the loading screen via S2C 0x1D (§5.2), never 0x51.

### 5.60 The authoritative round simulation, damage, and the death broadcast family (engine-research scope, 2026-07-03)

Witnessed originally to scope the port of server-side rounds — the D-NET-152 deferred tail.
The binary claims remain the retail source record; the current reimplementation status and
bounded residuals are recorded under PHYSICS/COLLISION ALIGNMENT below.

**The spawn is synchronous.** `RoundData_AddRound @ 0x4FDB40` inline-calls
`RoundData_SpawnRound @ 0x4EC0D0` — the `g_round_ring` is ONLY the tag-2 network fan-out
log; the simulation starts at fire time. SpawnRound (hit-params struct = the 11-dword block
AddRound builds: origin ptr, target, fire time, adm entry, weapon id, damage type/value,
direction/spread words): score-multiplier stamp, tracer interval (the AMMO `tracerRate`
byte +226), then the
ammo-class dispatch — driven by the AMMO record's flags dword and kztype word (the
`ammo.def` field map below): flag 0x400 `instantkillzone` enters the immediate damage leaf;
when the ammo kztype (word 22) == 1 `rounds_kz_Knife`, it additionally calls
`Weapon_RaycastAndSpawnImpact @ 0x4E8460`: euler→matrix ray out to that same AmmoDef's
`+0x38 kz_maxradius` vs terrain hi-res heightmap + `Projectile_RaycastProximitySlots
@ 0x4E5340` + water, 5-way hit class, impact effect from the AmmoDef `+0x68` 16-B-row
effects table + sound — EFFECTS ONLY, no damage call in this presenter. Normal bullets do
not call this leaf. Then flag 0x20 `Detonatesatchels`, 0x2000000 `DesignateTarget`
guided-tracker register, 0x10000 `shotgun` / 0x20000 `claymore` pellet bursts
(`Weapon_SpawnProjectileBurst*`, count = ammo `spread_count`), default (kztype 6
`rounds_kz_Bullets`) =
allocate a pool-3 projectile entity (`CEntityManager_AllocateSlot` +
`Entity_InitFromItemDef`), apply `Weapon_CalcRandomSpreadOffset` to the direction, velocity
= ammo speed/62 per tick from yaw/pitch 16.16 trig (multiply >> 22), owner/team stamp,
trail emitter + glow light. Other SpawnRound callers: the client's own-fire prediction
(`Entity_FireWeaponAndSendPacket @ 0x42BD80`), the client tag-2 re-sim
(`NetPacket_DeserializeRoundEvent @ 0x42F270`), death-explosion shrapnel
(`Entity_HandleInfantryDeath @ 0x443670`, `Entity_HandleDeathExplosion`,
`Entity_HandleVehicleDeathExplosion`), and save-load.

#### Exact ordinary-round spread and recoil (re-grilled and ported 2026-07-31)

The tag-2 ring remains a **pre-spread** fire log. `RoundData_AddRound` copies the
claimed yaw/pitch into the ring before its inline spawn; only the allocated live
round receives the final offsets. A receiving client therefore replays the same
deterministic calculation from the wire's `shot_seq`, rather than receiving final
trajectory angles. `[orig: RoundData_AddRound @ 0x4fdb40]`
`[orig: NetPacket_DeserializeRoundEvent @ 0x42f270]`

`RoundData_SpawnRound` classifies the shooter once for both weapon ERROR and
ammo recoil: prone (`MoveOrder & 0x100`) = 0, crouch (`&0x200`) = 1, otherwise
standing = 2; airborne or submerged forces 2, while an attached/mounted shooter
forces 1. Retail's submerged predicate compares signed
`Position.Z + CameraOffset.Z` against `Env_WaterHeightFixed`
(`entity+0x0C + entity+0x74` at `0x4ec2de..0x4ec2ef`). The portable host
projection uses fixed body position plus the Drowning flag; the decoded visual
projection uses the fixed wire-row position plus its swimming classification.
Neither core carries `CameraOffset.Z`, so that bounded eye-height residual
remains under D-INF-18. The fire-context subtype's high bit is
`verticalSpread`; the ordinary
weapon ERROR row is `verticalSpread ? 3 : category`. This selector is **not**
the HUD selector (`stance + 3*Player_CanFireWeapon()`, hud-re D-HUD-7).
`[orig: RoundData_SpawnRound @ 0x4ec0d0]`

For an ordinary weapon round, the spread magnitude in 16.16 degrees is

`S = weapon.ERROR[row] + (entity+0x380 >> 8) + (entity+0x384 >> 7)`.

The optional second-axis magnitude is `error_upTheta` (`weapon+0xD0`) when
`verticalSpread || prone`, otherwise `error_hipTheta` (`weapon+0xCC`). Both
theta fields default to zero. `ERROR` occupies six exact 16.16 rows at
`weapon+0xB0..+0xC4`; all eight values use `Math_ParseFixedPoint16`, not
`atof`. `[orig: WeaponDefs_ParseLineCallback @ 0x543b21]`
`[orig: Math_ParseFixedPoint16 @ 0x6131f0]`
`[orig: RoundData_SpawnRound @ 0x4ec0d0]`

The weapon path requires a weapon definition, a source entity carrying the
player flag `0x100`, and the weapon-spread rules gate: session rules bit
`0x2000`, or offline rules bit `0x4000`. That gate controls only the ordinary
weapon helper. AmmoDef `error` (`+0x18`) remains the fallback when no weapon
definition exists; recoil, shotgun spread, and that ammo fallback do not read
the weapon-spread gate. AI fire with a weapon definition does not take this
player-only weapon ERROR path. Weapon flag `UseSpreadTwo = 0x00400000` selects
the helper's alternate distribution. `[orig: RoundData_SpawnRound @ 0x4ec0d0]`

The portable sim represents the resolved rules outcome directly as
`RoundSim::weapon_spread_enabled` and defaults it on when no rules owner is
present. The original session/offline bit sources above are not mislabeled as
a recoil rule; plumbing a future rules object consists only of setting this
raw seam from the applicable bit.

`Weapon_CalcRandomSpreadOffset` returns exact zero offsets when `S==0`;
otherwise it is a uint32-wrap hash followed by x87 trig. For seed `n` (the
allocated round's `shot_seq` at round `+0x78`), let

```
p  = uint64(n) * n
lo = uint32(p) - n
hi = ror32((uint32(p >> 32) ^ 0xCC1CDC1D) + 0xC11ABB09, 16)
h  = rol32(lo + hi, 18); h += (int32(h) < 0 ? 0x001ABB09 : 0)
q  = rol32(h, 12);      q += (int32(q) < 0 ? 0x001ABB09 : 0)
a  = q & 0xFFFF; b = (h >> 8) & 0xFFFF
```

The constants are exact binary32 values loaded into the x87 path:
`B=0x43360B60` (182.04443359375), `Q=0x37C90FD0`, `T=0x38C90FD0`, and
`U=0x37800000` (1/65536). With truncation toward zero, normal mode produces
`yawOff = trunc(S*B*cos(a*Q)*sin(b*T))` and
`pitchOff = trunc((V ? V : S)*B*cos(a*Q)*cos(b*T))`. Alternate mode with
`V==0` uses radius `S*B*b*U` and angle `b*T`; with `V!=0`, yaw uses
`S*B*b*U*cos(b*T)` while pitch uses `V*B*a*U*sin(a*T)`. Yaw and pitch offsets
are wrap-added to the requested BAM32 angles. The binary32 constants are
promoted for the x87 intermediates: forcing every intermediate back to float
changes some retail answers by 1–4 BAM units. `[orig:
Weapon_CalcRandomSpreadOffset @ 0x4e4120]`

Representative retail vectors (decimal output BAM32 offsets):

| Mode | `S` | seed | `V` | yaw offset | pitch offset |
|---|---:|---:|---:|---:|---:|
| normal | `0x00010000` | `0x00000000` | 0 | -7340450 | 6771822 |
| normal | `0x00010000` | `0x12345678` | `0x00008000` | -4357031 | 3639341 |
| alternate | `0x00010000` | `0x12345678` | 0 | 9357827 | -5601610 |
| alternate | `0x00010000` | `0x12345678` | `0x00008000` | 9357827 | 66155 |
| normal | `0xFFFF0000` | `0xDEADBEEF` | 0 | 225733 | -8847762 |

These are pinned at the portable boundary by `npruntime_round_sim`; the signed
case guards both wrap and arithmetic conversion behavior.

The recoil impulse is deliberately **after** the successful ordinary spawn,
so this round uses the previous `entity+0x380`. Ammo `recoil` parses with
`atol` into three bytes at `+0xE3..+0xE5` (modulo narrowing). The selected
impulse is `recoil[category] << 18`, changed to `<< 20` while drowning or
underwater; `Flags & 0x10` (scope raised) applies exact binary32 0.75 and
truncates toward zero. There is no `mp_NoWeaponRecoil` read. The source must
exist, carry an ItemDef, and be a type-3 person. `[orig:
AmmoDef_ParseProperty @ 0x40a2d0]` `[orig: RoundData_SpawnRound @ 0x4ec0d0]`

The special branches preserve their retail ordering: shotgun flag `0x10000`
spawns its distinct radial pellet fan, then adds recoil, and returns without the ordinary
weapon ERROR/+0x380/+0x384 helper; claymore flag `0x20000` returns after its
fan and before stance/recoil; instant, detonator, and designator returns also
precede recoil. The shotgun helper uses draw one for
`radius = trunc(kz_pieslice*cos(draw*pi/2/65536))` and draw two for the
full-circle yaw/pitch pair; it is not the claymore's rectangular fan. `[orig:
RoundData_SpawnRound @ 0x4ec0d0; Weapon_SpawnProjectileBurstWithSpread
@ 0x4ebbb0]`

#### Per-tick accumulator order

Before camera construction and the later weapon-action/spawn pass,
`Entity_UpdateInfantryPlayerBody` updates the two signed accumulators:

- For recoil `R=entity+0x380`, compute `t=(R+4)>>3`, `half=t>>1`, subtract
  `half`, and snap to zero at signed `R<=0x300`. Add `t>>3` to entity pitch;
  always consume one `PRNG_Next16`, even at zero, and add `half` to yaw for an
  even result or subtract it for an odd result. There is no upper clamp.
- The local-player-only movement producer runs while moving (`MoveOrder&8`),
  on foot, with an equipped definition. It wrap-adds a stance/aim-scaled
  `clipweight+weaponweight` contribution to `M=entity+0x384`: one third when
  `Player_CanFireWeapon` succeeds, one third prone/not-drowning, two thirds
  crouched/not-drowning, otherwise 1.5. The one-third leg is signed integer
  division; the two floating legs truncate toward zero. Rising while airborne
  wrap-adds `0x01000000`. The shared local/remote/
  AI decay then applies `M -= (M+4)>>4` and snaps signed `M<=0x300` to zero;
  there is no upper clamp.

All additions and shifts above retain x86 32-bit wrap/arithmetic-shift
semantics. A local round spawned later in the same tick sees the already
decayed movement contribution and the pre-impulse recoil value. On a receiving
client, the tag-2 apply precedes that entity update, so a remote recoil impulse
is stamped and then decayed in the same frame. `[orig:
Entity_UpdateInfantryPlayerBody @ 0x4b40e0]` `[orig:
Entity_UpdateInfantryAI @ 0x4b9910]`

The client-side port retains `R` and a full sub-byte heading on each decoded
player row and performs the same stamp-before-decay ordering
(`netsim_client_view_recoil`). Its yaw leg uses the exact
`PRNG_Next16` recurrence and consumes one draw per decoded person in pass order;
the stream is subsystem-local and zero-seeded, however. Authority/local/AI
recoil likewise uses the exact recurrence and body-pass order, but its
`AiSystem` state is separate from other ported consumers of retail's same
process-global stream. Unrelated calls can therefore shift sign history on
either path (D-WPN-35). This bounded
visual-yaw residual does not change `R`, pitch drift, projectile/HUD spread, or
the recoil impulse. The port does not invent or transmit `M`: decoded remote
rows never run the local-only producer, so their zero-initialized value remains
the retail-equivalent zero.

The 2026-07-31 recoil/spread grill was read-only; it made no IDB changes.

**Authority: damage is host-only — witnessed in three independent gates.**
`Weapon_CalcImpactDamage @ 0x4EC920` returns 0 for a non-authority session peer
(`@0x4ec933`); `Projectile_ProcessDamageOnTarget @ 0x4E7FB0` guards the health write and
the team attacked-by/visibility matrices on `g_napi_np_ctx.is_authority` (`@0x4e809c`,
`@0x4e8127`); `Entity_ApplyWeaponDamage @ 0x4E6820` (the explosion applicator) early-outs
unless authority, with buildings further gated by `g_destroy_buildings` (`@0x4e6860`). A
client's local round sim — own-fire prediction and tag-2 re-simulation alike — is purely
visual (effects, decals, sound).

**The per-tick sim.** `Weapon_UpdateAllProjectiles` iterates live rounds →
`Projectile_UpdatePhysics @ 0x4E9D70` (ammo def = `g_ammoDefTable[276·idx]`, idx at
projectile+620, lifetime at +684). Flag 2 `ignore` only ages the round; flag 0x2000
`useownmove` dispatches an ammo-specific movement callback and skips the stock ballistic
ray/gravity/drag path. The witnessed `nade`/`schl`/`clym` callbacks are now ported
through the items.def class binding (§27 / correspondence §5.8); other callback/guidance
families remain open. Before an ordinary sweep, a strictly submerged round below
0x4000 Q16 speed is retired. Exact-zero velocity is another special leaf: no movement or
ray, `vz -= 167` even for NoGravity, and no drag. Otherwise the tick sweeps with the OLD
velocity through terrain, water (`Projectile_CheckWaterIntersection @ 0x4E59D0`), static
CFAC, dynamic CFAC, then person COBJ bone spheres. The 0.1 u / 6553 fp16 radius clamp at
`@0x4ea263` is conditional on network session + authority + FatBullets + a remote
player-owned round; it is not a universal bullet radius. A hit runs the 5-way switch
(`@0x4ea6a7`: 0 terrain, 1 static, 2 dynamic, 3 person, 4 water) and is consumed before
this tick's forces; damage therefore observes the pre-force velocity. A miss commits the
new position, applies the 167-Q16 gravity step unless NoGravity, then applies aerodynamic
drag for the NEXT sweep.

Aerodynamic flight drag is `Entity_ApplyDragAndBounceForce @ 0x4E5EC0`, not
`Projectile_ApplyDragDeceleration`. `Projectile_InitDragTable @ 0x4E78D0` sweeps
0..3999 ft/s through 40 inclusive power-law bands, repeatedly overwriting
`floor(f·0.3048)` and leaving BSS bin 1219 at zero. Runtime indexes
`clamp((62·|vQ16|)>>16,0,1219)`, performs the two signed truncations
`trunc(trunc((table[index]<<16)/ammo.drag)/62)`, projects that step opposite velocity,
and uses the fixed multiply `(step·dir + 0x8000)>>16`; at/below water the step is
multiplied by 25 first. A drag overshoot zeros all velocity. Below
`min_stable_velocity`, X/Y and positive Z receive the recovered 1/32 damping; the
NoGravity branch then adds the otherwise-skipped 167-Q16 Z step. A threshold crossing
also applies `tumble_error` in a randomized local frame. The deterministic branch is
ported; that PRNG/frame producer remains open. The separate
`Projectile_ApplyDragDeceleration @ 0x4E5CD0` is the impact-energy loss using ammo weight and the
surface-specific `armor_density` fields.

**Entity impact** (`Projectile_HandleEntityImpact @ 0x4E9390`): a child rolls to exactly
one vehicle parent when its item attribute has 0x20, the child is not type 1, and that
parent is type 1 (`@0x4e94e0`); the ARMING gate — elapsed ticks (projectile+676
initial − +684 remaining) < the ammo `arm_age` (dword 3) means the round is NOT ARMED yet
and spawns its `notarmmedammo` (+241) child in its place (`AmmoDef_LookupByName`, copies
692 B of the projectile) — the inert/dud variant of a grenade inside arming distance (the
old "penetration budget" reading was wrong);
weapon-type-15 bone/section damage via `Entity_ComputeBoneCollisionBounds`; kinetic clamp
(`Entity_ClampKineticEnergy`); pass-through flags 0x18000000 = `lawr|fgrenade`
(`@0x4e95a0`); then `Projectile_ProcessDamageOnTarget @ 0x4E7FB0`.

**The damage model is kinetic.** `Weapon_CalcImpactDamage @ 0x4EC920` uses the round's
REMAINING pre-force Q16 speed. The magnitude is first capped at the exact float boundary
`0x4EFFFE00`; the speed index is arithmetic-shifted from signed-32 wrapping
`magnitude * 62`, then upper-clamped to 1219 (`@0x4ecad6`). Base damage is truncating
signed division of wrapping `speedIndex * weight_in_grains` by 875. Normal infantry reads
the final/lowest overlapping section from `ray[32]` (`hitZoneData+0x80 @0x4ec9a1`):
zones 0-4 are x1.25, 5-8 x1.0, 9-12/15-18 x0.5, and 13-14 x3.0. The separate
first/highest `ray[31]` is copied to the damage-trigger hit record and drives
reaction/death animation. The itemDef+84 `&0x200` seat branch instead reads `ray[31]`;
zones 2/3/6/7 get x6.0. Normal zones 13/14 and those special seat zones set target+44
`|=0x800`. Each floating multiplier truncates separately. The shooter's per-ammo class
then applies x0.9 for class 1 or x1.1 for class 2 before ammo minimum and positive maximum
clamps.

In a network session a non-authority calculation returns zero. Authority plus
`g_OneShotKill` returns 2000 immediately, before zone/class/min/max; offline play ignores
that multiplayer option. All downstream target gates still apply. Distance falloff
emerges from aerodynamic drag—there is no range table.

`Projectile_HandleEntityImpact` can redirect damage through exactly one carrier hop:
the struck child must not be item type 1, must carry item attrib 0x20, and its direct
parent must be type 1. Presentation stays on the geometry actually hit. A physical hit
on an entity with no ItemDef consumes the projectile and presents its impact but returns
before damage calculation. `Projectile_ProcessDamageOnTarget` zeros damage for entity
flag 0x4000000, signed itemDef+400 impact armor -1, ammo `penetration_impact` below that
armor class, or nonzero entity+292 damage state. A type-1 vehicle with more than one
eligible live pool-0 direct/one-nested occupant reduces damage by
`min(count * damage_reduc_pp, damage_reduc_max)`. The occupant scan counts the ATTACH
chain — a candidate whose parentEntity(+40) is the vehicle, or whose attach carrier's
groundEntity(+0x28) is — never plain deck-standing
[orig: `Entity_CountMountedEntities @ 0x435970`]; the reimpl maps +40 to
`Entity::mount_target` (fixed 2026-07-20 from a ground-reference mis-channel).
Health and armor use signed-16 storage
semantics; damage clamps to remaining health, and itemDef+84 flag 0x40000000 applies the
retail NoDie `health - 1` clamp before the wrapping signed-16 subtraction.

Retail authority applies `health -= damage` (`@0x4e8127`); a kill calls
`Score_ProcessKillEvent @ 0x4FD400` (the scoring leaf itself emits no messages). A PLAYER
target (entity Flags & 0x100) also receives the projectile+688 multi-hit count, shooter
slot at target+442, owner at +376, and damage callback event 4. The ordinary bullet port
implements the collision, calculation, target gates, health/death staging, and impact
presentation; its separate peer callback/global-record tail, full scoring integration,
`armor_density` impact-energy path, and projectile-triggered explosive/AoE integration
remain distinct follow-ups.

**The `ammo.def` table** (`AmmoDef_LoadAll @ 0x40B0B0`, `Game_StartMission @0x52548a` —
the file is literally `ammo.def`, same encrypted-ASCII `File_ParseASCIIFile` key
0x2A5A8EAD as weapon.def §5.57; 276-B records at `g_ammoDefTable @ 0xA2ECE8`, two-pass
count→allocate→parse, `AmmoDef_InheritDefaults` per `end`). Token → offset map
(`AmmoDef_ParseProperty @ 0x40A2D0`; dword N = +4·N): `flag` OR-bits → +0 (30-name table
`@0x813500`: ignoredmg 1, ignore 2, shrapnel 4, silenced 8, water 0x10, Detonatesatchels
0x20, nosmoke 0x40, nocollide 0x80, nogravity 0x100, hasitem 0x200, instantkillzone 0x400,
ownerimmune 0x800, useownmove 0x2000, noage 0x4000, forcetracer 0x8000, shotgun 0x10000,
claymore 0x20000, NoOItems 0x80000, NoMItems 0x100000, NoDItems 0x200000, Priority
0x800000, ClipWater 0x1000000, DesignateTarget 0x2000000, IgnorFoilage 0x4000000, lawr
0x8000000, fgrenade 0x10000000, ClipWaterFx 0x20000000, + 3 `internal`); `velocity` → +4
(int, units/s); `max_age`/`arm_age` → +8/+12 in TICKS (`sub_40A0F0` = parsed 16.16 seconds
× 62 rounded); `frndlyTrcrID`/`foeTrcrID` → +16/+20 (item type_id → model index); `error`
(spread) → +24 fp16; `drag` → +28 fp16; `bullet_radius` → +32 fp16; `MF_Light` → +36/+40;
`spread_count` → +48 (pellets); `kz_minradius`/`kz_maxradius` → +52/+56 fp16;
`kz_pieslice` → +60 (deg → BAM ×11930464); `kztype` → word 22 (+44) from the 8-name table
`@0x8133E0` (0 null, 1 Knife, 2 Standard, 3 Medic, 4 RadiusBlast, 5 C4, 6 Bullets,
7 Slash); `kz_damage` → word 23 (+46); `min_stable_velocity` → +176; `tumble_error` →
+180 fp16; `weight_in_grains` → +184; `min_damage`/`max_damage` → +188/+192;
`penetration_impact`/`penetration_kz` → +196/+200; `armor_density` → +204/+208/+212;
`secondary_anim`/`kz_physics` bytes +224/+225; `tracerRate` byte +226; `recoil` bytes
+227..229; `dopplerdiv` byte +240; `notarmmedammo` string +241; guided params
(`turnrate_maxpit`/`maxyaw` +80/+84, `boresight_maxang` +88, `climb_angle`/`time`
+92/+96); effects_table sub-section (28 tags `@0x813420`).

**Explosions.** `Projectile_ProcessExplosionQueue @ 0x4EAD80` drives AoE via
function-pointer tables (`@0x4eadd0`, `@0x4eae44`) → `Entity_ApplyWeaponDamage @ 0x4E6820`:
authority gate, same-team protection when itemDef attrib 0x8000, base damage from
weaponDef+46, LINEAR distance falloff (blast radius), separate infantry (type 3) and
vehicle section paths, kill credit via `Score_ProcessKillEvent`.

**Death detection + the broadcast family** (every emit goes through
`NapiNPServer_SendFiltered @ 0x4C87E0`; mask 0x90 = alive + not-host):

- Per-tick `Entity_UpdateInfantryPlayerBody` / `Entity_UpdateInfantryAI` detect health ≤ 0
  → `Entity_CheckAndProcessDeath @ 0x51B550`: `Flags & 0x100` (player-controlled) →
  `GameEvent_PlayerDeath @ 0x516DD0`; else (AI) → S2C 0x13 death notify
  (`BuildDeathNotifyPayload`, mask 0x90) + scoring.
- `GameEvent_PlayerDeath @ 0x516DD0` [authority-gated at entry]: vehicle detach, clears
  every pool-0 entity's live-target (+92) that references the victim (the §5.9.1 0x40-word
  source), S2C 0x13 (`@0x516e9a`), respawn timer (620-tick recent-spawn rule /
  `g_respawn_timeout`, floor 3, slots +360/+364), random-seed resend, scoring
  accumulators (`Score_AccumulateKillByEntityType` ×2 + weapon stats), kill-type
  classification for the feed — suicide rand(0-2)+1, team kill rand+7, explosive weapon
  type_id 4091/4093/4095 → 24, special 49, HEADSHOT (flag 0x100 from CalcImpactDamage) →
  rand+32, vehicle kill (flag 0x800) → rand+10, knife (flag 0x400) → rand+13, standard
  rand+4, drowned 22, crashed (0x200) 23, environment 26 — then S2C 0x52 ×2
  (`NetPacket_WriteThreeInt32s`, kill stats), S2C 0x1E (`GameEvent_BuildPayload`:
  event_type + three pool indices + position — the §5.26 kill feed), S2C 0x54 ×2
  (`NetPacket_WriteEntityHandleWithByte` — the death/wounded minimap marker; also emitted
  by `GameEvent_RevivePlayer @ 0x517DB4` and `Server_BroadcastMedicRequest @ 0x515390`,
  D-NET-108).
- `Server_KillPlayerAndNotify @ 0x519E00` [authority]: marks the slot dead (+100567;
  entity+292 = −1 — the exact flag `Projectile_ProcessDamageOnTarget` checks), calls
  `Server_ProcessPlayerDeath`, optional S2C 0x32 sub-type 5 carrying the player NAME.
- `Server_ProcessPlayerDeath @ 0x517740`: killer pool-handle resolve, vehicle detach (+
  seat-flag 0x40000 killer-vehicle re-attach), spawn camera, `Entity_ResetToSpawnState
  @ 0x4B9610` (the D-NET-66 death teleport), per-gametype scoring, weapons re-init +
  `Server_SendWeaponSlotListToPlayer @ 0x502943` — a death re-sends 0x5A (the
  deploy-bundle 0x5A pairing seen in the golden), 0x1E event, KRBP death marker (§5.42).
  Callers: respawn request, kill-notify, bot update, the death queue.
- Non-player entity death/destruction — 16 handlers (infantry/vehicle/destructible death
  + explosions, AI death transitions, section damage, crane/water destruction) — all stage
  S2C 0x26 via the single emit point `Server_SendEntityStatePacket @ 0x509D70`
  (`NetPacket_WriteEntityHandleAndTeam`, mask 0x90, tick stamp at entity+560).
  Destructibles additionally `Server_SendDestructibleDeathPacket @ 0x50D95A` → S2C 0x2F;
  explosion effects broadcast via `Server_BroadcastExplosionEffect @ 0x5084A1` → S2C 0x21;
  shell-eject/fall physics sync → S2C 0x59.
- CORRECTION (table row filled): the C2S 0x13 handler `@ 0x514330` is
  `NapiNPServerMsg_HandleSectorAction` — pool-3 def-type-2044 sector actions (action 6
  arms a 30-tick timer at entity+885/886), weapon-fire/camera math for some types — NOT a
  death message. Only S2C 0x13 is the death notify.

**PORTED (2026-07-03, the MVP slice — same session as the witness pass).** `libs/def`
ammo.def parse (the §5.60 token subset incl. the flag/kztype tables; `def_parse_ammo_memory`;
pinned by `def_parse_ammo` against the real 75-entry fixture — the 5.56 block field-for-field)
→ `world::AmmoTable` + the weapon `round_type` → ammo-index resolve (`ammo_table_build`,
the adm+84 pair equivalent) → `world::RoundSim` (512-slot pool; spawn SYNCHRONOUS with the
0x06 ring append; velocity = ammo/62 per tick with wire yaw used directly as the mission
bearing; per-tick segment test vs pool-0 organics + the bilinear terrain column; the kinetic
damage number `min(62·|vel|,1219)·grains/875` floored/capped, clamped to remaining health)
→ death routing in `Server_TickUpdate` (S2C 0x13 `[victim][killerSource]` + S2C 0x1E
standard-kill feed event to every non-host in-match connection; a dead HOST player queues
for the 620-tick respawn release back to its recorded spawn point at template health; a
joiner's respawn rides its own deploy request) → engine feed `NovaSimulation::load_ammo_table`
(mission_runtime.gd, after the armory). Pinned by `npruntime_round_sim_test` (build+resolve,
spawn velocity/frame, 3-hit kill at 60/60/30, 0x13/0x1E bytes, no-auto-respawn for clients,
host respawn snap). That historical MVP deliberately used coarse collision and damage;
the current collision/damage status is the alignment record immediately below.

**PHYSICS/COLLISION ALIGNMENT (2026-07-19; supersedes the MVP collision/damage
deferrals above).** RoundSim now submits every tick segment to one authoritative
`CollisionWorld::trace_projectile` query. Its candidate order is exactly terrain → water
→ static proximity table → dynamic proximity table → person proximity table. Later
classes replace only on strict `<`, so terrain wins a tie with water/static, static wins
a tie with dynamic, and dynamic wins a tie with person. Within one static/dynamic table,
the recovered face routine uses `<=`; later equal-distance faces, sections, and slots
overwrite earlier ones. Hit classes therefore mean 0 terrain, 1 static, 2 dynamic,
3 person, and 4 water—not “building” for class 3. Entity kind remains separate metadata.

Static and dynamic bullets now use the exact Poly Collision LOD path
`Physics_RaycastAgainstBoneCollision @ 0x4E4CB0`: inverse live section matrix; local
segment/CFAC AABB overlap; `material_flags & 0x100` skip; foliage poly type 17 skip only
for AmmoDef flag `0x04000000`; Q14 CNRM plane crossing and the recovered 0x1/0x800 facing
predicate; integer hit distance; then the dominant-axis signed odd/even
`Math_PointInTriangle2D @ 0x414050` test over CVRT vertices. Section-local indices,
material flags, poly type, section/bone, face, transformed point, and normal survive into
the hit. The `.3di` bridge retains CVRT/CNRM/CFAC/COBJ source order and exact fixed fields.
One live Q22 matrix per COBJ can be published by the pose owner; a yaw/root matrix is used
only as the explicit fallback. The inverse selector resolves the entity+0x158 signed-Q16
uniform-scale override before itemDef+0x1B8: zero is the rigid-transpose sentinel, while a
nonzero scale uses the recovered scale-aware inverse. COBJ offsets are already baked into
the matrices and are never added a second time. Matrix state `m[15] & 3` disables
projectile faces.

The bundled **Super OED Manual v1.1 §1.1.3.4** supplies the canonical BVOL names:
**CB** is Generic Collision Box, **CL** is Collision for Ladder, **CA** is Collision Box
for Armory, **VC** is Collision for Vehicles, and **BB** is Blink Box. They are gameplay
volumes, not aliases for the projectile face mesh. CB/type 1 is the ordinary solid convex
volume used by generic LOS/ground/contact rays. CL/type 4 extracts a ladder alignment
frame (anchor plus authored yaw/pitch); the reimplementation has that low-level contact
path but not climb states/input/root motion/top exit. CA/type 6 sets the armory-zone flag
that gates `weapon.mnu`. VC/type 7 is selected as solid geometry by the vehicle collision
query when a section authors a VC/VK run; without one, that query falls back to CB/default
solids. BB/type 8 drives indoor/section visibility. For a resolved static/dynamic collision
instance, bullet narrow phase touches none of these BVOLs and requires CFAC triangles. An
entity with no resolved collision instance may still use the separately documented
compatibility sphere.

Persons use the separate recovered COBJ bone-sphere path. When a live section pose is
published, bones are tested descending; extra radius starts at bullet radius + 0.05 u;
bone 14 adds 65% of authored radius, other bones add 45%, and bones 15/16 cap at 0x3000.
The highest qualifying bone supplies the hit zone and the retail
`projection - authoredRadius/2` distance. Until organic pose publication is wired from
the renderer, one characterized torso COBJ is the bounded fallback; it deliberately
does not invent a bone zone. The person table returns its first qualifying entity rather
than the geometrically nearest person. The FatBullets 0.1 u clamp is applied only when
all recovered network/authority/setting/remote-player-owner gates hold.

Terrain is skipped for AmmoDef `nocollide` 0x80. Water uses the mission water plane and
requires a strict endpoint straddle. Shooter exclusion is released by shrapnel flag 4;
the extra-ignore entity and the controller/driver/gunner carrier exclusions are carried
by the same query. Dead and indestructible entities remain physical blockers.

RoundSim now carries the recovered ordinary force order around that query. Each public
float position/velocity is converted to Q16 for the tick; the nonzero sweep uses the old
velocity, a consumed hit gets no post-sweep force, and a miss commits its fixed endpoint
before gravity and `Entity_ApplyDragAndBounceForce` update the next-tick velocity. The
exact-zero, submerged-slow, Ignore, the UseOwnMove dispatch, NoGravity, dry/wet drag,
overshoot, bin-1219, and deterministic below-stable branches are pinned by
`projectile_combat`; the throwable motor bodies and exact fuse-head timing are pinned
separately by `throwables`.

Damage consequences are no longer organic-only. RoundSim implements the arming/dud
substitution as a live logical child rather than an impact-row-only swap: an early entity
contact resolves `notarmmedammo`, preserves owner/kinematics/elapsed age from the witnessed
692-B projectile-prefix copy, moves the replacement to the contact, installs the dud ammo
and max-age, and leaves the +692 trail/emitter slot behind. Exactly one eligible
child→vehicle carrier hop; MP authority and MP-only OneShotKill; the exact kinetic path
(`|velQ16|` capped at float `0x4EFFFE00`, signed-32 wrapping `*62`, arithmetic `>>16`,
upper-only 1219 clamp, then signed-32 wrapping `*grains` and IDIV `/875`); posed person zones and the
item-attrib-0x200 ×6 seat zones; the shooter's replicated per-ammo ×0.9/×1.1 class;
ammo min/max; indestructible, signed itemDef+400 impact-armor, penetration, and
entity+292 damage-state gates; vehicle occupant-count reduction; remaining-health and
NoDie clamps; the special-zone entity flag `|=0x800`; vehicle/static health/death staging;
and impact-row selection. A collision target with no ItemDef remains a physical impact but
returns before damage calculation. OneShotKill
returns 2000 before zone/class/min/max but still passes through the downstream armor,
occupant, remaining-health, and NoDie gates.

Remaining data/integration gaps are explicit: threshold-crossing `tumble_error` still
needs the retail PRNG and local frame; non-throwable `useownmove` classes still need their
ammo-specific callbacks/guidance (the witnessed grenade/satchel/claymore motors are
ported under D-THROW); `Projectile_ApplyDragDeceleration` still needs the `armor_density` impact-energy
path; explosive/AoE, bounce, and shell physics remain separate; production animated
organic section matrices are not yet published, so persons can use the bounded torso
fallback; and `LiveRound` still exposes float position/velocity carriers around the Q16
tick, losing low fixed bits at sufficiently large magnitudes. Spawn-time weapon spread,
the entity bone-disable/alternate-husk source, terrain `.TIL` surface overrides, and the
`lawr|fgrenade` pass-through branch also remain open. The retail replacement's distinct
pool-3 slot identity, same-frame allocator visitation, and copied fields that have no
`LiveRound` representation remain structural gaps; the in-slot child first advances next
tick and does not invent them. No BVOL-to-CFAC equivalence is guessed for any of these gaps.
The peer-side post-damage callback/global-record tail is not yet represented separately
from `RoundImpact`; production maintains the invariant that offline simulation is authoritative.

**Impact effects routed (2026-07-13/14, the PR #237 particle pass).** Ordinary ballistic
effects are produced at the physical collision, not at fire time:
`Projectile_UpdatePhysics @ 0x4e9d70` resolves terrain/static/dynamic/person/water, and its
type-specific impact handler
path calls `Projectile_SpawnImpactEffect @ 0x4e9b80`, and that presenter selects the AmmoDef
effects-table row for the resolved tag. `Weapon_RaycastAndSpawnImpact @ 0x4e8460` is the
separate Knife-only instant-kill-zone presenter described above; its `+0x38` input is
`AmmoDef.kz_maxradius` (2.5 in the rifle fixture), not a weapon trace range. The ammo
`effects_table` rows stage by tag name
against `g_AmmoEffectTagTable @ 0x813420` (28 canonical tags, index = tag id), duplicates
refused (`@ 0x40a502`), `none` columns zeroed, and the authored 4th count column parsed then
DISCARDED (`@ 0x40a587`) — dead data in retail. NOTE: the block-end bake COMPACTS the
staging (`AmmoDef_InitEffectsTable @ 0x409f20` allocates `16*(count+1)` and copies authored
tags in ascending order) while the presenter indexes by tag POSITION — sound only because
every shipped table authors the full contiguous 1..24 prefix (row index == tag id); the
port's 28-slot tag-addressed bake is byte-equivalent on shipped data and resolves correctly
on sparse mod tables where the original would misindex (an intentional bounded divergence,
noted in `world/ammo_table.h`). Port: authority/listen-host and single-player FIRE now
append the round ring (including the captured `((preConsumeClip & 3) << 4) | 2` primary mode
byte and the exact ordinary on-foot hip/ADS-raise/third-person subtype 12) and
synchronously spawn `world::RoundSim` from the production-tick eye origin and BAM
direction. Settled-FP and first-person-mounted integer zoom subtypes remain unmodeled
(D-WPN-8).
The sim emits bounded `RoundImpact` rows from its own physical hit
resolution (position, normalized direction, tag) →
`NovaSimulation::drain_round_impacts` resolves the rows baked by `np::build_ammo_table`
(`world/ammo_table.h kImpactEffectTagNames`) → `game_world._route_round_impacts`
destructively drains each row and presents both its generic World-domain particle transient
and 3D soundset, retaining production tick/order and catch-up age. Joiners now feed decoded
S2C tag-2 round events through `NetClientView`/`ClientRuntime` into a visual-only
`RoundSim`, so the impact route and flying throwable item presentation run client-side while
the authority gates above continue to prevent client damage. Decoded remote collision
(2026-07-23) is the wire-keyed proxy projection — never a cast of server handles into the
client-local registry: Player AND non-player Infantry rows join the person walk as
`ProjectilePersonProxy` (decoded position + torso stand-in; retail's posed bones remain the
shared D-WPN-8 residual), and decoded pool-1 movers join the dynamics pass as
`ProjectileDynamicProxy` — the AUTHORED collision geometry (items.def graphic → the same
CFAC model the local ghost had) posed from the decoded fields the retail client entity holds
(live coarse heading byte → entity+16; retained spawn/dead pitch/roll → entity+20/+24), in
wire-handle (= host pool slot) order in the retail pass position. A visual client's
load-frozen local pool-0/1 mission copies are excluded from the projectile walks (retail has
no such ghosts — its client pools ARE the decoded entities, §5.23); pool-2 statics keep
serving from the local load. The mounted shooter's own carrier is excluded via the decoded
carrier + host-fed seat table (Controller/Gunner/Driver, the ray[18] analog). Residuals:
PANM/turret section posing and husk-model substitution for wire dynamic proxies; movement
contact/blink/LOS still read the load-frozen local set; the person-table pins are
`projectile_combat` (`test_visual_dynamic_proxy_*`, `test_visual_infantry_proxy_*`,
`test_person_walk_orders_local_player_by_its_server_handle`) plus the
`coop_two_sim` decoded-vs-ghost pose test. That wire-handle order is MATERIALIZED rather than
merged in step (2026-07-25): the local player joins the walk under its published SERVER handle,
which is unrelated to its local registry slot, so `trace_projectile` builds one walk plan over
persons + proxies and `stable_sort`s it by that key before walking — persons appended first keeps
the old person-before-proxy tie rule, and the sort runs only when proxies participate, so the
authority/SP walk keeps registry (= retail pool) order byte for byte. Per-weapon tracer cadence is
still not retained in the client state — shooter TEAM is (`ClientEntityState::team`, retained and
consumed 2026-07-25) — and clean 0x46/0x5D disconnect retirement of the proxy set landed with
D-NET-176 (FIXED 2026-07-25; §5.16).
Deployed throwables still lack the 0x59/0x12
runtime path (D-WPN-8/D-THROW-7; §5.36). Three ledger rows record the audit:
**D-WPN-14 is resolved as a false reading** (ballistic arrival timing already matches),
**D-WPN-15** carries selection legs:
charmap sampler + `.TIL` overrides unported → terrain always takes the retail no-map dirt
default; person hits take tag 2; CFAC static/dynamic hits now carry `poly_type + 4`;
water-plane hits take tag 11. The remaining object-tag gap is the building
material-1→23 special case; and
**D-WPN-16** tracks the genuinely unported Knife/instant-kill-zone family. Pinned by
`nova_simulation_test.gd` (the local FIRE→impact route) and `npruntime_round_sim`
(the bake rules + the tag-2 impact row).

**v29 LIVE (2026-07-03, two retail clients + the host player,
`.scratch/retail_join_v29_game.pcapng`):** the pipeline worked end-to-end — 35 C 0x06
across both joiners, one kill applied host-side, and the death broadcast REACHED THE WIRE
(one S2C 0x13 + 0x1E pair delivered to BOTH clients; the killed joiner redeployed through
its own deploy request). Three defects surfaced and were fixed same-day: the fire-direction
frame (D-NET-153 — the kill asymmetry the user reported), tag-2 budget starvation
(D-NET-154 — zero round events despite live observers), and the stale 0x16 roster
(D-NET-155 — HUD player count stuck). The "anims not quite correct" report is the tracked
body-motor item (off-14 states / off-15 channel ratio), not a new defect.

**v30 LIVE (2026-07-03, same stack): D-NET-153 and D-NET-154 VERIFIED on the wire.** Kill
symmetry both ways — the 0x13 bodies read victim 0x0001 killerSource 2 (the direction v29
could not produce), victim 0x0002 killerSource 1, and victim 0x0000 killerSource 1 (the
HOST player killed by a joiner); **25 tag-2 round-events on the wire = the §5.9.1 positive
observer witness at last** (every one of the 25 fires echoed exactly once to the other
client; sample: `shooter=0x0002 origin+yaw/pitch+shotSeq` intact). Exactly three player
entities all session (0x0000/0x0001/0x0002) — a killed joiner's redeploy REUSES its entity,
no ghost bodies; the body holding its death spot until respawn is the retail-correct
D-NET-66 behavior (the standing-idle LOOK is the body-motor anim gap — the death anim
state is not streamed yet). D-NET-155's first cut was found half-broken (see its entry) and
re-fixed; HUD count re-verifies v31.

**Remaining port follow-ups:** the payload writers
(`BuildDeathNotifyPayload`, `GameEvent_BuildPayload`, `NetPacket_WriteThreeInt32s`,
`NetPacket_WriteEntityHandleWithByte`, `NetPacket_WriteEntityHandleAndTeam`) byte layouts;
the threshold-crossing tumble PRNG/local frame (spawn-time recoil/spread is closed above);
the SpawnRound default-path field flow into the 780-B round record (the array
`@ 0xB7E1A8`, 128 groups × 4 × 780 B, active-flag bytes `@ 0xB7DFA0` — witnessed via
`Weapon_UpdateAllProjectiles @ 0x4EC020`, zone floats RESOLVED: head 1.25 `flt_7C6F18`,
limbs 0.5 `flt_7C3B94`, zones-13/14 3.0 `flt_7C6F80`, vehicle seats 6.0 `flt_7CD510`);
the wounded/medic loop (`Player_OnDamageReceived @ 0x4DD880`,
`GameEvent_RevivePlayer`, S2C 0x3A); `Entity_InitSpawnedChild` at death (corpse/drop);
the S2C 0x13/0x52/0x54 client handler bodies (`@ 0x42EB50` §5.35, `@ 0x429040`).

Per-system verdicts from grilling the reimplementation against retail
`Jointops.exe` (Kong IDB). Each row cites the original entry point. Verdict:
**matching** | **divergent → fixed** | **divergent (accepted)** | **unknown**.

### 5.61 Advance & Secure — spawn selection, the zone chain, and the capture loop (engine-research scope, 2026-07-03)

Witnessed to scope the full-AS-game port (gametype 0x10010 on ASH_I5A): how the deploy map
gets its selectable zones, how the client's pick reaches and gates on the server, and the
complete server-side zone-capture loop. Confirm-only from the binary (no reimpl yet except
the §5.2c marker fallback); §5.2c (marker family + `Entity_FindBestSpawnPoint`), §5.19
(0x40 client decode), §5.26 (0x1E client decode), §5.31 (0x6E client decode) and §5.49
(0x6F/0x53 client decode) are the client-side halves of this witness.

**The zone data model.** A capture/spawn zone is an ordinary pools-1/2 entity whose ItemDef
carries attrib flags `0x20000` (capture trigger) and/or `0x40000` (deploy-selectable spawn
point) — the ASH_I5A "Change Team & Spawn Volume" tent/HQ objects (0x0575/0x0576, D-NET-70
pool routing) carry both. On the entity: `+538` (u8) = the authored ZONE NUMBER (0 = not a
chain zone — plain flag/base), `+354` = owning team, `+540` (i32 16.16) = the SECURE/control
fraction 0..1.0, `u16 +350` = the zone radius in world units, `+544`/`+545` = friendlies/
enemies in radius (refreshed each pass, ride the 0x6F tail), `+546/547/548/550` = the
client-side landing block (§5.49 0x53 / §5.31 0x6E). Pool-3 markers configure the chain:
def-types **6003/6096 → team 1, 6004/6097 → team 2, 6090/6098 → team 3, 6091/6099 → team 4**
(the marker's `+538` = that team's assigned/base slot number; any other numbered pool-3
marker → the team-0 row), def-type **6006** = the radius capture zones of the non-chain zone
modes (proximity only), def-type **6007** = sub-spawn scatter points inside a zone (below).
`[orig: ZoneSlotChain_BuildFromMission @ 0x4A2DE0; Server_UpdateCaptureZoneProximity
@ 0x5086A0 (6006 @ 0x5089EF)]`

**The zone-slot chain (the AS frontier).** An inline manager struct `g_zone_slot_chain
@ 0x24D1EBC` (renamed from the kong misnomer `CWeaponSlotManager` cluster — it manages
capture-zone slots, not weapons): five per-team `[ownedMask, assignedSlot]` dword pairs
(teams 0–4) + a `std::vector` of `{entity*, u8 rank}` wrappers. Built at mission start
(authority only, `Game_StartMission @ 0x524360 @ 0x526117`): pool-3 markers seed the
assigned slots, then every alive pools-1/2 entity with `+538 != 0` and def `0x20000` is
added; `ownedMask[team] = OR(1 << zone_no)` over that team's zone entities
`[orig: ZoneSlotChain_RebuildOwnershipMasks @ 0x4A26C0]`; `rank` = descending index within a
shared zone number `[orig: ZoneSlotChain_AssignZoneRanks @ 0x4A27F0]`. The FRONTIER rule
`[orig: ZoneSlotChain_IsZoneCapturableByTeam @ 0x4A2450]`: team T may capture zone Z iff
`Z == assignedSlot[T]` and Z isn't already T's, OR `(1<<Z) & mask[T]` and not T's, OR an
ADJACENT zone number Z±1 inside `mask[T]` is owned by T (the leapfrog chain — walk the
vector; an enemy-held adjacent kills that direction). Gametype 0x50010 (327696) is exempt
(every zone always capturable). `ZoneSlotChain_FindFrontierZone @ 0x4A2AC0` = the first
vector entry capturable by T → its zone number (the "go capture zone N" hint).
`ZoneSlotChain_GetWinningTeamIfAllOwned @ 0x4A2920` (gametypes 0x10010/0x50010 only) is the
all-zones-one-team check that suppresses further capture events (and feeds the AS win).

**How the deploy map is advertised.** There is NO dedicated zone-list message. The client
builds the deploy screen from (a) the world stream itself — the zone entities arrive as
ordinary 0x0D/0x10 records with team + type; (b) the per-player **S2C 0x0F variant-0 u32
owned-zone mask** = `ZoneSlotChain_GetOwnedZoneMask(player.team)` — bit `1<<zone_no` set iff
EVERY entity of that number belongs to the team (§5.55 already field-mapped the u32; its
producer is now witnessed) `[orig: NetPacket_WritePlayerState @ 0x4FF6B0 @ 0x4FF9A3]`;
(c) the 0x40 minimap color channel (§5.19); (d) the 0x6F/0x53 zone timers (§5.49); (e) the
0x6E spawn-wave status (below). The S2C 0x4D spawn-slot tip is NOT zone data (player-slot
index only, §4 row). A dead player also gets a private 0x1E event **0x3A (58)** carrying
`FindFrontierZone(team)` in the attacker byte — the deploy-screen objective hint
`[orig: Server_ProcessPlayerDeath @ 0x517740 @ 0x517A1D]`.

**The pick: C2S 0x0E `[i16 spawnHandle]`** (`Server_ProcessClientRequestRespawn @ 0x519AF0`;
§4 row refined). `0xFFFE` = auto-pick: spectators/unassigned resolve a team first
(`& 0x20000` → 1; `& 0x10000` → `2 − (tick & 1)`), then `find_spawn_entity_for_team
@ 0x4FC810` picks an owned zone on the frontier (enemy-capturable, or the team's own
frontier number) with control ≥ 1.0 — i.e. AS auto-deploy = "the front line"; gametype
0x50010 returns without spawning. A real handle resolves via `Server_ResolveSpawnTargetHandle
@ 0x4FE110` (pools 0/1/2 only, def attrib `0x40000` required, team must match — or the
player be teamless): pool 0 covers MOBILE spawn vehicles. Gates, in order: not already
pending (`+89932` = the revive latch), `+364 == 0` — **note this one is on the
`requestedHandle != -1` path only `[orig: @ 0x519c67]`, so a Default Spawn pick (`0xFFFF`)
skips it entirely** — then for a resolved target — a VEHICLE
target (`ItemDef.type == 1` + attrib `0x40000`) must be alive with a free seat
(`Entity_FindBestSeatSlot @ 0x4351F0`; the deploy then latches `entity+44 |= 0x4000` and
boards the seat after the reset), a NUMBERED zone requires `team match && control ≥ 0x10000`
(a contested zone stops accepting spawns), and the config `g_respawn_requires_team_dead
@ 0x24D2260` denies a target-less respawn while the team still has a live entity. The
requester must be dead (`entity+36 & 2`) or respawn-flagged (`+89912 & 0x10`)
(@ 0x519cc7 — the dead-or-pending gate).

**The post-death respawn HOLD, and why a first pick is expected to be dropped (witness
2026-07-26).** Immediately after the dead-or-pending gate the handler silently returns
while the requester's respawn timer runs — `if (*(_DWORD *)(playerSlot + 360)) return;`
`[orig: @ 0x519cf2]`. This gate applies to EVERY pick including the Default Spawn row, and
it is armed on EVERY death: `slot+360 = max(g_respawn_timeout @ 0x24D214C, 3)` seconds,
forced to 3 when the victim spawned within the last 620 ticks
`[orig: GameEvent_PlayerDeath @ 0x516ec4-0x516eeb]`; `slot+364` is armed alongside it only
when `SpawnZoneList_GetCount()` is non-zero `[orig: @ 0x516ecc]`, which is why the two
timers gate different paths. So the FIRST `0x0E` a player sends after dying — which is
exactly what a machine-opened death screen produces — normally lands inside the hold and
draws NO reply of any kind. Retail tolerates that because its picker re-dispatches on
every click (`DeathScreen_OnSpawnListSelect @ 0x553630` has no re-entry gate) and its
screen lifetime is the victim's own entity-dead state, not a client-side pick FSM. A port
that sends one pick and waits for a reply therefore wedges; see D-NET-186.

**The deploy-screen HOLD chain (witness 2026-07-03, D-NET-156 — the v31 root cause).** The
picker UI's lifetime is a per-frame SERVER signal, not a one-shot:

1. **Join** `[orig: Server_OnPlayerJoin @ 0x51A690]`: slot stateByte (+89912) = 1 (3 when
   `g_preround_delay_timer`) @ 0x51a6d3/0x51a6e2, then `|= 0x10` **iff
   `SpawnZoneList_GetCount() > 0`** @ 0x51a6f2 — **bit4 = RESPAWN-PENDING/undeployed, the
   JOIN-time marker only** (death does NOT set it; the death screen is client-local from the
   entity-dead flags). Slot state (+32) goes 6 (in-game) immediately — 0x0A flows to a
   deploy-screen client; **deploy is a PLAYER flag, not slot-state 9** (resolves the
   D-NET-134 confusion). `Server_PositionPlayerForSpawn` pre-places the entity (the deploy
   camera anchor), respawn timer +292 = 620 ticks; then 0x42 → first 0x0A → 0x0F → 0x4D
   `[u8 playerIndex]` → the random seed.
2. **Hold** — every 0x0A's `flags1` bit1 = `slot+89912 & 0x10` (§5.9 flags1 row; the writer
   also ORs the entity's hidden bit0 each frame → the golden pre-deploy record byte13
   `0x01`); the client sets `g_deploy_screen_active` from the bit EVERY frame, so one
   bit1=0 frame closes the screen — the 0x0F gameFlags-bit0 flash alone dies < 16 ms later.
3. **The pick send** `[orig: Input_HandleActionBinding case 12 @ 0x49b0c5-0x49b17b]`:
   C2S 0x0E `[i16]` = `SpawnZoneList_GetByIndex(param − 1)` converted to `pool<<12|slot`;
   param 0 → 0xFFFF, 65534 → 0xFFFE auto-pick; also clears dialogs + sets `dword_81474C = 1`.
4. **Clear + RELEASE** — a successful deploy runs `Server_ProcessPlayerDeath @ 0x517740`'s
   deploy leg, which **clears bit4 (`and 0xEF` @ 0x517791)** → the next 0x0A's flags1 bit1
   drops → the client closes the screen and enters the world (the hidden bit0 stops being
   re-ORed) — **and sends the release bundle: the loadout re-send
   (Server_SendWeaponSlotListToPlayer @ 0x502550) + the 0x61 seed (Server_SendRandomSeedToPlayer
   @ 0x5101a0) + the 0x1E hint, one datagram (golden f=240018, after the spawn-wave countdown;
   the pick itself drew only the 0x6E wave ack at f=238948)**. The bundle is load-bearing: the
   pick set the client's `dword_81474C` wait-gate and the **0x5A apply is what resets it**
   (§5.30 @ 0x4290E0) — without it the client never resumes its per-frame C2S 0x0C uplink
   (the v32 rubber-band, D-NET-156).

Reimpl: `netsim::Connection::respawn_pending` (set in `Server_BuildPlayerInfoAndAdd` iff
`world_has_spawn_zone`, host loopback exempt; cleared by the 0x0E dispatch case) → the
per-connection flags1 in `emit_connection_s2c`; the 0x0E case gates dead-or-pending. The
S2C 0x6E empty-group form (`[u8 0]`) goes 1 Hz to pending/dead players (`Server_TickUpdate`;
the recipient mask includes bit4 `[orig: NetPacket_WriteSpawnWaveStatus @ 0x5074c2]`).

**The deploy SCREEN itself (witness 2026-07-24 — the D-NET-96 "no deploy-map UI" close).**
The picker the hold chain drives is `death.mnu`'s **DEATH** screen — an ordinary .mnu
screen the engine binds by (screen, control) name, NOT a bespoke draw. The shroud reveal
driver `DeathScreen_UpdateShroudReveal @ 0x554730` (ex-sub_554730) shows `DEATH_SHROUD`
IMMEDIATELY while `g_deploy_screen_active` (the 0x0A flags1 bit1 latch) and only after a
240-tick delay for a plain death, refreshing `UI_UpdateDeathScreenContent @ 0x5536a0`
every 16 ticks. `UI_RegisterDeathScreenCallbacks @ 0x554610` binds: the MAP window's
input/render handler (`command_map_overlay_input_handler @ 0x554310` — left-drag pan,
right-drag/wheel exponential zoom ×1.005, clamp 0.1..10; its render pass calls the
WINDOWED map view draw `MapOverlay_DrawView @ 0x5a58e0` (ex-sub_5A58E0, the sibling of the
fullscreen `HUD_DrawMapOverlay @ 0x5a5f40`) at player-pos + pan), `SPAWNPOINTS_LIST`
select → `DeathScreen_OnSpawnListSelect @ 0x553630` → `Input_QueueEvent(12, node)`,
`SWAP_TEAMS`, `HIDDEN_BACK` → the CONFIRM_EXIT dialog, and the confirm pair. The list
populate (`@ 0x5536a0 @ 0x553a83`): row 0 = `"'<Menu/DEFAULT_SPAWN_KEY>' <Menu/HOME>"`
with node **0** (→ the parameter-0 pick), then one row per client zone-table entry that
is team-matching, def-attrib-0x40000, NOT minimap-flagged 0x40, and — for a numbered
zone — SECURED (`zone_timer.value >= limit`): text
`"'<'A'+idx>' <gametext WPNames/STRWPNAME%03d(idx+1)>"` with node **idx + 1**, where idx
= `SpawnZoneList_IndexOf @ 0x43B990` over the sorted registry; the team color rides
inline tags (`<c4040FF>`, team 2 `<cFF2020>`). `update_death_screen_ui @ 0x553150` fits
the initial map zoom to the zone AABB (`Entity_GetWorldBounds`) and shows
`SWAP_TEAMS`/`BUTTON_TEAMLIST` only for the TDM family. The zone REGISTRY builder
`Entity_BuildSpawnZoneList @ 0x43EAE0` is now fully witnessed: collect pool 2 then
pool 1 (attrib 0x40000, NO alive filter), AABB accumulated en route (NOTE:
`g_WorldBoundsMax @ 0xA89294` accumulates the MINIMUM and `g_WorldBoundsMin @ 0xA89288`
the maximum — swapped IDB names), bubble-sort ascending by
`((type==1 ? 2 : type==32 ? 1 : 0)<<16) | (unitType&0xFF)<<8 | (zone# & 0x1F)` (def
`type @ +92`, `unitType @ +406` — the items.def `unit_type` keyword), equal nonzero keys
stable, both-zero keys tie-broken by entity ADDRESS (`@ 0x43ecc6`). **Pick pacing:**
`NapiClient_WaitForGameStart @ 0x42cc10` sets the `dword_81474C` uplink hold at ENTRY
(before sending the empty C2S 0x0A) — not only the case-12 pick; EVERY S2C 0x5A apply
clears it (§5.30), so the initial grants re-open the uplink before the pick re-arms it.
Case 12 has NO re-entry gate and no client-side retry: a host silently dropping an
invalid/contested pick leaves the screen up (bit1 stays set) and each further list click
queues a fresh 0x0E.

Reimpl (2026-07-24, the deploy-screen slice): `world::build_spawn_zone_list` /
`spawn_zone_index_of` (libs/world/spawn_select — the sorted registry + true-min/max
AABB; the both-zero-key address tie is modeled as collect order, documented in the
header); `JoinerConnection::set_player_paced_deployment` + `frame_deployment_pick`
(the AwaitDeployPick stage — the shell paces the pick, re-picks allowed, headless
callers keep the auto parameter-0 default) + `frame_loadout_resubmit` (the armory
ACCEPT re-send) with `ClientRuntime` queueing; `NovaSimulation.get_deploy_spawn_zones`
/ `send_deployment_pick` / `get_join_assigned_team`; `NovaDeployScreenPresenter`
(godot/engine/world/deploy_screen_presenter.gd — death.mnu DEATH over the live world, the
witnessed populate/colors/row-0, self-closing on the release); the game_world join
watchdog now ENDS at the player-paced pick. Pinned by `npruntime_client_runtime`
`run_roundtrip_with_spawn_zones(paced)` (park → invalid pick silently dropped →
re-pick → release; the auto mode unchanged) and `zone_chain_test`
`test_spawn_zone_registry`. Residuals: the MAP window ships chrome-only until the
`MapOverlay_DrawView`/`@ 0x5a5f40` draw witness lands (D-HUD-19). The client-side
0x5A grant apply and live 0x6F team/value/limit fold landed 2026-07-24 (D-NET-170);
the per-zone occupant subrows still need deployed-roster data (D-HUD-19).

**Spawn waves.** `g_spawn_wave_list @ 0x24E0E48` (renamed from `stru_24E0E48`): 56-byte
entries `{player[8], count@+32, zoneEntity@+36, interval@+40, countdown@+44, preDelay@+48,
team@+52}`, built at mission start from every `0x40000` pools-2/1 entity — numbered zones
get `g_spawn_wave_time_zone @ 0x24D2250`, un-numbered (bases) `g_spawn_wave_time_base
@ 0x24D224C` (both from `apply_session_settings_to_globals @ 0x551500`; entries only exist
when the interval is configured — 0 ⇒ no wave system ⇒ instant deploys)
`[orig: SpawnWaveList_BuildFromMission @ 0x52A920]`. A 0x0E pick lands in the zone's group
(`SpawnWaveList_TryQueuePlayer @ 0x52A490` — dedupes, caps 8, evicts the player from other
groups) → the player gets S2C 0x6E and WAITS; if no group exists for the zone the deploy is
immediate. The 1 Hz `SpawnWaveList_Tick @ 0x52A550` → `SpawnWaveList_TickEntry @ 0x52A330`
releases ONE queued player per interval (`Server_ProcessPlayerDeath(player, zoneHandle)`,
countdown reloads from `+40`), and FLUSHES the whole group the moment the zone's control
drops below 1.0. A zone team flip resets its group (`SpawnWaveList_ResetOnZoneTeamChange
@ 0x52A5B0`). **S2C 0x6E** (§5.31's "squad roster" — actually the deploy-screen wave
status; the client's `teamSlotHandle → entity+548` is the WAVE COUNTDOWN in seconds):
`[u8 groupCount]` then per team-matching group `[u16 zoneHandle][u16 zoneIdx (spawn-zone
list index)][u8 queuedCount][u16 countdown][queuedCount × u16 playerHandle]`, sent on queue
join (`Server_SendSpawnWaveStatusToPlayer @ 0x50FF10`) and every second to each dead or
deploying player (mask 0x20) `[orig: NetPacket_WriteSpawnWaveStatus @ 0x507490;
Server_TickUpdate @ 0x51E0CF]`.

**Placement.** `Server_PositionPlayerForSpawn @ 0x50CF60` (ex-`CMap_SetupSpawnCamera`,
§5.2c) is the placement for BOTH paths. Picked target: copy the target entity's pos+angles,
offset by a named model userpoint when present (`modelgpm_FindUserpointByName @ 0x5B2170`;
name string is runtime-set — follow-up) else `z += 1.0`; for a NUMBERED zone, collect ≤ 32
pool-3 **type-6007** markers within the zone radius and round-robin `g_spawn_cycle_counter
% (count+1)` — 0 = the zone entity itself, else the (i−1)th 6007 marker (parent-transformed)
— the authored in-zone scatter. No/invalid pick: the §5.2c game-type marker chain
(6096-6099 primary per team, 6003/6004/6090/6091 fallback, 6094/6001 co-op, 6095/6002
DM/SP) through `Entity_FindBestSpawnPoint @ 0x50CCC0` (farthest-from-enemy, D-NET-115);
co-op additionally falls back to any team-matching numbered `0x40000` entity. `+89932`
(revive) overrides everything with the saved body position. The deploy itself is
`Server_ProcessPlayerDeath @ 0x517740` — death and deploy are ONE routine (arg2 = killer
handle on death, spawn-target handle on deploy): detach, position, `Entity_ResetToSpawnState
@ 0x4B9610`, respawn timer `+292 = 620` ticks, loadout + 0x1D list, seed 0x61 (mode 1), the
0x1E 0x3A hint, optional seat board.

**The 1 Hz capture block** — all of it inside `Server_TickUpdate @ 0x51D7E0`'s
`g_periodic_second_timer = 62` block (`@ 0x51DF50..0x51DF8C`), so every capture/secure rate
below is per-SECOND, while the client rescales wire seconds ×62 into ticks (§5.49):

1. **Proximity** `[orig: Server_UpdateCaptureZoneProximity @ 0x5086A0]` — per playing slot:
   zero the proximity bitmask `player+89868`; sync CRenderState field 0x1C (score) via a
   4-byte S2C 0x81 on change (`@ 0x508790`); set bit 0..4 for pool-1 def-types
   4095/4091/4093/4097/4096 within 20.0 (a carried flag counts at the carrier's position);
   set bit `1 << zone.team` for pool-3 6006 radius zones and for frontier-active numbered
   `0x40000` entities (3D: 2D dist ≤ radius, |dz| ≤ radius/2); drive the presence counters
   `[23593..95]` against the per-gametype config table (`sub_52D430(g_GameType, 0xC/0x24)` —
   follow-up) into `GameEvent_ProcessScoring @ 0x52F550`.
2. **Capture requests** ride the CT (Change Team Box) physics touch, not this block:
   a live player touching a `0x20000` entity (authority, no preround,
   gametype & 0x30000) calls
   `Server_OnPlayerTouchCaptureZone @ 0x500BA0` `[orig: caller
    movement collision resolver @ 0x4B2BD0 @ 0x4B3238]`: gate
   `zone un-numbered || player.team == zone.team || control ≤ 0` (an enemy can only START
   on an unsecured zone; the securing OWNER always marks presence), mark the active-capture
   presence slot, and queue `{zone, player.team, player}` — numbered zones only while on
   SOMEONE's frontier (`CaptureCtx_QueueCaptureRequest @ 0x53B7A0`; a refused numbered
   touch arms a 10 s repeat-nag latch `+89912 |= 0xC`).
3. **The secure pass** `[orig: Server_UpdateCaptureZoneEntities @ 0x519690]` — per numbered
   `0x20000` entity: if the ENEMY frontier cannot reach it, latch `control = 0x10000`; else
   run `calculate_capture_zone_control_delta @ 0x501120` (below); emit **S2C 0x6F** every
   pass (15 B `[u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 friendlies]
   [u8 enemies]`, mask 0x80 urgent — §5.49's value/limit ARE the control fraction, limit
   fixed 1.0) `[orig: NetPacket_WriteZoneTimerValue @ 0x506E70, emit @ 0x5197D9]`; on the
   0→1.0 edge broadcast **0x1E event 0x3B (59)** and on the →0 edge **0x3C (60)** (attacker
   byte = the zone's spawn-zone-list INDEX, victim byte = zone team) `[orig: @ 0x519839 /
   @ 0x51988E]`; convert every in-radius entity whose def has `+88 & 2` to the zone's team
   (`Server_ChangeEntityTeam @ 0x518D70` → the S2C 0x50 broadcast — armories/emplacements
   flip with the zone).
4. **The control formula** `[orig: calculate_capture_zone_control_delta @ 0x501120]`:
   `presence = friendlies − frontier-eligible enemies` in radius (both counts stored to
   `+544/+545`); zero presence ⇒ no change. Else `teamSize` = capturing side's player count
   `+ (6 − total)/2` when fewer than 6 in-session (small-server boost), soft-capped
   `x → cap + (x − cap)/2` at 20/40/60; `speed = teamSize × base` where base =
   `g_capture_speed_setting @ 0x24D2254`: 1 → 24, 2 → 48, else 12; when the capturing side
   owns fewer zones AND the round clock is inside the last `30 × g_respawn_time` seconds,
   an underdog catch-up subtracts up to half: `speed −= min(imbalance × boost, 1) × speed/2`
   with `imbalance = |zones₁ − zones₂| / totalZones`, `boost = 1 − 2·roundRemaining/(3720 ×
   g_respawn_time)` (3720 ticks = 60 s) `[orig: @ 0x5013AB..0x501439]`; a zone number shared
   by N entities divides speed by N. `delta = 65536 × presence / speed` (ftol; minimum
   magnitude 1, sign = presence), accumulated into `+540` with clamp [0, 0x10000]. At the
   observed default (`g_capture_speed_setting = −1` ⇒ base 12): one attacker on an empty
   3-player-team server secures ~0x10000/1820 ≈ 36 s.
5. **The timed-capture engine** `[orig: Server_UpdateCaptureZones @ 0x53B8F0]`, ctx
   `captureCtx @ 0xC947A8` (`+8/+12` = the request queue, 12-B `{zone, team, player}`;
   `+24/+28` = active captures, 152-B `{zone@0, team@4, progress@8, limit@12, player@16,
   presence[32]@20, rate@148}`): per active entry `CaptureCtx_UpdateActiveCaptureRate
   @ 0x53B600` counts the presence slots into `rate` (clamped 1..32; broadcasts the 3-byte
   **S2C 0x6C** `[u16 handle][u8 count]` on change `[orig: NetPacket_WriteZonePresenceCount
   @ 0x506DE0]`) and clears them. A queued request for a DIFFERENT team restarts the entry
   (`team = new, progress = 0, limit = g_capture_duration @ 0x24D2248`) + emits **S2C 0x53**
   (9 B `[u16 handle][u8 curTeam][u8 capturingTeam][u16 progress][u16 limit][u8 rate]`,
   §5.49's window fields de-mystified) `[orig: NetPacket_WriteZoneTimerWindow @ 0x506D00,
   emits @ 0x53B9C0/0x53BA36/0x53BA68/0x53BC02]`; otherwise `progress += rate`, 0x53 each
   pass, and on `progress ≥ limit` the zone FLIPS: `Server_ChangeEntityTeam(zone,
   capturingTeam)`, proximity scoring (`CaptureZone_CheckProximityScoring @ 0x500C50`), wave
   reset, `GameEvent_FlagCapture`, entry removed. Queue drain: a NUMBERED zone (or
   `g_capture_duration ≤ 0`) flips INSTANTLY — team change (via neutral when previously
   owned), `control = 0` (the new owner must now SECURE it — the Advance-and-Secure beat),
   FlagCapture event; an un-numbered flag zone neutralizes first and opens a timed active
   entry (`CaptureCtx_AddActiveCapture @ 0x53B510`).
6. **Flip events** `[orig: GameEvent_FlagCapture @ 0x50F6F0]` (requires def `0x40000`):
   numbered zone → to the capturer's team **0x33 (51)** / to the enemy team **0x32 (50)**
   when the frontier masks did not change, else **0x35 (53)** / **0x34 (52)** carrying each
   side's NEW `FindFrontierZone` number in the victim byte (team-filtered sends, mask
   0x180); then an all-recipients **0x38/0x39 (56/57)** banner keyed by the new owning team;
   un-numbered flag → **0x2B/0x2C (43/44)** by team (the §5.19 dvxi5 probe's
   `STRCND_PSP_*TAKEN` pair). All suppressed once `GetWinningTeamIfAllOwned` reports the
   match decided. The kill-feed body stays the §5.26 8-byte `GameEvent_BuildPayload
   @ 0x5054E0` shape — for zone events the "attacker/victim" bytes are the zone-list index
   and team/frontier numbers, NOT pool-0 indices.
7. **Team enforcement** `[orig: Server_EnforceZoneEntityTeams @ 0x519600]` — every numbered
   entity in the per-tick zone-numbered registry (`dword_A892D0/D4`, rebuilt by
   `Server_BuildEntitySlotLists @ 0x4F97A0`) is forced onto the team whose OWNED mask
   contains its zone number (team 1 precedence) — this is what flips the co-located
   `0x40000` spawn objects when the `0x20000` trigger objects change hands.

The deploy/spawn-zone REGISTRY (`g_spawn_zone_list/count @ 0xA89188/0xA89184`, rebuilt by
`Entity_BuildSpawnZoneList @ 0x43EAE0` — ex-"BuildSortedRenderList"): every pools-2/1
`0x40000` entity, sorted by `(class-priority, def+406, zone# & 0x1F)`, plus the deploy-map
AABB (`g_WorldBoundsMax/Min`). Its INDICES are what ride the 0x6E `zoneIdx` and the 0x1E
zone-event attacker bytes (`SpawnZoneList_IndexOf @ 0x43B990`).

**Config globals** (all mirrored from the parsed session-settings block by
`apply_session_settings_to_globals @ 0x551500 @ 0x551D3E..0x551DBD`): `g_capture_duration
@ 0x24D2248` (un-numbered flag capture time, wire `limit`), `g_capture_speed_setting
@ 0x24D2254`, `g_spawn_wave_time_base @ 0x24D224C`, `g_spawn_wave_time_zone @ 0x24D2250`,
`g_respawn_requires_team_dead @ 0x24D2260`. Observed defaults in the live IDB snapshot:
speed setting −1 (base 12), waves unset.

**Follow-ups (open):** the per-team `dword_C87B54 + 85·team` CRenderState field-6 gate that
can skip the primary marker chain in `Server_PositionPlayerForSpawn @ 0x50D1DB`; the
userpoint NAME used for spawn offsets (`off_7CF9C4` is runtime-set — static bytes are code);
the `sub_52D430 @ 0x52D430` per-gametype config table (indices 0xC = capture-score
threshold, 0x24 = scoring interval) and its table source; which gametype 0x50010 is (the
zone-chain-exempt sibling — KOTH family suspected); the exact session-settings VarList keys
behind the `0x2550B7x` mirror block; `CaptureCtx_MarkPresenceSlot @ 0x53B5C0` internals
(presence-slot indexing); the 0x1E event-string table rows for 50-60 in
`game_event_strcnd_key` should be cross-checked against these producer semantics at HUD
time. Win conditions (`Server_CheckWinConditions @ 0x51AD40` reads the chain masks
`@ 0x51AD87/0x51B4A2`) are roadmap item 4, unwitnessed here.

**Reimpl (slice 1 — spawn selection, same session).** Ported: `world::ZoneChain` +
`zone_chain_*` (`libs/world/zone_chain.{h,cpp}` — build/masks/ranks, the frontier rule, the
owned-zone mask, the control latch), `Entity::zone_number` (BMS byte 155 `lfp_group` carried
by `mission/promote.cpp`) / `zone_control` / `is_capture_trigger` / `is_spawn_point`
(items.def attribs `changeteam 0x20000` / `spawnpoint 0x40000`, already parsed by
`libs/def`; stamped + chain built + latched in `NovaSimulation::resolve_item_traits`);
`resolve_spawn_target` / `find_spawn_zone_for_team` / `spawn_pose_for_target` /
`select_player_spawn_for_team` (`world/spawn_select`); the full C2S 0x0E handler
(`npruntime/server_message_dispatch.cpp` — pick resolve, the zone control/team gate, the
0xFFFE frontier auto-pick, dead-only deploy, per-team marker fallback, the computed 0x1E
ev-0x3A frontier hint replacing the golden byte-blob); the per-recipient 0x0A phase-0
owned-zone mask (`PlayerReplicationState::uniform_team_mask`, default the golden 0x8); and
the per-team join placement (`Server_BuildPlayerInfoAndAdd` assigns the team BEFORE the
§5.2c marker scan — previously both AS teams spawned at the first family type present, i.e.
team 1's base). Pinned by `zone_chain_test` (the ASH_I5A shape: masks/frontier/latch,
capture progression, auto-pick, pick resolve, per-team markers — golden mask 0x8
reproduced). Slice-1 deferrals (all §5.61-cited in code): the spawn-wave system
(`g_spawn_wave_list` + 0x6E — host wave options default 0 = immediate deploys, matching
retail defaults), vehicle-seat deploys (seat model unported), deploy-time 0x61/0x1D
re-sends, and the 6007 in-zone scatter + userpoint offset.

**Reimpl (slice 2 — the capture loop, 2026-07-04, D-NET-162).** Ported:
`world::zone_capture_tick` (libs/world/zone_capture.{h,cpp} — the 1 Hz secure/control
pass with the enemy-frontier latch, the control-delta formula verbatim, secure edges,
instant numbered flips via neutral, mask rebuilds, non-trigger zone-object team
enforcement) + the npruntime 1 Hz wire block (0x6F change-gated + deploy-screen refresh,
the 0x1E zone-event family 0x3B/0x3C/50-53/56/57 team-filtered, 0x53 on flips, and the
0x40 zone + vehicle-blip overlay feed). `zone_chain_test` pins the delta formula and the
full flip→secure→contest→neutralize→retake cycle. Deferrals in the D-NET-162 row.

**v31 LIVE (2026-07-03, 2 retail clients): the deploy screen still did NOT appear — and
the wire shows ZERO C2S 0x0E all session, so the slice-1 pick handler went unexercised;
the gap is the ADVERTISING side (what makes the client SHOW the picker), not the pick
path.** Open leads for the alignment pass: (a) the phase-0 owned-zone mask rides the
per-frame 0x0A, but `Server_SendEntityStateToPlayer @ 0x517BA0` has the **state==6 deploy
gate** (D-NET-134 row) — a client sitting at the deploy screen (game state 9) receives no
0x0A, so the mask CANNOT be its picker source; the pre-deploy carrier is likely the S2C
0x0F body itself — `NetPacket_WriteWorldStateLoad0x0F @ 0x502D10` is UNWITNESSED (it reads
`g_respawn_requires_team_dead`, so it is the deploy-screen state block); (b) the zone
objects reach the client as pool-1 0x0D records — D-NET-70's "Change Team & Spawn Volume"
objects ride 0x0D **team-gated `spawn_flags & 0x10`**; whether our 0x0D encoder sets that
bit/team for the 1359 bunkers is unverified; (c) the CLIENT deploy-map list builder is
unwitnessed — find what populates the insertion/spawn list UI and which wire fields gate
each row. v31 also re-confirmed: vehicle attach dead (2 C2S 0x26 on the wire, no
dispatch case exists — silently dropped), HUD player count still wrong (only 6 S2C 0x16
all session; the D-NET-155 generation re-push needs a third look, and the client HUD
count SOURCE itself is unwitnessed), and the body-motor anim gap (off-14/off-15 echo)
re-reported. Artifacts: `.scratch/retail_join_v31_game.pcapng` (udp.port==32768 filter of
the 664 MB dual-interface raw).

**IDB changes (2026-07-03 AS session).** Function renames (46): the `CWeaponSlotManager*`/
`CWeaponSlotMask*` cluster → `ZoneSlotChain_{GetTeamMask@0x4A2350, ContainsEntity@0x4A23D0,
IsZoneCapturableByTeam@0x4A2450, GetOwnedZoneMask@0x4A2620, RebuildOwnershipMasks@0x4A26C0,
GetZoneInfo@0x4A2750, AssignZoneRanks@0x4A27F0 (ex-CNetQuality_UpdatePriorities),
GetWinningTeamIfAllOwned@0x4A2920, FindFrontierZone@0x4A2AC0,
RebuildMasksAndCheckUnchanged@0x4A2B60, Reset@0x4A2C30, AddZoneEntity@0x4A2D70,
BuildFromMission@0x4A2DE0}`; `Entity_BuildSortedRenderList @ 0x43EAE0` →
`Entity_BuildSpawnZoneList` (+ `SpawnZoneList_{GetCount@0x43B920, GetByIndex@0x43B930,
IndexOf@0x43B990}` over `g_spawn_zone_list/count`); `CMap_SetupSpawnCamera @ 0x50CF60` →
`Server_PositionPlayerForSpawn`; `sub_4FE110` → `Server_ResolveSpawnTargetHandle`;
`sub_500BA0` → `Server_OnPlayerTouchCaptureZone`; `calculate_capture_zone_score_delta
@ 0x501120` → `calculate_capture_zone_control_delta`; `Server_ChangePlayerTeam @ 0x518D70`
→ `Server_ChangeEntityTeam` (it retargets ANY entity — zones included); `Server_EnforceZoneEntityTeams` →
`Server_EnforceZoneEntityTeams`; the `stru_24E0E48` helpers → `SpawnWaveList_{TickEntry
@0x52A330 (ex-update_death_queue_node), RemovePlayer@0x52A410, TryQueuePlayer@0x52A490,
HasEntryForZone@0x52A520, Tick@0x52A550, ResetOnZoneTeamChange@0x52A5B0,
GetEntryInfo@0x52A700, CountEntriesForTeam@0x52A7E0, BuildFromMission@0x52A920
(ex-collect_weapon_overlay_entities), AppendEntry@0x52AB60}`; the capture-ctx helpers →
`CaptureCtx_{RemoveQueueEntries@0x53B340, AddActiveCapture@0x53B510,
MarkPresenceSlot@0x53B5C0, UpdateActiveCaptureRate@0x53B600, RemoveActiveCapture@0x53B6D0,
QueueCaptureRequest@0x53B7A0 (ex-CTerrainTile_AddOrUpdateBoneSlot),
MarkZonePresence@0x53B880, Reset@0x53BD00 (ex-CTextureInfo_Reset)}`; payload writers →
`NetPacket_{WriteZoneTimerWindow@0x506D00 (0x53), WriteZonePresenceCount@0x506DE0 (0x6C),
WriteZoneTimerValue@0x506E70 (0x6F), WriteSpawnWaveStatus@0x507490 (0x6E)}`;
`sub_50FF10` → `Server_SendSpawnWaveStatusToPlayer`. Data renames (8): `g_zone_slot_chain
@ 0x24D1EBC`, `g_spawn_wave_list @ 0x24E0E48`, `g_spawn_zone_list/count @ 0xA89188/84`,
`g_spawn_wave_time_base/zone @ 0x24D224C/50`, `g_capture_speed_setting @ 0x24D2254`,
`g_respawn_requires_team_dead @ 0x24D2260`. Entry comments on the 15 core functions.

### 5.62 The FP weapon action FSM — weapon.def ACTION rows → the 12-state pump (2026-07-09)

How the equipped weapon animates and sequences: the weapon.def ACTION rows bind into a
per-weapon **12-slot action table** and one per-tick pump advances an action QUEUE on the
equipped slot. Witnessed end to end this session (all anchored, Jointops.exe.kong.i64);
ported as `libs/world/weapon_fsm.{h,cpp}` + the `NovaSimulation` slot pump + the
GameWorld/LocalPlayerPresenter host wiring (PR #213 train). This is the runtime half the §5.16
fire pipeline and §5.58 reload round-trip plug into.

**The ACTION-row registry** [orig: `ActionDef_ParseScriptLine @ 0x4023c0`]. `action
"<name>"` find-or-creates a global ActionDef pool entry named `<prefix>_<name>` (the
prefix argument is the weapon's name; entry name at +122, `ActionDef_InitDefaults
@ 0x4022b0` memsets the record — so **absent keys default to 0**, only an explicit
`auto` writes the bake sentinel −1). The weapon.def driver
(`WeaponDefs_ParseLineCallback @ 0x543680`) latches `g_weaponParseInActionBlock
@ 0x252DB88` on `action` (after validating the name against the 12-suffix table;
the created row — `ActionDef_GetCurrent @ 0x401ef0` — is stored per-suffix at
`WeaponDef+0x2A4`) and forwards EVERY in-block line here — **before** its own
`action` dispatch (`@ 0x54388d`). A nested `action` while one is open is therefore
REFUSED: `@ 0x402409` logs "forgot an end", returns 1 WITHOUT creating the row, the
old row stays current (subsequent keys overwrite it, last writer wins), and the
driver ignores the rc — there is NO implicit closure; never-created suffixes become
zeroed generated defaults at bind time. (Corpus note, 2026-07-10: every shipped
weapon.def — JOX, JOTAC localres, RevX02, JO:CA, jox01, demo — is fully
END-terminated; only malformed data reaches the refusal.) Keys: `function` → +0 handler via
`ActionFuncDef_FindByName @ 0x401040` (unknown name → the `ActionSlot_ExecuteAction
@ 0x4020a0` placeholder + warn), `anim` → +58 (a literal `.adm` clip key, e.g.
`anim_wpn_fire`), `delaystart` → +36 / `delay`/`delayend` → +40 (ticks; `auto` → −1),
`soundset` → +8 / `soundsetend` → +12, `particle` → +16, `particleuserpoint` → +186,
`dupsound` → +44/+48, `action_value` → +52, `ctrlreg` → +28 / `ctrlreginc` → +32,
`texttoken` → +20. The function registry `g_actionFuncDefTable @ 0x829E58` (count
@ 0x829F30 = 18, 12-byte rows `{name, fn, min_params}`): `null`, `wpn_std_null`,
`wpn_std_{idle,emptyidle,fire,recoil,reload,empty,switchto,switchfrom,switchrank,
scopeup,scopeup_map,scopedown,scopedown_map,switchfrom_map}`, `powerup_pickup`,
`powerup_respawn`. Data sweep (JOX + REVX weapon.def corpora): only the NINE bare
suffixes ship as ACTION names (never scopeup/scopedown/overheated), and FUNCTION only
ever names `wpn_std_<own suffix>` — the `*_map` variants are unused by weapons.

**The bind + bake** [orig: `Anim_InitActions @ 0x541fa0`]. After a weapon block parses,
each of the 12 slots at `WeaponDef+0x2A4` binds by looking up `<weaponName>_<suffix>`
against the suffix table `@ 0x830B90` — 12 `{suffix, defaultHandler}` pairs in id order:
idle `@ 0x542920`, emptyidle `@ 0x542A20`, fire `@ 0x542B10`, recoil `@ 0x542DD0`,
reload `@ 0x5430B0`, empty `@ 0x543180`, switchto `@ 0x5431D0`, switchfrom `@ 0x5433B0`,
switchrank `@ 0x543500`, scopeup `@ 0x543290`, scopedown `@ 0x543320`, overheated →
the idle handler. Missing rows become generated defaults; a null/placeholder handler
takes the table default. The ANIM name resolves to an AnimMap slot (+24 via
`AnimMap_FindSlotByName @ 0x40cfa0` — **stricmp, case-insensitive**, comparing from
name+5 so the `anim_` prefix is skipped against the unprefixed 252-entry
`g_animStateNameTable @ 0x8135F0`; JOTAC-era defs author `ANIM_WPN_*` uppercase
while the .adm stores lowercase, D-WPN-10) and the −1 delays bake from the clip:
each `−1` field is its OWN `Anim_GetDurationTicks(adm, slot)` call — delaystart
`@ 0x5421c5`, delayend `@ 0x5421d8` — and every call is a CONSUMING ring read (below):
it serves the slot's current entry then advances it (`@ 0x53ee26`), so a both-auto
action consumes TWO ring entries and the two reads can serve different clips
(CORRECTED 2026-07-11: the earlier "both auto → ds = de = ticks" reading is the
single-clip special case, where every entry has one duration). The conversion is
**trunc(ms × 62.5/1000 + 0.5) + 1** — the 62.5 t/s constant `flt_7C3B3C` and the
round-to-nearest 0.5 `flt_7C3B94 @ 0x7C3B94` (byte-witnessed 2026-07-10, the
pre-#219 port truncated without the +0.5); `delayend = ticks`, minus `delaystart` when
`ticks > delaystart` (the just-baked delaystart); unresolved anim (`@ 0x542202`) / no
adm (`@ 0x542180`) / no anim key (`@ 0x542152`) → −1 collapses to 0 (existence is the
`FindSlotByName` LOOKUP `@ 0x5421ae`, never a read). Ends by playing global slot 241
(`wpn_idle`) on the weapon's adm.

**Multi-clip variant rings** (2026-07-11). A .adm row may list several quoted clips —
`anim_wpn_reload	"m4_1r" "m4_1r" "m4_1r2"` — and `AnimMap_ParseConfigLine @ 0x40cb60`
registers EVERY token on the same anim slot: `AnimMap_RegisterBoneNode @ 0x40c2d0`
links each into a per-slot CIRCULAR list (node+36 = next), so the slot is a variant
ring in authored order, and the duplication is the rotation weighting (r plays twice
per r2 cycle). Both consumers serve-then-advance the ring head (the per-entity
animState's slot array +72): `Anim_GetDurationTicks @ 0x53ee10` (the bake reads
above) and `AnimMap_PlayAnimBySlot @ 0x40bda0`, which also LATCHES the served entry
into the animState (+68 entry / +64 data / +60 slot) — playback samples the latch
while the head moves on. Corpus: 792 multi-clip rows across the REVX02 .adm set
(max 6 variants on one row), 72 in JOX. Worked REVVY M4 example: RELOAD authors
`delaystart 200 / delayend auto` → ONE bake read (serves entry 0, head → 1), so the
first reload PLAY serves entry 1 and the next served play is entry 2 — live-verified
in the weapon_round probe (a refused reload request advances nothing). Port mapping:
`libs/anim` adm keeps every token (`AdmEntry.values[]`, `value` = first);
`NovaSkeletalAnim` registers one clip per token under the same key (peek-only —
`get_clip_variant_count/lengths`, variant-arg getters/eval); the ring CURSORS live on
`NovaSimulation` (the animState+72 analog — `weapon_fsm_bake`’s per-auto-field reads
and the FSM play events consume them, and the play latch rides the weapon view as
`anim_variant`, the +68 analog, so both viewmodel parts follow one serve). Riders:
the sim re-seeds the rings per equip, riding the existing per-equip re-bake shape
(retail bakes a def once globally, so its rings persist across re-equips — D-WPN-6
family); the 3P body weapon channel and AI body clips still play variant 0 (the
per-entity body-adm rings are an open tail of §14.8).

**The slot + the pump** [orig: `WeaponAction_ProcessFrame @ 0x540e60`, driven per
pooled entity by `WeaponAction_ProcessAllEntities @ 0x542690`]. MountSlot (100 B):
counter +0, clip u16 +0x10, rate/muzzle-flash tick +0x14, Def +0x20, owner +0x24,
currentAction +0x2C, nextAction +0x30, prevAction +0x34, switchTimer +0x58 (i16),
phase +0x5A, kickIntensity +0x5B, charge +0x5C, flags +0x5E (bit0 = the §5.16 net-fire
pose latch, bit1 = FP), burstCounter +0x62. Phase protocol: a transition writes
phase=1 + counter=newDesc.delayStart; the handler's first tick flips 1→2, **starts
the action's clip on the equipped WeaponDef's own adm channel
(`WeaponDef+0x174`, the FP viewmodel rig) — local player only** (CORRECTED 2026-07-09:
the earlier "owner's animadm" reading; the 3P body's weapon layer is a separate
producer, world-wac-ai-re.md §14.8)
[orig: `ActionSlot_BeginActivePhase @ 0x53f830`; the effect shims
`@ 0x541860`/`@ 0x5419e0` write the same protocol and same play target — they differ
only in muzzle/particle spawning, forked by `ActionSlot_ExecuteActionTick @ 0x541a70`
on third person / vehicle-attack / remoteness]. The held-ready phase 0x40 also
flips to 2 and replays the clip, but skips the begin sound/ctrlreg/effect leg
(`@0x53f88b`; normal phase-1 begin sound `@0x53f873`). `ActionSlot_FinishActivePhase
@ 0x53f7b0(desc, slot, entity, next)` sets counter=delayEnd, nextAction=arg4, the
ACTIVE→DONE kick bump (skipped for RELOAD), phase=4. Pump tail per tick: kick decay;
overheat deny (heat > 0xFFFF converts a queued FIRE to EMPTY `@ 0x541046`); the IDLE
reseed (`counter==0 && current==next==0 → counter = idle.ds+de` `@ 0x54135d`);
`counter>0 → --counter, run handler`; at 0: the **rescope-after-reload** block
(`current==4 && next==0 && local && g_rescopeAfterReload @ 0xB7647C →
Player_ToggleWeaponScope` `@ 0x54139e`); then `current != next && phase ∈ {4,0}` →
transition (prev=current unless current ∈ {6,7}, current=next, next=0(idle),
counter=ds, phase=1, `word_B7C670=−1`, run the new handler; the phase==0x40 variant
re-marks 0x40 after).

**The handlers** (decisions, all witnessed): **idle** — LOOP; enter replays global 241
+ phase=4; empty mag → reserve>0 && `g_autoReloadEnabled @ 0x24D2118` →
`WeaponSlot_RequestReload`, else next=EMPTYIDLE + one-shot unscope (local, clip
capacity 1, `!(Flags & 0x20000000)`). **emptyidle** — LOOP on global 242; reserve>0 →
RequestReload (no auto-reload gate); else hold. **fire** — phase-1 recheck
`WeaponSlot_CanFire @ 0x541ba0`: busy weapon-child, underwater ban, and the clip leg
which on empty **writes nextAction itself** — 3 (RECOIL → the auto-reload arbiter)
when the class reserve has rounds else 1 (EMPTYIDLE) `@ 0x541c8b` — the abort adopts
it (`Finish(next=[esi+0x30])` read AFTER the call `@ 0x542b50`); the shot: the 3P body
weapon-channel stamps 62 `knife_attack` / 63 `grenade_attack` into `entity+0x2C8`
(keyed on the AdmDef kind dword `@ 0x24E8088`; rifles stamp nothing —
world-wac-ai-re.md §14.8.4 `@ 0x542bcb`), `Entity_CalcWeaponFirePosition @ 0x4dc750`,
`Entity_FireWeaponAndSendPacket @ 0x42bd80` (§5.16), `consume_weapon_ammo @ 0x540850`,
3-round burst (Flags&0x20) cycling 0→2→1→0, **next=RECOIL unconditional `@ 0x542c9e`**,
kick += recoil.ds+de+counter+10 cap 20. **recoil** — THE ARBITER: at clip end
(counter==0) phase=4, heat stamp (def+876/880 → +0x14, clamp 73728); local decision:
rounds → next = burst ? FIRE : IDLE; else reserve ≥ clipSize×unitsPerRound &&
auto-reload → RELOAD `@ 0x54301d`; else EMPTYIDLE + one-shot unscope + the def+0x168
auto-switch (`Player_SwitchToWeaponByHandle(65×def+0x164)` `@ 0x54307c`); the
held-trigger refire is a DEFERRED RE-QUEUE, not a per-tick request: the closing
recoil ticks (`counter <= 1` — the phase-4 delayend ticks, or ANY tick of a
zero-length recoil, entry included) re-queue binding 149
(`Input_QueueDeferredEvent(149, current_tick) @ 0x542e9d`), gated `owner==local &&
rounds && Flags&0x100 && (char)burst<=0 && Input_IsBindingActive(149)` `@ 0x542e7f`
— the dispatch lands in RequestFire before the next pump (RECOIL → next=FIRE). The
ROUNDS gate kills the chain at an empty magazine, so the arbiter's queued RELOAD
stands and the volley never resumes after an auto-reload without a fresh press
edge. **reload** — first tick
(phase bit0, no 0x80) sends C2S 0x25 (§5.58), phase|=0x80 (transient — the first shim
tick overwrites 2), stashes the scope (`g_rescopeAfterReload = g_weaponScopeActive`
unless Flags&0x40000) + unscopes; clip end → Finish(next=IDLE `push 0 @ 0x543169`),
burst=0. **empty** — dry-click one-shot → EMPTYIDLE. **switchto/switchfrom** — the
±30/tick switchTimer machines (−900 seed ≈ 0.48 s); switchfrom swaps
`EquippedSlot = g_pendingWeaponSlot @ 0xB75FD0` and queues SWITCHTO on the new slot
(`WeaponSlot_TryQueueSwitchTo @ 0x53f140`); instant on Flags&0x80. **switchrank** —
in-place swap (fire-mode/rank). **scopeup/scopedown** — timed one-shots (zero-length
on every shipped def; the ADS easing is the camera interp).

**The requests** (the input dispatch, `Input_HandleActionBinding_0 @ 0x4e0420`):
fire = `WeaponSlot_RequestFire @ 0x53efa0` (ex `sub_53EFA0`; via
`Player_RequestPrimaryFire @ 0x5414c0`, ex the kong-misnamed
`Terrain_UpdateColorInterpolation`) — AUTO (Flags&0x100): current {0,3,9,10}→FIRE,
{1}→EMPTY, {2}→deferred re-queue (`Input_QueueDeferredEvent @ 0x53effd` — the loop
self-sustains while FIRE is current, so a mid-FIRE press banks ONE follow-up shot
through the recoil dispatch even if released); SEMI: {0}→FIRE, {1}→EMPTY; charge weapons
(Flags&0x80000000) hold-release via `g_fireChargeStartTick @ 0xB76800` (≥31 ticks
scales the charge, binding 150). reload = `WeaponSlot_RequestReload @ 0x53f110`
(phase sign clear && queued next ∈ {0,1,11}); the key case 0xD3 pre-gates clip ≠
clipsize && reserve>0. ADS = case 6 (current ∉ {4,7}) → `Player_ToggleWeaponScope
@ 0x4df0c0`: gates def Flags&3 + `g_fpCameraInterp.activeFlag`; engage sets
`g_scopeEngaged @ 0x82CE94` (the §5.41 `g_weaponScopeActive` mirror settles later),
seat-flag C2S 0x1D/169, camera interp (15 steps; 7 for `Field0C & 0x200`) toward
`AltCamOffset` (the §5.40 tpos), `WeaponSlot_TryQueueScopeUp @ 0x53f050` (ex
"TryQueueReload" — queues 9, phase-gated {0,4}); disengage mirrors down
(`..ScopeDown @ 0x53f080`, ex "TryQueueUnload" — queues 10), FOV back to 80.0; the
zoom FOV (Flags&2): `g_cameraFovDeg @ 0x26C6848 = 80.0 / Player_GetClampedWeaponElevation`
(16.16) — the weapon.def `scope_max_mag` magnification.

**Port** (PR #213 train). `libs/world/weapon_fsm.{h,cpp}`: the bake
(`weapon_fsm_bake`, def-agnostic rows per ADR 0020 + a clip-seconds callback), the
pump + all 12 handlers as structural translations (ctest `weapon_fsm`: bake pins,
fire→recoil chain, auto cadence, semi edge, burst-3, empty paths, auto-reload +
§5.58 refill math, scope stash/rescope, request gates, non-local no-decision).
`libs/def` parses `scope_max_mag` (+ the `whileswimming` flag-table length fixed:
13, not 14 — the token never matched); `NovaWeaponDatabase` surfaces
flags/scope_max_mag/actions; `NovaSimulation` pumps the LOCAL player's slot once per
logic tick after the world advances (all four paths), keeps latest-value snapshot
serials for diagnostics/rebuild, and appends each tick's clip/begin/end payload to an
ordered destructive event batch with `age_ticks`; `GameWorld` bakes from the resolved
weapon dict + the loaded viewmodel's clip lengths and types the drained records;
`LocalPlayerPresenter` feeds LMB/R/RMB through the world-tick input path, drains every event
in order, plays FSM clips on BOTH viewmodel parts at their catch-up age, and realizes
ADS: the eased pos→tpos view bias
(15-tick fraction), the main-camera FOV 80h → 80/mag h→v through the live aspect,
reload/one-shot forced unscope + the pump's rescope. Live-verified (fp_clean_probe
`NOVA_VM_FSM=1`, JOX 05TR): 6-shot auto burst (clip 30→24, ~9-tick cadence from the
recoil clip), reload refill 24→30 with reserve 300→294 (the §5.58 refund math),
mid-reload RMB refused, ADS engage fraction→1 with cam fov 80h→40h, disengage clean.

**Divergences** (ledger D-WPN-1..15 plus the D-WPN-26 runtime-builder addendum): the FUNCTION registry unported (std-only in all
shipped data, D-WPN-1); single-pool ammo vs per-class pools (D-WPN-2); CanFire's
busy-child/underwater/score-lock legs + kick sound gate (D-WPN-3); the heat model
(`WeaponSlot_CalcAccumulatedHeat @ 0x53f780` internals unwitnessed, D-WPN-4); the
weapon-switch machinery seams (D-WPN-5); local-player + occupied mounted-parent pump,
with general non-local/unmounted coverage still open (D-WPN-6); production runtime
action-table bake lacks the ADM-duration source for authored `auto` delays (D-WPN-26); interim
ammo seed clipsize/startrounds (D-WPN-7); FSM↔net integration now carries joiner C2S 0x06
fire, the payload-addressed C2S 0x25 → S2C 0x49 reload round-trip, and decoded S2C tag-2
events into a visual-only client `RoundSim`; authority/SP fire continues to append the primary
ring row (including ordinary hip/raise/3P subtype 12) and spawn `RoundSim` synchronously.
D-WPN-8 remains open for settled-FP/mounted zoom subtypes, remote/vehicle 0x49 presentation,
remote shooter-team/per-weapon tracer metadata, posed-bone person-proxy collision (players AND
decoded infantry share the torso stand-in), clean-disconnect proxy retirement, and the joiner C2S 0x0C uplink's one-frame LOOK-ANGLE
lag (grilled 2026-07-23: retail's net frame packs BEFORE the motor — the
`Game_ProcessMainFrame @ 0x5263f0` chain — so the uplink POSITION being the previous tick's
integration is FAITHFUL; retail samples the look axes in `Input_ProcessFrame` before packing
while ours applies input after the net frame — one 16-ms tick of look lag, presentation-only).
The 0x06 `target_handle` NEEDS-RE is CLOSED (2026-07-23): the producer stamps
`aiRuntime[3]` — the AI current-target pointer — pool-packed by the writer with null → `0xFFFF`
[orig: WeaponAction_Fire @ 0x542c15; NetPacket_WriteEntityPositionUpdate @ 0x42a70d], so a
human shooter always sends `0xFFFF` and the joiner's `0xFFFF` is the witnessed value (§5.16); moving
decoded non-player Infantry/vehicle collision projection LANDED 2026-07-23 (wire-keyed
person + dynamic proxies at the decoded pose; visual-client local pool-0/1 ghost slots
excluded from the projectile walks — residuals: PANM/turret section posing and husk-model
substitution for wire dynamic proxies, and movement-contact projection, still read the
load-frozen local set); the
standard Scoped/Sighted SIGHTS-card selector, including SWITCHFROM and
NoCardSwitch/ForceScoped suppression, is ported; remaining ADS residuals are the
zoom-level keys, scope net notify, stance/NVG gates, movement reversal/auto-raise,
and HandGunUp follow leg (D-WPN-9);
**D-WPN-10** [reimpl divergence, FIXED 2026-07-10] the host clip-key lookup
(`NovaSkeletalAnim::find_clip`) compared case-SENSITIVELY where the original is
stricmp (`AnimMap_FindSlotByName @ 0x40cfa0`) — on JOTAC-era data (base localres +
RevX02 author `ANIM_WPN_*` uppercase; the .adm stores `anim_wpn_*` lowercase) every
`auto` delaystart/delayend collapsed to 0 and `has_anim` died, so the viewmodel
played no weapon-action clips (JOX's lowercase rows masked it). Fixed to
`nocasecmp_to`. **D-WPN-11** [reimpl divergence, FIXED 2026-07-11] `begin_active`
treated phase 0x40 as a normal phase-1 entry and emitted `action_started`, replaying
begin sound/effects; split to match `ActionSlot_BeginActivePhase`'s anim-only held
branch `@0x53f88b`. **D-WPN-12** [integration divergence, FIXED 2026-07-11] action
presentation crossed the sim/host boundary as a latest-value snapshot plus serials;
several fixed catch-up ticks before one present pass overwrote distinct payloads and
the host consumed a serial jump as one edge. Replaced by the ordered destructive event
batch above, including production-tick settled/third-person/vehicle routing state so a
catch-up cannot apply its final view state to earlier events; snapshot serials remain
diagnostic/rebuild state. **D-WPN-13** [reimpl
divergence, FIXED 2026-07-12] the port ran held auto fire as a per-tick
`request_fire` re-request where the original sustains the volley through the recoil
window's rounds-gated deferred re-queue (`@ 0x542e7f..0x542e9d`) — at an empty
magazine the ungated re-request overwrote the recoil arbiter's queued RELOAD every
tick, so a held trigger never auto-reloaded and the FSM thrashed FIRE↔RECOIL at
31 Hz replaying the recoil row's soundset/particle (REVVY M4: SHELLDROP + the muzzle
flash strobing on a dry gun). Fixed by porting the witnessed chain: a
`refire_queued` slot latch (the deferred-event queue) written by the recoil window
and by RequestFire's mid-FIRE re-queue (`@ 0x53effd`), consumed as the fire request
one tick later.

Recoil direct-effect events and impact rows remain fully transported. Every authored direct
payload is now submitted as an independent generic `Always` transient through its action
userpoint—REVX02 `WPN_M4AUTO` works because its actual muzzle is authored there, not because
the host recognizes `mflash*`. Casing and physical-collision impact rows use the same shared
EffectScene/atlas/ordered packet path and impact audio remains live. D-PTL-16 is closed; no FSM
timing workaround remains.

**The delaystart/delayend grill (2026-07-10, PR #219 validation).** Re-witnessed the
delay pipeline end to end: parse (`delaystart` → +36, `delay`/`delayend` → +40 — the
bare `delay` alias now ported; `auto` → −1, absent → 0), the bake (formula corrected
above), and every pump consumption site (transition `counter=ds @ 0x5413ea` +
`@ 0x54146d`; finish `counter=de @ 0x53f7b0`; idle reseed `ds+de @ 0x54135d`;
idle-with-queued-next counter cut to 0 `@ 0x541370`; kick `recoil.ds+de+counter+10`)
— all MATCHING. #219's interim "implicit ACTION closure" parser change was REVERTED
to the witnessed nested-`action` refusal (see the registry paragraph): its
justifying corpus claim (JOTAC AK47AUTO missing `end`s) is false — all six corpus
copies are fully terminated, and retail parses a genuinely-mixed file by swallowing,
not closing. Retail tolerates the resulting zero-delay generated defaults: at
counter==0 with phase 1/2 the pump still runs the handler (LABEL_118 `@ 0x5414a2`),
so a {ds 0, de 0} action finishes on its entry tick — shipped data leans on this
(every JO fire row is `delaystart 0` + explicit `delayend`; the AK's cadence is the
fire delayend + recoil {0,0}).

**The held-refire grill (2026-07-12, the PR #226 REVVY M4 fix round).** The REVVY
`WPN_M4AUTO` (fire {0,3} → recoil {0,0}, the standard retail shape) wedged at
clip-empty under a held trigger — root-caused to D-WPN-13 above and fixed by porting
the deferred-refire chain verbatim. The volley cadence is unchanged and now derived,
not invented: fire delayend N → an (N+2)-tick cycle (shot tick + N counter ticks +
the zero-length recoil pass whose re-queue dispatches the following tick) — REVVY M4
{0,3} = 5 ticks = 750 rpm, JOX AK {0,6} = 8 ticks ≈ 469 rpm. Behavioral corollaries
now pinned in ctest `weapon_fsm` (empty-mag held auto-reload with no FIRE↔RECOIL
thrash + no post-reload resume; the mid-FIRE banked tap; held-empty dry-click
silence — clicks are per press EDGE, the input dispatch is edge-based): a held
trigger alone never re-requests fire; every sustained volley is the recoil window's
re-queue loop.

**The frozen-viewmodel grill (2026-07-12, the PR #226 fix round 5).** With the refire
chain fixed the REVVY M4 still presented wrong live: ammo drained at the correct
5-tick cadence but the gun kicked once and froze until release. The sim, the event
batch, and the host drain all verified correct (headless FSM probe on the real dict;
per-tick drain dump; the weapon_round_probe NOVA_WR_ANIMTRACE playhead trace) — the
root cause was OUTSIDE the FSM: the `.bad` pose bake dropped every clip's final
channel key (the header `frame_count` counts INTERVALS; channels carry
`frame_count + 1` keys), and `m4_1f` — the M4 fire clip — is a ONE-frame clip whose
entire kick motion is key 0 -> key 1, so it collapsed to a static kicked pose
(D-ANIM-1, FIXED; ADR 0007 §3). Witnessed along the way and RECORDED AS RESIDUAL:
the FP weapon adm channel is CLOCKED BY THE FSM, not wall time — every play re-inits
the channel (`AnimChannel_InitFromData(.., rate 4096, phase 0.0)` inside
`AnimMap_PlayAnimBySlot @ 0x40bdd1`), and the begin shim's phase-2/DONE leg advances
it ONE dispatch per pump tick only while the action's COUNTER is nonzero and the
action row resolved an anim (`ActionSlot_BeginActivePhase @ 0x53f8d2..0x53f8de` —
the counter gate; anim-less rows like the shipped RECOILs freeze the channel). The
port free-runs clips at wall clock: at 62.5 Hz the witnessed advance is ~one
half-frame per tick (2 x 30 fps ~= 62.5), so the rates agree; the freeze during
anim-less/expired actions is the recorded divergence tail (rides D-WPN-6's
presentation family).

**IDB (2026-07-10 session).** Renamed: `ActionDef_GetCurrent @ 0x401ef0` (ex
`ActionDef_GetCurrent` — returns the open ActionDef), `g_currentActionDef @ 0xA2E8E8`,
`g_weaponParseInActionBlock @ 0x252DB88`, `g_weaponParseCurActionDef @ 0x252DB8C`.
Comments: the `@ 0x402409` refusal, the `@ 0x54388d` forward-before-dispatch order,
`@ 0x40cfa0` stricmp + name+5 prefix skip, `@ 0x401ef0`. idb_save run.

**IDB (this session).** Renamed: `WeaponSlot_RequestFire @ 0x53efa0`,
`Player_RequestPrimaryFire @ 0x5414c0`, `WeaponSlot_TryQueueScopeUp @ 0x53f050`,
`WeaponSlot_TryQueueScopeDown @ 0x53f080`, `g_pendingWeaponSlot @ 0xB75FD0`,
`g_rescopeAfterReload @ 0xB7647C`, `g_fireChargeStartTick @ 0xB76800`,
`g_autoReloadEnabled @ 0x24D2118`, `g_actionFuncDefTable @ 0x829E58` (+count).
Comments at the pump, bake, and request sites; idb_save run.

**The weapon-switch chain (same session, closes the SWITCHFROM/pending follow-up).**
`Player_SwitchToWeaponByHandle @ 0x4e0170` (handle = category×65 + rank): stance gate
(parentSlot ∉ {2,3,5}), clears `g_fireChargeStartTick`, then scans the category's 65
slots in the 100-B `weaponSlotArrayBase @ 0xB75FD4` pool from the def's own rank —
eligibility = def+932 type 1/2 or `calculate_kill_score @ 0x5407e0` (the §5.41
eligibility reuse) and `!(def+12 & 1)`; a full wrap plays the deny sound. The pick
lands in `Player_MountWeaponSlot @ 0x4dfa40`: **writes `g_pendingWeaponSlot = slot`
`@ 0x4dfb16`**, then queues the FSM — same category → `WeaponSlot_TryQueueSwitchRank
@ 0x53f1c0` (ex `sub_53F1C0`: phase {0,4,0x40} → counter=0, next=8), cross category →
`WeaponSlot_ForceQueueSwitchFrom @ 0x53f170` (ex `sub_53F170`: phase {0,4,0x40} →
HARD RESET counter=0/burst=0/switchTimer=0/current=0/prev=0, next=7; SwitchFrom's
timer expiry then swaps `EquippedSlot` from the pending global and queues SWITCHTO on
the NEW slot). Mount-scoped weapons (`Flags & 0x20000000`) auto-engage the scope
(`g_weaponScopeActive = 1` + `dword_B76808` zoom stash); a cross-category switch
resets the scope + FOV 80. View biases zeroed, `Player_UpdateFirstPersonCamera` runs
immediately, seat-flag 0x40000 → C2S 0x1D/169, scoped-capable slots re-arm the camera
interp. IDB: both queue helpers renamed + commented; saved.

**The ACTION sound legs (2026-07-10, the "no gun sounds" grill).** The rows carry TWO
sound fields and the engine plays them at different phase edges:

- **Begin leg** — `soundset` → `ActionDef+8`, resolved at parse
  (`SoundBank_FindSetByNameAnyBank @ 0x5274f0`; a missing set logs + skips the write).
  Played by `ActionSlot_PlaySound @ 0x4010c0` from every begin shim on the phase 1→2
  edge (`ActionSlot_BeginActivePhase @ 0x53f873`, `..ExecuteActionWithEffect @ 0x5418b0
  / @ 0x541976`, `..ExecuteActionNoEffect @ 0x541a2a`) — 3D at the owner entity
  (`Entity_PlaySound3D_FullVolume @ 0x528e20`; local uses entity+4 position directly).
  The plain-Begin held-variant (0x40) plays anim only, no sound `@ 0x53f88b`
  (D-WPN-11, fixed in the port 2026-07-11).
- **End leg** — `soundsetend` → `ActionDef+12`. Played by the end shim `@ 0x401100`
  (ex kong "ActionSlot_RenderModelWithLOD" — a misnomer; its "LOD loop" is the
  `dupsound` repeat scheduler) from `ActionSlot_FinishActivePhase @ 0x53f7d6`, gated
  on the phase byte being 2 (ACTIVE) at entry `@ 0x53f7b9` — a phase-1 abort finishes
  silently. FinishActivePhase is reached from exactly two handlers:
  `WeaponAction_Fire @ 0x542d1a` (per shot — **the gunshot lives here**: 118/130 REVX
  and 83/89 JOX fire rows use `soundsetend`, not `soundset`) and `WeaponAction_Reload
  @ 0x54316e` (completion). `dupsound N M` (`+44` count / `+48` interval; `N==1`
  normalized to 0 `@ 0x40260f`) schedules N−1 delayed repeats — zero uses in the
  JOX/REVX corpora (data-dead).
- **Weapon-level sounds** — four 24-B name strings at WeaponDef `+0x2F8/+0x310/+0x328/
  +0x340` from keys `soundfireloop`/`soundtrailoff`/`soundhead`/`soundlockedtone`
  (`WeaponDefs_ParseLineCallback @ 0x5444c8..0x544578`), resolved post-parse into ids
  `+0x294/+0x298/+0x29C/+0x2A0` (`WeaponDef_ResolveAllReferences @ 0x54042c..0x540482`).
  The fire handler plays `+0x294` only when `kickIntensity == 0` (volley start)
  `@ 0x542ccc..0x542ce9`. **Zero uses in JOX+REVX weapon.def** — the leg is data-dead
  for our SKUs (witnessed, unported; the `sub_527AD0(+8)` read after the fire begin is
  a discarded pure read `set+72 << 16` — no audible effect).
- **Effect (particle) routing** — the local player's begins route through
  `ActionSlot_ExecuteActionTick @ 0x541a70`: only FIRE (`slot+44 == 2`) can take the
  with-effect shim, and only in third person (`g_camera_mode`), from a vehicle-attack
  seat, or un-scoped FP (`g_FpWeaponViewFlags & 1 && !g_weaponScopeActive @ 0x541aba` —
  the ex-`dword_24D20C0` weapon-view flag, renamed 2026-07-13); every other local begin
  is the no-effect shim `@ 0x541b17`. **Correction (2026-07-13 grill)**: the earlier
  reading "casing ejects never spawn in your own FP view" was WRONG — the tick gate only
  covers the BEGIN leg. `WeaponAction_Recoil @ 0x542dd0` spawns the recoil row's
  particle DIRECTLY at the arbiter tick (counter reaches 0 after delaystart), gated for
  the local player on `ActionDef+16 && currentAction==3 && g_FpWeaponViewFlags & 1`
  (`@ 0x542efa` → `ActionSlot_SpawnEffect @ 0x542f64`) — NO scope condition, so brass
  ejects in your own FP view even while settled in ADS; the spawn passes param7=0, so
  no handle records and casings are never suppressed by a live predecessor. The
  `g_weaponScopeActive` term itself is the SETTLED state: it is promoted to 1 only when
  the ADS camera ease completes (`Player_UpdatePerFrame @ 0x4de4f7`, the sole 1-writer;
  the toggle `@ 0x4df0c0` sets `g_scopeEngaged` and leaves it 0), so muzzle flashes
  still spawn during the raise. Remote entities always take the with-effect shim
  `@ 0x541a83` (an MP seam for us). The muzzle spawn is double-spawn-guarded:
  `@ 0x5418c8` spawns only when `slot+24` is clear; `ActionSlot_SpawnEffect @ 0x401f20`
  records the GROUP handle there; descriptor dwords 12/13 install
  `ActionSlot_ClearEffectHandle @ 0x53f760` through
  `CEffectGroup_SetDeathCallback @ 0x5e1940`, and `CEffectGroup_Destroy @ 0x5e3460`
  invokes it — the suppression
  window is exactly the whole live group's lifetime. The
  spawn point is the weapon model's `launchuserpoint` (name `+0x2E8`
  → resolved 1-based index `+0x2D4` on the **gfx3** model; per-entity fallback
  `Entity_GetWeaponSlotByte(entity, clip & 3, 1) @ 0x541912`); the ACTION rows' own
  `particleuserpoint` names (ActionDef+186) resolve per weapon at mission start to a **3P
  index (ActionDef+57)** and a **1P index (ActionDef+56)**, stricmp case-insensitive
  (`WeaponDef_ResolveAllReferences @ 0x540270`, `modelgpm_FindUserpointByName
  @ 0x5b2170`). **Both model assignments corrected 2026-07-27** — this row previously put
  `+0x2D4` on gfx1 and had the two ActionDef indices the wrong way round, which would
  anchor every third-person muzzle effect against the first-person viewmodel. Re-derived
  from the register bases: `ebp` = def+0x39C in that function (its validity byte is read
  at `[ebp-0x388]` = def+0x14), so `[ebp-0x22C]` = def+0x170 = **gfx3** and `[ebp-0x230]`
  = def+0x16C = **gfx1**. The gfx3 pointer is the one live across the `+0x2D4` store
  `@ 0x5402c2`/`@ 0x5402ca` and across the loop writing ActionDef+57 `@ 0x54039e`; the
  gfx1 pointer is loaded `@ 0x5403ab` for the loop writing ActionDef+56 `@ 0x54040f`.
  Reload begins pass
  effectScale 0.0 (`@ 0x543150`) — reload rows' particles never spawn anywhere.
- **Cadence refutation** — the June "+20 fire-expiry" note is refuted at both request
  sites: `WeaponSlot_RequestFire @ 0x53efa0` and `WeaponSlot_CanFire @ 0x541ba0` carry
  no tick gate; the rate of fire IS the fire row's `delayend` (AK47AUTO `delayend 6` ≈
  625 rpm at 62.5 Hz) + the recoil delays. `WeaponSlot_CanFire`'s other legs annotated:
  busy weapon child (`Entity_FindChildByDefType(e,1,1)`), underwater refusal (def
  `Underwater 0x4` / entity swim `0x8000` vs `Env_WaterHeightFixed`), and the empty leg
  writing `slot+48` = 3 (reserve) / 1 — the port's `can_fire_ammo` shape.

Port row: each tick's `WeaponFsmEvents` clip/begin/end payload is copied into
`NovaSimulation`'s ordered event batch → `GameWorld.drain_local_player_weapon_events`
types and destructively drains it → `LocalPlayerPresenter` consumes every record in order.
Each record carries `age_ticks`, the production-tick position, and the settled,
third-person, and vehicle-attack routing flags after that tick's view promoter, so a clip
emitted early in a multi-tick catch-up starts at its correct presentation age, 3D sounds
retain their tick-local origin, and effects use the view state that existed when the action
was produced. Snapshot serials
remain available for diagnostics and viewmodel rebuild, but are not the event-delivery
mechanism (D-WPN-12). `action_finished` is still emitted only by `finish_active` on the
was-ACTIVE edge; the begin-leg SOUND follows `action_started`, except held-ready 0x40
which replays only the animation (D-WPN-11).
The muzzle particle leg is ported (2026-07-13/14): the `@ 0x541a70` routing gates with the
SETTLE-gated scope suppression (`scope_fraction >= 1.0` = the `@ 0x4de4f7` promoter),
and `spawn_effect_unless_alive` / the slot+24 guard. The recoil direct leg is ported through
the ordered event boundary (`WeaponFsmEvents.action_effect` at the arbiter tick). Data-authored
rows render generically as independent unsuppressed `Always` transients through the live action
bone, including REVX02 `WPN_M4AUTO`'s RECOIL/`MFLASH01` muzzle and FIRE/`BCASING` casing.
The shared value scene, atlas, packet compiler, and ordered RD renderer close D-PTL-16
(`weapon_fsm_test.cpp`, `nova_simulation_test.gd`,
`local_player_presenter_test.gd`).
The witness record for the spawn/attach machinery is
[particles/ptl-format-re.md §4](../particles/ptl-format-re.md). ctest
`weapon_fsm` pins both legs + the silent abort; GUT `local_player_presenter_test` pins
the sound drains.

**The FLAGS table + the ADS toggle protocol (2026-07-10, the nocardswitch grill).**
The weapon.def `flags` token table is fully witnessed: a 16-B-stride
`{name, 0, flags1 bit, flags2 bit}` table `@ 0x830bf0` — flags1: Scoped 1, Sighted 2,
Underwater 4, ShowComander 8, NoClipsNoDraw 0x10, Burst 0x20, NotDropable 0x40,
Emplaced 0x80, Auto 0x100, norangecheck 0x200, ShowRange 0x400, ShowElevation 0x800,
Armor 0x1000, OkWhileJumping 0x2000, OnlyFireScoped 0x4000, LollyPop 0x8000,
AbsorbPitch 0x10000, NoMove 0x20000, ForceCrouch 0x40000, OnlyScoped 0x80000,
2DImpact 0x100000, UseDesignator 0x200000, UseSpreadTwo 0x400000,
ShowImpactDist 0x800000, WhileSwimming 0x1000000, **NoCardSwitch 0x2000000**,
HandGunUp 0x4000000, QuickSwitch 0x8000000, OnlyFireLocked 0x10000000,
ForceScoped 0x20000000, LaserBeam 0x40000000, PowerThrow 0x80000000; flags2:
NoSelect 1, Parachute 2, Thermal 4, Monitor 8, ViewLock 0x10, onlylockscoped 0x20,
NoAmmoTypes 0x40, showhudpip 0x80, FixVerticalOfst 0x100, **Inset 0x200**,
NoAutoZero 0x400, Invisible 0x800. The libs/def 7-entry subset had WhileSwimming
aliased onto Underwater's 0x4 — replaced with the full two-dword table
(`DefWeaponDef.flags2` appended; both FFI mirrors extended).

`Player_ToggleWeaponScope @ 0x4df0c0`, re-read in full:

- Every toggle is REFUSED while the scope-camera interp runs
  (`!g_fpCameraInterp.activeFlag @ 0x4df177`), and while swimming/parachuting
  (entity Flags 0xA000), on mounts with parentSlot 2/5, un-scoping a ForceScoped
  weapon (`0x20000000 && g_weaponScopeActive @ 0x4df12d`), or NVG-blocked Inset
  weapons (flags2 0x200 + `g_NVGActive`).
- The ease is `CNetPlayerInterp_Setup` between the def POS (`AltCamOffset` + 0x10C)
  and TPOS (+0x124): **15 steps, or 7 for Inset weapons** (`Def->Field0C & 0x200`
  `@ 0x4df1d2/@ 0x4df33f`), and **1 step on the hipfire-return leg**
  (`g_scopeHipfire @ 0x82CE98`, set 1 on disengage `@ 0x4df212`, 0 on engage
  `@ 0x4df373`). The stepper (`CNetPlayer_InterpolateTransformStep @ 0x4ddd20`)
  writes the view bias globals (`g_view_pos_bias_* @ 0xB76520..`,
  `g_view_rot_bias_* @ 0xB7652C..`) the FP camera adds each frame;
  `Player_MountWeaponSlot` zeroes them `@ 0x4dfbcf`.
- Sighted-weapon FOV: engaged FP = `80 / Player_GetClampedWeaponElevation
  @ 0x4dc6b0` — the slot's ADJUSTABLE zoom (`MountSlot.Elevation`), seeded to
  `Def->MaxElevation` (scope_max_mag) on first use and clamped [0, max]; 3P or
  disengaged = 80 (`g_cameraFovDeg = 0x500000`). Port note: our 80/scope_max_mag
  equals the seeded default; the zoom-adjust input is an open tail.
- The engage leg replicates C2S 0x1D (type 169) when seat-flag 0x40000 allows,
  and seat-flag 0x10000 zeroes the pitch.

**The standard weapon card + NoCardSwitch (0x2000000), re-witnessed
2026-07-19.** `Render_ProcessMainSceneFrame` derives two selector bytes after
the ADS settle:

- The **Scoped** path begins at `0x5ca299`.
  `Player_IsEquippedWeaponScoped @ 0x4dcc80` requires `WeaponDef.Flags & 1`
  and `g_weaponScopeActive`. An Inset weapon (`WeaponDef.Flags2 & 0x200`) does
  not set the standard Scoped-card byte; the ordinary path sets it at
  `0x5ca2c7`.
- The independent **Sighted** predicate at `0x4dcd30`, called at `0x5ca2cc`,
  requires `WeaponDef.Flags & 2`, `g_weaponScopeActive`, and
  `MountSlot.currentAction != SWITCHFROM (7)`. Its standard-card byte is set
  at `0x5ca2d5`. This is why a Sighted-only weapon is not equivalent to an
  unscoped weapon for card selection.
- The predicate at `0x4dcce0`, called at `0x5ca2f6`, is
  `slot && def && (Flags & NoCardSwitch) && !(Flags & ForceScoped)`. When true
  it clears **both** selector bytes at `0x5ca2ff` and `0x5ca304`. ForceScoped
  therefore overrides NoCardSwitch for this card-selection purpose.

The post-clear bytes gate the standard-card renderer: the Sighted path calls
`draw_weapon_sight_overlays @ 0x4dce00` at `0x5caaf3/0x5caafa`; otherwise the
Scoped path calls it at `0x5cab01/0x5cab08`, followed by the circle fallback at
`0x5cab15`. With both selectors clear, the first-person viewmodel candidate is
enabled at `0x5ca32c..0x5ca343` and consumed at `0x5ca822..0x5ca829`.

The weapon-def `SIGHTS` block is **content, not the selector**.
`draw_weapon_sight_overlays` reads only the row count at `WeaponDef+0x258` and
the rows at `WeaponDef+0x1c8` (stride `0x24`); it does not inspect Scoped,
Sighted, or NoCardSwitch. It reports authored-row presence even when a texture
handle is missing, so a nonzero row count can suppress the Scoped circle
fallback while producing a blank reticle. REVX02 `WPN_M4AUTO_EOTECH` is a
concrete Sighted-only case (`Flags = 0x01000902`, no NoCardSwitch): it authors
`M4ET_SGT.TGA` plus the additive/scaled `et_rtcle.tga` row, and both assets
resolve in the retail data.

NoCardSwitch also has the previously witnessed, independent reload-camera
consumer. While the equipped slot is mid-RELOAD on a weapon **without** the
flag, `Player_IsReloadingCardSwitchWeapon @ 0x4dcdd0` (ex kong
"Player_IsDriverInVehicle"; its "seat 4" was `currentAction == RELOAD`) makes
(a) `Player_UpdateFirstPersonCamera` skip the view-bias add, rotation, and
position legs (`@ 0x4dd439/@ 0x4dd4cc`), so the sight picture drops instantly
for the reload, and (b) `Player_CanFireWeapon @ 0x5cf7be` refuse fire.
NoCardSwitch weapons (REVX: knife, pistols, shotgun, the PointAim MGs,
mortar/emplaced/turrets) keep their raised view through a reload. Another
`test edx, 2000000h` site remains in `RoundData_SpawnRound @ 0x4ec249`
(unclassified — likely a different dword; open). Related reload rule:
**ForceCrouch (0x40000) weapons skip the reload scope stash entirely**
(`@ 0x543126` -> `g_rescopeAfterReload = 0 @ 0x54313d`) — the mortars keep the
sight view; every other weapon stashes + unscopes `@ 0x54312f`. The one-shot
last-round unscope sites are gated on `!ForceScoped` (0x20000000).

Port row: libs/def carries the full flag table + `flags2`;
`world::weapon_sights_card_eligible` owns the dynamic
Scoped/Sighted/SWITCHFROM/NoCardSwitch/ForceScoped predicate; the sim exposes
that result as `scope_card_active`; and the HUD materializes all authored
SIGHTS rows, using the sim selector only for visibility. The existing
`suppress_view_bias` remains the separate NoCardSwitch reload-camera rule.
ctest `player_view`/`weapon_fsm`/`def_parse_weapons` and the focused simulation
and HUD GUT cases pin the selector and authored-row legs.

**The movement-scope legs (2026-07-11, the weapon-round grill).** The input packer
`Player_PackInputStateToEntity @ 0x4df450` latches `g_movementKeyHeld @ 0xB7653B`
on any of the four direction keys (`@ 0x4df4bb` / cleared `@ 0x4df4f9`) and drives:
(a) **unscope-on-move** — SETTLED at scope (`g_weaponScopeActive`, promoted only at
ease completion by `Player_UpdatePerFrame @ 0x4de4f7`: `scopeActive = engaged`)
on a Scoped (flags 1) weapon, movement routes through `Player_ToggleWeaponScope`
(`@ 0x4df4c9..0x4df4ec`) = the full unscope, subject to the toggle's own
**ForceScoped pin** (`(flags1 & 0x20000000) == 0 || !scopeActive` `@ 0x4df12d` —
a grill catch: the pin previously lived only on the FSM one-shot path in the
port); (b) **scope-UP refusal while moving** — `g_movementKeyHeld && (flags & 1)
-> return` `@ 0x4df29c`. PORTED: `player_view_move_input` /
`player_view_scope_up_blocked` + the `NovaSimulation` wiring; the prior
"@ 0x4df0a2" site note was the FP-viewmodel render epilogue (corrected).
WITNESSED-DEFERRED (the tri-state follow-up): the mid-UP-ease movement reversal
(`@ 0x4df548` — ease back to hip, `g_scopeEngaged` stays latched), the settled
drop-with-memory (`@ 0x4df5ae`, masked by `flags & 0x20000080`), and the
auto-re-raise on key release (`@ 0x4df607`) — all keep `engaged` latched while
away from the scope, which needs the explicit engaged/active/hipfire tri-state
our derived `scope_card_active` cannot represent. Related new witness: weapons
with **HandGunUp (0x4000000)** auto-follow player flag 0x4000 raise/lower when
the ease is idle (`@ 0x4de444..0x4de4b7`; the player-flag writer is unwalked).

**The weapon-particle timing grill (2026-07-15, the "are effects firing at the right
time" round).** Byte-verified the whole action-effect timing chain and found the port
faithful on every timing axis — and two positional/system divergences. (1) The pump's
tick contract confirmed at the instruction level: a `counter > 0` tick ONLY decrements
and runs the current handler (`@ 0x541417..0x54143d`); the transition to `nextAction`
runs on the FOLLOWING counter==0 tick (`@ 0x5413b1..0x541412`) — the port's pump tail
is the same shape, so cadence and effect phase match (the REVX02 `WPN_M4AUTO` corpus
note: its FIRE row authors the CASING at `BCASING` and its RECOIL row the muzzle flash
`EFFECT_M16MF @ MFLASH01` with fire `delayend 3` — the flash landing ~4 ticks after
each bang on that mod's data is AUTHORED, not a port bug; base JO authors flash-on-fire
/ casing-on-recoil). (2) The `ExecuteActionTick` fork `@ 0x541a70` byte-confirmed: local
non-FIRE begins take the no-effect shim (`@ 0x541b17`); local FIRE takes with-effect only
when mounted-parent-3, `g_FpWeaponViewFlags&1 && !g_weaponScopeActive`, camera mode, or
vehicle-attack (`@ 0x541aba..0x541ac1`); remote entities always with-effect (`@ 0x541a83`).
(3) **THE LIVE ACTION-EFFECT TRACKER** (new witness): `MountSlot+0x18` (ex-"sndHandle"
misnomer → `actionEffectHandle`) — the handle `ActionSlot_SpawnEffect` records with its
anchor action index (`+0x28` → `effectAnchorActionIdx`) — is RE-ANCHORED EVERY PUMP TICK
to that action's bone (`actionTable[+0x28]` → `Entity_ComputeActionTransform` →
`CEffectEmitter_UpdatePositionAndParams @ 0x5f6810`, the ex-"Audio_SetListenerAndEffect-
Positions" kong misnomer: a pure live-emitter position/orientation/attenuation/blend
update gated on `g_ParticlesDisabled`), and released underwater (`pos.Z <= water` without
`Flags&4`, `@ 0x540efd`) — retail muzzle flashes are BONE-TRACKED for their whole life
[orig: the tracker leg @ 0x540edf..0x540fe6]. **D-WPN-17** [reimpl divergence, FIXED
2026-07-15]: the port spawned the local flash `BINDING_WORLD` at the spawn-time
userpoint — while strafing/turning the flash trailed the muzzle. Fixed: the host spawns
owner-bound (`BINDING_FOLLOW_OWNER`) and registers a live anchor resolver
(`GameWorld.register_effect_anchor` → the spawning action's userpoint through the
current viewmodel pose) polled by the effect world's owner-pose sync; anchors drop on
viewmodel-generation turnover. (4) **THE HEAT WINDOW + OVERHEAT GLOW LEG** (new witness):
`MountSlot+0x14` (ex-"muzzleFlashEndTick" → `heatWindowEndTick`, stamped by the recoil
heat writers `@ 0x542fa0`/`@ 0x542fdc`) opens a per-tick window in the pump
(`@ 0x540fed..0x54125f`): `WeaponSlot_CalcAccumulatedHeat` each tick; heat > 0xFFFF
converts a queued FIRE to EMPTY (`@ 0x541046`, the known overheat deny); heat >
`Def.heatGlowThreshold` computes a 0..0xFFFF glow fraction, resolves the OVERHEATED row
(`actionTable[11]`) muzzle bone via `Entity_ComputeWeaponFireTransform`, and spawns
(`@ 0x54122c` → `+0x1C heatGlowEmitterHandle`) or per-tick moves/re-intensifies
(descriptor +40/+44 = the fraction) the barrel-glow emitter; released when heat drops,
the window expires, or underwater. Corpus: the emplaced .50s/miniguns/DShK/turrets author
`FX_OVERHEAT1 @ HEAT` live; the infantry MG rows ship the keys commented out. UNPORTED —
the emplaced-gun barrel glow is a §8-class follow-up (extends D-WPN-4's heat family).
(5) The kick sound-layer leg (`dword_24E0E80` gate: `Def.kickEffectId` + the decaying
kick intensity → `SoundEmitter_RegisterSetLayers @ 0x54132a`, per tick while kick > 0) —
an unported AUDIO seam, noted with D-WPN-3's sound family. (6) **D-WPN-18** [reimpl
divergence, FIXED 2026-07-15]: the LOCAL fire leg fed `RoundSim` a `(90 − heading)`
mission-yaw bearing where the round bearing frame IS the engine heading frame
(`RoundSim`'s `(cos, sin)` mission-axis mapping is wire-validated on the 0x06 yaw BAM —
the same D-NET-153 flip, reintroduced on the local leg): every local shot flew mirrored
across the NE diagonal, landing impact effects 90° off the aim ray (the `fp_impact_probe`
pin: aiming due north put impacts 14.75 m due east; post-fix they sit on the ray).
`dir_yaw = p->heading` directly now; the `nova_simulation_test` fire case moved its
target onto the true bearing and pins the drained impact position (the old east-side
target had been passing on the compensating error). IDB this session: the two MountSlot
field renames above, `+0x1C` → `heatGlowEmitterHandle`, `CEffectEmitter_UpdatePositionAndParams
@ 0x5f6810` renamed, tracker/glow witness comments at `@ 0x540edf`/`@ 0x540fed`; idb_save run.

**The Barrett/REVVY per-shot particle report (2026-07-15, suppression adjudicated
2026-07-16).** The reported REVX02 behavior — `WPN_Barret` plume missing on repeated
shots in the port, casing emission outliving release — motivated a timing A/B. The
static chain itself is now conclusive: the `@0x5418c8` guard records a GROUP handle;
`CEffectGroup_SetDeathCallback @0x5e1940` stores `ActionSlot_ClearEffectHandle
@0x53f760` at group+0x5C/+0x60; `CEffectGroup_Destroy @0x5e3460` invokes it only after
the child list is empty. `CEffectGroup_AdvanceChildrenAndReap @0x5e59a0` unlinks each
dead child immediately but does not call the group callback. Therefore those visual
observations do not establish an early suppression re-arm; the first-child hypothesis
recorded later in this historical session was disproved by the 2026-07-16 adversarial
read below. Remaining A/B candidates are emitter-clock/lifetime conversion and the
separate per-entity effect-slot updater `Entity_UpdateMuzzleFlashAndEffects @0x493149`,
not the action slot's death callback. Also witnessed, unported: the per-shot muzzle
LIGHT (`ActionSlot_SpawnEffect` tail `@0x402080`: `AmmoDef+36` gates
`LightPool_SpawnGlowEffect @0x56c960`, handle at `entity+436`, blend snapped to 1.0).

**The oscarmike adjudication (2026-07-15, session 3 continued).** The maintainer
pointed at the archived `on-godot-oscarmike` attempt as having better-feeling weapon
particle timing. Its mechanism: spawn the row's particle at the action FINISH
(their reading of `ActionSlot_FinishActivePhase` -> `ActionSlot_PlayEndSoundAndDupes` as "soundsetend + particle"),
UNGUARDED, one hand-authored short Godot scene per shot, parented to the weapon.
Adjudicated against the binary: `ActionSlot_PlayEndSoundAndDupes @ 0x401100` is
SOUNDS ONLY — the end soundset (`ActionDef+12`) plus the `dupsound` echo scheduler
(`ActionDef+44/+48` count/interval via `EffectSlot_AllocateAndInit`, local flag 2 /
remote flag 0) — their finish-leg particle is an invention, and their unguarded
spawn contradicts the witnessed `@ 0x5418c8` guard. Their muzzle-parenting matches
the D-WPN-17 tracker port. `WeaponAction_Fire` fully read this round: the
counter>0/phase-4 holding ticks call the PLAIN `ActionSlot_BeginActivePhase`
(no effect leg); the with-effect `ExecuteActionTick` runs ONLY on the shot tick with
`param7 = ActionSlot_ClearEffectHandle` (`@ 0x542cb4`), so the flash spawn is
per-shot-attempted but handle-guarded, as recorded. **Cross-binary check**: the
guard (`cmp [esi+18h],0; jnz`) and the handle/anchor record
(`mov edx,[esi+2Ch]; mov [esi+18h],edi; mov [esi+28h],edx`) byte-match the user's
JOTAC-era `Jointops.exe` at the same VAs (file 0x1418c8 -> 0x5418c8; 0x208c ->
0x40208c) — the per-shot-particle contradiction is NOT a binary-version delta.
A temporary `NOVA_WPNFX_UNGUARDED` host toggle served as the discriminating A/B
instrument for one round and was removed; the binary's guarded path remains authoritative.

**CORRECTED TO THE WHOLE-GROUP-DEATH WINDOW (2026-07-16 adversarial review).** The
2026-07-15 A/B result was useful presentation evidence, but it could not identify the
callback's object lifetime. The complete call chain does: `ActionSlot_ExecuteAction`
(`@0x4020a0`) calls `ActionSlot_SpawnEffect @0x401f20`; the world/group spawn
(`@0x5f6df0` → `@0x5ea200`) installs `ActionSlot_ClearEffectHandle @0x53f760` through
`CEffectGroup_SetDeathCallback @0x5e1940`, which writes group+0x5C/+0x60.
`CEffectGroup_AdvanceChildrenAndReap @0x5e59a0` removes and destroys each dead child
immediately, but the callback is invoked by `CEffectGroup_Destroy @0x5e3460` only when
the GROUP is destroyed. The earlier `CEffectEmitter_OnChildDied @0x5ef9c0` parent-gate
interpretation was the wrong object/callback chain. Therefore `SuppressWhileOwned`
and `ReplaceOwned` both retain their slot mapping through the final child; their
admission behavior differs on a new spawn, not on callback lifetime. The port now
recycles dead children individually and clears the suppression mapping only at final
group death. D-WPN-19 records the false-reading correction and the whole-group
regression replaces the former first-child contract.

**The weapon heat model (witnessed + ported 2026-07-22).** Heat is **not a stored
accumulator** — the slot carries a DEADLINE and the level is recomputed from what is
left of it, so the linear cooldown is implicit and costs no per-tick work.

- **Read** [orig: `WeaponSlot_CalcAccumulatedHeat @ 0x53f780`]:
  `heat = def+880 × (slot+0x14 − current_tick)`, gated on `def+876 != 0` and on the
  deadline still being in the future; otherwise 0. The gate is the DEF field, so a
  weapon with no heat model reads 0 even if a stale window survived a def swap.
- **Write**, once per shot at the recoil arbiter [orig: `WeaponAction_Recoil
  @ 0x542f8b..0x542fdc`]: a lapsed window first restarts from `current_tick`
  (`@ 0x542fa0` — time spent cold is never credited), then the deadline advances by
  `def+876 / def+880 + 1` ticks (`@ 0x542fb4`), i.e. one shot's worth of cooling. If
  the resulting level exceeds **73727** the window is re-stamped to
  `current_tick + 73728/def+880 + 1` (`@ 0x542fc4`/`@ 0x542fdc`) — a **ceiling of
  0x12000**, so a held trigger cannot bank unbounded heat.
- **Deny** [orig: `WeaponAction_ProcessFrame @ 0x541046`]: while the window is live,
  a level above **0xFFFF** converts a QUEUED `FIRE(2)` into `EMPTY(5)` — the dry-fire
  click. It does not cancel an in-flight FIRE and does not stop the level climbing;
  the gun simply coughs. Because the deny only rewrites a *queued* action and the
  auto-fire volley is sustained by the recoil window's deferred re-queue, an overheat
  BREAKS the chain: the player must release and press again. The 0xFFFF deny against
  the 0x12000 ceiling is the hysteresis — worst case ~196 ticks (~3.1 s) of lockout.
- **Expiry** [orig: `@ 0x54125f`]: a window that has lapsed — or any window while the
  owner is submerged and the def lacks `Underwater` (Flags 0x4) — is zeroed and the
  glow emitter released.
- **The def keys** [orig: `WeaponDefs_ParseLineCallback`]. `heat_values <pctPerShot>,
  <pctPerSecondCool>` `@ 0x543eb7`: each value goes through the engine's digit parser
  (`Math_ParseFixedPoint16 @ 0x6131f0`, NOT atof) and is then **integer-divided** —
  `def+0x36C = v0/100` (percent -> a 0..0x10000 fraction) and `def+0x370 = v1/6200`
  (percent-per-second -> per-tick at the 62 Hz logic rate). Both divides truncate and
  both are load-bearing, because the runtime divides the two results against each
  other. `heat_effect <fx>,<threshold>` `@ 0x543e36` stores the glow effect name at
  `def+0x358` and the raw 16.16 threshold at `def+0x374`; shipped rows author
  `heat_effect heat, .5, 30, 60` and the parser reads only the first two values, so
  the trailing pair is authored-but-unread in retail too. A third key `heat_sound`
  (`def+0x368`, `@ 0x543e85`) exists in the parser and **no shipped weapon.def
  authors it**.
- **Who authors it.** Thirteen JOX entries, all emplaced or vehicle-mounted heavy
  guns — `WPN_EMPLCD50`, `EMPLCDMINI`, `EMPLCDGRND`, `EMP50TRI`, `UG50cal`,
  `EMPLCD50NA`, `EMP50TRI180`, `EMPLCDHUM`, `EMPLCDBOT`, `EMP50BOT180`, `EMPLCDSUV`,
  `EMPLCD50SUV`, `QUAD50`. No infantry weapon authors heat, so the model is inert on
  foot and the HUD bar correctly never appears there. Worked example — the .50's
  `heat_values 2,4` parses to 1310 per shot / 42 per tick, i.e. 32 ticks of window a
  shot; fired at the 8-tick action cadence the level climbs ~1008 a shot, lights the
  glow (threshold `.5` = 0x8000) around the 33rd and denies at the 65th.
- **The HUD feed** [orig: `HUD_BuildEntityInfo @ 0x4b852e`]: the same accumulator
  writes `hudInfo+60`, clamped to 0xFFFF at `@ 0x4b854d`, which is what
  `HUD_DrawWeaponHeatBar @ 0x599700` fills from (D-HUD-15).

Ported as `DefWeaponDef::heat_*` + `WeaponFsmDef::heat_*`/`WeaponSlotState::
heat_window_end_tick`/`weapon_slot_accumulated_heat` + the `NovaSimulation` weapon-view
feed. The submerged term has no live source yet (D-WPN-29).

**What the glow half still needs (D-WPN-28; writer audit corrected 2026-07-29).**
The level is ported. Retail has two dedicated model CTRL writers plus a separate
particle effect. OpenNova hosts the first-person writer and the witnessed
authority-side carrier-attachment writer. Two bounded seams remain:

1. The **particle emitter** — the `@ 0x54109e..0x54122c` leg above. This is the actual
   overheat visual (shipped data authors `FX_OVERHEAT1` on the emplaced .50s, miniguns,
   DShK and turrets) and it is a host effect seam, still unported.
2. **Compact-joiner reconstruction.** The entity compact carries neither the
   parent's heat window nor the full UseGun attachment ownership needed by the
   retail writer. Joiner rows therefore leave the scoped value unavailable
   instead of synthesizing a phase or stale cold zero (D-3DI-2).

The implemented **world-model `HEAT_GLOW` CTRL register** is global ordinal 54 in the 96-entry,
   32-byte descriptor table (`aLodFrac @ 0x83dce8`; resolver
   `[orig: CtrlName_ToOrdinal @ 0x57b290]`; per-model ordinal store
   `[orig: sub_5B4640 @ 0x5B4640; @ 0x5B46E6]`). `B50Cal.3di` carries
   `[HEAT_GLOW, EWEAP_GUNYAW, EWEAP_GUNPITCH]` in local order, and the loader
   remaps those references to global 54/55/56. The world writer is
   `HUD_CacheWeaponSlotInfo @ 0x440930`; its only caller is the valid-bone branch
   of `Entity_AttachToBoneAndUpdateTransform @ 0x546518`, which passes the
   **parent carrier** while updating a UseGun child. It caps live heat at
   `0xFFFF` and writes ordinal 54 at `0x440969`/`0x440991`. OpenNova validates
   that same carrier/child/seat relation, publishes scoped cold zero, and feeds
   the value to authority/SP/listen parent PANM, collision, and wire-direct
   presentation. Writer-scoped composition releases only this source and
   restores any underlying register value.
   The first-person path writes at `0x4DEEC2..0x4DEEF5`
   `[orig: Player_RenderFirstPersonViewModel @ 0x4DED60]` and is now hosted:
   `NovaSimulation::get_local_player_weapon_state` emits `heat_glow` clamped to
   `[0,0x10000]`, `PlayerWeaponView` carries it, and `LocalPlayerPresenter` writes it
   on every owned viewmodel submit, including literal zero. The register is
   cleared only when viewmodel ownership ends.

The generic ACTION `ctrlreg <NAME>` ramp is an additional writer path
`[orig: ActionDef_ParseScriptLine @ 0x4027FA; ActionSlot_ExecuteAction
@ 0x4020CC; CtrlRegAnimSlot_Allocate @ 0x401CA0;
CtrlRegAnimSlot_UpdateAll @ 0x401BF0]`. No shipped `weapon.def` authors that key,
but that corpus fact does not disable the dedicated heat writers. OpenNova now
has the signed CTRL catalog/consumer path and both dedicated model publishers;
D-WPN-28 remains open for the particle emitter and compact-joiner
reconstruction. The generic animator and remaining producer census are D-3DI-2.

A related find while walking this: `ActionSlot_ExecuteAction @ 0x4020a0` (the DEFAULT
action handler) carries its own copy of the heat-window stamp, gated on
`MountSlot+0x2C == RECOIL(3)` `[orig: @ 0x402242..0x402299]`, byte-identical to the one
in `WeaponAction_Recoil`. It only runs for a recoil row that does NOT name
`wpn_std_recoil`, which no shipped row does (D-WPN-1), so there is no double stamp in
practice and the port's single stamp in the recoil handler is correct for all shipped
data.

**Open follow-ups:** who queues OVERHEATED(11) as an ACTION (its ROW is consumed as
glow data by the heat window leg above — whether anything transitions TO state 11
remains unwalked);
the `*_map` scope function variants;
the `g_FpWeaponViewFlags` option bits beyond bit 0; the `word_B7C670` transition write vs the §5.16 shot-seq;
remote-entity action sounds/effects (the `@ 0x541a83` leg) once remote slots pump;
the RoundData_SpawnRound 0x2000000 test site; the slot Elevation zoom-adjust input;
the scope tri-state (engaged/active/hipfire) for the movement reversal legs; the
HandGunUp auto-follow (player flag 0x4000 writer); the emplaced-gun overheat glow port
(the heat window leg above); the kick sound-layer port (`SoundEmitter_RegisterSetLayers`);
the Barrett/REVVY visual A/B outside the now-closed action-slot suppression question
(per-entity effect-slot writers `@0x493149`, emission-clock conversion, and the muzzle
light `@0x56c960`);
loose-REVX audio: a loose
extract advertises no expansion, so revx02.LWF (the M82 GS_/GF_ sets) never
loads — the expansion setting must name it (config, not code).

### 5.63 The spawn-kit chain, the per-player slot pool, map availability rules, and manual switching (the loadout grill, 2026-07-18)

Reimpl: `libs/world/weapon_inventory.{h,cpp}` (the pool/kit/walk translations, ctest
`weapon_inventory`), `NovaSimulation` (`rebuild_local_player_loadout` + the switch/commit
seams), `local_player_presenter.gd` (keys 1..9, `[`/`]`), `armory_presenter.gd`/`armory_menu_companion.gd`
(availability filter + multi-slot ACCEPT), `mission_runtime.gd` (the .bms promote), GUT
`nova_simulation_test.gd` / `armory_presenter_test.gd`.

**The spawn-kit buffer.** `restrictionData @ 0x24D4E00` (IDB comment proposes
`g_spawnLoadoutBuffer` — the name is a misnomer; the availability table is separate) is a
2048-B `{name\0 ammoPri\0 ammoSec\0 flags\0}*` tuple buffer — the SAME format as the
armory per-class buffers `[orig: g_armoryLoadoutBufferByClass @ 0x25DD740]`. Writers:
`Mission_LoadBMSFile @ 0x40f4e0` (SP: the .bms loadout chunk, sanitized via
`AIProfile_SanitizeConfigData @ 0x40cfe0`, each tuple availability-filtered by catalog
index `@ 0x40f834`, and a synthesized `{"WPN_KNIFE","-1","-1","-1"}` when everything
filters out `@ 0x40f899`; in a NET session both chunks are seek-skipped `@ 0x40f6a1`);
`Game_StartMission` + `apply_session_settings_to_globals @ 0x551500` (SP priority:
mission-list entry+1308 buffer → profile offline loadout `byte_256113C` → the literal
`"WPN_M4AUTO"` `@ 0x5246be/@ 0x5519e4` — the shipped default-kit rule the reimpl's
D-NET-143 default now rides); `NapiNPClientMsg_0x050` team assign + the Game_StartMission
MP leg (the profile per-class buffer at `classBlock+6+2048*(class-5)` `@ 0x431a70/
@ 0x5257d1`, immediately re-submitted as C2S 0x2F via `NetPacket_SendLoadoutSubmit
@ 0x42cdc0` — renamed 2026-07-24 from the `NetPacket_SendWeaponRestrictionMask` misnomer,
full builder witness §5.56 — with weaponSlotIndex 195); the S2C 0x5A apply `@ 0x4293e4`. Readers:
`Player_InitPlayer`, `Server_InitAllPlayerEntitiesForRound @ 0x516aa0` (the default for
players with no submitted loadout), `Server_SendWeaponSlotListToPlayer @ 0x502622`,
`Armory_StageLoadoutBuffers @ 0x564290` (armory open staging).

**Player_InitPlayer @ 0x4e15f0 — the spawn weapon leg.** `g_currentWeaponSlot = 195`
(category 3 = the Primary key, rank 0) `@ 0x4e17ff`; class/avatar from
`g_charClassTeam1/2 @ 0x24D20E0/E4` + `g_avatarTeam1/2` by entity Team (1/3 vs 2/4);
`AvatarDef_BuildDisplayList @ 0x54b9e0` expands the kit tuples into 255×40-B name entries
(each def's `loadout_subclasses` sub-variants append by name `@ 0x54bb07`);
`WeaponSlotPool_ResetAllEntries @ 0x53f240` → `WeaponSlotTable_LoadAllFromDefs @ 0x5414e0`
(slot = table + 100·(rank + 65·category) `@ 0x5415d3`; a DIFFERENT def on an occupied
combo logs "overloading" and the incumbent stays `@ 0x5415d6`; carry bits entity+44 |= 8
(flags&0x1000) / 0x10 (flags2&2) `@ 0x5415aa`) → `WeaponSlots_SeedAmmoPoolsFromDefs
@ 0x541690` (renamed this session from the `WeaponOverlay_BuildTypeLookup` misnomer:
pools[def ammoclass byte +0xD8] = the per-class `classrounds` override else `startrounds`)
→ `WeaponSlots_RecalculateAmmoFromCapacity @ 0x542280` (return the clip to the pool, then
draw min(clipsize·units, pool) — the −1-startrounds degeneration §5.57 notes is literal)
→ `Player_SelectWeaponSlot(195)` + `Player_SwitchToWeaponByHandle(195)`
`@ 0x4e1995/@ 0x4e19a1`. Host weapon slots = the server per-player table via
`Entity_GetWeaponSlotsPtr @ 0x510010`; a client falls back to the local array
`unk_B62F18` (log "not using net weapon slots"). `g_weaponRestoreFlag @ 0x24D4DFA`
(renamed from byte_24D4DFA) == 1 short-circuits the fill into
`WeaponOverlay_RestoreFromBackup @ 0x4ddc80` (mid-mission restore).

**weapon.def keywords closed this session.** `classrounds <class> <n>` `[orig: handler
@ 0x543ab0]`: the class token resolves through the char-class VALUE table `@ 0x830EE8`
(medic=1, sniper=2, gunner=3, rifleman=5, engineer=6; ≤6 enforced; 4 never ships) and
stores at AdmDef+0x60+value·4 — the per-class startrounds override the pool seeder
consumes `@ 0x5416c4`. `switchcategory <N>` `[orig: handler @ 0x5445a8]`: +0x168 flag=1,
+0x164=N; consumed by the `WeaponAction_Recoil` tail `@ 0x543062` —
`Player_SwitchToWeaponByHandle(N*65)` after every recoil completion (the grenade/LAW
auto-switchback). Both parse into `DefWeaponDef` (`classrounds[7]` at the raw table
indices, `switchcategory`/`has_switchcategory`) and ride `WeaponTableEntry`.

**The ammo pools.** Per-ammo-class carried pools: class 1 lives at entity+288 (u16),
every other class in `g_localAmmoPools @ 0xB75FE8` (renamed from `outTable`) locally or
`serverPlayer+88664+4*class` on the authority; caps = the `ammoclass_max_carry` table
`@ 0x24E7DE0` (class-1 cap `@ 0x24E7DE4`) applied on every set/add `[orig:
WeaponSlot_SetAmmoCount @ 0x540b50 / WeaponSlot_AddAmmo @ 0x540a20 /
Entity_GetScoreValueBySlotType @ 0x5406e0 pool leg]`. The reimpl folds both storages into
one per-entity array keyed by a build-time ammo-class registry (D-WPN-24).

**The three switching walks.**
- `Player_SelectWeaponSlot @ 0x4dd680` — CATEGORY-level select. Stages entity+0x308
  (pending) and commits to EquippedSlot +0x118 unless seated (`parentEntity && parentSlot
  ∈ {2,3}` `@ 0x4dd6fc`); stamps `equippedAdmIndex` entity+0x2B0 `@ 0x4dd727`. The exact
  requested slot is taken ONLY when its def has flags2&1 NoSelect (the parachute-style
  forced equips, `@ 0x4dd6d8` — the bit polarity is INVERTED vs the scans); otherwise the
  first populated non-NoSelect slot of the category (rank order `@ 0x4dd749`), else
  global (`@ 0x4dd76d`); −1 = the global scan directly.
- `Player_SwitchToWeaponByHandle @ 0x4e0170` — the category-key walk (input actions
  200–210 pass `(action-200)*65` `@ 0x4e1144`). Gates: `parentSlot ∉ {2,3,5}` (the raw
  SeatType values — Controller/Gunner/Driver; passengers switch freely), entity
  Flags&3 clear, the equipped FSM (blocked while FIRE; RELOAD blocks same-category
  presses only; RECOIL blocks cross-category only `@ 0x4e0192`), and the entity+0x68
  AI-slot binding (writer `Entity_AllocateAISlot @ 0x40d2f4`) gating the whole mount
  walk `@ 0x4e023a`. No equipped slot → Select(handle) else Select(−1) first. Same
  category = rank cycle: advance-first `(rank+1) % 65` from the equipped rank; different
  category = exact-slot check then the rank scan from 0. Eligibility `@ 0x4e02c3`:
  weapon_class (+0x3A4) ∈ {1,2} OR nonzero ammo score (`calculate_kill_score @ 0x5407e0`
  as the slot predicate = class pool + loaded clip), AND !(flags2&1). A full wrap plays
  the deny sound `PlaySoundOnDedicatedServer(dword_24E08C4)` `@ 0x4e0354` (set name
  unwitnessed — D-WPN-22).
- `Player_CycleWeaponSlot @ 0x4dfe70` — next/previous (input cases 212/214, ±1) over ALL
  780 combos with wraparound; EVERY candidate needs the ammo score (no weapon_class
  exemption `@ 0x4dff39`); reaching the start again returns silently (no deny). Cases
  212/214 dual-purpose to `Player_AdjustWeaponElevation(±2)` when manning a can-fire
  elevatable gun; 222/223 = `Player_AdjustWeaponZoomLevel(±1)`.

**The mount + commit.** `Player_MountWeaponSlot @ 0x4dfa40`: stamps `g_pendingWeaponSlot
@ 0xB75FD0`, queues SWITCHRANK on the equipped slot when the new def shares its category
(`WeaponSlot_TryQueueSwitchRank @ 0x53f1c0`: phase ∈ {0,4,0x40} → counter=0, next=8) else
SWITCHFROM (`WeaponSlot_ForceQueueSwitchFrom @ 0x53f170`: refused while 6/7 current;
complete phase → full reset + next=7, else next=idle — the request drops); scope state
(flags 0x20000000 → scope on + def fov; cross-category → fov 80.0), view-bias zeroing
`@ 0x4dfbcf`, camera update; the completion consumes the pending slot (the equip swap +
the FP model re-resolve `count_weapon_effects_and_update_viewmodel @ 0x4dc9e0`). The
reimpl commits on the FSM's switchfrom/switchrank `action_finished`, re-installing the
viewmodel through the host event drain (`switch_to_weapon`).

**ToSpecial/QuickSwitch (input case 220, unported — D-WPN-23).**
This path was previously mislabeled Binoculars. Action 220 is ToSpecial (catalog id 37,
default F): global 0xB75FE0 is the slot whose def carries `QuickSwitch 0x08000000`
(found at table load `@ 0x54165a`; only retail `WPN_MAG58_PointAim` authors it), and
0xB75FE4 stashes the equipped slot. Press equips the target via
`Player_EquipWeaponByEntity @ 0x4e0370`; release (runtime binding words
`0x81B68C/E`) re-equips the stash. True Binoculars is the independent action 26/default-B
state machine described in §5.39 and performs no inventory swap. Msg 0x38
(`handle_weapon_switch_packet @0x4260b0`) is the server-confirmed weapon-entity swap
against the tracked pickup entity at localPlayer+0x140 — also unported.

**Map availability rules.** `g_armoryWeaponAvailability @ 0x24D5600`, 255 ints indexed by
catalog/adm index. Values: 0 banned, 1 allowed (default), 2 ARMORY-ZONE-ONLY (the server
0x2F gate `@ 0x515a4a` additionally requires the requester's entity Flags&0x400000),
3 mission-allowed; the armory populate lists any nonzero `@ 0x566e6b`; the SP mission
loadout filter drops zeros `@ 0x40f834`. Builder `build_item_restriction_table
@ 0x54ddb0`: name-list mode consumes `{name\0 statusByte}` pairs (the .bms
item_availability chunk — the pair value is the RAW byte, −1 → 3), template mode copies
255 ints (−1 → 3), sub-variants INHERIT the parent's value through the
`loadout_subclasses` skip walk, no list = all-1. Writers: `Game_StartMission @ 0x5246e8`
(SP: the mission-list entry+3356 template else all-1; class word entry+4376 →
`g_hostClassAllowMask`), `apply_session_settings_to_globals @ 0x551831` (MP host: the
per-weapon session settings ints `@ 0x2550CA8` with 3 = defer-to-mission),
`CAdminServer_HandleWeaponCommand @ 0x4045a0` (live admin), the mission-list scanners
(`Mission_BuildMapListFromPFF @ 0x562910` / `MissionList_ScanAndBuildFromFiles
@ 0x563170` — they compile each .mis item_availability chunk into the 4584-B mission-list
entry: +1308 the 2048-B loadout buffer, +3356 the 255-int availability template, +4376
the class-allow word). The second weapon.def parse `WeaponDef_LoadAll @ 0x54dd10` fills
the 255×192 loadout catalog `g_weaponDefTable @ 0x2540ce0` ("None"@0; +36
loadout_subclasses, +40 display-name ptr, +108 weapon_class routing, +112/+116 the
char/team masks, +144 icon) — same index space as AdmDefs for a fresh parse.

**The wire messages.** S2C 0x66 weapon restrictions: `[u8 count]{[u8 idx][u8 value]}*`,
only values 0/2 travel, the client resets to all-1 first `[orig:
NapiNPClientMsg_HandleWeaponRestrictions @ 0x42d4c0; serializer
NetPacket_SerializeWeaponRestrictionTable @ 0x5102c0, sent from
Server_SendInitialGameStateToPlayer @ 0x51c0ca]`. S2C 0x76 class-allow mask:
`[u16 mask]` `[orig: NapiNPClientMsg_HandleClassAllowMask @ 0x42d540 (renamed from
_0x076); serializer NetPacket_WriteClassAllowMask @ 0x510350 (renamed from the
NetPacket_WriteServerTick16 misnomer)]`. `g_hostClassAllowMask @ 0x24D59FC` writers
closed the §menu-re open question: MP = per-class host settings ints `@ 0x25510A4+`
(1 = always, 2 = allow iff the mission entry's class-word bit, else never — 10 bits
`@ 0x551887..0x551996`); SP = the mission entry word `@ 0x551a1d`. The armory-open key
gate reads `g_hostWeaponsRule @ 0xA85B6C` (renamed; SP or 0 = armory allowed
`@ 0x4e0b29`; S2C 0x0A copies it to clients `@ 0x4300e3`; the WPN_ARMORY/NEVER/MISSION
radio values remain unwalked).

**Server round init.** `Server_InitAllPlayerEntitiesForRound @ 0x516aa0` per-player-slot
offsets (stride 100584): +416 team, +464 the 780×100 weapon table, +78464 the 255×40
display list, +94408 the per-player 2048-B loadout buffer (defaulted from restrictionData
when the +5/+96483 flags say no submitted loadout), then the same
display→fill chain as the local leg.

**Reimpl mapping.** `libs/world/weapon_inventory.{h,cpp}` carries the structural
translations (each function cites its original); `libs/npruntime/weapon_table_build.cpp`
fills the new `WeaponTableEntry` fields + the ammo-class registry/caps;
`weapon_fsm_queue_switch_from/rank` land beside the other request writers;
`NovaSimulation.rebuild_local_player_loadout` is the Player_InitPlayer weapon leg,
`request_local_player_weapon_category/cycle` the input dispatch, the commit rides the
FSM `action_finished` seam. Evidence: ctest `weapon_inventory`; GUT
`nova_simulation_test.gd` (`test_armory_reads_and_clears_authoritative_local_loadout`,
`test_local_fire_spawns_the_authoritative_round_and_impact`), `armory_presenter_test.gd`.

**IDB changes (2026-07-18):** renamed `NetPacket_WriteServerTick16 →
NetPacket_WriteClassAllowMask`, `NapiNPClientMsg_0x076 →
NapiNPClientMsg_HandleClassAllowMask`, `sub_564290 → Armory_StageLoadoutBuffers`,
`WeaponOverlay_BuildTypeLookup → WeaponSlots_SeedAmmoPoolsFromDefs`, `outTable →
g_localAmmoPools`, `dword_B75FE0/4 → g_binocularsWeaponSlot/g_binocularsStashedSlot`,
`word_81B68C/E → g_binocularsBindingKey0/1`, `dword_A85B6C → g_hostWeaponsRule`,
`word_A76412/6 → g_bmsLoadoutChunkLen/g_bmsAvailabilityChunkLen`, `byte_24D4DFA →
g_weaponRestoreFlag`; rename proposal left as a comment: `restrictionData →
g_spawnLoadoutBuffer` (human-curated name, proposal-first policy).

**Erratum (2026-07-21):** the 0xB75FE0/4 and 0x81B68C/E names in that 2026-07-18
rename list are wrong: they belong to ToSpecial/QuickSwitch, not Binoculars. Treat them as
`g_quickSwitchWeaponSlot` / `g_quickSwitchStashedSlot` and the corresponding
ToSpecial binding words; Binocular globals are 0xB76538..0xB7653B.

**Open follow-ups:** the entity+0x68 AI-binding value DURING Player_InitPlayer (decides
whether the spawn switch's mount walk runs — D-WPN-21 models it as not-yet-bound); the
deny sound set behind `dword_24E08C4` (loader unwalked); the WPN_ARMORY/NEVER/MISSION
radio values of `g_hostWeaponsRule`; the retail ammo-class builtin id table `@ 0x830F10`;
the def+0xDC ammo pass-type path (`sub_5405F0`/`sub_540670`) for shared-pool "clip"
weapons; the armory per-class buffer indexing quirk (`Armory_StageLoadoutBuffers` copies
0x5800 B into `g_armoryLoadoutBufferByClass[5]` — 11 slots of a 16-slot profile block);
the mission-list scanners' class-word source inside the .mis.

New divergences this session: D-WPN-20..24 (docs/divergence-ledger.md).

### 5.64 The host-initiated session close — the connection-description punt (live retail capture, 2026-07-26)

**How it surfaced.** An OpenNova joiner sat at the DEATH deploy screen on a stock retail JO
1.7.5.7 co-op host. After roughly six minutes the host sent ONE message and then went
permanently silent; our client never noticed, kept RTT-pinging a closed session, and left the
deploy screen up accepting clicks that went nowhere. Reproduced identically twice — two
sessions, same last three packets. That one message is retail's **punt**: the transport's own
disconnect event, and the channel a stock host uses to close a session on its own terms.

**Carrier — not a session opcode.** The punt rides as an INNER protocol message with the
high-table selector set (flag bit `0x80`, the `settings_update` bit of D-NET-5) and low tag
**3**, i.e. **full tag `0x103`**. `nw_pp` renders it `tag=0x1103` because it prints
`tag | (settings_update ? 0x1000 : 0)`. The builder mints it via
`NapiNPMessage_Create(msg_id 3, msg_flags 0x10, msg_class 1)` — `msg_class 1` IS the high-bit
table selector — with `len_field_size 3` (LEN8), so the observed flag byte is `0xA0`
(`0x80` high-table | `0x20` LEN8). Both directions index the ONE high-bit table
(`g_np_msginfo_highbit @ 0x849E80`; rows 0/1/2/3 = `0x621940` / `0x6219F0` / `0x62A040` /
`0x621AE0`, and both `msginfo_high_client` and `msginfo_high_server` are built from that single
table by `NapiNPProtocol_InitMsgInfoIndex @ 0x61E400`), so a host sends the identical record a
leaving client sends. This is §4's `H:0x03` row.
[orig: `NapiNPDataTransfer_SendDescription @ 0x628C80` -> `NapiNPMessage_Create @ 0x627FC0`
(`len_field_size 3 @ 0x628316`)]

**Body — a flat TLV run, `NAME\0 [u16 len LE][len bytes value]`.** The ordinary NAPI in-match
TLV shape (`NapiNP_WriteTLV @ 0x61DD60` / `NapiNP_ReadTLV @ 0x61DBE0`), and an empty name
terminates the walk BEFORE the length read. The seven-field set is **exactly the C2S `0x46`
ClientGoodBye's, minus that message's leading session-key dword** — the same builder vocabulary,
because both are the same disconnect event on two different carriers.

| Field | Type | Meaning |
|---|---|---|
| `DS` | u32 | sender role (1 = server, 2 = client). The receiver reads it and **deliberately does not store it** (`@ 0x621BA5`) — it re-derives the role from its own connection. |
| `DC` | u32 | disconnect class. The client's reason dispatch runs only for **`DC == 2`** (`@ 0x4C6563`). |
| `DP1` / `DP2` | u32 | event params; the punt builder leaves both zero (`[3]`/`[4]` of the event struct below), and the capture agrees. |
| `DSTR` | string | free-form text, retail keeps the first 128 bytes. For a punt this is the `sprintf`-formatted mismatch type. |
| `DPC` | u32 | the reason CODE — the client's exit-reason switch keys on THIS (`@ 0x4C6569`). |
| `DDSTR` | string | event tag, retail keeps the first 32 bytes. |

Captured punt, byte-exact (80-byte payload, the whole message the host sent before going
silent):

```
44 53 00 04 00 01 00 00 00                 DS    len 4  = 1        (server)
44 43 00 04 00 02 00 00 00                 DC    len 4  = 2        (the dispatched class)
44 50 31 00 04 00 00 00 00 00              DP1   len 4  = 0
44 50 32 00 04 00 00 00 00 00              DP2   len 4  = 0
44 53 54 52 00 04 00 74 33 35 00           DSTR  len 4  = "t35"    (mismatchType 35)
44 50 43 00 04 00 21 00 00 00              DPC   len 4  = 33
44 44 53 54 52 00 0d 00 "LogPuntEvent\0"   DDSTR len 13 = "LogPuntEvent"
```

**Sender chain.** `Server_LogCRCMismatchPunt @ 0x517ED0` formats
`sprintf(msg, "t%d", mismatchType)` (the literal `"t%d"` is at `@ 0x7CFB9C`; IDA mislabels it
`off_`) and calls `CNapiNPConnection_SendChatMessage(conn, msg, 33, "LogPuntEvent")`
`@ 0x517F5A`. The literal **33** is DPC and the literal **`"LogPuntEvent"`** is DDSTR — both
match the capture exactly, and DSTR is the formatted type, so `"t35"` means mismatchType 35.
`CNapiNPConnection_SendChatMessage @ 0x4C7EF0` builds the 0xB8 event
(`[1] = role`, `[2] = 2`, `[3]`/`[4] = 0`, `[5..36] = reason(128)`, `[37] = reasonCode`,
`[38..45] = extra(32)`) and hands it to `NapiNPDataTransfer_SendDescription @ 0x628C80` — i.e.
the DS=1 / DC=2 / DP1=0 / DP2=0 / DSTR / DPC=33 / DDSTR of the capture, field for field.
Naming caveat: **the `SendChatMessage` name is not to be read as chat** — both witnessed call
sites (this one and the `"PUNT ACRC"` leg of §5.17) use it to ship a disconnect EVENT. Whether
it has a genuine chat caller is unwitnessed, so the IDB name stands until someone settles that.

**`Server_LogCRCMismatchPunt` is a SHARED punt-message helper, not evidence of a CRC failure.**
Its `mismatchType` argument selects the reason text, and three distinct families call it:

| type | Call site | What it actually is |
|---|---|---|
| 6 | `Server_CheckPlayerViolations @ 0x51ABD0` | the per-player violation sweep. |
| 16 / 24 | `Server_UpdateAllActivePlayerSlots @ 0x518820` | the **eighth-silent-round reap**: every 744 ticks the host calls `Server_SendEntityHandleAndInputState` and increments the per-slot counters `slot[22473]` / `slot[22493]`, punting once either reaches 8. |
| 35 | `Server_TickUpdate` (`push 23h @ 0x51E13A` -> `call @ 0x51E13D`) | the **SIX-MINUTE IDLE KICK**, guarded by `cmp eax, 57E40h @ 0x51E109` where `eax = GetTickCount() - slot[+0x188E4]` — `0x57E40` = 360000 ms. |

So the punt this session captured is retail's **AFK timeout**: a player parked at the deploy
screen never refreshes the slot's last-activity stamp, and at six minutes the host closes the
session. That is faithful host behavior — the defect was entirely ours, in ignoring the kick.

**Durable guard (do not re-derive):** `handle_anti_cheat_crc_check @ 0x502050` punts ONLY on a
**wrong** CRC (`"PUNT ACRC"`, §5.17) and never on silence. Not answering an anti-cheat challenge
is not itself punished there; the silence pressure lives in the `@ 0x518820` counters above.

**Receiver.** `CNapiNPConnection_HandleDescriptionPacket @ 0x621AE0` (the `H:0x03` row) walks
the TLVs case-insensitively in ANY order with zero defaults (`@ 0x621B8C..0x621C7D`; DS matched
then not stored `@ 0x621BA5`), stores the event only when its slot is still invalid
(`@ 0x621D3C`) — so the FIRST record wins and a repeat cannot restate the cause — then leaves
the active state: `state 5 -> 6 @ 0x621D5F`, or a `pending_disconnect` latch `@ 0x621D6B`
mid-connect, either way reaching
`CNapiNPConnection_TeardownActiveConnection @ 0x6253C0` (the leave/teardown catalogued in the
§8 `CNapiNPConnection_*` naming pass: throttled goodbye packets, leave callbacks, crypto/session
key clear). Receipt is TERMINAL at any stage.

**What the player sees — no dialog.** `CNapiNetwork_OnDisconnectedFromServer @ 0x4C63D0` gates
the reason dispatch on `DC == 2` (`@ 0x4C6563`) and switches on `DPC` (`@ 0x4C6569`); it also
writes its own line to `_connectlog.txt` (`@ 0x4C6486`). The captured **DPC 33** hits no
`g_mission_exit_reason` row and falls to `Input_QueueEvent(3, ...)` `@ 0x4C67A4`, whose action
sets `g_mission_exit_reason = 1` and drops the connection
(`CNapiNetwork_DisconnectActiveConnection(ctx, "I.C:CIDEMIS")`, `Input_HandleActionBinding`
case 3 `@ 0x49AF2C`); reason 1 takes the plain teardown + nav-push `"MainMenu"` arm of the
mission-exit dispatcher (`@ 0x5684A8` -> `@ 0x568654`) with **no error string**. Only exit
reasons 9..19 (disconnect codes 35..46/49) reach
`CNapiNetwork_GetDisconnectReasonString @ 0x4C7000`, which formats the `gameerr.bin`
`"MPGameDisconnectCodes"` entry with `[[$]]` replaced by DSTR. So the faithful outcome for this
kick is abort-to-menu with the reason as a log line — an in-world dialog would be invention.

**Observed alongside (same session).** The host streamed 37× S2C `0x30` and 36× S2C `0x31`
anti-cheat challenges at the parked joiner, and we answered ZERO of both until this change
(§5.65). Neither is the kick's trigger — the type-35 arm reads only the per-slot activity stamp
(whose refresh site is still unwitnessed, D-NET-182) — but an unanswered challenge stream is a
real interop gap on its own, and silence is exactly what the `@ 0x518820` reap counts.

**Reimpl (2026-07-26).** Wire: `PROTOCOL_TAG_CONNECTION_DESCRIPTION = 0x103`, the typed
`DisconnectEvent{ds,dc,dp1,dp2,dstr,dpc,ddstr}` and `parse_disconnect_event()` live in
`libs/npwire/.../session_hello.{h,cpp}` beside `client_goodbye_to_bytes` — the encoder of the
identical field set — and reuse that file's TLV helpers. Runtime: `JoinerConnection`'s dispatch
loop admits this ONE settings-flagged record ahead of the settings skip, gated on the exact full
tag AND on the body decoding as a description; `on_host_disconnect()` latches the first event
only, composes a player-facing reason from DPC/DC/DDSTR/DSTR, and fails the connection to
`Phase::Error`, so `session_lost()` is true at ANY phase (the 120000 ms silence reap of
D-NET-177 stays the in-match fallback for a host that vanishes without one). Host surfacing:
`NovaSimulation::is_session_lost()` is the bound state test (the reason string is presentation,
not state); `NovaDeployScreenPresenter` checks it before the deployment-release edge and calls
`teardown()` instead of emitting `closed` — the punt clears the same pick-pending flag a release
does, and closing there handed the shell back to `State.WORLD` over a dead session; `main_game`
routes both loss causes through one `_abort_to_menu(stage, reason)` leg. Pinned by
`npruntime_host_punt` (the 80 captured bytes decoded field for field, order/shape robustness, a
joiner parked at the deploy screen closed by the capture, and ordinary settings traffic
undisturbed) and `godot/tests/net/host_punt_surfacing_test.gd` (5 tests: the world raises the
close once, the deploy screen tears down without emitting `closed`, the deployment release still
closes normally, a healthy wait is untouched, and the shell returns to the menu).
D-NET-177 updated; D-NET-182 mints the remainder (our HOST sends no punt at all).

### 5.65 S2C `0x30` / `0x31` — the anti-cheat challenge pair, and the client reply builders (2026-07-26)

The two challenges a stock host streams continuously at every connected client. `nw_pp`'s
long-standing annotation (`0x30 -> reply C2S 0x20`) is CONFIRMED against the binary; the reply
BUILDERS are witnessed here for the first time. Both dispatch rows are gated `!is_authority`
(table `@ 0x82AE28`: row `0x30 @ 0x82B1A8`, row `0x31 @ 0x82B1B8`).

**S2C `0x30` -> C2S `0x20` (5 B).** Request `[u8 id][u16 challenge]` (§5.35). The reply is
`[u8 echoed id][u32 LE challenge ^ source]`. The `id` selects the source: `0xFF` asks for the
whole loaded weapon-slot table, anything else for that one `.adm` entry — and **`0xFF` with a
NONZERO challenge short-circuits to the literal 42** without touching state (`@ 0x42AFCC`).
Both real sources CRC retail's in-memory 1120-byte `.adm` image (the weapon-slot form
concatenates every loaded entry after overwriting its volatile fields with sentinels).
[orig: `NapiNPClientMsg_HandleChecksumRequest @ 0x431170` (reply tag 0x20 `@ 0x4311D7`) ->
`NetPacket_WriteEntityChecksum @ 0x42AFB0`; `AnimDef_ComputeChecksum @ 0x541EF0` returns 0 for an
uninitialized slot `@ 0x541F30`; `WeaponSlotDef_ComputeSanitizedChecksum @ 0x53F8F0` returns 0
with no active slot `@ 0x53FBCA`]

**S2C `0x31` -> C2S `0x21` (9 B).** Request `[u8 ammoIndex][u16 xorKey]` — the same 3-byte shape
as `0x30`, a different source. The reply is `[u8 echoed index][u32 LE crc][u32 LE echoed key]`,
where the crc is over the indexed **276-byte** ammo-definition record with six volatile dwords
temporarily zeroed. Retail's **out-of-range arm writes a ZERO crc dword — not `key ^ 0`**
(`@ 0x42B114`) — and still echoes the key (`@ 0x42B14D`).
[orig: `NapiNPClientMsg_0x031 @ 0x4311E0` (reply tag 0x21 `@ 0x431245`) ->
`NetPacket_WriteEntityCRCChecksum @ 0x42B020`]

**Correction to §5.17.** That section read the C2S `0x21` reply as an effective 5 B
(`[u8 player_index][u32 expected_crc]`) plus 4 trailing zero bytes dismissed as framing padding.
The builder settles it: the trailing dword is the **echoed challenge key** (`@ 0x42B14D`), zero
in that capture because the challenge carried a zero key. The host-side reading is unchanged —
`handle_anti_cheat_crc_check @ 0x502050` still never advances its cursor past byte 5.

**Reimpl (2026-07-26) — WE DO NOT ANSWER EITHER CHALLENGE.** `libs/npwire` gains
`LoadoutCrcRequest` + `decode_loadout_crc_request` (the catalog row for S 0x31 moves
`PrinterOnly -> Decoded`, and `nw_pp` gained `print_tag_31` so that promise holds), and
`JoinerConnection` decodes both — then deliberately sends nothing.

The first cut DID answer, using retail's own nothing-loaded returns (`source = (id == 0xFF &&
challenge != 0) ? 42 : 0` for `0x30`, a zero crc dword plus the echoed key for `0x31` — the `42`
is retail's literal `@ 0x42AFCC`, the zeros are what `@ 0x541F30` / `@ 0x53FBCA` return with
nothing loaded and what the out-of-range arm `@ 0x42B114` writes). A live retail co-op host
REJECTED both within one exchange: the placeholder `0x20` drew `DC=2 DPC=46 "PUNT ACRC"` and the
placeholder `0x21` drew `"PUNT WCRC"`, killing sessions that had been joining cleanly while the
client was silent. The asymmetry is the whole point — the host recomputes each checksum over its
own tables and disconnects on a DIFFERENCE (`handle_anti_cheat_crc_check @ 0x502050`), whereas a
challenge that is never answered costs nothing (the strike counter it would otherwise clear lives
on the `0x1C` path, §5.34).

So the values above are recorded as the SHAPE to reproduce once the sources exist, not as
something to send. Answering honestly requires building retail's two in-memory byte images —
the 1120-byte `.adm` image and the 276-byte ammo record — which OpenNova does not model
(D-NET-181). The silence is pinned by `npruntime_host_punt`
(`check_crc_challenges_are_not_answered`) so a future "helpful" reply cannot regress joining.

### 5.66 The multiplayer loadout SOURCE — the per-side/per-class profile page in `weapon.sav` (live retail↔retail capture, 2026-07-26)

§5.56/§5.57 settle what the host DOES with a C2S `0x2F`. This section settles where a stock client's
kit and class byte COME FROM in a session — the question PR #300's weapon-identity defect turned on.
The answer is: **not the mission.** In a net session retail submits the player profile's per-side,
per-class kit page, and the class byte it announces is the same byte that selected that page.

**The `.bms` kit is skipped in a session.** `[orig: Mission_LoadBMSFile @ 0x40F4E0]` branches on
`g_napi_np_ctx.is_in_session` (`@ 0x40f694`): in a session it `fseek`s past BOTH the loadout chunk
(`@ 0x40f6b2`, length `g_bmsLoadoutChunkLen`) and the item-availability chunk (`@ 0x40f6e1`,
`g_bmsAvailabilityChunkLen`). Only the non-session arm reads them, availability-filters the rows
(`@ 0x40f834`), substitutes `{"WPN_KNIFE","-1","-1","-1"}` when the filtered list is empty
(`@ 0x40f899`), and writes `restrictionData` (`@ 0x40f961`). `is_in_session` is set for a LISTEN HOST
as well as a joiner (§6.3, offset 0x058), so the skip covers both.

**One integer selects the class byte AND the kit.** `[orig: Game_StartMission @ 0x524360]`, gated on
`is_in_session` AND `is_mp_session_peer` (`@ 0x525767` / `@ 0x525773`), then at `@ 0x525793`:

1. the team latch `byte_A85B48` picks the side block — 1 or 3 → the BLUE block at `g_charSelClass`
   (`@ 0x2551130`), else the RED block (`@ 0x2559136`), each strided `0x1080C` by `g_curProfileSlot`;
2. `movzx eax, byte ptr [esi]` (`@ 0x5257c6`) reads that side's **class byte**;
3. `switch (class - 5)` selects one of five `0x800`-byte kit pages at `+6 / +0x806 / +0x1006 /
   +0x1806 / +0x2006` and `Buffer_CopyUntilDoubleNull` copies it over `restrictionData` (`@ 0x525813`);
4. `NetPacket_SendLoadoutSubmit(team = byte_A85B48, class = eax, restrictionData, 195)` (`@ 0x525836`).

A SECOND submit follows at `@ 0x525c2e` with the same kit but `weaponSlot = g_currentWeaponSlot`
instead of the literal 195 — the pair §5.56 records as the golden's `0xC3`/`0xD4`.

**The team re-latch re-picks the page.** `[orig: NapiNPClientMsg_TeamAssign @ 0x431910]` leg 4 runs
the identical selection for the NEW side (`@ 0x431a35`..`@ 0x431a9a`) and re-submits with that side's
class byte and slot **195 raw** (`@ 0x431a9e`) — see the S2C `0x50` dispatch row.

**Why a retail kit is (mostly) class-legal.** The submit builder applies NO `charfilter`/`teamfilter`
test — `[orig: NetPacket_SendLoadoutSubmit @ 0x42CDC0]` gates only on `AvatarDef_FindByName`
(`@ 0x42cf0b`). Prevention lives in the kit editor: `[orig: populate_weapon_slot_lists @ 0x560430]`
fills the PLAYER_INFO PRIMARY/SECONDARY/ACCESSORY lists only with rows passing
`(g_playerInfoClassMask & def[+116])` and `(g_playerInfoTeamMask & def[+112])` (`@ 0x5604ea`).
**It filters only those three selectable slots** — the knife/grenade tail of a page is not
class-filtered, so a page CAN legitimately carry an entry the host will drop (a class-6 page keeping
`WPN_GRENADEHE`, `charfilter medic,gunner,rifleman`, is the worked example). A drop is therefore not
by itself evidence of a defect.

**Defaults when `weapon.sav` is absent.** `[orig: PlayerProfile_InitDefaults @ 0x54bb40]` sets BOTH
sides' class byte to **8** (`@ 0x54bbe0` / `@ 0x54bbe3`), seeds the single-player page at `+65548`
with the literal `"WPN_M4AUTO"` (`@ 0x54bced`), and seeds each class page with ONE weapon name
(`@ 0x54bcf7`..`@ 0x54bde4`): blue classes 5..9 = `WPN_M4AUTO` / `WPN_SR25` / `WPN_M60` /
`WPN_M4AUTO` / `WPN_M16BURST`; red = `WPN_AK47AUTO` / `WPN_DRAGUNOV` / `WPN_PKM` / `WPN_AK47AUTO` /
`WPN_AK74AUTO`. `[orig: apply_session_settings_to_globals @ 0x5516ab]` then clamps each side's class
byte into `[5,9]` (`@ 0x5516d0`..`@ 0x5516ec`) and publishes them as `g_charClassTeam1` /
`g_charClassTeam2`.

**`weapon.sav` — the on-disk form.** Loaded by `[orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0]`
from `expansion\<g_ExpansionName>\weapon.sav` when an expansion is active, else `weapon.sav`
(`@ 0x54f68c`..`@ 0x54f6b7`) — i.e. the profile kit is **expansion-scoped**, like the ADM index
space it names into (D-NET-178).

| offset | size | field |
|---|---|---|
| `+0` | 16 | header: `u32 magic` `0x43425046` (`"FPBC"`), `u32 version` `0x31313230` (`"0211"`), `u32 flags`, `u32 extra`; both magics checked at `@ 0x54f6fd` |
| `+16` | `5 × 0x1080C` | five profile-slot records, read back to back (`@ 0x54f70a`, stride 67596) |

Record (`0x1080C` = 67596 B): BLUE side block `[0 .. 32773]`, RED side block `[32774 .. 65547]`
(stride `0x8006`, the same figure `@ 0x5516e4` walks), then the single-player kit page
`[65548 .. 67595]` (`byte_256113C`, consumed at `@ 0x5246a8` and `@ 0x5519c3` with the
`"WPN_M4AUTO"` literal fallback). Side block: `+0` `u8` class (5..9); `+1`,`+2` `u8` avatar bytes and
`+4` `u16` packed avatar id (written by `lookup_entity_slot_and_pack_entry @ 0x54bbea`); `+6` five
2048-byte kit pages, class `c` at `+6 + 2048*(c-5)`. The remaining `+10246 .. +32773` are **zero
across the retail corpus and UNWITNESSED** — we write zeros rather than carrying them through
(ADR 0003).

A kit page is a `Buffer_CopyUntilDoubleNull` blob `[orig: @ 0x562f30]`: NUL-separated ASCII ending in
a double NUL, consumed four tokens at a time as `(name, ammo_primary, ammo_secondary, flags)` with
the three numbers as decimal text, truncated to the low byte on the wire (`-1` → `0xFF`,
`@ 0x42cf7c` / `@ 0x42cfbc` / `@ 0x42cff7`). Page ORDER is data and reaches the wire verbatim; the
observed serializer writes `[KNIFE][PRIMARY][SECONDARY][ACCESSORY][GRENADES…]`.

**Live evidence** (`.scratch/golden/retail-coop-playerinfo-join.pcapng`, two stock v1.7.5.7 clients,
revx02, CP08.BMS; the joiner's PLAYER_INFO screen was captured beforehand). Session `s=32769` is the
retail exchange — the same capture also contains OpenNova dev-server sessions on `ASH_I5A.BMS`
whose `0x5A` is the empty-table request echo (all ammo `255`), which must not be mistaken for retail.

Joiner = Joint Ops (blue), **Rifleman**. Frame 2239 carries the `0x2F` PAIR, both
`team=1 class=8 weaponSlot=195`, entries `{1, 69, 3, 90, 83, 84, 85}` =
`{WPN_KNIFE, WPN_M14_AimPoint, WPN_colt45, WPN_SATCHEL_CHARGE, WPN_GRENADEFB, WPN_GRENADEHE,
WPN_GRENADESM}` — **the profile's blue class-8 page, not CP08.BMS's authored `WPN_SR25` sniper kit.**
Frame 2240 grants all seven plus the sub-variant `91` (`WPN_SATCHEL_DETONATOR`). The in-game armory
swap (frame 4437) re-submits the same 7-entry page shape as a SINGLE `0x2F`, primary `69` → `65`
(`WPN_M4_ACOG_AUTO`), `weaponSlot=195`.

The file was re-read after the session and had mutated exactly as the model predicts: both side class
bytes moved `8` → `9` when the host player selected Engineer, and the blue class-9 page read
`{WPN_KNIFE, WPN_M16BURST, WPN_colt45, WPN_AT4, WPN_GRENADEFB, WPN_GRENADESM, WPN_GRENADEHE_1}` —
the host's PLAYER_INFO screen field for field.

**Not settled by this capture** — recorded so a future session does not re-derive them:

- `weaponSlot` 195-vs-live-slot. Every primary involved (`WPN_M14_AimPoint`, `WPN_M4_ACOG_AUTO`) is
  category 3 rank 0, whose pool slot IS 195, so `g_currentWeaponSlot == 195` and the two candidates
  are indistinguishable here. The `195` → `212` pair in `retail-lan-host-join.pcapng` remains the
  only witness that the second submit varies; settling it needs a primary at a nonzero rank.
- The class-6 page prediction (its `WPN_GRENADEHE` should be dropped for `type_mask` 2) — untested.
- A nonzero S2C `0x66` restriction table; every capture to date carries count 0, so the availability
  gate (§5.56 gate c) and the `g_armoryWeaponAvailability` index-space question are still untested.
- Two concurrent JO processes share one `weapon.sav` and the last writer wins — the host instance
  clobbered the joiner's class-8 page here, so the file is NOT a record of what a second live
  instance submitted.
- `revx02` pool slots are not unique: `WPN_colt45`/`WPN_M9Beretta` both sit at category 2 rank 0, and
  `WPN_M4_ACOG_AUTO`/`WPN_M14_AimPoint` both at category 3 rank 0.

**C2S `0x06` byte 32, incidental but decisive.** All six retail fired-rounds in this capture carry
`extras = (0x03, 0x0c, 0x00)` — off32 `0x03`, off33 `0x0c`, off34 `0x00` — with `adm = 69` tracking
the equipped weapon. off32 is the `entity+352` byte D-WPN-8 records as unmodeled/zero on our
producer, now witnessed live with a nonzero retail value. **off34 is `0x00` on retail too**, so
emitting zero there is not a divergence.

### 5.67 The same-weapon first-person viewmodel bleed — a RETAIL defect (live retail↔retail A/B, 2026-07-26)

**This is the original engine misbehaving, not an OpenNova divergence.** Recorded here so a
future session recognises the symptom instead of re-deriving it from the wire. Ledger row
**D-NET-184**; agent-facing summary in `.agents/interop.md`.

**Symptom.** On a LISTEN HOST, when another player holding the SAME weapon fires, the host's own
FIRST-PERSON weapon visibly reacts. The shooter's third-person avatar does not. The joiner is
never affected in the reverse direction. It stops the instant either side switches weapon, and it
happens when firing into the air — nothing about it involves hits.

**Live A/B, two stock v1.7.5.7 clients** (`.scratch/golden/retail-retail-same-weapon-viewmodel-ab.pcapng`,
one retail session, no OpenNova traffic; both players on `WPN_colt45` (ADM 3), which is on every
blue profile page): (1) joiner fires, host watches its own viewmodel — REACTS; (2) host switches
weapon — STOPS immediately; (3) reverse direction — never affected.

**The shared object is per-WEAPON-TYPE and lives inside ONE process.** The joiner's process is not
involved: it sends a C2S `0x06` saying "I fired", and the host does the rest to itself. A script
emitting valid `0x06` reproduces it identically.

- `weapon.def` parses once into `AdmDefs[]`, one entry per weapon TYPE, and each entry's anim
  object at `+372` is allocated at parse time [orig: `Anim_InitActions @0x541FE4`/`@0x541FEF`].
- A slot's `Def` is that shared ADM pointer [orig: `WeaponSlot_InitFromDef @0x53EEA1` <-
  `AdmDef_GetEntryByIndex`]. Two entities on one weapon therefore share one `Def` and one `+372`.
- The FP viewmodel is posed straight out of it:
  `if (weaponDef->field_174) Entity_BuildBoneWorldMatrices(.., weaponDef->field_174, ..)`
  [orig: `Player_RenderFirstPersonViewModel @0x4DEF75`/`@0x4DF028`]. The only other consumer is the
  `fpModel` branch of `Entity_GetCameraTransform @0x4B8D43` — third-person entities are posed by a
  DIFFERENT function, `Entity_BuildBoneTransformMatrices @0x4B8CCD`, which is why no avatar moves.

**Trigger chain.** C2S `0x06` -> the host arms the SHOOTER's slot
(`entity+280 = &playerSlot[100*roundSlotIndex + 464]`, `currentAction = 2`) and invokes the ADM
FIRE action [orig: `Server_ClientFiredRound @0x50C28B`/`@0x50C30D`] -> the host ticks EVERY pool-0
entity's slot each frame with no locality gate [orig: `WeaponAction_ProcessAllEntities @0x5426AD`]
-> FIRE sets `nextAction = 3` RECOIL [orig: `@0x542C9E`], the transition zeroes it
[orig: `@0x5413E4`] and it falls to action 0 IDLE [orig: `@0x5413D0`] -> `WeaponAction_Idle`
re-seeds the SHARED object with `AnimMap_PlayAnimBySlot(weaponDefPtr->field_174, 241)` and **no
owner test** [orig: `@0x542945`/`@0x54294D`/`@0x542955`; `WeaponAction_EmptyIdle` slot 242
`@0x542A43`/`@0x542A53`].

**Why it is an oversight, not a design.** Every other play onto `Def+372` IS gated on
`ownerEntity == g_local_player_entity` [orig: `ActionSlot_ExecuteActionWithEffect @0x541893`,
`@0x54195A`, `@0x5419B8`], `ActionSlot_ExecuteActionTick` routes on the same test
[orig: `@0x541A83`], and the host's own shots are explicitly excluded from the remote arm
[orig: `@0x50C18D`]. The engine already separates "simulate for everyone" from "present for the
owner"; the two idle handlers call `AnimMap_PlayAnimBySlot` directly and bypass that split.

**Why each property follows.** First-person only — that object is not a 3P pose source.
Same-weapon only — collision needs an identical `Def` pointer. Host only — 0 of the 115 CLIENT
dispatch rows handle tag `0x06` (only server row 120 does), so a joiner never processes another
player's fired round; it learns of shots through the round-event fan in S2C `0x0A`, which touches
no FSM. Fire only — fire is the only client message that invokes an ADM action on a remote slot;
the reload path is pure ammo arithmetic and never touches `nextAction` or `phase`
[orig: `NapiNPServerMsg_HandleReloadRequest @0x514DF0` -> `WeaponSlot_ReloadAmmo @0x541720`].
One reaction per shot — the idle handler early-outs while `phase == Done`, so only a RE-ENTRY
replays, and `FIRE -> RECOIL -> IDLE` is exactly one round trip.

**The fix, if it is ever wanted** (e.g. for a modded binary): add the sibling guard at the two
sites — keep simulating every entity, only stop PRESENTING for non-owners. Nothing is lost,
because a remote player's weapon is not drawn from that object. The structural fix is to hang the
playhead off the entity rather than the `WeaponDef`, which is what OpenNova does and why we are
immune.

**Two of our own wire bugs were found and fixed while chasing this, and NEITHER was the cause** —
do not re-open them as suspects. (a) The C2S `0x06` `hit_part` word was shipped as a bare sequence
where retail packs `(roster slot << 9) | (seq & 0x1FF)`; (b) off32 (`entity+0x160`, the ammo-def
index) was shipped as 0 where retail sends the equipped weapon's index. Both are real divergences,
both are fixed under D-WPN-8, and the symptom survived each.

## 6. Struct reference

All structs typed in the IDB during the 2026-04-26 per-class typing pass (Stage 5 of the
decomp-quality plan) and the follow-on refinements. Witness rule: every named field has ≥2
independent decompile witnesses unless flagged. One Kong artifact to know: the
`CNapiSession_*` name prefix is **heterogeneous** (it covers both particle/effect code and
message-queue code) — it is not a real class.

### 6.1 `NapiNPMsgInfo` / `NapiNPOpcodeInfo` (16 B each)

| Struct | Layout | Notes |
|---|---|---|
| `NapiNPMsgInfo` | `{u32 msg_id, u32 magic, handler, handler2}` | sentinel = `magic==0`; `handler2` always 0 in observed entries |
| `NapiNPOpcodeInfo` | `{u32 index, u32 opcode, u32 magic, handler}` | magic always `0x7C08C6` (= addr of `font_name`; possible string pointer, see §5.0); sentinel `index=0xFFFFFFFF` |

### 6.2 `CNapiNetwork_*` / `CNapiServer*` method family (42 methods; receiver = `NapiNPServerCtx`)

The NAPI "CNapiNetwork" class methods (range 0x4a8040-0x4ca4a0) all operate on the game
singleton `g_napi_np_ctx` (§6.3). **There is no separate `CNapiNetwork` struct.** An undersized
4432-B duplicate type by that name existed in the IDB and was **deleted 2026-06-27** (grill below);
the single canonical receiver is `NapiNPServerCtx` (§6.3). All `__thiscall` methods are now typed
`(NapiNPServerCtx *this)`; the callbacks are `__cdecl` with the ctx as the first arg.

Roster (retail `Jointops.exe`):

- **Lifecycle:** `_Init @0x4ca4a0` (list heads + manager pointers + settings; ping 3000/2/10, §6.6),
  `_ClearState @0x4c8690` (zeroes the whole **0x1470 = 5232 B** object + re-inits
  `game_settings`/`net_config`; was Kong `CNapiServerInfo_Init` — a misnomer, it resets the ctx not a
  sub-struct), `_Shutdown @0x4ca440`.
- **Transport:** `_SetTransportMode @0x4c8750` (writes `socket_state` +0x54), `_OpenTransportSocket
  @0x4c6a40` (opens a UDP socket only for modes 2/3/4), `_TearDownSocket @0x4c4c90`, `_SendUDPPacket
  @0x4c4d30`, `_GetLocalAddress @0x4c4f60` (static `__stdcall`, no `this`).
- **Pump** (thin wrappers over `NapiNP*_Pump`; flag bitmask decoded in-IDB): `_PumpManagerReceive
  @0x4c4d10` (mgr flag 4 = receive pass), `_PumpServerProtocolRecv @0x4c4ee0` (flags 25),
  `_PumpServerProtocolSend @0x4c4f00` (737), `_PumpClientProtocolRecv @0x4c4fe0` (26),
  `_PumpClientProtocolSend @0x4c5000` (738), `_PumpTransportAndProtocol @0x4c6e00`, `_PumpAndCheckState
  @0x4c6e80`, `_DrainProtocolTimers @0x4c6de0`, `_DrainPendingDataTransfers @0x4c6e50`. Kong named the
  four protocol pumps `PumpProtocolType<flags>`; renamed recv/send × server/client per the
  `[orig: NapiNPProtocol_Pump @ 0x62a650]` decode (bit 0x8 = `PumpRecvQueues`, 0x3F0 = per-conn) and
  `[orig: CNapiNPConnection_PumpFlags @ 0x629780]` (0x20 = enumerator-send, 0x40 = state machine; low
  bit 0x1 selects server-role connections, 0x2 client-role), corroborated by the caller split
  (`Server_*` vs `Client_*`/`NetClient_*`).
- **Session/state:** `_IsSessionActive @0x4c6f00`, `_GetSessionUptime @0x4c6ed0`,
  `_UpdateSessionTimestamps @0x4c6f20`, `_RandomizeTimeout @0x4c4d80` (writes `randomized_timeout_ms`
  +0x1194, value 1000-9999 ms — **retail addr; the `0x4a6d50` cited in `libs/napi` & `libs/novaworld`
  is the jodemo image, a different binary**), `_UpdateDedicatedServerFlag @0x4c6d50`, `_FindPlayerByName
  @0x4c69e0`, `_ParseServerVarList @0x4c4310`, `_SetNetLogFile @0x4c69a0`, `_QueueReliableMessage
  @0x4c4fa0`, `_GetDisconnectReasonString @0x4c7000` (fills `disconnect_reason_buf` +0x1270),
  `_DisconnectActiveConnection @0x4c9140` (builds a `NapiNPDisconnectEvent` on `napi_conn` then
  `[orig: CNapiNPConnection_RequestDisconnect @ 0x61e0f0]`; was Kong `SendPunkBusterChat` — a misnomer,
  there is no chat path, only a disconnect-with-reason).
- **Callbacks (`__cdecl`, ctx as first arg):** `_ValidateJoinRequest @0x4c61b0`, `_OnConnectedToServer
  @0x4c62e0` (writes `active_connection_id` +0x1190), `_OnDisconnectedFromServer @0x4c63d0` (writes
  `disconnect_event_buf`), `_CheckPlayerTimeouts @0x4c8ad0`. `_OnSessionDiscovered @0x4c8470` and
  `_OnSessionRemoved @0x4c68c0` take a session-list head (not the ctx); `_QueueEventEntry @0x4c6890`
  takes a player object.
- **Server subclass:** `CNapiServer_ProcessPendingPlayerSpawns @0x4c8dc0`,
  `CNapiServer_DisconnectPendingSpawnBans @0x4c9290`, `CNapiServer_OnPlayerDisconnected @0x4c94d0`
  (`__cdecl`), `CNapiServerConfig_BuildFlags @0x4c4dc0`. `CNapiServerInfo_SerializeToSession @0x4c3650`
  and `CNapiServerInfo_ClearAllStrings @0x4cad10` genuinely operate on a separate ~520-B server-info
  struct (fields BT/VN/BN/DB/.../PBC/NWUVERSION), not the ctx — names retained.
- **Not a network method:** `0x4a8040` (Kong `CNapiNetwork_GetConnectionParams`) reads display
  width/height/AA-level from the video-config object `off_840960`; sole caller `Game_InitSubsystems`
  right after `Renderer_SetDisplayModeWithFallback`. Renamed `VideoConfig_GetResolution` and removed
  from the family.

### 6.3 `NapiNPServerCtx` — `g_napi_np_ctx @ 0xB5CBC8` (5232 B = 0x1470, 473 xrefs)

`NapiNPServerCtx` is the **single canonical type** for this object and the receiver of the entire
`CNapiNetwork_*`/`CNapiServer*` family (§6.2). The previously-documented separate `CNapiNetwork`
struct was an undersized (4432 B) duplicate of the same layout and was deleted from the IDB
2026-06-27. The **true size is 0x1470 = 5232 B**, witnessed by `[orig: CNapiNetwork_ClearState @
0x4c8690]` doing `memset(&g_napi_np_ctx, 0, 0x1470)` and by `[orig: CNapiServer_OnPlayerDisconnected
@ 0x4c94d0]` writing at +0x11A8; the struct was grown to 5232 and the four interior addresses that
IDA had auto-named as standalone globals (0xB5DD70/74, 0xB5DDB4, 0xB5DDB8) were folded back in as
ctx fields. Applied layout:

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0x000 | `list_heads[5]` | 80 | `+0x04` = enumerated session-list head (NovaWorld + LAN); `+0x24` = player connection-list head (PCID dup-check walks it in `[orig: Server_ValidatePlayerJoinRequest @ 0x512100]`) |
| 0x050 | `transport_mode` | 4 | 1=NovaWorld, 2=LAN (UI enumeration branches ==1 → SetTransportMode(4), ==2 → (2)); NovaWorld-only AppId/JoinTicket gates check ==1 |
| 0x054 | `socket_state` | 4 | |
| 0x058 | `is_in_session` | 4 | non-zero whenever an MP session is in progress; gates `[orig: Server_PumpNetworkTransport @ 0x4FD960]`, 14 branches of `[orig: Server_TickUpdate @ 0x51D7E0]`, 11 of `[orig: Game_StartMission @ 0x524360]`, the whole body of `[orig: NetClient_FlushAndSync @ 0x424710]` |
| 0x05C | `connection_mode` | 4 | host/client mode written by `[orig: CGameSession_SetConnectionMode @ 0x4c49f0]` (0=none, 1=host, 2=client, 3=host+client); single player uses 3 (§5.0). Was `field_5C` |
| 0x060 | `is_authority` | 4 | non-zero on host/server (preserved Kong name); set by `SetConnectionMode` = is_host bit of `connection_mode` (§5.0) |
| 0x064 | `is_mp_session_peer` | 4 | renamed 2026-04-26 from `is_dedicated_server` (see below); set by `SetConnectionMode` = is_client bit of `connection_mode` (mode 3 → 1, §5.0) |
| 0x068 | pad | 3372 | NAPI internals |
| 0xD94 | `disconnect_event_buf` | 184 | |
| 0xE4C-0xE54 | `field_E4C/E50/E54` | 12 | |
| 0xE58 | `np_manager` | 4 | `NapiNPManager *` |
| 0xE5C | `np_protocol` | 4 | `NapiNPProtocol *` (was Kong `player_list_owner`); `[orig: NapiNPServer_SendFiltered @ 0x4C87E0]` derefs its connection list at +0xEBC |
| 0xE60 | `field_E60` | 4 | protocol pointer cached for the client frame path (`[orig: Client_ProcessNetworkFrame @ 0x42C180]`); never proven to differ from `np_protocol` |
| 0xE64 | `ping_manager` | 4 | `NapiPingManager *` (global alias `0xB5DA2C`) |
| 0xE68 | `game_settings` | 216 | inline `NapiGameSettings` (§6.4); side passwords at absolute 0xEA8/0xEC8 drive join-reject codes 19/20 |
| 0xF40 | `server_info_buf` | 72 | `+0xF3C` region note: a PunkBuster handle/flag is read at 0xF3C (single witness, `Server_TickUpdate` → `PBServer_Shutdown`) |
| 0xF88 | `net_config` | 520 | |
| 0x1190 | `active_connection_id` | 4 | set on connect from the connection's id (`OnConnectedToServer`), cleared on disconnect. Was `field_1190` |
| 0x1194 | `randomized_timeout_ms` | 4 | 1000-9999 ms, written by `[orig: CNapiNetwork_RandomizeTimeout @ 0x4c4d80]`. Was pad |
| 0x1198 | `send_mask` | 4 | bitmask used by `NapiNPServer_SendFiltered` (preserved) |
| 0x119C | `send_target_player` | 4 | preserved |
| 0x11A0 | `send_target_slot` | 4 | preserved (was `send_target_state`) |
| 0x11A4 | `send_filter_416` | 4 | preserved |
| 0x11A8 | `field_11A8` | 4 | cleared (=0) on player disconnect by `CNapiServer_OnPlayerDisconnected`; semantics unconfirmed |
| 0x11AC | `field_11AC` / `field_11EC` / `field_11F0` | 196 | interior fields (formerly auto-named globals); not yet individually witnessed |
| 0x1270 | `disconnect_reason_buf` | 512 | localized disconnect/error string built by `[orig: CNapiNetwork_GetDisconnectReasonString @ 0x4c7000]`; ends at 0x1470 |

`is_mp_session_peer` rename rationale (user-approved, applied to the IDB): the literal
"is dedicated server" reading is contradicted by three witnesses —
`[orig: CNapiGameSession_BuildHostVarLists @ 0x4D0B50]` publishes the lobby key `Dedicated=0`
when the flag is set (inverted); `[orig: Chat_SendTeamMessage @ 0x49A900]` takes the
client-style reliable-uplink branch when set and the host broadcast branch on `is_authority`
(a real dedi host would never take the first); `[orig: Game_StartMission @ 0x524360]` gates
both polarities in ways only consistent with "peer in an MP session".
`[orig: Game_ParseCommandLineAndInit @ 0x4A7310]` never sets it from `/SERVEONLY`.

Deliberately not applied from the maximal field map: inline `gs_*` password names at the
singleton level (the agent's inline arithmetic was off by one 32-byte slot; the authoritative
offsets are the `NapiGameSettings` ones in §6.4 — side A at settings+0x40 = absolute 0xEA8,
side B at +0x60 = 0xEC8, both directly witnessed in `Server_ValidatePlayerJoinRequest`), a
separate `active_protocol` name for 0xE60, and `pb_server_handle` at 0xF3C (single witness).
Admin SET-command password buffers live in a game-state struct reached through `np_protocol`,
not inline in the singleton.

### 6.4 `NapiGameSettings` (216 B; inline at `CNapiNetwork+3688` / `g_napi_np_ctx+0xE68`)

10 of 11 fields named, all ≥2 witnesses. Strongest source:
`[orig: ServerConfig_ApplyHostSetting @ 0x4a6000]` maps host-config keys to exactly the
globals that `[orig: CNapiGameSession_BuildAndCreateSession @ 0x5694d0]` copies into this
struct. Other witnesses: `[orig: SinglePlayer_StartMission @ 0x561af0]` (default init),
`[orig: Server_ValidatePlayerJoinRequest @ 0x512100]` + `[orig: NapiNPServerMsg_0x001 @
0x512ed0]` (validation sink), `[orig: Game_SaveConfig @ 0x54c490]` (game.cfg writer),
`[orig: UI_PopulateHostSettingsFromConfig @ 0x555fe0]` (host-screen widget labels),
`[orig: CNapiGameSession_BuildHostVarLists @ 0x4d0b50]` (lobby publish),
`[orig: CAdminServer_HandleSetCommand @ 0x405a60]` (admin SET).

| Offset | Field | Type | Config key / semantics |
|---|---|---|---|
| 0x00 | `server_name` | char[32] | lobby-visible `"ServerName"`; saved as `game_name`; UI widget `GAME_NAME` |
| 0x20 | `server_password` | char[32] | `MPHostGamePassword` / admin SET `ServerPassword`; widget `SERVER_PASSWORD` |
| 0x40 | `side_a_password` | char[32] | `MPHostSidePasswordA`; widget `BLUE_PW`; mismatch → join-reject code 19 |
| 0x60 | `side_b_password` | char[32] | `MPHostSidePasswordB`; widget `RED_PW`; mismatch → join-reject code 20 |
| 0x80 | `internet_address` | char[64] | connect-target hostname/IP, default `"0.0.0.0"`; resolved via `Napi_ResolveAddress` in transport mode 3 |
| 0xC0 | `max_players` | u32 | `MaxPlayers`, clamped 1..65 |
| 0xC4 | `use_lineup_queue` | u32 | `UseLineUpQueue` |
| 0xC8 | `lineup_queue_size` | u32 | `LineUpQueueSize` |
| 0xCC | `game_type` | u32 | `mp_gametype` enum (the mission entry's gametype dword); discrete values not enumerated |
| 0xD0 | `mp_attributes` | u32 | `mpattrib` bitmask — observed bits: 0x001 NoTracers, 0x004 TeamChoose, 0x008 FFWarning-suppress, 0x200 NoFriendlyFire, 0x400 NoFriendlyTag, 0x8000 ClaymorePref; `[orig: CNapiServerConfig_BuildFlags @ 0x4c4dc0]` re-derives a public flag word |
| 0xD4 | `_pad_0xD4` | 4 | actually the `sv_punkbuster` server flag carried into the session blob (single witness pair; rename to `sv_punkbuster_enabled` once a second witness lands; see `[orig: Config_SetPunkBusterServerEnabled @ 0x4d94c0]`) |

### 6.5 `NapiNPProtocol` (4064 B, 94 named members; reached via `g_napi_np_ctx.np_protocol`)

Full layout after the `_pad_0x500` refinement (2120 unknown bytes → 100% named):

| Offset | Field | Type/size | Notes |
|---|---|---|---|
| 0x000 | `manager` | ptr | `NapiNPManager *` |
| 0x004 | `link` | 16 | `NapiListNode` |
| 0x014 | `instance_id` | u32 | |
| 0x018 | `company` | char[64] | |
| 0x058 | `machine_name` | char[64] | |
| 0x098 | `build_date` | char[64] | |
| 0x0D8 | `unk_0xD8` | u32 | |
| 0x0DC | `game_name` | char[64] | |
| 0x11C | `version_block` | 16 | `NapiNPVarBlock` |
| 0x12C | `version_string` | char[64] | |
| 0x16C | `max_players_string` | char[64] | |
| 0x1AC | `build_string` | char[64] | |
| 0x1EC | `msginfo_client_table` | ptr | flat S2C table (§4) |
| 0x1F0 | `msginfo_server_table` | ptr | flat C2S table |
| 0x1F4 | `msginfo_flags0` / `disable_processing` / `msginfo_flags2` / `msginfo_flags3` | 4×u8 | |
| 0x1F8 | `callback_ctx` | ptr | |
| 0x1FC | pad | 16 | |
| 0x20C | `msginfo_high_client_index` / `_len` | ptr+u32 | high-bit dispatch index |
| 0x214 | `msginfo_client_index` / `_len` | ptr+u32 | inverted msg_id index |
| 0x21C | `msginfo_high_server_index` / `_len` | ptr+u32 | |
| 0x224 | `msginfo_server_index` / `_len` | ptr+u32 | |
| 0x22C | `pool_client` | 32 | actually a `NapiFifo`, not `NapiNPBufferPool` (Kong mislabel) |
| 0x24C | `pool_server` | 32 | same |
| 0x26C | `cb_client_msg` / `_unhandled` / `_error` / `cb_client_0..3` | 7 ptrs | client-direction callbacks |
| 0x288 | `nstmout_path` | char[64] | actually the **session/server name** (lobby-visible; "HOST STARTED \"%s\"" log) — rename candidate `session_name` |
| 0x2C8 | pad | 8 | two server callbacks set in CreateSession: `CNapiServer_OnPlayerDisconnected`, `CNapiClient_OnDisconnected` |
| 0x2D0 | `cb_server_0..2`, `cb_server_msg` / `_unhandled` / `_error`, `cb_server_3..8` | 12 ptrs | server-direction callbacks |
| 0x300 | `log_buffer` | char[512] | misnamed — compared against the client `PW` TLV in HandleClientJoin; rename candidate `server_password[512]` |
| 0x500 | `server_flags` | u32 | `P1` TLV (server config flag word) |
| 0x504 | `build_flags` | u32 | `P2` TLV (`CNapiServerConfig_BuildFlags`) |
| 0x508-0x51C | `p3_count`..`p8_count` | 6×u32 | `P3`..`P8` TLVs; always zeroed in retail JO (reserved slots) |
| 0x520 | `np_count` | u32 | `NP` TLV (zeroed) |
| 0x524 | `max_players` | u32 | `MP` TLV; clamped 1..251 |
| 0x528 | `npw_count` | u32 | `NPW` TLV (zeroed) |
| 0x52C | `gen_session_seed_flag` | u32 | 1 → regenerate `session_seed_id` at host start |
| 0x530 | `session_seed_id` | u32 | `(GetTickCount + rand) % 900000 + 100000` |
| 0x534 | `host_key` | u32 | `HK` TLV; `NapiNP_GenerateSessionKey()` at StartServer; validated against the client's HK on join (mismatch → result 3) |
| 0x538 | `host_running` | u32 | 1 once StartServer succeeds; HandleClientHello rejects when 0 |
| 0x53C | `host_start_tick` | u32 | GetTickCount at StartServer; uptime base |
| 0x540 | `host_stop_tick` | u32 | GetTickCount at StopServer |
| 0x544 | `host_run_duration_ms` | u32 | stop − start, frozen post-stop |
| 0x548 | `server_user_string1` | char[512] | `SUS1` TLV (populated from a global server-info string) |
| 0x748 | `server_user_string2` | char[512] | `SUS2` TLV (from CGameSession +4056; likely server URL/NF) |
| 0x948 | `server_user_string3` | char[512] | `SUS3` TLV (cleared in retail) |
| 0xB48 | `server_user_string4` | char[512] | `SUS4` TLV (cleared in retail) |
| 0xD48-0xD4C | `unk_0xD48/0xD4C` | 2×u32 | gates a write-flag block |
| 0xD50 | `unk_0xD50` + pad[63] | 64 | NUL-terminated string, emitted as TLV — likely `country_code[64]` (`CN`) |
| 0xD90 | `unk_0xD90` + pad[63] | 64 | likely `timezone[64]` (`TZB`) or `language[64]` (`LNG`) |
| 0xDD0 | `unk_0xDD0` | u32 | TZB DWORD payload? |
| 0xDD4 | `unk_0xDD4` | 112 | actually `opcode_msg_count[14]` + `opcode_msg_bytes[14]` per-opcode stats (indexed by `NapiNPOpcodeInfo.index <= 0xD`; zeroed at Create and StartServer) |
| 0xE44 | `cs_dir1` | 60 | `NapiCSConfig` |
| 0xE80 | `cs_dir0` | 60 | `NapiCSConfig` |
| 0xEBC | `connection_list` | 16 | `NapiListHead` — the list `SendFiltered`/timeouts walk |
| 0xECC | pad | 16 | |
| 0xEDC | `log_netflow` | 80 | `NapiLog` |
| 0xF2C | `unk_0xF2C` | u32 | |
| 0xF30 | `log_condump` | 80 | `NapiLog` |
| 0xF80 | `log_inout` | 80 | `NapiLog` |
| 0xFD0 | `unk_0xFD0` | u32 | connection-lookup cache: packed `{addr,port}[N]` table ptr |
| 0xFD4 | `unk_0xFD4` | u32 | parallel `NapiNPConnection*[N]` ptr |
| 0xFD8 | `unk_0xFD8` | u32 | cache entry count |
| 0xFDC | `unk_0xFDC` | u32 | unknown (capacity / dirty flag?) |

Strongest witnesses for the 0x500 region: `[orig: NapiNPProtocol_SendServerInfoPacket @
0x6204b0]` reads each DWORD in order and emits the matching TLV tag;
`[orig: CNapiGameSession_CreateSession @ 0x4c97c0]` performs the symmetric writes with
explicit 512-byte copies into the SUS buffers. Host-state DWORDs confirmed by StartServer
(`NapiNPProtocol_StartServer`), `StopServer @ 0x62a820`, `NapiNPProtocol_Create @ 0x625a10`,
`NapiNPTimer_GenerateRandomId @ 0x61e533`, and HandleClientJoin's HK validation.

#### `NapiCSConfig` defaults and direction mirroring

`[orig: CNapiGameSession_InitNPConnection @ 0x4D3BE0]` initializes both protocol CS templates
(`proto+0xE44` and `proto+0xE80`) to the same 15-dword default block. `[orig:
CNapiNPConnection_Create @ 0x62ACB0]` copies those protocol templates into each connection with
direction-dependent mirroring:

| Connection type | `conn+0x17C` | `conn+0x1B8` |
|---|---|---|
| server-side connection (`type == 1`) | `proto+0xE44` | `proto+0xE80` |
| client-side connection (`type == 2`) | `proto+0xE80` | `proto+0xE44` |

Field defaults:

| Index | Working field name | Default |
|---|---|---:|
| 0 | `timeout_ms` | 240000 |
| 1 | `recv_max_per_tick` | 4 |
| 2 | `send_interval_ms` | 0 |
| 3 | `send_holdoff_ticks` | 0 |
| 4 | `idle_send_interval_ms` | 60000 |
| 5 | `active_send_interval_ms` | 1000 |
| 6 | `packet_queue_interval_ms` | `0xffffffff` |
| 7 | `allow_dir0_update` | 0 |
| 8 | `static_msg_payload_max` | 2048 |
| 9 | `static_msg_count` | 128 |
| 10 | `packet_queue_max` | 100 |
| 11 | `msg_out_max` | 500 |
| 12 | `msg_out_overflow_log` | 1 |
| 13 | `max_packet_bytes` | 1300 |
| 14 | `max_packets_per_tick` | `0xffffffff` |

The `max_packet_bytes` default is the clamped MTU value `0x514` (min 100, max `0x10000`).
`[orig: NapiNPServer_GetSendHoldoffTicks @ 0x4C4AB0]` computes field 3 as one of
`1/3/4/6/12` depending on transport/LAN mode; the observed retail/OpenNova loopback update used
`12`.

### 6.6 `NapiPingManager` (declared 288 B; **real allocation 116 B**)

`[orig: NapiPingManager_Create @ 0x6303D0]` allocates 116 (0x74) bytes; every witnessed access
is within `0x00..0x73`. The trailing 172 bytes of the declared struct are dead (kept only for
size compatibility). Global instance via `g_napi_np_ctx.ping_manager` (`0xB5DA2C`).
Initializer `[orig: NapiConnection_Init @ 0x6302F0]` (misnamed — only called from Create;
rename candidate `NapiPingManager_Init`).

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0x00 | `mem_mgr_handle` | int | allocator handle |
| 0x04 | `state` | int | −1 stopped, 0 init, 1 paused, 2 running, 3 idle |
| 0x08 | `start_tick_ms` | u32 | |
| 0x0C | `last_active_tick_ms` | u32 | |
| 0x10 | `elapsed_ms` | u32 | |
| 0x14 | `periodicity_ms` | int | **misnamed** — actually the response timeout before retry; default 3000 |
| 0x18 | `field_18` | int | actually `max_retries`; default 2 |
| 0x1C | `max_attempts` | int | **misnamed** — actually the per-entry send throttle in ms; default 1, set to 10 by `CNapiNetwork_Init` |
| 0x20 | `socket_initialized` | int | 1 if `Network_CreateRawSocket` succeeded |
| 0x24 | `socket` | SOCKET | raw UDP socket |
| 0x28 | `last_pump_tick_ms` | u32 | 100 ms pump throttle |
| 0x2C | `last_send_tick_ms` | u32 | backdated by the send throttle at init |
| 0x30 | `thread_running` / `thread_stop_request` + pad | 4 | NapiThread block start |
| 0x34 | `thread_proc` | fn ptr | = `CNapiNPConnection_NetworkThreadProc` |
| 0x38 | `thread_param` | ptr | = this |
| 0x3C-0x48 | `field_3C..48` | 4×u32 | zeroed by `NapiThread_Reset`; no read sites |
| 0x4C | `active_count` | int | entries with state>0; gates thread launch/continue and the idle transition |
| 0x50 | `state3_count` | int | entries with state==3; gates the recvfrom loop |
| 0x54 | `anchor_self` | ptr | back-pointer; entries store `&mgr->anchor_self` as owner |
| 0x58 | `entry_head` | ptr | first `CNapiPingEntry` (also walked by `[orig: Server_SendPingMetricsToGate @ 0x511BF0]`) |
| 0x5C | `entry_tail` | ptr | (single witness; structurally paired) |
| 0x60 | `entry_count` | int | guards metrics emit |
| 0x64 | `callback_ctx` | ptr | `CNapiGateManager *` in practice |
| 0x68 | `callback_event` | fn ptr | fires on entry start AND done; set to `sub_63BC60` |
| 0x6C | `add_jitter_flag` | u8 | fudges RTT by `rand()%15` in `[orig: CNapiNPConnection_HandlePingResponse @ 0x62FC20]` (single witness) |
| 0x70 | `sleep_ms` | int | thread loop sleep; default 5, clamped to 1000 |
| 0x74 | dead pad | 172 | beyond the real allocation |

Per-entry tick logic: `[orig: CNapiPingEntry_ProcessTick @ 0x62F900]` retries after the
response timeout, gives up after `max_retries`, and throttles sends to one per
`send_throttle_ms`. Stale `sub_` names identified: `CNapiNPConnection_Start` = `NapiPingManager_Start`,
`CNapiNPConnection_OnTick` = `NapiPingManager_Pump` (100 ms throttle), `sub_62FE10` =
`NapiPingManager_DestroyEntries`.

### 6.7 `CNapiGateManager` (304 B; methods 0x4ce6b0-0x637b30; 14/14 typed)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | `gate_type` | int | set in InitDefaults from arg |
| 4 | `gate_state` | int | pre-connect cleanup state {−9, −8, −2, −1, 1, 2} |
| 8 | `hostname` | char[24] | default `"gs.novaworld.net"` |
| 32 | `conn_state` | int | state machine [−8..3] in SetState |
| 36 | `state2_enter_tick` | int | GetTickCount at state→2 |
| 40 | `state2_exit_tick` | int | GetTickCount at state 2→other |
| 44 | `state2_duration_ms` | int | |
| 48 | `field_30` | int | semantics unclear (response state / buffer metadata?) |
| 52 | `response_buffer` | ptr | freed when state→0 |
| 56-68 | `field_38/3C/40/44` | 4×int | semantics unclear |
| 72 | `port` | int | default **7597** |
| 76 | `protocol` | char[64] | default **`"jop:cus2"`** (retail JO gate-probe tag; jodemo uses `jopd:cus4`) |
| 140 | pad | 164 | embedded NapiThread + NapiMutex (type when those are declared) |

Size confirmed by the constructor's 0x130 memset. The struct literally hard-codes the
retail gate-probe identity.

### 6.8 `ItemDef` health fields (net-spawn relevant)

> The full 2780-byte `ItemDef` layout, the `type`/`attrib`/`attrib2` enums, and
> the complete `ItemDef → GamePlayerEntity` copy table now live in
> [`../world/itemdef-re.md`](../world/itemdef-re.md) (D-ITEMDEF-n). The two
> health fields below are the net-spawn-relevant slice.

The 2780-byte `ItemDef` got two fields lifted out of `pad_17C` during the spawn
investigation:

| Offset | Field | Flows to |
|---|---|---|
| 0x17C | `healthMax` (i16) | `entity[+286]` on creation |
| 0x17E | `armorMax` (i16) | `entity[+288]` |

Witness chain: `[orig: Entity_InitFromItemDef @ 0x49e550]` copies both on entity creation;
`[orig: Player_BuildTag0CInputBody @ 0x42a550]` refuses to serialize player input while
`entity[+286] == 0`; `[orig: Entity_KillByNetId @ 0x43dc10]` and
`[orig: AI_CheckVehicleStuckState @ 0x465480]` clear it on death/stuck;
`[orig: Entity_CalcAverageGroundHeight @ 0x45733f]` uses `<= 0` as a skip-dead guard. The AI
class flag relevant to §5.6 is `ItemDef[+84] & 0x100000`. Open ItemDef follow-ups: `+0x138` →
`entity[+456]` (post-physics handler / model ptr?), `+0x148` init-callback fn ptr (with
recursion guard vs `Entity_InitFromItemDef` itself), `+0x158` → `entity[+452]`, 28 unknown
bytes after `armorMax`.

### 6.9 `CAdminServer` — remote admin/RCON console + the authoritative server-state field map

`CAdminServer_*` (30 methods, `0x402bf0`–`0x406f50`) is the original engine's **remote
admin/RCON server**: a TCP listener (`[orig: CAdminServer_Listen @ 0x406e00]` →
`AcceptConnection @0x405580` → `HandleLogin @0x405870`) that parses text commands
(`[orig: CAdminServer_DispatchCommand @ 0x406720]`) and reads/writes live server state.
It is **not** the in-match game-state owner (that is the listen-server host, §5.0/§5.2a) —
but because its `SET`/`STATUS`/`GET` verbs read and write the *same* globals the host
advertises and the config save mirrors, it is the single best **enumeration** of what
"server state" the engine actually keeps. Use it as the spec for our consolidated
`GameConfig` + authoritative server state (faithful-port target; ADR 0013).

**Settable config — `[orig: CAdminServer_HandleSetCommand @ 0x405a60]`.** Each `SET <key>
<val>` writes a **runtime global** AND a **persisted shadow** (`dword_2550xxx`, flushed by
`[orig: Game_SaveConfig @ 0x54c490]`); the rule *flags* pack into one bitfield
`g_rules_flags @ 0x24D1E34` (shadow `0x2550A04`). On a name/password change the host
recomputes the wire-advertised `np_protocol->server_flags = game_settings.game_type` and
`build_flags = [orig: CNapiServerConfig_BuildFlags @ 0x4c4dc0]` (§6.5). Identity strings
route into `g_napi_np_ctx.game_settings` (§6.4) via `CAITask_SetName @0x402bd0` /
`CAdminServer_SetSidePassword @0x402bf0`.

| `SET` key | runtime global | notes |
|---|---|---|
| `ServerName` | `g_ServerName @ 0x2550A5D` (32 B) | also → `np_protocol->nstmout_path` (advertised name) |
| `ServerPassword` | `g_ServerPassword @ 0x2550A08` (17 B) | empty arg clears; → `game_settings` |
| `SideAPassword` / `SideBPassword` | `g_SideAPassword @ 0x2550A3B` / `g_SideBPassword @ 0x2550A4C` (17 B) | per-side join gate (§3 reject 19/20) |
| `GameTime` | `g_respawn_time @ 0x24D2140` | also sets `dword_24C1958 = 3720 * val` (frame budget) |
| `KOTHLimit` | `g_time_limit_minutes @ 0x24D2144` | |
| `KillLimit` | `g_score_limit @ 0x24D2134` | **name/global swap**: `KillLimit`→`score_limit` |
| `MaxScore` | `g_kill_limit @ 0x24D2138` | **name/global swap**: `MaxScore`→`kill_limit` |
| `MaxFriendlyKills` | `g_max_friendly_kills @ 0x24D2244` | |
| `StartDelay` | `g_StartDelay @ 0x24D2160` | |
| `AutoBalanceOnRecycle` | `g_autobalance_enabled @ 0x24D2190` | |
| `PuntVote`/`VotePercent`/`VoteNumPlayersReq` | `g_votekick_enabled @ 0x24D226C` / `g_votekick_percent @ 0x24D2274` (float) / `g_votekick_min_players @ 0x24D2270` | |
| `ChangeTeamInterval`/`Penalty`/`Delay` | `0x24D2280` / `0x24D2284` / `g_capture_duration @ 0x24D2248` | |
| `DoMinPingCheck`/`MinPing`/`DoMaxPingCheck`/`MaxPing` | `0x24D21B0`/`0x24D21AC`/`0x24D21B8`/`0x24D21B4` | |
| `FatBullets`/`OneShotKill`/`ArmoryTimer` | `0x24D21A0`/`0x24D219C`/`g_ArmoryTimer @ 0x25510F0` | |
| `Tracers` | `g_rules_flags & 0x1` (inverted) | flag bit |
| `ChangeTeam` | `g_rules_flags & 0x4` | flag bit |
| `FriendlyFire` | `g_rules_flags & 0x200` (inverted) | flag bit |
| `FriendlyTags` | `g_rules_flags & 0x400` (inverted) | flag bit |
| `TeamTriggerClaymore` | `g_rules_flags & 0x8000` | flag bit |

**Live state — `[orig: CAdminServer_HandleStatus @ 0x402e30]`.** The status read enumerates
the *runtime* server state, and is load-bearing for the "pools / entities / bookkeeping"
model: the **roster is the player-entity slot array itself**, not a connection-side cache.
`STATUS` walks `capacity @ 0x24C0CA4` slots from `g_player_slots @ 0x24C0CA8` (stride
`0x18E88` bytes), and for each active slot reads name/team/class/kills/deaths/ping **off the
entity** (`name @ entity-32`, `slot# @ entity-13 dwords`, `team @ entity+344`, `class`,
kills/deaths via `[orig: CRenderState_GetFieldByIndex @ 0x52d7d0]` fields 6/4, ping). Plus
the session header: active server name `0x24D1FA4`, uptime `[orig: CSessionTimer @ 0x24E3E88]`,
TOD `Env_CurTimeFixed24`, current map `g_map_file_name @ 0x24D1F3E`, `g_GameType @ 0x24D2128`,
and the mission-rotation queue `g_entity_action_queue @ 0xC86FDC` (current/next/one-shot/
flipped/2x flags) against `missionListOut @ 0x2551118` (stride 4584).

The status read is byte-precise about *where* each roster field lives — witnessed at the
`sprintf` @0x403182 that formats one player line off `renderState` (the per-slot entity):
`name @ entity-32` (`%-16s`), `slot# @ *(entity-13 dwords)`, **`team @ *((uint8_t*)entity + 344)`**,
`class @ entity[22437]`, `deaths/kills` via `[orig: CRenderState_GetFieldByIndex @0x52d7d0]`
fields 4/6, `ping @ entity[23582]`. The session header reads `g_GameType @0x24D2128` (via
`get_game_type_abbreviation @0x520fd0`), so STATUS, the S2C 0x08 block, and the S2C 0x7B body
all read the SAME game-type global (see the witness note below).

**Witness — one `g_GameType`, one `g_server_name_str`, one BuildFlags copy.** The reimpl had
split each of these into multiple diverging copies; IDA shows the wire serializers read one
global each:
- **`g_GameType @0x24D2128`** is read by BOTH `[orig: ServerConfig_SerializeToPacket @0x505bd0]`
  (the S2C 0x08 block, `dword[3]` @0x505c2b) AND `[orig: NapiNPMsg_0x7B_BuildPayload @0x507740]`
  (the S2C 0x7B `gametype` u32 @0x5078ce and its title-selection gate `(g_GameType & 0xFFFDFFFF)
  == 0x10020` @0x507822) AND STATUS above. `[orig: CNapiServerConfig_BuildFlags @0x4c4dc0]` reads
  a SEPARATE `game_settings.game_type` copy (ctx+0xCC @0x4c4e3a) for its `& 0x10000` team-gate — a
  snapshot of `g_GameType` set at session build, equal in a live session.
- **`g_server_name_str @0x24D1FC4`** is read by BOTH `[orig: NetPacket_WriteServerNameAndMapFile
  @0x505780]` (the S2C 0x2C) AND `NapiNPMsg_0x7B_BuildPayload` (the 0x7B serverName @0x5077f1);
  `game_settings.server_name` (ctx+0xE68) is the distinct lobby/`nstmout_path` name.
- The 0x08 block's other 9 dwords map to the standalone rule globals in wire order (`g_respawn_time
  @0x24D2140`, `g_time_limit_minutes @0x24D2144`, `g_replay_enabled @0x24D2120`, **`g_GameType`**,
  `g_max_team_lives @0x24D2130`, `g_score_limit @0x24D2134`, `g_respawn_timeout @0x24D214C`,
  `g_StartDelay @0x24D2160`, `g_destroy_buildings @0x24D2164`, `g_death_messages @0x24D2168`), then
  7 bytes (`byte_24D234C..byte_24D2360` + `dword_24D2110` low byte), then the BuildFlags dword.
  **The five formerly-unnamed dwords were witnessed 2026-07-01** by tracing each to its
  `Config_ParseSettingsLine @0x54f740` setting-name compare through `apply_session_settings_to_globals
  @0x551500` (cfg global → live rule global): dword[2] = SET `replay` (cfg @0x2550B24),
  dword[4] = SET `max_team_lives` (cfg @0x2550ABC), dword[6] = SET `timeout` (cfg @0x2550B34; read by
  `GameEvent_PlayerDeath @0x516dd0` — the respawn timeout), dword[8] = SET `destroybuild` (cfg
  @0x2550ACC; read by `Entity_ApplyWeaponDamage @0x4e6820`), dword[9] = SET `deathmes` (cfg
  @0x2550AD0; read ×3 by `GameEvent_PlayerDeath`). The two BuildFlags inputs likewise:
  `dword_2550A04` IS the mpattrib bitfield store (`ServerConfig_ApplyHostSetting @0x4a6000` maps SET
  `TeamChoose` → bit 0x4 direct @0x4a63d9, `TeamFF` → 0x200 inverted, `FriendlyTag` → 0x400 inverted,
  `ClaymorePref` → …), so the `|0x4` input = **TeamChoose**; `dword_2550CA4` = cfg
  `mp_allowsniperscopezoom` (@0x550ac9; read by `WeaponSlot_InitFromDef @0x53ee70`) → `|0x10000`.
  Reimpl `GameConfig` fields renamed accordingly (`replay_enabled`/`max_team_lives`/`respawn_timeout`/
  `destroy_buildings`/`death_messages`/`team_choose`/`allow_sniper_scope_zoom`).

**Consequence for the reimpl (D-NET-132, ADR 0013) — IMPLEMENTED:** team/class/slot/kills/deaths
are derived from the authoritative pool-0 entity, so `NapiNPConnection` holds the entity *handle*
(`link.owned_entity`) as the single binding and the §5.1 reply builders read team (@entity+344) and
the wire handle THROUGH it from the live `world::EntityRegistry` — no per-connection reply cache.
The pre-World reactive path (`bind_session_reply_player`, a World-less session-responder / test host)
stamps `owned_entity` with the bare wire handle, so it resolves the same way (team defaults when no
live entity backs the handle). `player_slot` (roster ORDER, not on the entity) + the echoed
`player_name` stay on `conn.reply`. The former `ServerRules` + `NapiGameSettings` (§6.3/6.4) +
`SessionReplyConfig` (§5.1) are merged into one `GameConfig` (`libs/npruntime/.../game_config.h`)
mirroring the `SET` field set above, with the three diverging gametype copies collapsed onto the one
`g_GameType`-role field; the persisted `dword_2550xxx` shadow + `g_rules_flags` packing are a
config-file concern modeled only if we add cfg persistence.

## 7. Landed architecture

The design that shipped in PR #37, was reverted in PR #50, and is now landed on master.
`web/` and `apps/novaworld_server/` are on master.

- **One standalone C++ server binary, three listeners**: gate UDP :7597, HTTP :8080 (the
  `/api/*` routes plus the bundled Vue web portal from `web/dist/`), NW UDP :64206. Shared
  in-memory connection registry; SQLite state at `backend/data/state.db` (schema kept abstract
  enough to swap libpq later). HTTP framework was decided as Drogon but implemented with Crow
  + standalone Asio.
- **Protocol code lives once** in Godot-free libs: `libs/novacrypto` (Layer 2 cipher),
  `libs/napi` (Layer 2-3 framing/session/TLV), `libs/npwire` (the in-game wire codec, NWU
  session framing, and capture/replay chain — extracted post-landing, ADR 0019), and
  `libs/novaworld` (the matchmaking/service lib: Layer-4 PN browser/session message sets,
  connection registry, and db wrapper). PN dispatch happens inside `libs/novaworld`; the
  NovaWorld server routes `NOVAWORLDUDP` to lobby containers and
  `JointOperations`/`JOINTOPERATIONS` to a World-less `libs/npruntime` session-responder ctx
  (the experimental in-match `GameSession`/`GameServerRuntime` were retired at npruntime P8).
- **Godot is the client only**: GDExtension binding under `godot/engine/network/`; servers are
  pure C++ with no Godot dependency. A future Godot admin/stats viewer would talk to the
  standalone server over HTTP, never be the server.
- **Reference equivalence**: `libs/napi/tlv.h::NapiMessage` is byte-for-byte equivalent to the
  Python reference container parser (`onnet/onnw/protocol/container_parser.py`); the wire
  markers are those in §1. The authoritative dissector for opcode/message enumeration is
  `novaworld_udp.lua`.
- State at revert: gate + NW UDP + HTTP listeners, ServerAuth defaults matched to retail
  captures (MI=0x113f, CR=1, NovaworldName="NWServer", 62-char SCRK, 60-char hex NWUID),
  Layer-4 session dispatch, web portal build, DB migrations, and the Godot client demo were
  done; the legacy `NW*.dll` HTTP routes (§2) followed; full retail end-to-end join (spawn
  flow, §5) was still being chased.

### 7.1 Client session state machine (ADR 0010, Phase 1)

The client direction of the session flow is a Godot-free state machine,
`libs/novaworld/client_session.{h,cpp}` — the mirror of `LobbySession`/
`nw_udp_listener.cpp` with request and response inverted. The `NovaWorldClient`
GDExtension binding (`godot/engine/network/`) is now a thin socket pump: the
gate leg yields the NW UDP host:port, then `ClientSession` runs
`HELLO → AUTH → {ClientConnected → ServerStartVerify → ClientRequestVerifyResult →
ServerVerifyResult} → Verified`.

Two client-direction parsers were added (inverses of the existing serializers):
`parse_server_hello` (recovers the host key `HK` the client must echo) and
`parse_server_auth` (recovers `CR`/`SK`/server `SCRK` + the CS/CU control
fields). Phase 1 closed two stubs in the old binding: the hardcoded
`send_session_join(/*server_hk=*/0)` (now echoes `ServerHello.HK` in
`ClientAuth.HK`) and the empty `0x83` handler (now decodes the lobby stream and
drives the verify handshake to `ServerVerifyResult`). The inner-stream crypto is
symmetric: the client encrypts its `0x43` with its own `SCRK`, decrypts inbound
`0x83` with the server's `SCRK`; outbound `session_id` = the server `SK`.

Verified offline by `tests/novaworld/client_session_loopback_test.cpp`, which
drives `ClientSession` against the real server-side parsers/builders +
`LobbySession` in-process and asserts the `HK` echo and the
`Verified`/`SessIdString` outcome. The HELLO and AUTH legs now **have** fresh
IDA witnesses against real NW — the identity gates in `HandleClientHello @
0x6213B0` (NW-S1) and `HandleClientJoin @ 0x62B750` plus the retail `0x42`
builder `CNapiNPConnection_SendClientJoin @ 0x61fe20` (NW-S2, §7 Wave 1) — confirmed
live: the client reaches `session_join`, and after NW-S2 the join carries the
identity block real NW requires for `ServerAuth`. The verify framing is still
inferred from the container set (§3). Live-smoke past AUTH and the host/join
legs are ADR 0010 Phases 3-5.

### Wave 1 — gate protocol + session envelope (2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| Gate response emit (`apps/novaworld_server/gate_listener.cpp::build_gate_response`) | `CNapiGateManager_ProcessResponse @ 0x4ced20` | **matching** | Emits the required POSTIPADDRESS/POSTIPPORT (see NW-G1); the wire shape (`GATEPROTOCOL "1.0"` + `VAR "k" "v"` CRLF lines) is what the retail parser consumes. |
| Gate response parse (`libs/novaworld/gate_response.cpp`) | `CNapiGateManager_ProcessResponse @ 0x4ced20`, tokenizer `String_TokenizeQuotedToArray @ 0x616d60` | **divergent → fixed (×2)** | (1) The parser was missing 6 of retail's 19 keys (LOBBYNAME, USEJUNCTION, CLEARJUNCTION, GLSVSSREQUEST, GLSVSSRIMS, GLSVSSAGRMS) and carried 2 non-retail keys (CUS, PVT). Missing keys added; CUS/PVT kept as flagged tolerant extras. The header's address citation was wrong (`0x4ad330` is `SaveFile_WriteFullState`); corrected to `0x4ced20`. (2) **2026-06-11**: the tokenizer split on whitespace only and left the quotes attached, so the real gate's quoted lines (`VAR "POSTIPADDRESS" "127.0.0.1"`) matched no key (`var_count==0`) and the Godot client rejected every real-NW reply as "bad gate response". Retail's tokenizer `String_TokenizeQuotedToArray @ 0x616d60` toggles on `"` and never copies it (quotes stripped, whitespace inside quotes kept); our `tokenize_line` now mirrors it and uses `tokens[2]` as the value (= retail's `tokenValue`). The envelope was never the issue (the symptom was the post-envelope parse, not "bad gate envelope"). Covered by a quoted-format case in `gate_response_test` + a quoted `gate_server_loopback_test` body. |
| Gate manager defaults (`§6.7` struct) | `CNapiGateManager_InitDefaults @ 0x4d1460` | **matching** | hostname `gs.novaworld.net` @ +8, port 7597 @ +72, tag `jop:cus2` @ +76 — exactly the §6.7 layout. Note: the base `CNapiGateManager_Init @ 0x633f90` defaults to `novaworld.net` @ +64 / port @ +192; the game layer's `InitDefaults` overrides it, so the effective retail gate host is `gs.novaworld.net`. |
| Session HELLO TLV (`libs/npwire/session_hello.cpp`) | `NapiNPProtocol_HandleClientHello @ 0x6213B0` | **matching** | Flat TLV tag set confirmed: NVS, CO, AP, BDAT, PN (game id), PG (16-byte key), PV1, PV2 — plus retail-only validated/echo tags PV3/PM/CI/EIP/EPN/ET. Retail validates NVS == the Milota version string `"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic"`, PN == server game id, PG == server key (16 B), PV1 == server build. SESSION NWU key `"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj"` @ 0x7DFC50 confirmed. |
| ClientHello identity (`libs/novaworld/client_session.cpp::build_client_hello`) — **client direction** | `HandleClientHello @ 0x6213B0` validation + `CNapiGameSession_InitNPConnection @ 0x4d3be0` constants | **divergent → fixed** | NW-S1 (**2026-06-11**): the Godot client sent `NVS="OpenNova Godot Client 0.1"` and **no PG**. Our own server's `parse_client_hello` doesn't validate, so OpenNova accepted it — but real NW's `HandleClientHello` does the LABEL_68 check: if `NVS != Milota` **or** `PN != game id` **or** the 16-byte `PG != proto+284` **or** `PV1 != proto+300`, it `return 0`s and **sends no ServerInfo/ServerHello** → the client times out in `session_hello`. The client now sends the retail-faithful identity from `InitNPConnection @ 0x4d3be0`: `NVS` = Milota, `PN` = `NOVAWORLDUDP`, `PV1` = `"0.0.0 2/10/2004 EM"`, and `PG` = the 16-byte `NOVAWORLDUDP` protocol GUID built by `sub_62E750 @ 0x62e750` from `(-655487758, 58574, 17549, 144,180,29,66,179,100,171,113)` → bytes `F2 0C EE D8 CE E4 8D 44 90 B4 1D 42 B3 64 AB 71` (`[u32 LE][u16 LE][u16 LE][8B]`). CO/AP/BDAT are read but not validated, kept as our identity. Pinned in `client_session_loopback_test`. (Reference is IDA only — `opennova-int` is server-only and never modeled the client direction.) |
| ClientAuth identity (`libs/npwire/session_hello.cpp::client_auth_to_bytes` + `client_session.cpp::build_client_auth`) — **client direction** | `HandleClientJoin @ 0x62B750` validation + client builder `CNapiNPConnection_SendClientJoin @ 0x61fe20` | **divergent → fixed** | NW-S2 (**2026-06-11**): after the NW-S1 hello fix the client advanced `session_hello → session_join` but timed out — real NW never sent `ServerAuth(0x82)`. `HandleClientJoin @ 0x62B750` re-runs the **same** identity gate as the hello and silently `return 0`s (no ServerAuth) unless `NVS==Milota && PN==proto+220 && PG==proto+284(16 B) && PV1==proto+300`; its `is_server` branch additionally requires `HK==proto+1332` (the echo), `PV2==proto+364` (`"1"`), and a non-empty `NA`. Our `client_auth_to_bytes` emitted **only** `CI/HK/CK/NA/SIP/SPN/SCRK/CU` — the entire identity block was missing, so the gate failed. Retail's own `0x42` builder is `CNapiNPConnection_SendClientJoin @ 0x61fe20` (a Kong **misnomer** — `packet_type=66='B'`=0x42, not the hello), which emits `NVS/CO/AP/BDAT/[DE]/PN/PG/PV1/PV2/[PV3]` ahead of `CI/HK/CK/NA/[PW]/SIP/SPN/CU/SCRK/[NF/DCNT/RCNT]`, identity sourced from `CNapiGameSession_InitNPConnection @ 0x4d3be0` (`CO="NovaLogic Inc, Calabasas CA U.S.A."`, `BDAT="Jul 21 2009 18:54:41"`, `PV2="1"` @ +364). The fix emits the identity block in retail order (each tag gated on non-empty/non-zero, as retail does), reusing the same `Config` values that already pass the hello gate. `parse_client_auth` made symmetric. Pinned in `client_session_loopback_test` (identity-block assertions) + a `client_auth_to_bytes`↔`parse_client_auth` round-trip in `session_hello_roundtrip_test`. (Reference is IDA only — `opennova-int` is server-only.) Proposed IDB rename recorded: `0x61fe20 → CNapiNPConnection_SendClientJoin`. |
| Session containers (`libs/novaworld/lobby_session.cpp`) | NOVAWORLDUDP dispatch (§3) | **matching (spot-checked)** | All ten containers dispatched with the documented replies (§3); covered by `lobby_session_test`. Field-for-field read order vs retail handlers deferred to a wave-1 follow-up where it matters for a specific reply. |

#### NW-S3 — the 0x42 join is protocol-complete; real NW gates acceptance on an authenticated session

After NW-S2 the client still times out in `session_join` against live NW. Grilling the
server-side accept path (retail `Jointops.exe`, which contains the host/server code) shows
**the protocol handshake itself is now correct and would be answered on the first 0x42** — the
remaining gate is account/session authentication that lives in the NW *server's* callbacks,
which are not in this binary:

- `HandleClientHello @ 0x6213B0` replies via `NapiNPProtocol_SendServerInfoPacket @ 0x6204b0`
  with **opcode 0x81** (so our `parse_server_hello` on 0x81 is right). It writes the `HK` tag =
  `proto->host_key` at **offset 0x534 (=1332)**. `HandleClientHello`'s version gate is *identical*
  to `HandleClientJoin`'s (NVS/PN/PG/PV1) — and our `0x41` already passes it (we receive the
  ServerInfo), which independently proves our flat-TLV encoding and NVS/PN/PG/PV1 values are
  correct.
- `HandleClientJoin @ 0x62B750` echo-checks the client `HK` against `proto[333]` = `proto+1332`
  = the same `host_key` it advertised. So **the HK echo is sound** (we read and re-send it). PV2
  is checked vs `proto+364` (`"1"`, a protocol constant despite the `max_players_string` Kong
  name) and NA must be non-empty — both satisfied.
- A *fresh, accepted* join sends `ServerSessionInit` **inline on the first 0x42**, no retransmit:
  `CNapiNPConnection_OnStateChange @ 0x626060` (state 1) calls `cb_server_1` (`proto+724`) and, if
  it returns ≥ 0, `CNapiNPConnection_SendSessionInit @ 0x620ef0`. Earlier, `HandleClientJoin`
  itself runs `cb_server_0` (`proto+720`); on `< 0` it destroys the connection and stores the
  reply tag **`NP.C:PCCR:ILC`** ("Illegal Login Callback"). These two callbacks are the
  account-auth gate and live in the NW **server** binary — not witnessable here.
- The retail client carries the auth/session context the server expects:
  `CNapiGameSession_ConnectToNovaWorld @ 0x4d4640` builds the connection's CU var list via
  `CNapiVarList_SetOrCreate` — **`Application`, `BuildDateAndTime`, `Debug`, `CountryName`,
  `Language`, `TimeZoneBias`, `GateTag`, `MetTag`, `UdpCode1`, `UdpCode2`, `MaxPacketSize`** —
  which `CNapiNPConnection_SendClientJoin @ 0x61fe20` emits as `CU` chunks in the 0x42. `MetTag`
  (`byte_B5F4BC`) is set by the **gate handler** `CNapiGateManager_ProcessResponse @ 0x4ced20`,
  and `InitNPConnection @ 0x4d3be0` pulls login cookies (`CookieJar_GetCookiesForURL`). Our client
  sends **no CU chunks** and holds **no authenticated session**.

**Verdict: unknown → blocked on auth.** The bare NP handshake (gate → hello → join) is necessary
but not sufficient for live NW: the matchmaking server rejects an unauthenticated join in
`cb_server_0`/`cb_server_1`. Completing AUTH requires the **HTTP account login (ADR 0010 Phase 3,
NWLogin/EPASK)** — which establishes the session cookie and the `MetTag`/`UdpCode*` values — plus
emitting the retail CU-chunk set in the 0x42. This **reorders the ADR**: login is a *prerequisite*
for completing the UDP AUTH, not a post-connect step. (`OnNovaWorldConnected @ 0x4d1570` fires
*after* SessionInit and fills a separate in-game credential form; that is distinct from the
pre-connect web login that authorizes the join.)

**The auth chain, end to end (de-risk grill, 2026-06-11; CU mapping re-grilled 2026-06-24):**
1. `UDPCODE1`/`UDPCODE2` — the session-auth codes the 0x42 join carries as `UdpCode1`/`UdpCode2`
   CU chunks — are **gate-response VAR keys**, parsed in `ProcessResponse @ 0x4ced20`. Our
   `gate_response.cpp` already parses both. So they come from the **gate**, not a separate endpoint.
   The byte-global → CU-var mapping was witnessed end to end (2026-06-24): in `ProcessResponse`
   the gate VAR handlers call mission-info setters on `&byte_B5F450` (the `CMissionInfo` base),
   and `ConnectToNovaWorld @ 0x4d4640` reads those globals back to build the CU list. **The Kong
   setter names `CMissionInfo_SetGateTag` / `_SetMetTag` are misnomers** — they do NOT write the
   GateTag/MetTag CU values:
   - `UDPCODE1 → CMissionInfo_SetGateTag @ 0x4cd990` writes base+1176 = `byte_B5F8E8` → emitted as
     the **`UdpCode1`** CU.
   - `UDPCODE2 → CMissionInfo_SetMetTag @ 0x4cd9b0` writes base+1208 = `byte_B5F908` → emitted as
     the **`UdpCode2`** CU.
   - `METLABEL → CMissionInfo_SetTargetName @ 0x4cd950` writes base+108 = `byte_B5F4BC` → emitted as
     the **`MetTag`** CU.
   - The **`GateTag`** CU is `stru_B5FF50.protocol`, a **const protocol/gate tag** (our ClientAuth
     `na`, e.g. `"jop:cus2"`) — NOT sourced from any gate VAR.
   So the faithful CU mapping is `GateTag = na`, `MetTag = METLABEL`, `UdpCode1 = UDPCODE1`,
   `UdpCode2 = UDPCODE2` (built by `make_novaworld_join_cu`, libs/novaworld). The earlier
   shorthand "`UDPCODE1 → SetGateTag`, `UDPCODE2 → SetMetTag`" described the *call targets*, whose
   names mislead — it does **not** mean the GateTag/MetTag CUs carry UDPCODE1/2.
2. The gate issues them only to an **authenticated** request. Authentication is a **web-form login**
   through the in-game browser (`CUIBrowser` @ `dword_2551100`): `load_persistent_login_credentials
   @ 0x557330` fills the `NAME` (username) + password fields; persisted creds live in
   `PERSISTENTREMEMBERLOGINDATA`, decrypted by `NapiNP_DecodeEncryptedKeyValue @ 0x619470` with key
   `"SLHALI289SZ79210987ZS:OCV789YHK2QJ3HSKDJHVS978THYG23"`. (EPASK crypto = NW-C2.)
3. So **live-NW Phase 3 = web login → gate issues `UDPCODE1/2` → emit them (+ env vars) as CU chunks
   in the 0x42.** Our gate parser already captures `UDPCODE1/2`; the missing pieces are (a) the web
   login that makes the gate issue them and (b) attaching the CU-chunk set to `ClientAuth`.

**Port status (F2, 2026-06-24): part (b) is done.** The 0x42-join CU set is now built faithfully by
`make_novaworld_join_cu` (libs/novaworld) — the exact 11-chunk `ConnectToNovaWorld @ 0x4d4640` set
in retail order, type 2, with `CountryName`/`Language`/`TimeZoneBias` empty on the join (locale
rides the verify `Cookie`). **Both** NovaWorld directions carry it: `NovaWorldClient` (join) and
`NovaWorldHost` (host registration) populate `ClientSession::Config.cu_vars` from the gate response
(`MetTag ← METLABEL`, `UdpCode1/2 ← UDPCODE1/2`, `GateTag = na`). A `ClientSession` so configured
emits the chunks in its generated `ClientAuth`, pinned by `tests/novaworld/client_session_cu_test`
(builder shape + the 0x42 actually carrying all 11). This **closes the "our client sends no CU
chunks" gap** in the verdict above. Remaining for live NW is only part (a), the HTTP account login
(ADR 0010 Phase 3) — and per Wave 5 below it is needed for the **account/GSB** leg, not to reach the
lobby `Verified`. Against the permissive OpenNova gate the codes are empty and acceptance does not
depend on them.

**Scope note — wire compatibility vs live-service traffic.** Wire compatibility is a standing design
requirement in all directions — our clients join original servers, our servers serve original clients,
and opennova↔opennova works the same way on one protocol (root `CLAUDE.md` Conventions;
`../engine-primer.md` §3). What *this particular auth chain* buys is narrower: its full form is required
only to impersonate a retail client against **NovaLogic's live hosted NW**, a parity-testing scenario
done sparingly (no load/abuse). The everyday OpenNova path is OpenNova client ↔ the OpenNova server
(`apps/novaworld_server`), whose `cb_server_0`/`cb_server_1` are permissive — there the full handshake
already reaches `Verified` (`client_session_loopback_test`). The `session_join` timeout is therefore
expected against live NW and is **not** a blocker for the OpenNova-server path; it gates only the
retail-impersonation parity scenario.

Empirical ground truth available cheaply: retail JO logs the entire gate dialogue to
`_connectlog.txt` (every `GATE LINE #NN [...]`, set by `ProcessResponse`'s `g_ConnectLogEnabled`
path) when launched with `/connectlog`. One retail launch against the same live endpoint captures
the exact VAR set the gate returns (incl. whether `UDPCODE1/2` are present) and resolves any
remaining unknowns (`UdpCode1/2` source `byte_B5F8E8`/`B5F908` had no writer xref).

#### NW-G1 — POSTIPADDRESS / POSTIPPORT are required (resolved)

The standing question (jodemo requires them; onnet omits them yet works on retail JO)
is resolved by `CNapiGateManager_ProcessResponse @ 0x4ced20`. After tokenizing the
response, retail:

1. fails to state `-9` if `parsedFieldCount == 0` (no recognized VAR line);
2. computes a junction/direct-connect bypass `dword_B5FD2C` from command-line flags
   (`dword_B5F928`, `dword_B4C71C`, overridable by `dword_829F88`);
3. **unless that bypass is set**, fails to `-9` with "NO NW POST IP" if `dword_B5F490`
   (POSTIPADDRESS) is unset, then "NO NW POST PORT" if `dword_B5F494` (POSTIPPORT) is
   unset.

So retail genuinely requires both VARs on the normal (non-junction) path — the same as
jodemo. onnet's omission was a bug; the PR #37 stack adding them
(`gate_listener.cpp`) was the correct fix and is what we reland. Command-line overrides
(`dword_B4C6B0`/`dword_B4C6B4`) can supply the POST IP/port directly, which is the only
way the fields become optional.

#### NW-L2 — NovaLogic hostnames the client contacts (launcher hosts set)

String sweep of retail `Jointops.exe` for the launcher's redirect set:

| String | Where | Role | Redirect? |
|---|---|---|---|
| `gs.novaworld.net` @ 0x7cc368 | `CNapiGateManager_InitDefaults @ 0x4d1460` (+8) | **effective gate host**, port 7597 | **yes** (the one the launcher must map) |
| `novaworld.net` @ 0x7e052c | `CNapiGateManager_Init @ 0x633f90` (+64) | base-default gate host, overridden by InitDefaults | optional (belt-and-braces) |
| `http://www.novaworld2.com` / `.../patch/%d` @ 0x7dc144/0x7dc160 | `sub_5C7240` | patch / update URL | **no** (outside matchmaking; we do not manage patches) |
| `http://%s` @ 0x7cc3e8 | `CNapiGameSession_OnNovaWorldConnected @ 0x4d1570` | startup-URL format (domain comes from the gate response) | n/a (not a hostname) |

Conclusion: redirecting **`gs.novaworld.net`** is sufficient for the JO matchmaking flow;
adding `novaworld.net` is harmless belt-and-braces. The launcher's server-driven
redirect set (default `["gs.novaworld.net"]`) is correct; `novaworld.net` can be added
server-side without a launcher release. DFX2's gate host (`dfx2:0:cus:buffy` tag) is
grill item NW-L1, deferred to wave 2 (needs the dfx2.exe IDB).

### Wave 2 — server-browser format (partial, 2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| GSB builder (`libs/novaworld/gsb.cpp`) | retail GSB chunk strings | **matching (retail)** | NW-G2: retail `Jointops.exe` contains the `SVRS` (@0x63d793) and `FLDS` (@0x63d7b1) chunk strings and lacks `GLB `/`PLYR`. Our builder emits `IVAR`/`FLDS`/`SVRS`/`XXXX` — the GSB format — so it matches the **retail** client. |
| GSB parser (`libs/novaworld/gsb.cpp::gsb_parse_response`) | retail GSB chunk-tag pool `SVRS`/`GSB `/`FLDS` @0x63d793–0x63d7b1 | **matching (retail)** | NW-G3: client-direction inverse of the anchored builder (ADR 0010 Phase 2). The retail response header `"GSB "` is at 0x63d7a5, adjacent to the parser's tag pool; our parser checks the same header, decrypts each chunk with the SUBTRACT chain, and reads rows positionally against the SVRS field table. Verified by `gsb_parse_roundtrip` (build→parse is byte-faithful). |
| GSB **request** URL (client GET) | *(no binary literal)* | **matching by construction (OpenNova)** | NW-G3: `.gsb` / `jop_2.gsb` / `GSB_SERVER` / `?a=1` are **absent** as string literals in retail `Jointops.exe`. The client does not construct the GSB path/query — it issues a plain HTTP GET of a URL handed to it at runtime in the server-sent menu (the `GSB_SERVER` template substitution our `http_listener` produces). For OpenNova we control that URL, so the request matches by construction. |

#### NW-G2 — GSB (retail) vs GLB (demo) is a per-binary split

The standing disagreement (the Phase C.2 note records the browser format as
`GLB `/`FIEL`/`DATA`/`PLYR`, "NOT onnet's `GSB `/`SVRS`/`FLDS`") resolves as a
**per-binary difference**, not a bug: that GLB layout was witnessed against
*jodemo* (the Joint Operations demo), while retail `Jointops.exe` (JO:CA, the
deploy target) uses the GSB chunk format our `gsb.cpp` already emits. String
evidence in the retail image: `SVRS`, `FLDS` present; `GLB `, `PLYR` absent.

Open: a definitive jodemo-side GLB spec (chunk-by-chunk) needs the jodemo IDB
and, if demo support is wanted, a separate demo GSB/GLB builder. Deferred with
NW-L1 (the DFX2 gate hostname, also a different-binary item) to a wave-2
follow-up that loads those IDBs. The retail path — the one that matters for the
deploy target — is matching.

#### NW-G3 — GSB client parser + request (ADR 0010 Phase 2)

Two client-direction findings, grilled while landing the Godot client's server
browser:

1. **Response parser.** `gsb_parse_response` (`libs/novaworld/gsb.cpp`) is the
   exact inverse of the anchored `gsb_build_response`: it checks the `"GSB "`
   header (retail @0x63d7a5, adjacent to the parser's chunk-tag pool `SVRS`
   @0x63d793 / `FLDS` @0x63d7b1), decrypts each chunk payload with the SUBTRACT
   chain (our `nwu_encrypt`) under `GSB_NWU_KEY`, and reads each server row
   positionally against the field-name table in the SVRS(fields) chunk (lookup
   is case-insensitive, as the retail browser folds case). The GSB row carries
   only `rid` + the four IPv4 octets — **no port** — so the parser leaves
   `port = 0` (host:port for the actual join arrives via a later leg). Locked by
   `gsb_parse_roundtrip` (build→parse byte-faithful). An anchoring IDA comment
   is on 0x63d7a5.

2. **Request URL.** A string sweep of retail `Jointops.exe` for `.gsb`,
   `jop_2.gsb`, `GSB_SERVER`, and `?a=1` finds **none of them** — the only GSB
   string in the image is the `"GSB "` response header. So the client does not
   build the browser URL: it issues a plain HTTP GET of a URL supplied at
   runtime by the server, the `GSB_SERVER` template/menu substitution our
   `http_listener` already produces (`http://<host>:<port>/jop_2.gsb`). The
   `?a=1` seen in captures is part of that server-provided URL, passed through
   verbatim. For OpenNova we own both ends of that URL, so the request is
   **matching by construction**; our Godot client GETs the OpenNova GSB URL
   (derived from the configured host + `/jop_2.gsb`). **Open (real-NW only):**
   whether the engine's shared HTTP client attaches the login cookies
   (`NWHANDLE`/`PCID`) to the GSB fetch is coupled to the login flow and is
   carried into Phase 3 (EPASK login); OpenNova's `handle_gsb` ignores cookies.

### Wave 3 — login/session crypto (2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| NWU cipher (`libs/novacrypto/src/nwu.cpp`) | `NapiNP_EncryptBuffer @ 0x6187b0` / `NapiNP_DecryptBuffer @ 0x618880` (retail) | **matching (byte-exact, retail)** | NW-C1: re-grilled against retail (was jodemo-anchored only). All six primitives + the seed + the 3-step derive + the 4-phase order match. Name-swap confirmed at byte level. Fixed a doc bug: the LCG multiplier `78665521` is `0x04B05731`, not `0x04B02631` as commented. |
| EPASK login-form encrypt (`libs/novacrypto/src/epask.cpp`) | `EPASK_Encrypt @ 0x6669a0` (core) ← edit-widget vtable `+0x38` `build_form_field_query_string @ 0x657760` ← `build_url_and_submit_request @ 0x63e3f0` | **matching (byte-exact, retail)** | NW-C2: polymorphic dispatch resolved. Core = NWU-add → modexp `pow(byte+2,exp,mod)` 4-byte LE (`EPASK_ModexpEncrypt @ 0x666600`) → NWU-add → A-P low-first (`NapiNP_EncodeToHexAlpha @ 0x666570`); `exp:mod:key` split (`parse_colon_delimited_string @ 0x666710`); the NWU copy `NapiNP_EncryptBufferAlt @ 0x6668e0` is byte-identical to `0x6187b0`. Golden vectors equal the test's own ciphertext fixtures. |
| PUBcrypto `PUB*` join fields (`libs/novacrypto/src/pubcrypto.cpp`) | `NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0` (encode) / `NapiNP_DecodeEncryptedString @ 0x619130` (decode) | **matching (byte-exact, retail)** | NW-C3: PUB encode = CRC32-append (`NapiNP_ComputeCRC @ 0x618770`, MPEG-2) → NWU-encrypt → A-P. Single-key path == `encode_pub_value`; because the encrypt step *is* `0x6187b0`, this proves `ticket_transform` == NWU. The colon-key multi-layer form is the Python remember-cookie (out of our scope). |
| url_cipher `NK`/`CK` join tokens (`libs/novacrypto/src/url_cipher.cpp`) | `parse_connection_query_string @ 0x54dfb0` | **matching (byte-exact, retail)** | NW-C4: `plain[i] = cipher[i] - key[i] + '0'`, `'&'`(38)-terminated; keys NK@`0x7d3f30` `"diheijefhgcdjcgcjcfbd"`, CK@`0x7d3f04` `"cfhdcegjigecjehcgjdhe"` (jodemo: `Auth_ParseRegistrationURL @ 0x514c40`, keys `0x74d8a0`/`0x74d874`). `BK` is the literal `"986119"`, not a cipher. |

#### NW-C1 — NWU cipher is byte-exact in retail (resolved)

The core keyed cipher under EPASK, PUBcrypto, GSB, the gate, and session payloads.
Verified primitive-by-primitive against retail `Jointops.exe`:

- `NapiNP_ComputeKeySeed @ 0x618430` — `null → 3252`; `Σ(i + key[i]²) + len + 50`, key
  bytes signed. Exact match to `nwu_compute_seed`.
- `NapiPRNG_Init @ 0x62e430` — LCG struct `{state@0, multiplier@4, counter@8}`, multiplier
  `78665521` (`0x04B05731`); only the low 16 bits (`0x5731`) participate.
- `Crypto_AddWithKey @ 0x6182d0` — `buf[i] += key[i % klen]` (ADD), so retail
  *EncryptBuffer* is the ADD chain = our `nwu_decrypt`. Confirms the name-swap.
- `Crypto_AddProgressive @ 0x618250` — `buf[i] += seed+i; seed += step`.
- `Crypto_AddLCG @ 0x6183b0` — `state = u16(mult·state + 1); buf[i] += state`.
- `NapiNP_ReverseBuffer @ 0x618210` — `len>>1` front/back swaps.

The three NWU keys are all confirmed against retail: gate `"GATEAPI"`, GSB
`"3209452104342624532341"`, and session-payload (opcode 0x47/0x87)
`"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj"` @ `0x7DFC50`. Verdict: matching.

#### NW-C2 — EPASK login-form encrypt is byte-exact in retail (resolved)

The client login submit (`build_url_and_submit_request @ 0x63e3f0`) reads the server-issued
`EPASK` cookie (`exp:mod:key`), forms `<url>?EPASK=<key>`, and dispatches each form field
through the widget vtable `+0x38`. For the text/password EDIT widget that slot is
`build_form_field_query_string @ 0x657760`, which sizes its output at `8·len`
(= modexp ×4 · A-P ×2) and calls the EPASK core `EPASK_Encrypt @ 0x6669a0`:

1. `NapiNP_EncryptBufferAlt @ 0x6668e0` — NWU ADD chain (key-add, reverse, progression-add,
   LCG-add; multiplier `0x5731`, reverse-flag mult `0x31`), byte-identical to `0x6187b0`.
2. `EPASK_ModexpEncrypt @ 0x666600` — per byte, `modular_exponentiation(byte + 2, exp, mod)`
   (`@ 0x666470`) stored as a 32-bit little-endian word. The `+2` and 4-byte expansion match
   `epask.cpp::modexp_encrypt` exactly (guard: `modulus > 258`).
3. `NapiNP_EncryptBufferAlt` again on the expanded buffer.
4. `NapiNP_EncodeToHexAlpha @ 0x666570` — A-P low-nibble-first (`'A'+lo` then `'A'+hi`).

`exp:mod:key` is split by `parse_colon_delimited_string @ 0x666710` (== `epask_from_string`).
The brute-force modexp *decrypt* is server-side (absent from the client); our `epask_decrypt`
is that server half. Verdict: matching.

#### NW-C3 — PUBcrypto `PUB*` fields are byte-exact in retail (resolved)

`NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0` is `encode_pub_value` for a single key:
append `NapiNP_ComputeCRC @ 0x618770` (CRC-32/MPEG-2: init `-1`, MSB-first, no final xor,
table `dword_849938`) little-endian, then `NapiNP_EncryptBuffer @ 0x6187b0` (NWU), then A-P
low-first; `NapiNP_DecodeEncryptedString @ 0x619130` is the inverse. Because the encrypt step
*is* `0x6187b0`, this proves `pubcrypto.cpp::ticket_transform` == NWU (the Python
`_ticket_transform` is an inlined NWU copy). The colon-separated multi-key form of `0x618fd0`
is the Python remember-cookie, which we do not port. Verdict: matching.

#### NW-C4 — url_cipher `NK`/`CK` tokens are byte-exact in retail (resolved)

`parse_connection_query_string @ 0x54dfb0` decodes the join-redirect query: `NK=`/`CK=` run
through `plain[i] = cipher[i] - key[i] + '0'`, stopping at the first `'&'` (38); `NI`/`NP`/`BK`/`LN`/`GS`
are copied verbatim. Keys (retail): NK `"diheijefhgcdjcgcjcfbd"` @ `0x7d3f30`, CK
`"cfhdcegjigecjehcgjdhe"` @ `0x7d3f04` — byte-identical to `url_cipher.h` (jodemo had them at
`0x74d8a0`/`0x74d874` under `Auth_ParseRegistrationURL @ 0x514c40`). `url_cipher_decode`
matches; `url_cipher_encode` is the inverse used server-side. `BK` is the constant `"986119"`,
not a cipher. Verdict: matching.

All of wave 3 (NW-C1..C4) is now matching byte-exact against retail; equivalence was
additionally proven by compiling the actual `libs/novacrypto` sources and byte-comparing to
the production-proven `opennova-int` Python on golden vectors, plus an adversarial
from-scratch re-derivation.

### Wave 4 — client verify leg + Phase-3 login contract (2026-06-12)

Context: our Godot client completes gate → hello → AUTH against **genuine NovaLogic
NovaWorld** (`gs.novaworld.net` = 207.178.209.201; web/asset host 207.178.209.204 — these
are NovaLogic's boxes, the parity target, **not** anything we deploy), but its
`ClientConnected` (0x43) draws no `ServerStartVerify` (0x83). Against the OpenNova server the
same client reaches `Verified`. This grill establishes why, and pins the still-un-witnessed
Phase-3 (login) wire contract. IDA: retail `Jointops.exe` (kong IDB).

#### NW-S5 — the verify leg needs the HTTP login first; our `ClientConnected` body is already correct

- **`ClientConnected` is bare in retail too.** `CNapiGameSession_SendClientConnected @ 0x4cfe30`
  builds a statement named `"ClientConnected"` with **zero fields**, queued via
  `CNapiNPConnection_QueueMessage @ 0x628640`. Our `client_session.cpp` `on_server_auth` emits the
  same bare container — **matching**. (The old source comment claiming retail "re-sends identity
  here" was wrong and has been corrected.)
- **Timing divergence (fixed 2026-07-19, D-NET-21).** Retail does **not** send `ClientConnected` on `ServerAuth`. The only
  caller is `CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400`, gated on
  `np_conn_state == 5 && session_state == 2` — i.e. only after the NP layer's `ServerSessionInit`
  has completed and `CNapiGameSession_OnNovaWorldConnected @ 0x4d1570` has run (it sets session
  state 2 at its tail). At the Wave-7 audit, `ClientSession` fired it while parsing `ServerAuth`.
  It now emits from the explicit periodic-update boundary after the handler completes.
- **Missing login→verify bridge.** Retail's `ClientRequestVerifyResult` builder
  `CNapiGameSession_SendVerifyRequest @ 0x4d3620` attaches **`SessIdString`** (from `session+1148`,
  empty on the first pass) **and a `"Cookie"` var-list** serialized from `session+388`, which
  `CNapiSession_ReadLocaleInfo @ 0x4ce390` fills from the HTTP login cookie jar. Our client sends a
  **bare** `ClientRequestVerifyResult`. The read side (`ServerVerifyResult` →
  `CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800`: `atol(Success)` ≠ 0 → copy
  `SessIdString` → state 4 → dispatch `StartHosting`/`StartPlaying` by `session+296`) matches ours.
- **Root cause.** Both divergences trace to the same prerequisite: the verify leg on live NW
  carries login-derived cookies the client only has after the HTTP account login. The
  `ServerStartVerify`/`ServerVerifyResult` *server* gate lives in the NW server binary (not
  `Jointops.exe`), so the exact precondition is not directly witnessable, but the client
  unconditionally carries the login `Cookie` var-list into the verify request. **NW-S5 is therefore
  a consequence of NW-S3: it resolves once Phase 3 (login) lands.** This **supersedes the reverted
  NW-S4 empty-root-wrapper hypothesis**, which was a wrong guess (the capture that motivated it is a
  §4 host-join, not the §3 lobby verify leg — see §5.8).

#### NW-S5/B — Phase-3 login wire contract (client-direction, now witnessed)

- **Submit method = POST.** The primary `NAME`/`PASSWORD` login is the `FORM_POST` widget →
  `GopherWebWidget_SendHttpPost @ 0x658b30`: `POST <path> HTTP/1.0`,
  `Content-type: application/x-www-form-urlencoded`, body assembled by
  `build_form_field_query_string @ 0x657760` (EDIT-widget values EPASK-encrypted; hidden fields and
  the echoed `EPASK` public key plaintext). The `?EPASK=` **GET** path
  (`build_url_and_submit_request @ 0x63e3f0`, `HandleScriptedAction` case 4) is a *secondary/express*
  action, **not** the main login — an earlier note over-attributed it. This matches the
  POST-then-GET-relay model in `apps/novaworld_server` and `onnw`.
- **EPASK source.** The `exp:mod:key` bundle arrives as a `Set-Cookie` on the prepare/start page,
  stored in the jar (`CookieJar_UpdateFromURL @ 0x64e630`), read back by name
  (`sub_64EA90 @ 0x64ea90`), and echoed as the plaintext `EPASK` form field.
- **Cookies ride every request, subnet-keyed (resolves the ADR 0010 Phase-2 open question).** Both
  the GET (`CUIBrowser_SendHTTPRequest @ 0x658840`) and POST builders truncate the request host to
  its subnet (`Network_TruncateIPToSubnet @ 0x62dfe0`) and emit `Cookie: name=value;` for **every**
  jar entry on that subnet — so on live NW the `.gsb` server-browser fetch **does** carry
  `NWHANDLE`/`PCID`/`LOGINSESSIONTAG`. (Kong misnames: `0x61e150 "CookieJar_GetCookiesForURL"` does
  not touch cookies; the real read is `sub_64EA40 @ 0x64ea40`.)
- **No post-login gate re-probe.** `UdpCode1`/`UdpCode2` (`byte_B5F8E8`/`byte_B5F908`) are written
  only by the gate-response handler and read only by `ConnectToNovaWorld @ 0x4d4640`; login feeds
  the UDP session indirectly — its cookies make the (authenticated) gate request return
  `UDPCODE1/2` into the join CU, and ride the verify `Cookie` var-list — not via a second gate
  exchange.

Status: `libs/novaworld/http_login.{h,cpp}` now carries the Godot-free pieces of this contract
(`build_credentials_post_body`, `parse_set_cookie_values`, `CookieJar`), proven against the
server's own decode path by `tests/novaworld/http_login_test` (ctest `http_login`). The remaining
Phase-3 work is host-side: the binding's HTTP login chain (prepare GET → login POST → relay GET,
cookie capture) and the panel's credential fields. Rename proposals from this grill were applied in the Wave-5 IDA rename pass;
the IDB is the live record.

> **SUPERSEDED by Wave 5 (below).** Wave 4's "the verify leg needs the HTTP login first" /
> "the verify `Cookie` var-list is lifted from the login cookie jar" conclusion was an
> *inference* (the verify server gate is not in `Jointops.exe`). A full packet capture of a
> **successful** retail session against the real `.204` then refuted it: login is **not** a
> verify prerequisite, and the verify `Cookie` var-list is CD-key/hardware identity (with the
> CD-key fields **empty**), not login cookies. The Phase-3 HTTP login work above is still
> correct and still needed — for the **account/GSB** leg that follows VALIDATE — just not for
> reaching VALIDATE. Same correction applies to NW-S3's "blocked on auth" verdict (§7 Wave 1).

### Wave 5 — the genuine .204 lobby, captured end to end (2026-06-12)

The standing diagnosis (Wave 4 NW-S5, Wave 1 NW-S3) held that completing the lobby verify
against live NW required an authenticated session (web login → gate-issued `UdpCode1/2` →
verify `Cookie` jar). That was inference; the verify server gate lives in the NW server binary,
not `Jointops.exe`. A **full Wireshark capture of a successful retail JO session against the
real `207.178.209.204:64206`** (`fixtures/novaworld/nw204_lobby.hexcap`, frames 8538–10651;
retail's own `_connectlog.txt`) settles it empirically. Decoded byte-for-byte through this
repo's own libs by `tests/novaworld/nw204_lobby_decode_test` (ctest `nw204_lobby_decode`) — no
re-implemented crypto, so the output is ground truth.

The lobby exchange (raw UDP payload bytes; `cooked` = after the 4-byte CRC envelope):

| frame | dir | bytes | op | meaning |
|---|---|---|---|---|
| 8538 | C→S | 274 | 0x41 | ClientHello (`NVS`=Milota, `PN`=NOVAWORLDUDP, `AP`="JOINTOPS.EXE") |
| 8933 | S→C | 313 | 0x81 | ServerHello (`SN`="NWServer") |
| 8977 | C→S | 641 | 0x42 | ClientAuth/Join — 11 CU chunks, **all type=2** |
| 9729 | S→C | 650 | 0x82 | **ServerSessionInit** (`CR`=1, `SK`, 61-char SCRK, NWUID CU) |
| 9730 | S→C | 42  | 0x83 | two high-table `H:0x00` CS config updates, **not** ServerStartVerify |
| 9739 | C→S | 18  | 0x43 | header-only **ack** (seq=1, ack=1) |
| 9778 | C→S | 40  | 0x43 | ClientConnected (seq=2) — bare "ClientConnected" statement |
| 10163 | S→C | 42 | 0x83 | **ServerStartVerify** (seq=2) |
| 10166 | C→S | 896 | 0x43 | **ClientRequestVerifyResult** (seq=3) — the 892B verify |
| 10620 | S→C | 151 | 0x83 | **ServerVerifyResult** `Success=1` (+ `ConnectCommands` var-list) |
| 10651 | C→S | 18 | 0x43 | header-only ack (seq=4) → VALIDATED |

Established facts (all witnessed in the capture, IDA where noted):

1. **Login is NOT a verify prerequisite.** Retail reaches VALIDATED (connectlog "YIPPEE WE ARE
   CONNECTED AND VALIDATED") *before* its first `nwprepare.dll` GET. The gate returned the
   literal placeholders `udpcode1="abc"` / `udpcode2="xyz"` — the same ones our client gets — and
   retail still validated. The HTTP login is for the **account/GSB** leg that follows, not the
   lobby verify. (Refutes Wave 1 NW-S3 and Wave 4 NW-S5.)
2. **0x82 is `ServerSessionInit`, not "ServerAuth".** `CNapiNPConnection_SendSessionInit @
   0x620ef0` emits opcode 0x82 (`-126`) carrying CI/SK/CS×30/CU/SCRK/NA/RIP/RPN; the client
   receiver is `NapiNP_HandleServerJoinResponse @ 0x629840` (`CR`=`byte_7DFDE8`≠0 ⇒ success ⇒
   `conn_state=5` → `OnStateChange @ 0x626060` → `OnNovaWorldConnected @ 0x4d1570`). Our code
   keeps the legacy "ServerAuth" name for opcode 0x82; it is the SessionInit. The `CS` TLVs in
   this packet are the full connection-settings snapshot; later high-table `H:0x00` packets are
   sparse updates of the same fields (§4 high-table control messages).
3. **The verify `Cookie` var-list is CD-key/hardware identity, and the lobby verify is NOT
   credential-gated.** The 892B `ClientRequestVerifyResult` (frame 10166) decodes to
   `SessIdString=""` + a `ClientVarList(VarList="Cookie")` of `ClientVar{VarFNum="0",VarName,
   VarValue}`: CountryName/Language/TimeZoneBias, MyInstalledExpBits, **NWUID** (echoed from the
   SessionInit's NWUID CU), **NWCDKIID=""**, **NWCDKIIDEXP1=""**, NWPSSK/NWUSID (hardware
   fingerprints), NWHWI (GPU$mem$res). The CD-key fields are **empty** on the wire yet the server
   returns `Success=1`. So the verify is registration/telemetry, not a CD-key check. Source:
   `CNapiGameSession_SendVerifyRequest @ 0x4d3620` + `CNapiSession_ReadLocaleInfo @ 0x4ce390`
   reading the browser form fields `OnNovaWorldConnected @ 0x4d1570` set.
4. **The real stall was the DSP seq/ack layer.** Retail's outbound 0x43 seq is **1-based**
   (1,2,3,4) and it sends explicit header-only **acks** (frame 9739 after the settings 0x83;
   frame 10651 after VerifyResult). Our client started seq at 0 — making `ack=0` ambiguous with
   "acked nothing" — and never acked the settings packet, so the peer's reliable layer stalled
   after SessionInit (matching the observed "0x82 received, then timeout"). ClientConnected
   timing (`ProcessPeriodicUpdate @ 0x4d4400` gates it on `conn_state==5 && session==2`) is a
   secondary, non-blocking divergence.

**Fixes landed (this wave).** `client_session.{h,cpp}`: 1-based outbound seq; header-only ack for
any inbound 0x83 that delivered content but drew no substantive reply (settings, VerifyResult);
`build_verify_request()` emits the full `Cookie` var-list from `Config::verify_cookie_vars`, with
NWUID echoed from the SessionInit; opcode-0x82 NWUID extraction in `on_server_auth`. The binding
(`nova_world_client.cpp`) sends the retail join CU set (11 chunks, type=2) and populates the
verify identity (CD-key fields empty, NWHWI/locale best-effort telemetry — the verify is not
gated on them). The OpenNova-server loopback (`client_session_loopback_test`, now also asserting
the verify structure + NWUID echo) stays green: it keeps `verify_cookie_vars` empty for a bare
verify, which the permissive server accepts. Oracle: `nw204_lobby_decode_test`. There is **no
CD-key boundary** for the lobby VALIDATE — the milestone is reachable without credentials.

**Live result (2026-06-12):** our client reached **CONNECTED/VALIDATED against the real `.204`**
(`~/Desktop/capture_opennova.pcapng`): `0x41(259)→0x81(293)→0x42(617)→0x82(650)→H:0x00 CS 0x83(42)
→ClientConnected(40)+ack(18)→ServerStartVerify(42)→verify(882)→ServerVerifyResult(151)→` 2 s
keepalives. The seq/ack fix was the whole story.

### Wave 6 — the authenticated web flow vs real `.204` (2026-06-12)

After the lobby VALIDATE the client browses/joins over HTTP to the NovaWorld **web host**. The
exact contract is witnessed in the user's retail capture (`~/Desktop/capture.pcapng`, HTTP to
`207.178.209.204:80`), not inferred. Sequence (UI-asset `*.mnx`/`*.tga` GETs omitted):

1. `GET /nwprepare.dll?ver1=3&ver2=2345&cc=us&gt=jop:cus2&url=jop_2_start.htm` → Set-Cookie
   `YOURIP/VER1=3/VER2=2345/GT=jop:cus2/CC=us/EPASK=<exp:mod:key>`.
2. `GET /NWStart.dll?MSGBASE=…&IN=jop_2_main.htm&OUT=jop_2_login.htm&verfile=jop_2.ver&…&junction=…`
   → Set-Cookie `USEJUNCTION=0`. **Required** (frame 12178).
3. `POST /NWLogin.dll` → Set-Cookie `LOGINSESSIONTAG` (+ empty NWHANDLE/PCID/…).
4. `GET /NWLogin.dll` (poll, repeat) → Set-Cookie `NWHANDLE=ljim, PCID=A-A02-085D18, NWH/NWI/NWV/
   NWD/EXPBITS, PERSISTENT…` once the auth completes (frame 39723).
5. `GET /jop_2.gsb?a=1` → binary `GSB ` blob.
6. `GET /NWJoin.dll?needexpkey=…&success=jop_2_join.joi&failure=…&relay=…&msgbase=…&nodb=…&pfid=28&
   mode=Login&rid=<RID>` → Set-Cookie `NWJOINSESSIONTAG`; then `GET /NWJoin.dll` → `.joi`
   `[NK&CK&NI&NP&BK]`.

Findings (witnessed, correcting prior inference):
- **The web host comes from the UDP SessionInit**, CU `NovaworldWebDomainNameAndPortNumber`
  (= `207.178.209.204:80`). The gate's `startupurl` carries a literal `[domainname]` (plus
  `[VER1]/[VER2]/[CC]/[GT]`) the client substitutes. `OnNovaWorldConnected @ 0x4d1570` installs
  this domain.
- **Every login form field is EPASK-encrypted EXCEPT the echoed `EPASK` bundle** — not just
  NAME/PASSWORD but pfid/needtoagree/nodb/relay/msgbase/enterkey/failure/success too. (The earlier
  "hidden fields plaintext" read — NW-S5/B — is **refuted** by the capture; that was the source of
  the server's benign "non-A-P decrypt" warnings against our plaintext fields.)
- **The login POST carries the CD-key/hardware identity as HTTP cookies** — the SAME set as the
  UDP verify var-list (CountryName/Language/TimeZoneBias/MyInstalledExpBits/NWUID/NWCDKIID=""/
  NWCDKIIDEXP1=""/NWPSSK/NWUSID/NWHWI) — set by the client (browser form fields), plus the
  prepare/NWStart cookies. Login is **POST then poll** `GET /NWLogin.dll` until NWHANDLE/PCID
  populate.
- **Login is for the account/GSB leg, not the lobby verify** (the lobby VALIDATEs before any HTTP).
  The user's account (`ljim`) authenticates against live `.204`, so its account backend is alive.

**Landed (this wave):** the binding extracts the SessionInit web domain (`server_web_domain()`),
`http_base()` uses it when the gate `startupurl` is templated (the OpenNova concrete path is
byte-unchanged), `resolve_startup_url()` substitutes the placeholders, the login chain adds the
NWStart step + seeds the identity cookies + POSTs the all-encrypted body (`build_login_post_body`,
per-field encrypt) + polls NWLogin, the GSB fetch adds `?a=1`, and NWJoin uses the full witnessed
phase-1 query. `http_login_test` gains an all-encrypted-body case; 20/20 scoped net ctest green;
GDExtension builds clean.

**CONFIRMED LIVE against `.204` (2026-06-12, `~/Desktop/capture_opennova2.pcapng`):** the full
out-game flow ran end to end — `GET /jop_2.gsb?a=1` (unauth on connect; logged as "7 real
servers", an UNDERCOUNT: the then-parser kept only the final SVRS record, D-NET-191) →
`GET /nwprepare.dll?ver1=3&cc=us&gt=jop:cus2` (substituted template) → `GET /NWStart.dll` →
`POST /NWLogin.dll` (all-encrypted) → two `GET /NWLogin.dll?tag=…` polls → auth as **ljim**
(`PCID A-A02-085D18`, matching the retail capture) → authenticated `jop_2.gsb` → two-phase
`NWJoin.dll?rid=167782351` → `.joi` → a **270-byte JointOperations ClientHello to the real game
host `207.178.209.204:3875`** (proto switch). The OpenNova client now logs in, browses, and joins
on genuine NovaLogic NovaWorld. (Confirmed: `.204`'s GSB is fetchable unauthenticated; the lobby
verify needs no login; the account login is for identity/join.) Minor follow-up: GSB server names
carrying a Latin-1 `©` (0xA9) trip a Godot UTF-8 warning — decode GSB strings Latin-1→UTF-8 for
display. The JointOperations session stops at the hello (ADR 0009 in-match seam — no gameplay yet).

### Wave 7 — full client↔NovaWorld parity sweep (2026-06-14)

Exhaustive 3-pass grill of all 24 client systems (`libs/novaworld` + `libs/novacrypto` +
`libs/napi` + the `NovaWorldClient` binding) against retail `Jointops.exe` (kong IDB). Each
reported divergence was adversarially re-verified by an independent skeptic that re-decompiled
the cited address (read-only multi-agent grill → refutation → this record). 35 divergences
confirmed in passes 1–2 plus the C4/C5/D1 set in pass 3; 11 claims refuted.

**Verdict table (24 systems)** — matching 9 · partial 11 · divergent 4

| System | Verdict | Note |
|---|---|---|
| A1 gate-probe | **matching** | crypto chain byte-verified end to end |
| A2 gate-response | partial | multi-radix literal parse witnessed, deferred low-value (D-NET-9); leniency fixes landed (D-NET-10..15) |
| A3 clienthello | partial | ServerHello FIXED (flat builder, D-NET-16/18); ClientHello DE/PV3/PM/ET still unmodeled (D-NET-17) |
| A4 clientauth | partial | CS-table/gating/JFC rejection fields fixed; remaining issues are outside A4 |
| A5 client-session-fsm | partial | Success atol, Cookie parent, ClientConnected timing (D-NET-19..22) |
| A6 protocol-message | partial | 0x80 selector + LEN8/16 fixed; frag reset/truncated stream remain (D-NET-7/8) |
| A7 verify-containers | partial | Success atol (= D-NET-19); verify-cookie claim refuted |
| A8 web-domain | **matching** | SessionInit CU web-domain install confirmed |
| A9 session-timing | **matching** | timeout value/citation + NWEC defaults FIXED (D-NET-23..25, 2026-06-27) |
| B1 nwu | **matching** | byte-exact (NW-C1) |
| B2 epask | **matching** | byte-exact (NW-C2); edge cases fixed (D-NET-26/27) |
| B3 pubcrypto | **matching** | byte-exact (NW-C3) |
| B4 url-cipher | **matching** | byte-exact (NW-C4) |
| B5 crc32-table | **matching** | table identical @0x849938, check 0x0376E6E7 |
| B6 napi-tlv | **matching** | statement-param length guard added (D-NET-28 FIXED) |
| B7 napi-envelope | **matching** | 4-byte mode exact; var-header mode out of scope (D-NET-29) |
| C1 http-login-build | partial | all 3 reported claims refuted; POST/all-encrypted model correct |
| C2 cookie-jar | **matching** | per-cookie `Cookie:` headers + subnet key ported (D-NET-30 FIXED) |
| C3 login-orchestration | partial | markup-derived URLs (D-NET-31) + shared cookie fix |
| C4 gsb-parse | **matching** | format fixed + byte-verified vs genuine `.204` (D-NET-32..36) |
| C5 joi-regurl | partial | documentation only; ':' separator + HOSTKEY trim confirmed |
| D1 join-handoff | partial | JO PV1 + dial-from-NK fixed; JO PG remains open (D-NET-49) |
| D2 client-play-request | **matching** | CurrentlyPlaying + ClientVarList wrap fixed; parseable by our server (D-NET-37..39) |
| D3 host-registration | partial | text blob corrected (D-NET-40..46); host request VarList wrapping remains tracked |

The cryptographic + framing foundation re-confirmed byte-exact (NW-C1..C4, CRC32, NAPI
TLV/envelope) — no code change. Defects concentrate in GSB (C4), the join/host client-direction
builders (D1/D2/D3), 0x0A runtime replacement, and remaining ServerHello/cookie edge cases.

### Wave 8 — 2026-07-01 branch-validation grill (in-match net core, `net-promote-to-core`)

Grilled the recent branch work (ADR 0013 consolidations + retail-join wire fixes + the 0x0A ported
subset) function-by-function against the kong IDB. Scope and verdicts:

| System | Reimpl | Verdict | Key witness |
|---|---|---|---|
| SessionSequencing/SessionCrypto framing | `frame_session_packet`/`deframe_session_packet` (`libs/npwire/protocol_message.{h,cpp}`) | **PARTIAL — framing, contiguous gate, and LAN loss recovery matching** | `CNapiNPConnection_SendSessionPacket @ 0x61edd0` stamps `[remote_key][seq][ack][u8 0]`; `CNapiNPConnection_ParseMessages @ 0x625bc0` admits only the contiguous frontier and prunes retained records by admitted ACK; `BuildMissingSeqList @0x6234b0` / `SendMissingSeqList @0x623560` emit client `0x44` or server `0x84` only after the receive pump drains; `NapiNP_HandleResendList @0x623800` validates the local key and rebuilds retained message records under the requested old sequence with the sender's current ACK. OpenNova now mirrors those paths in both directions, including the ordinary `0x43`/`0x83` receiver-local session-key gate, 16-sequence request cap, 100-packet future queue, same-batch reorder suppression, resend-list key validation, zero sentinel, and ACK retirement. LAN retention uses the witnessed `msg_out_max=0x4b0` override (`CNapiNetwork_Init @0x4cab20/@0x4cabf0`; combined-count guard in `NapiNPMessage_Create @0x627fc0`). The ordered gate and retention are explicit owner opt-ins. Generic lobby `ClientSession`/`NwUdpListener`, which have no witnessed `0x44`/`0x84` owner, now use a no-queue high-water policy: a strictly newer packet crosses a gap so permanent loss cannot deadlock the lobby, while sequence zero, stale packets, and duplicates are suppressed without ACK regression. Remaining tail: retail's retained-record timeout expiry and overflow-triggered disconnect policy are not modeled. |
| `np::slice_batch_pages` chunker | `libs/npruntime/batch_chunker.h` | **MATCHING (byte boundary)** — D-NET-135 fixed 2026-07-20 | budget 650 with per-pool margin, guard AFTER each record: 0x0C `+100 > 650` (`serialize_entity_states_to_buffer @ 0x5030a0` @0x50340d), 0x20 `+30 > 650` (`@ 0x503460` @0x503694), 0x10 `+40 > 650` (`serialize_pool2_static_to_buffer @ 0x5042F0`), 0x0D `+110 > 650` (`serialize_entity_pool_to_packet_0 @ 0x503940`). Phase order 0x10→0x0D→0x0C→0x20→0x45 confirmed (`Server_SendInitialGameStateToPlayer @ 0x51bba0` state-4 cases 1..5). |
| Retail-join player record (minimap flags / net_id / playerClass) | `build_pool0_organic_batch` (`libs/netsim/entity_wire_bridge.cpp`) | flags bit 0x100 + playerClass clamp **matching**; bit 0x01 model **divergent** (D-NET-136); net_id encoding **divergent-tolerable** (D-NET-137) | `Server_PlayerAdd @ 0x51cbc0` (`entity+36 \|= 1` @0x51d0da per-entity, remote adds only; class [5,9]-else-8 clamp @0x51d102; entity+120 = event+76 = connection_id @0x51d068); packer `lookup_entity_slot_and_pack_entry @ 0x57ad40` (@0x57ae47); decoder `MinimapSlot_FindByPackedId @ 0x57a270` (renamed from `sub_57A270`); client self-heal `NapiNPClientMsg_0x00C @ 0x42eadb`. |
| 0x22→0x46 ack-walk | `dispatch_session_replies case 0x22` + `encode_player_sync`/`_removal` | **FIXED to echo** (was server-computed) | §5.33 update: echo @ 0x505f05, client walk-terminator @ `NapiNPClientMsg_PlayerSync @ 0x431370` tail (`slot+1 < g_max_player_slots`, re-request `0x5CF7`); removal = 3-B early return @ 0x505f37; `Server_PlayerAdd` broadcast fieldFlags 0x1CF7 (`push 7415` @0x51d2bf). `cstr_fixed` misnomer → `cstr_capped` (strings are strlen+1 on the wire). |
| 0x0A ported subset | `build_0a_frame`/`emit_connection_s2c` (`libs/netsim/connection_fan.cpp`) | ported subset **matching**; deferrals correctly characterized (D-NET-134 stands); health byte **divergent (D-NET-138 — FIXED 2026-07-02**, pack ported from `Entity_GetHealthClassification @ 0x4AD4E0`; live v11: 0 C 0x0F**)** | `Server_SendEntityStateToPlayer @ 0x517ba0` (deploy gate `+32==6`, `++phase` before first write, eye ref, budget halving `+89876`/uptime>2000, unreliable send flags (0,1)); sub-block 0 = weapon/reload/uniform (`@ 0x4ff81b`; `FrameAimBlock` → `FrameWeaponBlock` rename everywhere); sub-block 1 values confirmed (C6EAE0=20/C6EAE4=13/fps/cpu/round-secs). |
| GameConfig unwitnessed fields | `libs/npruntime/game_config.h` | **all 7 named** (§6.9 update) | `Config_ParseSettingsLine @ 0x54f740` + `apply_session_settings_to_globals @ 0x551500` + `ServerConfig_ApplyHostSetting @ 0x4a6000`: `replay`/`max_team_lives`/`timeout`/`destroybuild`/`deathmes`/`TeamChoose`(bit 0x4 of the mpattrib store `dword_2550A04`)/`mp_allowsniperscopezoom`. |
| D-NET-127 post-handshake bodies | `emit_post_handshake_burst` (`server_message_dispatch.cpp`) | **fully witnessed; observation-carry closed** (§5.45 update) | trio owner = `CNapiServer_ProcessPendingPlayerSpawns @ 0x4c8dc0`; `0x03` = `NetPacket_WriteWeaponRestrictionFlag @ 0x502ac0`; `0x05` = `NetPacket_WriteBoolTrue @ 0x502c00`; `0x04` = `NetPacket_WriteSlotAssignment @ 0x502b30` (24-B field map); `0x00` pair = `CNapiNPConnection_SendConfigUpdate @ 0x6286e0`. |

Evidence tests: full ctest green post-fix (226 tests, incl. `npruntime_golden_lan_join`,
`npruntime_golden_lan_join_session`, `npruntime_handshake_server`, `netsim_two_peer_fanout`,
`novaworld_protocol_message`, `npruntime_batch_chunker`, `nw_message_coverage`).

IDB changes made during the session: `serialize_pool2_static_to_buffer @ 0x5042F0` (define_func over
two stale code-islands + rename from `loc_5042F0`/`sub_5042F0`, signature set),
`MinimapSlot_FindByPackedId @ 0x57a270` (rename from `sub_57A270`), correspondence comments on
0x61edd0 / 0x625bc0 / 0x5042F0 / 0x514c90 / 0x505e80 / 0x431370 / 0x4ff6b0 / 0x517ba0 / 0x4ad580 /
0x502b30 / 0x51cbc0 / 0x57ad40. Globals renamed: `dword_24D2120/30/4C/64/68` →
`g_replay_enabled`/`g_max_team_lives`/`g_respawn_timeout`/`g_destroy_buildings`/`g_death_messages`,
their `dword_2550xxx` cfg shadows → `g_cfg_*`, `dword_2550A04` → `g_mpattrib_flags`,
`dword_2550CA4` → `g_mp_allowsniperscopezoom`; `sub_502B30` → `NetPacket_WriteSlotAssignment`;
`CNapiNPConnection_SendSessionPacket @ 0x61edd0` prototype corrected to
`(NapiNPConnection *conn, unsigned int packet_seq)` (was a bogus `__thiscall(int *)`). IDB saved.

### Wave 9 — 2026-07-27 GSB row/stream-semantics grill (`worktree-gsb`)

Re-grilled C4 gsb (`libs/novaworld` `gsb.{h,cpp}` + the `http_listener` emit + the
`NovaWorldClient` consume) after the live browser showed a partial server list. Wave 7's C4
"matching" verdict was premature on two axes; both fixed (D-NET-190..193, D-NET-35 amended):

| Finding | Witness |
|---|---|
| Row dword1 = the host IPv4 (ping target), NOT a port | XXXX finalize → `NapiGameList_StartPingSweep @ 0x63bcf0` formats entry+4 (`Network_FormatIPAddressToString @ 0x62de80`) → `NapiPingEntry_Create @ 0x62ffb0` per row; results land per row via `NapiGameList_OnPingResult @ 0x63bc60` (raw -4→-3, -3/-2/-1→-2, 0→ms from transferInfo[11]) |
| Row dword0 = rid, the `@RID@` join substitution | `CLanServerBrowser_UpdateServerList_0 @ 0x660200` case 3 reads entries[row] dword0, `sprintf "%d"` @ 0x660386 splices it over `@RID@` (str @ 0x7e258c) in the markup NWJoin URL (`jop_2_main.mnx:818`, GLB_JOIN source="SERVER_LIST") |
| SVRS records accumulate; "GSB " = gated reset; undersized records skip | SVRS append loop @ 0x63dbec..0x63dc0d (no per-record clear); reset gate `payload dword0 == 0x00010000` @ 0x63d8f2 (frees fields + rows, zeroes ctx+120/+124, frees ping array ctx+136, event 7); FLDS/SVRS payload<2 skips @ 0x63d7c2 / @ 0x63da43; no record-level error path exists |
| Game-list ctx layout | +96 field names / +100 count / +104 cap / +108 entries (stride 20: rid, ip, values, playerCnt, names) / +112 count / +116 cap / +120 totalServers (+= declared) / +124 totalPlayers (+= row u16) / +128 parse offset (incremental HTTP callback; XXXX branch returns without advancing) / +132 ping conn / +136 ping results (init -1). Events: 2 ping-result, 6 list-updated (per SVRS when count 0 + on XXXX), 7 reset, 8 kv-append (`NapiGameList_AppendKeyValuePair @ 0x63dea0`) via `Observable_NotifyListeners @ 0x63b140` |

Live proof: the genuine `.204` fixture = **8 SVRS records / 58 rows**, all 58 decoding to
public IPv4s; a live `GET /jop_2.gsb?a=1` (2026-07-27) = 8 records / 52 rows whose FINAL
record holds one row — the old clear-per-record parser surfaced exactly that one server
(the reported symptom). Evidence tests: `gsb_roundtrip` (positional in_addr byte assert),
`gsb_parse_roundtrip`, `gsb_retail_semantics` (NEW — accumulate / gated reset / undersized
skip / terminator, via real-builder chunk splices), `gsb_real204_decode` (58/58 non-zero-IP
oracle; argv override runs the same decoder on live blobs), `http_flow`, and
`novaworld_panel_test.gd` (browser rows carry the address).

IDB changes made during the session: renames `NapiGameList_StartPingSweep @ 0x63bcf0`,
`NapiGameList_OnPingResult @ 0x63bc60`, `Observable_NotifyListeners @ 0x63b140`,
`NapiGameList_AppendKeyValuePair @ 0x63dea0`, `NapiGameList_StartFetch @ 0x63dd10`
(user-approved rename of the curated misnomer `load_xml_content` — it starts the GSB HTTP
fetch and stores the ping-sweep gate flags at ctx+68, nothing XML), locals
`rowRid`/`rowIpAddr` in 0x63d740; 15 corrected comments incl. the 0x660333 `@RID@` witness
and replacing a wrong FVNG/GRTG/SVHX tag comment (real tags: GSB /FLDS/SVRS/XXXX). IDB saved.

**Wave 9 addendum — joiner mission bring-up (same day, after live custom-map joins failed).**
Engine-research pass answering "why can't we join custom-map servers": retail's joiner NEVER
opens the mission `.bms` — `Game_StartMission @ 0x524360` authority-gates every mission-file
leg, and the non-authority arm builds terrain/env from the wire 0x0B header
(`g_BmsHeaderBlock @ 0xA761D0`) with the world contents arriving as the §5.2a spawn stream.
There is NO map-download mechanism in JO retail and none is needed. Full chain + our
divergence (local-`.bms` requirement + a native teardown segfault on the abort path):
D-NET-194; §5.28 correction; §5.4 field notes. Second IDB batch: renames
`NapiNPClientMsg_0x00B @ 0x422660` → `NapiNPClientMsg_HandleBMSHeader`,
`MultiByteStr @ 0xA76214` → `Bms_MapBaseName`, `byte_A761D0` → `g_BmsHeaderBlock`; witness
comments @ 0x524751 / 0x524d8f / 0x524df1 / 0x40e250 / 0x6109ce; user-approved rename
`Bms_TerrainBaseName @ 0xA762E8` → `Bms_TileSetName` (it derives `<name>.TGA`/`<name>.TSD`
and feeds `XML_ParseTileInfo @ 0x4cc830` — the tile set, not the terrain). IDB saved.

## 8. D-NET divergence catalog

Every D-NET divergence, grouped by reimpl area. IDs are stable and never renumbered;
entries accumulate from the grill sessions (the §5 findings and the §7 waves) and keep the
status tag recorded at their last update (FIXED = applied with green ctest; TRACKED =
confirmed, fix specified, not yet applied). Dispositions map to the canonical vocabulary
in [divergence-ledger.md](../divergence-ledger.md).

`session_hello.cpp` (A4 ClientAuth/ServerSessionInit 0x42/0x82):
- **D-NET-1** [HIGH, FIXED] CS field default tables were onnet guesses, wrong at idx 4/8/9/10/12/13. Engine template (IDENTICAL both directions): `{0:240000,1:4,4:60000,5:1000,6:0xFFFFFFFF,8:2048,9:128,10:100,11:500,12:1,13:MTU(1300),14:0xFFFFFFFF}`. [orig: CNapiGameSession_InitNPConnection @ 0x4d3e1f / CNapiNPConnection_Create @ 0x62acb0 / CNapiNPConnection_SendSessionInit @ 0x620ef0]
- **D-NET-2** [LOW, FIXED] CI/HK/CK were emitted unconditionally; retail gates each on non-zero (like SIP/SPN). [orig: CNapiNPConnection_SendClientJoin @ 0x61fe20]
- **D-NET-3** [LOW, FIXED] `parse_server_auth` now parses JFC/JFP/JFS rejected-join fields (failure code/param/string); SCRK-less rejections are valid auth packets and surface as rejections, not malformed. [orig: NapiNP_HandleServerJoinResponse @ 0x629840]
- **D-NET-4** [LOW, FIXED] RIP/RPN were emitted unconditionally; retail gates on peer_addr/peer_port != 0. [orig: CNapiNPConnection_SendSessionInit @ 0x620ef0]

`protocol_message.cpp` (A6):
- **D-NET-5** [HIGH, FIXED] high-table/include-seq selector is flag bit **0x80**, not 0x01; `full_tag = (flags&0x80?0x100:0)|tag`; wire bit 0x01 is unused/reserved. [orig: CNapiNPConnection_DispatchMessage @ 0x622570 / NapiNPProtocol_FindMsgInfo @ 0x61e380 / NapiNP_WriteMessageRecord @ 0x61da90]
- **D-NET-6** [HIGH, FIXED] LEN8(0x20)/LEN16(0x40) parse precedence inverted — retail tests LEN8 FIRST. [orig: CNapiNPConnection_ParseMessages @ 0x625bc0]
- **D-NET-7** [MED, FIXED] reassembly clears the buffer on the FIRST fragment (`(flags&6)==4`) before appending — implemented in `reassemble_protocol_payload` (`protocol_message.cpp`, verified 2026-06-27; carries a regression-history note vs an earlier `frag_first && !frag_end` rewrite). [orig: CNapiNPConnection_DispatchMessage @ 0x622570 / NapiBuffer_SetLength @ 0x634220]
- **D-NET-8** [LOW, FIXED 2026-06-27] truncated inner stream: the original substitutes 0 for missing fields and still dispatches the final partial message (read from its zero-padded 64 KB buffer), then stops — it does not bail. `parse_protocol_messages` (`protocol_message.cpp`) now emits the partial message (payload = available bytes zero-padded to the claimed length) on a truncated LEN8/LEN16/SKIP/body field instead of dropping it; valid packets are unaffected. Test: `protocol_message` `check_truncated_message_is_dispatched_zero_padded`. [orig: CNapiNPConnection_ParseMessages @ 0x625bc0]
- **D-NET-164** [HIGH, PARTIAL — contiguous gate + LAN NACK/resend PORTED 2026-07-23; generic high-water policy 2026-07-24] Retail validates the receiver-local key before reading session sequence state, then dispatches only across the contiguous inbound frontier. The LAN/game-session opt-in consumes stale/exact duplicates without redispatch, queues future packets (cap 100), and drains them in order when a gap closes. A future packet latches a missing check; the host and joiner resolve it only after their receive FIFO drains, so seq 3 then seq 2 in one batch sends no needless NACK, while a surviving gap emits one direction-correct `0x84`/`0x44`. The body is the peer-local key followed by up to sixteen missing-sequence dwords — a BUILDER cap: `SendMissingSeqList` passes max 16 into `BuildMissingSeqList @0x6234b0` (`missing_seq_buf[16]` @0x62367a), while the RECEIVER validates its own key then walks every dword present up to the 0x10000 size guard (`buf+size-4` loop @0x623917..0x6239da) — the reimpl decoder deliberately mirrors the uncapped receiver. A requested sequence of ZERO is live retail behavior, not a dead sentinel: the handler substitutes `out_packet_seq+1` (@0x6239a4), sends that fresh next-sequence packet, and advances `out_packet_seq` when it minted one (@0x6239c9..0x6239e3). Senders retain reliable message records by assigned sequence and reconstruct a requested packet with its old sequence plus the current ACK; only ACKs from packets admitted through the contiguous gate retire records. LAN/game connections opt into the directly witnessed 1200-record cap (`CNapiNetwork_Init` loads `ebp = 0x4B0` @0x4ca9dc and stores it to both connection-config profiles @0x4cab20/@0x4cabf0; `NapiNPMessage_Create @0x627fc0` checks pending + retained + 1). Generic lobby `ClientSession` and `NwUdpListener` remain recovery/retention-disabled because no NOVAWORLDUDP NACK owner is witnessed, but they no longer directly redispatch arbitrary order: their shared no-queue high-water gate admits newer sequences across a permanent loss and suppresses zero/stale/duplicate traffic. Regressions cover receiver-key mismatch, both true-loss game directions, malformed/key-mismatched requests, current-ACK reconstruction, ACK retirement, same-batch reorder suppression, lobby late/duplicate/permanent-loss behavior, the standalone JO listener's real socket-drain NACK, queue bounds, and encode-failure transactionality (`npruntime_session_resend`, `novaworld_protocol_message`, `nw_host_register_e2e`). **Residual:** retained-record timeout expiry and retail's overflow-disconnect side effect remain unported; the exact LAN count bound is enforced. [orig: NapiNPProtocol_HandleSessionPacket @0x626b72 / CNapiNPConnection_ParseMessages @0x625bc0 / BuildMissingSeqList @0x6234b0 / SendMissingSeqList @0x623560 / NapiNP_HandleResendList @0x623800 / CNapiNPConnection_BuildOutgoingPackets @0x628430]
  **D-NET-164 active-send refinement (PORTED 2026-07-23, interval corrected 2026-07-24):**
  retention alone does not resend a
  semantic packet. With retained records still outstanding, retail's send-interval pump waits
  strictly more than 10000 ms — the JOINTOPERATIONS template's `active_send_interval_ms`
  (`CNapiNetwork_Init @ 0x4ca4a0`; the 1000 ms first ported here was the NOVAWORLDUDP service
  template's value @0x4d3e60) — and mints a **new header-only sequence**. The induced gap makes the
  receiver emit `0x44`/`0x84`; `NapiNP_HandleResendList` then reconstructs the requested old
  semantic sequence from retained records while stamping the current ACK. Ordinary queued
  application output wins that send boundary and resets the interval, so no redundant empty packet
  precedes it. OpenNova mirrors this on both the joiner and host—the latter is required to recover
  a dropped initial-settings packet. `npruntime_joiner_handshake_retry` pins no probe at exactly
  10000 ms, a probe after 10000 ms, and recovery in both directions. [orig:
  `CNapiNetwork_Init @ 0x4ca4a0` /
  `CNapiNPConnection_PumpSendIntervals @ 0x628fd0` /
  `CNapiNPConnection_BuildOutgoingPackets @ 0x628430` /
  `CNapiNPConnection_SendSessionPacket @ 0x61edd0` /
  `NapiNP_HandleResendList @ 0x623800`]
- **D-NET-165** [LOW, OPEN + NEEDS-RE] The LAN `0x81` ServerHello omits `P2` and `SUS1` (both unmodeled live host state) instead of replaying capture-shaped placeholders (§5.0c). Stock hosts populate them; whether a retail browser tolerates their absence (blank row column vs dropped row) is unwitnessed — settle by pointing a stock client's LAN browser at an OpenNova host. [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0 (conditional per-tag writes); Nwu_HandleServerHello @ 0x626d20 (the browser consume)]
- **D-NET-166** [MED, OPEN] The C2S JOIN `VERSIONCRCSTRING` is emitted as the constant `"0"`. When the host runs an expansion, `Server_ValidatePlayerJoinRequest @0x512100` compares `atol()` of the uploaded string against its `g_expansion_checksum` @0xb4c5a4 (reject DPC=48) — retail computes that checksum as CRC-32/MPEG-2 over the loose `expansion/<name>/version.txt`. `"0"` matches every install without that file (the live revx02 golden) and is rejected by any host whose install carries one. Close by plumbing the runtime resource path into the joiner and computing the same CRC over the same file (§5.0d).
- **D-NET-167** [LOW, OPEN] The game ClientAuth never carries the join-password `FID` or team-choice `JSP` CUs. `@0x512100`'s squad-password (DC=21) and side-password (DC=18/19/20, `jsp[60]` team choice) legs therefore reject every OpenNova join to a password-protected host. Close with a join-password prompt feeding `FID` (+ `JSP` for the team preference). LAN-reachable: retail LAN hosts can set passwords.
- **D-NET-168** [MED, FIXED 2026-07-24] The joiner's post-`0x1A` `0x2F` pair uploaded a FIXED default kit (capture-shaped header `02 08 C3|D4` + seven ADM rows) — the shell's applied local kit had no wire seam, so the host's granted per-slot table reflected the default, not the player's pick. Closed by the client-builder witness (§5.56, `NetPacket_SendLoadoutSubmit @ 0x42cdc0`): `JoinerConnection` now latches the wire team from the S2C 0x04 tail byte (`byte_A85B48` parity) and composes both submissions from the binding's `set_loadout_kit` seam (`NovaSimulation::push_joiner_loadout_kit` — the applied spawn kit's ADM rows, the latched class, slot 195 then the equipped combo, mirroring `Game_StartMission @ 0x525836/@ 0x525c2e`). Headless callers keep the capture-default kit byte-for-byte. Pinned by `npruntime_client_runtime` (exact canned pair under the 0x04 team; injected kit through the zones e2e) and `nw_ingame_encode` `loadout_submit_roundtrip`.
- **D-NET-169** [MED, OPEN (guarded) + NEEDS-RE] Self-identification stays NAME-MATCH (D.0/§5.23) while retail's is numeric (`Player_FindLocalPlayerEntity @0x4e0090` walks the player table by ConnectionId/dcb; the roster binding arrives via 0x4D player-index + 0x46 player-sync `entity_slot_id`, §5.21). Name-match cannot disambiguate two live players sharing a callsign. GUARDS (2026-07-24): the joiner fails the join with a duplicate-callsign error when a second same-name organic record with a different slot arrives pre-release (post-release it keeps its latched handle), and the shell's default callsign is uniquified per machine (`NovaPlayerProfile`). Burn-down = witness the 0x4D semantics and port the numeric walk.
- **D-NET-170** [HIGH, FIXED 2026-07-24] S2C `0x5A` is now an authoritative receive-side state channel, not merely a deploy-release signal: `ClientRuntime` retains the newest decoded `WeaponLoadout` with a revision, and `NovaSimulation` rebuilds the local slot pool from that grant before actions without sending a new C2S `0x2F`. S2C `0x6F` and `0x53` are retained per zone; the DEATH list overlays BMS zone identity with the live 0x6F team/value/limit secured gate. Real-UDP tests pin mid-session grant replacement and live zone removal/reappearance; `deploy_screen_presenter_test.gd` pins stable selection by the zone's wire parameter rather than row index. [orig: `WeaponLoadout_ApplyFromBuffer @0x4290E0`; `UI_UpdateDeathScreenContent @0x5536a0`]
- **D-NET-175** [HIGH, FIXED 2026-07-24] The full periodic request trio is live (§5.34): one holdoff-gated, MTU-batched send boundary carries `0x1C`, `0x08`, and `0x3D`. `0x3D` pages the renderer-finalized registry of unique loaded non-foliage `.3DI` definitions, frozen before world reveal, and later S2C spawns cannot mutate it. `0x1C` now comes from the real boot-soft `charattr.def` domain: an exact 16×124-byte CHARACTER table with retail section/key/value semantics, wrapped class selection plus active/id validation, and ordered S2C `0x41` property clears. The stock class-8 row CRC `0x22A25E01` reproduces two independent retail replies; missing resources and invalid classes retain retail's zero result.

`gate_response.cpp` (A2):
- **D-NET-9** [LOW, WITNESSED — deferred low-value] `NapiScript_ParseLiteralValue @0x62db00` tries, in order: char-literal `'x'` → `NapiScript_ParseHexValue @0x62d610` → `parse_octal_integer @0x62d7b0` → `parse_binary_literal @0x62d900` → `NapiScript_ParseDecimalIntegerB @0x62da20`. The reimpl parses gate port/literal fields as DECIMAL only. Real gate responses are decimal, so the other four radixes are unexercised robustness — re-prioritized MED→LOW; the 4-radix port is witnessed-and-ready but deferred (poor value/risk). [orig: NapiScript_ParseLiteralValue @ 0x62db00]
  - **Complete parser semantics (2026-07-05, so the future port is a clean lift):**
    - **char** `'x'`: `input[0]=='\''` && `input[1]!=0` && `input[2]=='\''` → value = `(int)input[1]`, consumed = 3.
    - **hex** (`@0x62d610`): prefix `$` (Pascal) | `0x`/`0X` (C) | bare digits + `h`/`H` suffix (asm). Digits `0-9A-Fa-f`; a `.` after a digit → fail (it's a float); bare form REQUIRES the `h`/`H` suffix; prefixed form rejects a trailing alpha other than `h`/`H`. value = `16·v + digit`.
    - **octal** (`@0x62d7b0`): prefix leading `0` | bare digits + `o`/`O` suffix. Digits `0-7`; a `.` → fail; leading-`0` form fails if the terminator isdigit (`08`, `09`) — so `017` parses as octal 15; bare form REQUIRES `o`/`O`. value = `8·v + digit`.
    - **binary** (`@0x62d900`): prefix `%` | bare digits + `b`/`B` suffix. Digits `0/1`; a `.` → fail; bare form REQUIRES `b`/`B`. value = `2·v + digit`.
    - **decimal** (`@0x62da20`): digits `0-9` + optional `d`/`D` suffix; a `.` → fail; value = `atol`. **No sign** — the literal parser is UNSIGNED (`-`/`+` is not handled here; a signed field is a separate atoi path).
    - **Decimal-safety of the try-order:** plain `"80"`/`"16"` fail hex/octal/binary (no prefix/suffix; `8`,`9` aren't octal, non-`0/1` aren't binary) and fall to decimal — so routing decimal gate values through the full dispatcher is safe. The ONE behavior change vs our decimal-only path is leading-zero numbers (`"017"` → octal 15, retail-faithful) and the `d`/`h`/`o`/`b` suffixes.
  - **Integration finding:** the obvious reimpl call site `gate_response.cpp` `parse_int` (a strict SIGNED full-string decimal validator) is **dead code — zero callers**; the live gate-value paths are `atoi_loose` (METPING) and gsb `parse_int_or_zero`, both decimal. So the port is not a drop-in swap: it needs (a) a faithful unsigned `napi_parse_literal_value` in `libs/npwire` with the 5-radix + consumed-count contract above, and (b) identifying WHICH retail gate fields route through `NapiScript_ParseLiteralValue` (vs a plain atoi) before wiring — a small NEEDS-RE refinement gating the otherwise-mechanical port.
- **D-NET-10** [MED, FIXED] store full 32-bit port; drop the [0,65535] reject (retail stores verbatim, presence = non-zero). [orig: CNapiGateManager_ProcessResponse @ 0x4ced20 (@ 0x4cf1ae)]
- **D-NET-11** [MED, FIXED] IPv4 octets >255 accepted (mask to uint8), not rejected. [orig: Network_ParseIPv4AddressOctets @ 0x62dc10]
- **D-NET-12** [LOW, FIXED] IPv4 parse stops after the 4th octet, ignores trailing chars. [orig: 0x62dc10]
- **D-NET-13** [LOW, FIXED] remove CUS/PVT phantom keys (exactly 19 real keys; CUS/PVT counted-but-ignored). [orig: 0x4ced20]
- **D-NET-14** [LOW, FIXED] `is_ws` should match `isspace` (add \v 0x0B, \f 0x0C). [orig: String_TokenizeQuotedToArray @ 0x616d60]
- **D-NET-15** [LOW, FIXED] `atoi_loose` must skip leading whitespace (atol semantics). [orig: 0x4ced20 (atol @ 0x76ab0a)]

`session_hello.cpp` (A3 ClientHello/ServerHello):
- **D-NET-16** [MED, FIXED 2026-06-27] `server_hello_to_bytes` (`session_hello.cpp`) is now the witnessed FLAT builder: SF emitted UNCONDITIONALLY, P1/P2/NP/MP each only when nonzero, NO PL tag, SUS1/SUS2 gated on non-empty — the `is_game_server`/PL two-branch is removed from the encoder (the parser stays lenient so decoders still read a stray PL). Grilled vs the full decompile @0x6204b0 (CI/CO/AP/BDAT/DE/UT/PN/PG/PV1/PV2/PV3/HK/SN/SF/P1/P2/P3-P8/NP/MP/NPW/NC/RIP/RPN/SUS1-4/RIPE/EPN/ET, each individually gated). Tests: `session`/`client_session_loopback`/`golden_lan_join_session` green. [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0]
- **D-NET-17** [LOW, TRACKED] ClientHello DE/PV3/PM/ET fields unmodeled (gated off for the stock client, so byte-correct for the common case). The SERVER side of these (DE/PV3/ET) is now witnessed @0x6204b0; the ClientHello PARSER modeling them is the remaining work. [orig: NapiNPSession_SendAnnouncePacket @ 0x61fa00]
  **Full announce-TLV field map (witnessed 2026-07-05 @ 0x61fa00, in emit order):** `NVS` (version string, always) → `CO` company / `AP` app-name / `BDAT` bin-data (each if its string is non-empty) → **`DE`** = `session_info[54]` u32, iff nonzero → `PN` host-name → `PG` session GUID 16 B iff `!NapiGUID_IsNull` → `PV1`/`PV2`/**`PV3`** version strings (each if non-empty; `PV3` = `session_info+428`) → `CI` = `session_param[5]` u32 iff set → **`PM`** = `transport_info[36]` u8-as-u32 player count, iff `!transport_info[37]` → `EIP` = `target_addr` / `EPN` = `target_port` (each iff nonzero) → **`ET`** = `extra_param` u32 iff nonzero. So the four D-NET-17 fields are all CONDITIONAL — absent in the stock host announce (their gates false: DE/ET zero, PV3 empty, PM only when transport_info[37]==0). Modeling them faithfully needs the session-state sources (`session_info[54]`=DE, the PV3 version string, `extra_param`=ET) which the reimpl does not yet track — the reason this stays LOW/deferred, not a quick port.
- **D-NET-18** [LOW, FIXED 2026-07-22] `server_hello_to_bytes` field order/gating now matches @0x6204b0 for the modeled field set (SF unconditional, never PL, UT/count fields nonzero-gated, SUS non-empty-gated). The final modeled-field residual, an unconditional zero-valued `UT`, is closed and pinned by `session_hello_roundtrip_test`. [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0]

`client_session.cpp` (A5/A7):
- **D-NET-19** [MED, FIXED] `Success` compared as exact "1"; retail uses `atol(Success) != 0`. [orig: CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800]
- **D-NET-20** [MED, **FIXED 2026-07-05**] `build_verify_request` now emits the `ClientVarList(VarList="Cookie")` parent unconditionally (empty when no cookie vars are configured), matching retail's SerializeVarList includeAll=1; pinned by the empty-cfg flow in `client_session_loopback_test`. [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 / NapiStatement_SerializeVarList @ 0x4d0660]
- **D-NET-21** [LOW, **FIXED 2026-07-19**] `ClientConnected` now leaves `ClientSession::process_periodic_update` exactly once after `ServerSessionInit` moves the client to `Verifying`; handling opcode 0x82 itself emits no reply. `NovaWorldClient` and `NovaWorldHost` call the periodic boundary after draining inbound datagrams, and both the deterministic loopback and UDP host-registration harnesses exercise it. Test: `client_session_loopback` pins the silent synchronous handler, first-periodic emission, and no repeat on later updates. [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400, `conn_state==5 && session_state==2` -> CNapiGameSession_SendClientConnected @ 0x4cfe30]
- **D-NET-22** [LOW, BINDING] the verify Cookie var-list is data-driven (locale + NW* identity) from client env — registry/Win32 glue belongs in the Godot binding, not `libs/`. Also fix the `client_session.h:99-110` comment. [orig: CNapiSession_ReadLocaleInfo @ 0x4ce390 / CNapiGameSession_SendLocaleAndVerify @ 0x4d57e0]

`napi/session.{h,cpp}` (A9):
- **D-NET-23** [MED, FIXED 2026-06-27] `SESSION_CONNECT_TIMEOUT_MS` corrected 20000→**60000** (0xEA60 — the ConnectOrHost connect/host poll, witnessed as the immediate in BOTH GetTickCount loops @0x4d4f10); the 20000ms (0x4E20) periodic-update timeout is now its own constant `SESSION_PERIODIC_UPDATE_TIMEOUT_MS`. (`libs/napi/session.h`; `napi session` test.) [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10 / ProcessPeriodicUpdate @ 0x4d4400]
- **D-NET-24** [LOW, FIXED 2026-06-27] `SESSION_HANDSHAKE_RETRANSMIT_MS` was a misnomer (a 1300-BYTE message chunk size, not a ms interval) — renamed `SESSION_MESSAGE_CHUNK_BYTES`. [orig: CNapiNPConnection_QueueMessage @ 0x628640]
- **D-NET-25** [LOW, FIXED 2026-06-27] added `Reject1009`→NWEC14; `novaworld_error_from_code` unknown-NONZERO-reject default now → `UnknownReject`→NWEC13 (was wrongly TimeoutPoll→NWEC02; NWEC02 is the code -1 poll-timeout path). (`libs/napi/session.{h,cpp}`.) [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10 dword_B60110 switch]

`novacrypto/epask.cpp` (B2, edge-case only):
- **D-NET-26** [LOW, FIXED] `epask_from_string` uses `_atoi64` semantics (return 0, no throw). [orig: parse_colon_delimited_string @ 0x666710]
- **D-NET-27** [LOW, FIXED] `epask_encrypt` truncates plaintext at first NUL (strlen). [orig: EPASK_Encrypt @ 0x6669a0]

`napi` tlv/envelope (B6/B7):
- **D-NET-28** [LOW, FIXED 2026-06-27] The statement-param limits are WITNESSED real at `NapiStatementParam_Create @0x632b30` (name `strlen-1 > 0x3E` ⇒ 1..63; `dataSize >= 4096` ⇒ 0..4095; reject = error flag + null). `make_client_var_list` (`libs/napi/session.cpp`) now skips a ClientVar whose name/value data exceeds 4095 (the param names are the fixed VarFNum/VarName/VarValue literals, always in [1,63]) — the faithful reject. Note: this is the gate STATEMENT layer, distinct from the in-match TLV codec `NapiNP_WriteTLV @0x61dd60`, which uses a plain u16 length (0xFFFF) with no such limit — our `libs/napi/tlv.cpp` already matches that. [orig: NapiStatementParam_Create @ 0x632b30]
- **D-NET-29** [LOW, SCOPE] envelope variable-header (first-dword==0) decode mode unsupported — documented scope decision. [orig: NapiNP_UnpackPacket @ 0x62ca20]

`http_login.cpp` + binding (C2/C3):
- **D-NET-30** [MED, **FIXED 2026-07-05**] `LobbyHttpFlow::request_headers` now emits one `Cookie: name=value;` header PER cookie (trailing `;`, insertion order) via `CookieJar::cookie_header_lines()`, matching retail's per-entry `sprintf("Cookie: %s=%s;")` loop — not a single merged line. Our own server already expected this (`request_cookie_header` merges multiple `Cookie:` headers; the merged-line client was the inconsistency). The subnet key is ported as the free `subnet_key()` (valid dotted IPv4 -> first two octets, else unchanged); the gate/login/host/join/GSB family shares one host, so the single-jar model still holds and the key is applied when a caller keys per host. [orig: CUIBrowser_SendHTTPRequest @ 0x658840 (per-cookie `Cookie: %s=%s;`); Network_TruncateIPToSubnet @ 0x62dfe0 (IPv4 /16)]
- **D-NET-31** [LOW, DOC] login URLs/params are markup-derived (`nw_startup.mnx`), not C literals; `[CC]`/`[GT]` tokens and the `[domainname]` lower-casing are non-retail. [orig: gate STARTUPURL via 0x4ced20]

`gsb.cpp` (C4) — FIXED, **byte-verified against the genuine `.204` blob**
(`fixtures/novaworld/nw204_jop_2.gsb`, `gsb_real204_decode_test` — 8 SVRS records /
58 servers, all with public IPv4s, 26 FLDS columns, decoded through the XXXX
terminator; the earlier "8 servers" reading here was itself the D-NET-191
clear-per-record artifact):
- **D-NET-32** [HIGH, FIXED] no bare "GSB " file header — "GSB " (0x20425347) is the FIRST chunk's TAG (reset/init; payload dword0==0x00010000). [orig: NapiGameList_ProcessEncryptedResponse @ 0x63d740]
- **D-NET-33** [HIGH, FIXED] chunk layout is `[magic:4 @+0][len:u32 @+4][payload @+8]`, advance len+8 — magic is a PREFIX, not the suffix we emitted. [orig: 0x63d740 (@ 0x63d78b / 0x63d76c / 0x63d781)]
- **D-NET-34** [HIGH, FIXED] tags: GSB =init, FLDS=field-names, SVRS=rows, XXXX=terminator; dropped the bogus FLDS-as-summary/TotalServers chunk. The 26 FLDS column names+order are confirmed IDENTICAL to retail. (`.204` also sends an "IVAR" chunk between GSB and FLDS, but the retail parser — and ours — ignore unknown tags, so the builder omits it harmlessly.) [orig: 0x63d740]
- **D-NET-35** [HIGH, FIXED; **AMENDED 2026-07-27**] SVRS row = `[u32 rid][4-byte host IPv4]` then 26 positional NUL-term values (FLDS-keyed) then `[u16 playerCount]` then player names. The FIRST u32 is the host id / join `rid` — retail's "serverIP" parser local is a misnomer for THAT dword (the `.204` values, e.g. 0x0A0027A0, sit in the Wave-6 join-`rid` range and are not addresses), and the CONNECT address still arrives via the NK join token. This entry originally called the SECOND dword a `u32 port`; that was wrong — it is the host's IPv4 in in_addr byte order, the ping target (see D-NET-190). We previously misread dword0 as an IP and dropped the player list. [orig: 0x63d740 (@ 0x63da60..)]
- **D-NET-36** [LOW, DISPLAY] GSB strings are Latin-1 — transcode to UTF-8 at the Godot display layer, not the parser. [orig: 0x63d740]
- **D-NET-190** [HIGH, FIXED 2026-07-27] Row dword1 is the host's IPv4 in in_addr byte order — NOT a port. On the XXXX finalize retail walks every accumulated row and pings entry+4: `NapiGameList_StartPingSweep @ 0x63bcf0` formats it via `Network_FormatIPAddressToString @ 0x62de80` and creates one ping entry per row (`NapiPingEntry_Create @ 0x62ffb0`); results land per row via `NapiGameList_OnPingResult @ 0x63bc60` (raw -4→-3, -3/-2/-1→-2, 0→ms from transferInfo[11]; event 2). Our builder emitted the u16 host port as a LE u32 there, so a retail browser pointed at an OpenNova list pinged garbage addresses (port 64206 → 206.250.0.0). `GsbServerEntry.port` → `std::string ip` (dotted; emitted/parsed as the raw 4 in_addr bytes, unparseable → 0.0.0.0); `http_listener` fills it from `HostRow.host_ip`; the `NovaWorldClient` row dict carries `ip` (the dead `port` key is dropped). Pinned by the positional in_addr byte assert in `gsb_roundtrip_test` and the 58/58 non-zero public IPv4s of the genuine `.204` fixture. [orig: NapiGameList_StartPingSweep @ 0x63bcf0]
- **D-NET-191** [HIGH, FIXED 2026-07-27] SVRS records ACCUMULATE across the stream — retail's SVRS arm appends rows to the ctx entry array with NO per-record clear (append loop @ 0x63dbec..0x63dc0d; totals ctx+120/+124 both `+=`). Our parser reset the list per record, so only the final SVRS chunk survived — the live "missing servers" symptom: the genuine `.204` fixture carries 8 SVRS records / 58 rows, and the 2026-07-27 live `jop_2.gsb?a=1` carried 8 records / 52 rows whose final record holds ONE row (the old parser showed exactly that one server). `parse_servers` now appends; only a valid "GSB " reset (or a new fetch) empties the list. Pinned by `gsb_retail_semantics_test` (multi-SVRS splice of real builder chunks). [orig: NapiGameList_ProcessEncryptedResponse @ 0x63d740 (@ 0x63dbec..0x63dc0d)]
- **D-NET-192** [MED, FIXED 2026-07-27] The "GSB " record is the RESET, gated: when its payload begins with u32 0x00010000 (@ 0x63d8f2) retail frees the field table + every accumulated row, zeroes the totals (ctx+120/+124), frees the ping-results array (ctx+136), and fires event 7; ANY other "GSB " payload (short or mismatched) is skipped with no reset. FLDS/SVRS records with payload < 2 are likewise skipped in place (@ 0x63d7c2 / @ 0x63da43) — retail has no record-level error path at all. Ported: mid-stream gated reset + undersized-record skips in `gsb_parse_response`; pinned by `gsb_retail_semantics_test` (valid reset clears, mismatched "GSB " does not, undersized FLDS/SVRS/GSB records skip without clobbering state). [orig: 0x63d740 (@ 0x63d8f2 / @ 0x63d7c2 / @ 0x63da43)]
- **D-NET-193** [LOW, DOC — deliberate host hardening] Retail's parser is an incremental HTTP callback with NO failure return: the parse offset persists at ctx+128, the XXXX branch returns without advancing, and a short buffer simply waits for more body. `NapiGameList_StartFetch @ 0x63dd10` (renamed this session from the curated misnomer `load_xml_content`) starts the fetch and stores the ctx+68 flags that gate the ping sweep; UI events flow through `Observable_NotifyListeners @ 0x63b140` (list-updated fires per SVRS record when count is 0 and on XXXX; kv pairs via `NapiGameList_AppendKeyValuePair @ 0x63dea0`). Our `gsb_parse_response` is deliberately ONE-SHOT and bounds-checked — it requires the XXXX terminator and rejects short/forged buffers (64-bit length check) — documented hardening on server-supplied bytes, not a parity bug. rid signedness note: `CLanServerBrowser_UpdateServerList_0 @ 0x660200` prints the rid `%d` (SIGNED) into the `@RID@` splice; our server-side `std::stoul` wraps negative text back to the same u32, so values round-trip. [orig: 0x63dd10 / 0x63b140 / 0x63dea0 / 0x660200]

`napi/session.cpp` (D2 ClientPlayRequest) — FIXED (now parseable by our own server):
- **D-NET-37** [HIGH, FIXED] `make_client_play_request` now emits the top-level `CurrentlyPlaying` field (decimal of the flag) FIRST. [orig: CNapiGameSession_SendPlayRequest @ 0x4d3920]
- **D-NET-38** [HIGH, FIXED] var-lists are wrapped as `ClientVarList` containers (a `VarList` field carrying the list name + `ClientVar` children with VarFNum/VarName/VarValue) via the new `make_client_var_list` helper — the shape `extract_var_lists` parses. (`make_client_host_request` still needs the same treatment — tracked under D3/host.) [orig: NapiStatement_SerializeVarList @ 0x4d0660]
- **D-NET-39** [MED, FIXED] var-list child order is Cookie then PlaySetup; the stale `session.h` `SendPlayRequest @ 0x4af990` citation corrected to `0x4d3920`. Locked by `session_test`. [orig: 0x4d3920]

`lobby_update.cpp` (D3 host registration):
- **D-NET-40** [HIGH, FIXED] remove the fabricated `is_delete` / "Port = -1 DELETE" / 4×-send path (no such string in the binary; single SendUDPPacket). Teardown is a separate mechanism (likely ClientStopHosting TLV @ 0x4d04e0). Also fix header anchor 0x4d2e10 → 0x4fe8c0. [orig: Lobby_UpdateServerInfo @ 0x4fe8c0]
- **D-NET-41** [HIGH, FIXED] sanitize HostKey + every key/value/player name: ' '/'?'/'@'/'=' → '+', empty → "---" (NOT lobby_name). [orig: String_SanitizeForLobby @ 0x4fe750]
- **D-NET-42** [HIGH, FIXED] key set: drop HostDID/AccessCodeList; add Mod/Msg/GCC; rename Uptime→Age, TimezoneBias→TZB; gate CountryName/Lang/TZB on dword_B5F4E4; match the exact VarList order. [orig: 0x4fe8c0]
- **D-NET-43** [HIGH, FIXED] booleans via STRNOVA11/12 tokens (not "Yes"/"No"); Ver1="3"/Ver2="2345" (not "1"/"2780"). [orig: 0x4fe8c0 (@ 0x4ff3e0)]
- **D-NET-44** [MED, FIXED] two spaces before HostKey; drive keys from an ordered VarList (LobbyName first, re-emitted). [orig: 0x4fe8c0 (@ 0x4ff4e1)]
- **D-NET-45** [MED, FIXED] Stat="N" always; LevelRange always emitted as a single space. [orig: 0x4fe8c0 (~0x4ff215)]
- **D-NET-46** [MED, FIXED] model the gated ` p=<player>` suffix (bare ` p=` fallback), after all ` k = v` pairs. [orig: 0x4fe8c0 (@ 0x4ff560)]

`client_session.cpp` + binding (D1 join handoff) — DIVERGENT (ADR 0009 in-match seam):
- **D-NET-47** [MED, FIXED] JointOperations game-host hello PV1 must be "0.0.0 1/12/2004 EM" (NOT the lobby PV1 "0.0.0 2/10/2004 EM"); wrong PV1 = hard reject. PN casing still needs a direct host witness; code keeps the existing `JointOperations` casing. [orig: CNapiNetwork_Init @ 0x4ca4a0 / NapiNPProtocol_HandleClientJoin @ 0x62B750]
- **D-NET-48** [LOW, FIXED] dial the host from DECODED NK (url-cipher, split ':'), not plaintext NI/NP (NI/NP feed only the proxy slots). [orig: parse_connection_query_string @ 0x54dfb0 / CNapiGameSession_ConnectOrHost @ 0x4d4f10]
- **D-NET-49** [OPEN] `jointoperations_pg()` is a placeholder; the in-match PG (16B @ proto+284) is unwitnessed (source `NapiNPVarBlock_Copy @ 0x4ca7af`, not sub_62E750). Re-discover.
- **D-NET-194** [HIGH, OPEN — the custom-map join blocker, witnessed 2026-07-27] Our joiner REQUIRES the host's mission `.bms` locally (`GameWorld.load_mission_as_joiner` → `load_mission` aborts "not found in <root>"), where retail's joiner NEVER opens the mission file at all. `Game_StartMission @ 0x524360` gates every mission-file leg on authority: the early exists-check (@ 0x524751, abort exit-reason 0x14) and BOTH `Mission_LoadBMSFile @ 0x40F4E0` call sites (@ 0x524b5d fresh-load arm, @ 0x524ffa SP/authority-restart arm) are authority/SP-only. The in-session NON-authority arm (@ 0x524d8f) instead runs a sync-wait (`NapiClient_WaitForDisconnect @ 0x42cb20`), `Game_LoadTerrainDuringConnect @ 0x520710` (call @ 0x524df1), minimap slot models, then `NapiClient_WaitForGameStart @ 0x42cc10` — and every mission-identity input is a field of `g_BmsHeaderBlock @ 0xA761D0`, the 616-byte header S2C 0x0B stored (`NapiNPClientMsg_HandleBMSHeader @ 0x422660`, renamed this session): map basename +0x44 (`Bms_MapBaseName @ 0xA76214`, the env/TOD-config key — `Terrain_LoadEnvironmentConfig @ 0x610940` arg1 → `Environment_LoadTimeOfDayConfig @ 0x57db30`), environment name +0xDC (`Bms_EnvironmentName @ 0xA762AC`), tile-set basename +0x118 (@ 0xA762E8 → `<name>.TGA` / `<name>.TSD`, ext @ 0x7df3e4), plus the §5.4 water/fog/TOD override fields. The world CONTENTS arrive as the §5.2a initial-state stream (§5.28's small 0x60/0x64 metadata transfers, 0x0F, and the 0x10/0x0D/0x0C/0x20 spawn batches). There is NO map-download mechanism in JO retail and none is needed — custom missions reference stock terrain/tile-set/env assets by name, so a joiner resolves everything from shipped data. CONSEQUENCE, live 2026-07-27: joins to custom-map servers on the live service abort at mission load ("HmS Eastern Islands T.bms" / "Operation: Glass Arro.bms" / "AS - Flooded Village.bms" not found) — a failure retail cannot exhibit — and one such abort segfaulted in native teardown after the in-match hello handoff (the abort path itself is divergence-only code with a crash of its own). Port = drive the joiner's world bring-up from the wire 0x0B header + spawn stream instead of the local `.bms` (terrain/env/tile-set identity from the header; zones/entities already arrive via the decoded spawn batches — `Entity_BuildSpawnZoneList @ 0x43EAE0` scans POOLS, not the file, §5.29).

`replication_min.cpp` + `game_session.cpp` (in-match player state — §5.10):
- **D-NET-50** [HIGH, FIXED] `build_tag_0a_world_reference` shipped a 623-byte verbatim retail blob (`kRetailTag0aPayload`, only bytes 0-11 patched) on a 300 ms gameplay cadence — an ADR-0003 raw-passthrough violation. Replaced with a **field-driven builder**: it constructs a `FrameUpdate` from the host's `PlayerReplicationState` + `config_.replicated_entities` (anchor = subject world position; one tag=1 compact record per replicated entity, positions 16-bit compressed relative to the anchor via the new `network_compress_fixedpoint`, classed by `GameEntitySnapshot.entity_class`) and emits it via the new `encode_frame_update` — the exact inverse of `decode_frame_update`. Ported `network_compress_fixedpoint` (with a documented zero-guard divergence) + added `encode_frame_update` / `encode_weapon_hit_record` (ingame_encode). Validated by encode↔decode round-trips (compressor + whole-frame) and a `game_session` end-to-end assertion that the tick's 0x0A `decode_frame_update`-cleans. Guided/Unknown classes are skipped (no 0x0A compact form). Vehicle-local (mounted) compression + env/timer sub-block rotation are tracked follow-ups. [orig: NetPacket_SerializePlayerState @ 0x4C09C0 case 1 / NapiNPClientMsg_0x00A @ 0x42FEC0 event loop / Network_CompressFixedPoint @ 0x4C2780]
- **D-NET-51** [HIGH, FIXED] `handle_tag_0c_player_input` now uses the shared 5-byte entity sub-header + 43-byte extended (type-10) decoder from §5.10 instead of raw offsets. [orig: NetPacket_SerializePlayerState @ 0x4C09C0 case 4 / dispatch_entity_packet_callback @ 0x4D6A80]

`replication_min.cpp` + `game_session.cpp` (pool-entity spawn/sync — §5.11/§5.12):
- **D-NET-52** [DOC, FIXED] §5.6 trailer layout previously read `[u16][u32][cstring]`. The retail handler reads `aiProfile1` and `aiProfile2` with `cursor += 2` on a `uint16_t*` — both fields are **4 wire bytes** (the Hex-Rays render shows `uint16_t*` as the value type, but the cursor advance and the destination slot writes are `_DWORD`). Cross-witnessed against 195/437 trailer-carrying 0x0D records in the 2026-06-16b loopback. Update §5.6 + §5.11 (this commit). [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x43311e / 0x433131)]
- **D-NET-53** [HIGH, FIXED] `build_tag_0d_spawn_points` no longer emits ungated weapon-slot zeros before the always-read bone/other byte; multi-record batches decode without leftover/misalignment. [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 — weapon block gated by `spawnFlags & 0x400`)]
- **D-NET-54** [LOW, DOC] In-source field-table comments at `replication_min.cpp:417-422` (and the mirror at line 524-526) label the always-byte at +290 "bone_attach byte" and `flags&0x10` as "team". Per §5.11 the always-byte is unnamed in retail (Hex-Rays calls it `teamByte`; field is at +290), and `flags&0x10` writes `orientByte` to +354. Update the in-source comments to match §5.11. Wire-emitted bytes are unchanged by this fix — comment-only. [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x432e29 = flags&0x10 → +354; @ 0x43310a = unconditional u8 → +290)]
- **D-NET-55** [HIGH, TRACKED] No `build_tag_20_pool3_sync` builder exists; `game_session.cpp` dispatch (around lines 1023-1075) has no inbound `handle_tag_20_*` either — every S2C 0x20 falls through to `handle_unknown_or_passive_tag`. Pool-3 markers / waypoints / nav-nodes are therefore not registered into the client's pool 3, which blocks AI navigation, target markers, and any spawn-select markers that resolve via pool 3. §5.12 has the full record map; the builder needs a `[u16 start_idx][u16 count]` header + per-entity flag-driven serializer matching the witnessed 29-payload / 792-entity loopback shape. **Reframed (D-NET-84):** the host emits `0x20` ONLY from `Server_SendInitialGameStateToPlayer @ 0x51BBA0` phase 4 (per-join, paged, load-only) — there is no mid-game patrol stream, so the open work is purely the inbound runtime wiring, and the operation_whitenoise stock 29-payload load batch is the reference shape. [orig: NapiNPClientMsg_0x020 @ 0x425C00 / serialize_entity_pool_to_packet @ 0x503460]
  **RESOLVED (2026-06-27, verified):** the npruntime rework wired BOTH halves the retired game_session.cpp lacked. Builder: `encode_pool3_sync_batch` (`libs/npwire/ingame_encode`) over `netsim::build_pool3_spawn_marker_batch`, emitted from the §5.2a world-stream phase 4 in `Server_SendInitialGameStateToPlayer` (`server_initial_state.cpp`) — exactly the D-NET-84 per-join load-only shape. Inbound: `NetClientView::apply` case `0x20` → `apply_pool3_batch` → `decode_pool3_sync_batch` registers each marker into the client `ClientState` pool (`net_client_view.cpp`). Tested end-to-end by `npruntime_initial_state_burst` (asserts the 0x20 body decodes + carries the 6002 spawn marker).
- **D-NET-56** [MED, FIXED] `decode_pool_spawn_batch` (ingame_decode.cpp) read `extra_handle_0/1` only inside `if (weapon_mask)`, under-reading by 4 B on the (`0x400` set, mask==0) path. The handler's mask==0 branch (`goto LABEL_110`) skips the per-bit loop but still consumes both extras unconditionally once `0x400` is set; moved the extras read outside the mask!=0 guard. **Latent:** retail's encoder `serialize_entity_pool_to_packet_0 @ 0x503940` only sets `0x400` when its mask (`itemDef+604`) is non-zero, so the byte-witness capture never produced mask==0 and `nw_ingame_pool_records_test` stayed green — but the client handler reads it regardless, so the port must match. Found by grilling the encode side for the Phase-1 host world-stream (trust-but-verify of already-written code). [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 LABEL_110)]
- **D-NET-57** [DOC, FIXED] §5.10 player compact record: the yaw_byte landing was cited as `entity+0x14`. Validated against the actual `NetPacket_SerializePlayerState` case 1 (write) + case 2 (read) — the read lands **yaw (byte 10) at `entity+0x10`** and **pitch (byte 11) at `entity+0x14`** (case-2 spawn branch writes `entity+0x10/0x14/0x18` = the heading/pitch/roll Euler triple). The decompiler's `pitchPacked`/`rollPacked` slot names are reused-stack artifacts, not the field semantics. **Wire layout, field widths, and yaw@10/pitch@11 ORDER are unchanged and confirmed correct** — `encode_player_compact_record` and `decode_player_compact_record` need no change; only the doc/struct landing-offset comment is corrected. This was the validation pass the player-compact encoder needed (the case-switch function exceeds a single decompile, so the case-1 write + case-2 read were extracted via Hex-Rays `py_eval`). [orig: NetPacket_SerializePlayerState @ 0x4C09C0 (case 1 write; case 2 read @ ~0x4c0730 entity+0x10/0x14/0x18 stores)]

Controlled-capture validation (probe mission "ON RE Probe AS dvxi5", dvxi5 / A&S 0x10000, host + "TestPlayer", 2026-06-17):
- **D-NET-58** [HIGH, DOC+CODE] §5.11 0x0D team/orient labels were CROSSED (inherited from D-NET-54 trusting the handler-side Hex-Rays name). The `spawnFlags&0x0010`-gated byte at **entity+354 is TEAM** (1=Blue/2=Red); the unconditional post-weapon byte at **entity+290 is a bone/other byte, NOT team**. entity+354 is the unified team landing shared with the 0x20 path (§5.12 flag 0x08). Renamed `ingame_decode.h PoolSpawnRecord.orient_byte→team_byte` (+354, gate 0x10) and `team_byte→bone_byte` (+290), with matching `ingame_encode.cpp`/`nw_pp` updates. Controlled witness: trucks authored team 1/2 → +354 = 0x01/0x02, +290 = 0x00. [orig: serialize_entity_pool_to_packet_0 @ 0x503940 (team_byte=*(entity+354); bone_byte=*(entity+290))]
- **D-NET-59** [HIGH, DOC+CODE] §5.12 0x20 `flags&0x01` field is the engine's `entry[4]` **`movement_val` @ entitySlot+16**, written RAW (no pool-resolve) — a 32-bit BAM heading for pool-3 start markers, NOT a `pool<<12\|slot` parent handle. Renamed `ingame_decode.h Pool3SyncRecord.parent_handle→movement_val`. Controlled witness: Blue starts 0x40000000 (90°), Red starts 0xc0000000 (270°), team-correlated. [orig: serialize_entity_pool_to_packet @ 0x503460 (movement_val=entry[4], written raw) / NapiNPClientMsg_0x020 @ 0x425C00]
- **D-NET-60** [LOW, DOC] §5.4 0x0B icon-key offset: "full_00" observed at off **220-226**, not the documented 246-253. Signature(0-3)/name(4-35)/designer(36-67)/basename(68) all matched their documented offsets, so only the icon row is suspect — re-diff against more retail maps or annotate as header-variant-dependent. Note: the synthesized header title-cases the basename to "Dvxi5" at +68 (client terrain lookup is case-insensitive). [orig: byte_A761D0 @ §5.5]
- **D-NET-61** [INFO, VALIDATED] The `/PROFILE` `.sph` server-log (§5.22) — the engine's own decoded per-frame view of the SAME probe session — was decoded (`libs/npwire/serverlog_decode.{h,cpp}`, `nw_pp` `.sph` mode, `nw_serverlog_decode_test`) and cross-validated against the `.pcapng`: FooPlayer (Red, pool-0 handle 0x0005) spawn state `(70.0, 25.0, 56.306)/0xc0000000` matches **byte-for-byte** across `.sph` `PDAT`, C2S 0x0C extended uplink (§5.10), and the S2C 0x0A header `refs` triple — independently confirming the 0x0C decoder, the 16.16/-Z + 32-bit-BAM conventions, pool-0=players (the recorder iterates `g_pool_list[0]`), and team@entity+354 (re-confirms D-NET-58 via the `FEDP` roster: TestPlayer=Blue/1, FooPlayer=Red/2). No code divergence — a validation pass + new oracle tooling. Two IDB-fidelity fixes were required to read the recorder: `sub_522350`→`Game_TeardownMission` decompilation was blocked by phantom-arg prototypes on 0-arg callees (`Database_GetFieldValue` is actually `void __thiscall Database_FreeFieldEntries`; `File_Seek`/`Terrain_RenderSectorsWithWhiteFog`/`CEffectWorld_IsNameAvailable` retyped to 0 args — each 1 xref, 0 stack-arg reads). [orig: Game_ProcessMainFrame @ 0x5263f0 / CServerLog_WritePositionRecord @ 0x4e1b00 / CServerLog_WritePlayerNameRecord @ 0x4e1cc0]
- **D-NET-62** [INFO, VALIDATED] Authored-mission cross-validation of pools 1/2/3 (§5.24) — the dvxi5 probe's *known* `mission.bms`, serialized by the retail host, decoded field-for-field on the wire (the sibling of D-NET-61 for the pools the `.sph` can't see). Lands the **S2C 0x0C organic-spawn field map + decoder** (`decode_organic_spawn_batch` / `OrganicSpawnRecord`, §5.23) — byte-exact consume on the probe's 6-organic batch (4 AI `0x0816` + 2 players `0x14B9`); the shared pcap reader (`apps/common/pcap_reader`, nw_pp factored onto it); and two tests (`nw_pool_groundtruth_test` reads the real `.scratch` pcap directly; `nw_pool_decode_unit_test` inline-pcap round-trips 0x0D/0x20 through the full S2C stack). Confirms: type_id/position/team reproduce (posX/posY lossless i32 16.16; posZ re-grounds ≤1u for vehicles/AI, markers keep authored z); the heading convention **`wire_BAM = 90 - facing`** (pinned by AI authored at facing {0,90,180,270} → wire {90°,0°,270°,180°}; the 0x20 markers at facing {0,180} alone could not distinguish it from `facing+90`); and team @ **entity+354** — the onhook PoC's `+146`/`+196` reads are inside `GamePlayerEntity.pad5`, a runtime/display mirror, NOT the BMS team (same mislabel class as the PDAT `+42` STAT byte, §5.22). No divergence in the pool decoders — a new field map + validation oracle. [orig: NapiNPClientMsg_0x00C @ 0x42E730 / serialize_entity_pool_to_packet_0 @ 0x503940 / CServerLog_WritePlayerNameRecord @ 0x4e1cc0]
- **D-NET-63** [MED, DOC+CODE] §5.13 vehicle compact record field labels corrected (the rename the 2026-06-16d footnote deferred). Re-grilled the mode-2 (read) path of `Entity_SerializeMountedVehicleState @ 0x460560`: the pre-branch i16 (`yaw_high`) and the two mounted-branch i16s are the **orientation / rider Euler triple Z/Y/X** landing at **entity+576/584/580** (fed to `Math_BuildFixedPointMatrixFromEulerAngles`), and the unmounted block is **turret-pitch raw i16 (entity+286) + weapon-aim Y/Z (read-dest `vehicleData[177/178]`) + weapon-heading BAM (`vehicleData[179]`)** — distinct from the genuine weapon-X compressed u16 (entity+160). The write side has NO shared trailing field, so the reimpl's formerly-shared `final_heading` is split per branch into `euler_x` (mounted) / `weapon_heading_bam` (unmounted). Renamed `ingame_decode.h VehicleCompactRecord` (`yaw_high→euler_z`, `secondary_heading→euler_y`, `final_heading→euler_x|weapon_heading_bam`, `weapon_x_compressed→weapon_x`, `weapon_y_raw→turret_pitch_raw`, `weapon_z_compressed→weapon_aim_y`, `weapon_heading_compressed→weapon_aim_z`) with matching `ingame_encode.cpp` / `nw_pp.cpp` / `replay_timeline.cpp` / `nw_ingame_compact_records_test` / `nw_ingame_encode_test`. Also split the §5.13 table's "landing" column into write-source vs read-dest (it had conflated write `vehicleData[136]` with read-dest `vehicleData[177]`). **Wire bytes, read order, and sizes (15 B mounted / 21 B not) are unchanged** — label-only; round-trip + byte-witness tests stay green. [orig: Entity_SerializeVehicleState @ 0x460560 (read path @ 0x4605a3..0x460aff; Euler matrix build @ 0x460a0f → Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40)] (Function renamed again 2026-07-04: the "mounted/rider" reading itself was the misnomer — see the §5.13 dead-pose correction + D-NET-161.)
- **D-NET-64** [PARTIAL, DOC+CODE] §5.15 guided weapon record upgraded from "TBD" to a documented per-(mode, field-group) matrix + structural port. `Entity_SerializeGuidedMissileState @ 0x447C50` is a `mode (packetCtx[6] ∈ {1..4}) × field-group (packetCtx[7] ∈ {1..6})` codec (write-full/read-full/write-delta/read-apply across status / clear-target / target+pos / type+pos / pos / attach-offsets), NOT a fixed compact. **Framing resolved:** `dispatch_entity_packet_callback @ 0x4D6A80` copies the 5-byte entity sub-header's `sub_op` byte into `packetCtx[7]`, so the field-group selector rides the wire as `sub_op` (1..6 for guided; 10/11 = extended/compact for the §5.10b classes), and hardwires `packetCtx[6]=4` (read-apply) on the host C2S-receive path. The serializer rejects format 11, confirming guided never legitimately appears as a 0x0A compact — `decode_frame_update`'s fail-closed on `EntityClass::Guided` is correct. Landed `GuidedRecord` + `encode_guided_field_group`/`decode_guided_field_group` (`ingame_encode.cpp`/`ingame_decode.cpp`) + `nw_ingame_guided_test` (per-(mode,group) round-trip; the write-side 1-B `0x00` status/clear marker is the dispatcher's framing, read side reads 0 B). **DEFERRED:** wiring into the 0x0C entity-packet dispatch + per-group field validation — no capture carries guided traffic (the 2026-06-16b loopback fired no rockets). Verdict partial (IDA-structural, round-trip-pinned, wire-unvalidated). [orig: Entity_SerializeGuidedMissileState @ 0x447C50 / dispatch_entity_packet_callback @ 0x4D6A80]
- **D-NET-65** [HIGH, DOC] High-bit protocol-message packets are a separate NAPI high-table control namespace, not low-table gameplay tags and not generic "unknown settings." Retail registers only `H:0x00..H:0x03` in `g_np_msginfo_highbit @ 0x849E80`: `H:0x00` is CS config update, `H:0x01` is connection name/tag update, `H:0x02` is data-transfer control, and `H:0x03` is description packet. `H:0x00` is the sparse runtime update form of the opcode-`0x82` `CS` TLVs: payload `[direction:u8][mask:u32le][u32 per set field]`, with the same 15 `NapiCSConfig` field indexes documented under §6.5. Observed masks match IDA callers: `0x2000` -> field 13 `max_packet_bytes=1300` from `NapiNPServer_HandleNewConnection`, and `0x0008` -> field 3 `send_holdoff_ticks=12` from `NapiNPServer_UpdateHoldoffTicks`. Also corrected the terminology trap: `NA=jop:cus2` is connection/game/gate tag state in the NOVAWORLDUDP path, not the player display name (`NWHANDLE`/`CHAR`). No source change in this commit; this records the finding and implementation implication. [orig: NapiNPProtocol_InitMsgInfoIndex @ 0x61E400 / NapiNPProtocol_FindMsgInfo @ 0x61E380 / CNapiNPConnection_DispatchMessage @ 0x622570 / CNapiNPConnection_HandleCSConfigUpdate @ 0x621940 / CNapiNPConnection_SendSessionInit @ 0x620EF0 / NapiNP_HandleServerJoinResponse @ 0x629840 / CNapiNPConnection_SendConfigUpdate @ 0x6286E0 / NapiNPServer_HandleNewConnection @ 0x4C8040 / NapiNPServer_UpdateHoldoffTicks @ 0x4C5F40 / NapiNPServer_GetSendHoldoffTicks @ 0x4C4AB0]
- **D-NET-66** [HIGH, FIXED] The replay timeline (§5.25) had **no death/respawn lifecycle** — an entity was one monotonically-accumulating track, so a kill followed by a respawn-elsewhere read as two consecutive samples and `interp_pos` / the viewer's `posAt` **linearly interpolated a glide** from the death spot to the spawn point (the reported "players drift when they die"). This is unfaithful: the engine never interpolates across a death — `Entity_KillBySlotId @ 0x42BCE0` sets the dead flag `Flags & 2`, and the dead→alive transition relocates the entity and calls `Entity_ResetToSpawnState @ 0x4B9610` (a SNAP). The read path gates on this exact bit: `NetPacket_SerializeInfantryEntityState @ 0x4C0320` branches on `flagsByte & 2` (the wire dead/spectator bit), and `NetPacket_SerializePlayerState @ 0x4C09C0` does `test [entity+0x24], 2` → set position directly + `Entity_ResetToSpawnState`. Modeled from BOTH wire signals: the per-record dead bit (S2C `0x0A` compact `flags & 0x02`, set while the ragdoll is broadcast and cleared at the respawn record — empirically brackets victim `0x4`: dead f=1998→2142, respawn snap f=2216) AND the kill stream (S2C `0x26`/`0x4E` — the only signal when a victim drops out of the `0x0A` set, e.g. victim `0x5`: records stop f=1934, killed f=2344, reappears at spawn f=3651). Added `ReplaySample.dead/respawn` + `mark_lifecycle` (flags the dead→alive transition `respawn`, run on the full timeline AND each projected per-participant view); `interp_pos`, the viewer `posAt`, and the trail polyline never bridge a `respawn` sample (hold at the death spot, styled dead, then teleport); `nw_pp` emits `dead`/`respawn`; the previously-missing S2C `0x4E` batch-despawn fold (`decode_batch_kill`) now emits a Kill per slot. CI: `nw_replay_timeline_test::test_death_respawn` (ragdoll path + records-stop path). Non-death disconnect/cull gaps (no kill, no flag — e.g. the `0x46` `0x8000` player-leave / `0x5D` destroy list) remain a separate despawn-channel grill. [orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320 / NetPacket_SerializePlayerState @ 0x4C09C0 / Entity_KillBySlotId @ 0x42BCE0 / Entity_ResetToSpawnState @ 0x4B9610 / NapiNPClientMsg_HandleBatchKill @ 0x431870]
- **D-NET-67** [HIGH, FIXED] Mounted (vehicle-local) `0x0A` compact records were **skipped** in the replay timeline (`frame_record_world_sample` returned false on `is_mounted_parent`), so a passenger/driver/gunner froze at its last on-foot position while the vehicle drove off (the reported "not handling being attached to a vehicle"). The read path instead lifts the record's vehicle-LOCAL position to world: unmounted (`parent == 0xFFFF` / `≥0x5000`) → `pos += anchor`; mounted → `Entity_TransformLocalToWorld(&local, &local, parentEntity+1)` where `parentEntity+1` is the parent's `{x,y,z, yawBAM, pitchBAM, rollBAM}` at entity+4..+24. Ported `Entity_TransformLocalToWorld @ 0x43BD00` as `network_transform_local_to_world` (ingame_decode) — a faithful Euler roll(X)→pitch(Y)→yaw(Z) rotation of the local offset in 22-bit fixed-point (`sin/cos ×2²²`, `imul` + `shrd …,22`, traced from the disassembly's output assignments), then add the parent's world position; the disasm reads angles via `fild` (signed 32-bit BAM). Wired into `build_replay_timeline`: mounted records are deferred (`MountedRec`: vehicle-local offset + parent handle), then `resolve_mounted` lifts each rider once its parent's world track is known — **bottom-up so nested mounts resolve** (a rider on a weapon mount on a vehicle; the seat-local offset on the wire already encodes driver vs passenger vs gunner, so the per-level transform is uniform). The C2S `0x0C` own-player uplink is also vehicle-local when mounted (§5.10), so it's deferred the same way (tagged `ClientUplink` so the owner's projected view keeps it). The rider's world heading = parent yaw + local yaw (`outWorld[3] = ref[3] + local[3]`). **Wire limitation (not a divergence):** the unmounted vehicle record transmits only the parent's yaw (`euler_z`, §5.13); the engine integrates pitch/roll locally and they are not on the wire, so the lift feeds pitch=roll=0. Validated on the medium loopback (players `0x3`/`0x4`/`0x5` ride vehicles `0x1000`–`0x1002` through motion) + CI `nw_replay_timeline_test::test_mount_transform` (exact yaw=0 identity + a crafted rider-on-parent lands exactly at the ported transform). `std::sin/cos` vs the x87 path is a CRT/platform primitive. [orig: Entity_TransformLocalToWorld @ 0x43BD00 (read path @ NetPacket_SerializeInfantryEntityState @ 0x4C0320 / NetPacket_SerializePlayerState @ 0x4C09C0)]
- **D-NET-68** [DOC, FIXED] **JO has no raw-input (keys/axes/buttons) channel — player movement is state-replicated, and the §5.4 C2S table mislabeled two unrelated tags as one.** A player's client simulates its own movement locally and uploads the *computed pose* (world position 16.16 + heading/pitch/anim) once per frame via C2S `0x0C` extended (§5.10, `PlayerExtendedUplink` — byte-validated client-origin by D-NET-61: the position matched across `.sph` / C2S 0x0C / S2C 0x0A). The host **read-applies** that reported pose — `dispatch_entity_packet_callback @ 0x4D6A80` hardwires `packetCtx[6]=4` (read-apply), stages the position at the smooth-target `entity+0x234` and interpolates the live entity toward it — and validates plausibility (speed/time-sync + weapon tallies); it does **not** re-simulate movement from inputs. So the host is authoritative as the relay/validator/coordinator (canonical world broadcast, vehicles, AI, hit resolution, anti-cheat), **not** as a movement simulator — there are no inputs on the wire to simulate from. Two §5.4 C2S rows that implied a phantom input stream are corrected from decompiling their handlers: (1) `0x08` "entity movement/state delta" → `validate_time_sync @ 0x502210`, an anti-speedhack that checks `[u32 sessionId][u32 gameTimestamp]` deltas stay within 3% of `GetTickCount` wall-clock; (2) `0x0F` "client input frame (movement + buttons; ~33 ms cadence)" → `NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180`, a `[u16 pool-0/1 handle]` info request whose host serializes that entity's info and broadcasts S2C `0x18` (the fallback spawn-menu "query loop" of pool-1 slots `0x10NN` is this request, not an input frame). **Naming note (no rename):** the C2S 0x0C "player input" terminology — `Player_BuildTag0CInputBody @ 0x42A550`, the reimpl `handle_tag_0c_player_input` — denotes the client's per-frame POSITION/STATE upload, not raw input; left as-is (IDB renames are shared state; "input" is defensible for the per-frame submission), clarified here for the record. **Implication for the runtime client/host split:** a faithful client simulates its own player and emits a `0x0C`-style pose; it does not ship inputs for the host to run. Doc-only — no source change. [orig: validate_time_sync @ 0x502210 / NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180 / dispatch_entity_packet_callback @ 0x4D6A80 / Player_BuildTag0CInputBody @ 0x42A550]

Controlled-capture validation (probe mission "ON RE Probe TDM Dvxi3", probe 2 / Team Deathmatch 0x20000000, host + joiner on a separate install, 2026-06-18):
- **D-NET-69** [HIGH, DOC] §4 / §5.28 mission delivery corrected. The §4 table described S2C `0x60`/`0x64` as a chunked `.bms` file transfer with C2S `0x33`/`0x37` re-requests (inherited from the reverted stack; §5.8 had already flagged `0x60`/`0x64` as never byte-compared). The probe2 capture — a real download forced by a joiner whose install lacked `probe2.bms` — shows mission delivery is **streamed, not a bulk file copy**: S2C `0x60` = a mission ANNOUNCE (`[u32 type=1][u32 bodyLen][u32 reserved]` + a `SERVERNAME`/`MISSIONNAME` VarList string table), S2C `0x64` = one compact mission CHUNK (same 12-B header + an opaque ~180-B payload), then the S2C `0x0B` literal 616-B BMS header, S2C `0x0F`, and the entity spawn batches (`0x10`/`0x0D`/`0x0C`/`0x20`). The C2S `0x33`/`0x37` re-requests **never fired** — matching the host emit order in §5.2a (no chunk train). Corrected the §4 `0x60`/`0x64`/`0x33`/`0x37` rows + the catalog notes; landed the §5.28 field map. **[Partly superseded by D-NET-74, 2026-06-18b:** the IDA grill of `0x432350`/`0x432410` shows `0x60`/`0x64` ARE genuine chunked file transfers — header `[u32 transferId][u32 totalSize][u32 chunkOffset]` + raw file bytes, C2S `0x33`/`0x37` re-request on an incomplete transfer. The re-requests didn't fire because probe2 completed each transfer in ONE chunk, not because delivery is stream-only; the `type=1/bodyLen/reserved` header reading and the "announce VarList" attribution were single-chunk artifacts (the VarList is the transferred file's *content*, not a 0x60 field layout). See the corrected §5.28.] [orig: NapiNPClientMsg @ 0x432350 (0x60) / @ 0x432410 (0x64) / Server_SendInitialGameStateToPlayer @ 0x51bba0 (§5.2a emit order)]
- **D-NET-70** [INFO, VALIDATED] Pool assignment is by entity **capability, not editor "kind"**: purely-static structures (armory, oil pump, oil towers/pipes/docks/tanks) replicate via S2C `0x10` (pool-2 static-entity batch), while destructible / AI-bearing objects (oil-field LFP `0x0135`/`0x0136`, the drivable fuel truck) ride S2C `0x0D` (pool-1) alongside vehicles. Wire-validated against the probe2 `dvxi3_manifest.txt`: every authored type observed on exactly one pool with byte-exact position/team, so the manifest's `wire_tag` column is now wire-confirmed (no re-hypothesis). [orig: NapiNPClientMsg_0x010 @ 0x433400 (pool-2) / NapiNPClientMsg_0x00D @ 0x432C40 (pool-1)]
- **D-NET-71** [HIGH, FIXED] S2C `0x10` (pool-2 static-entity batch) had the §5.9 field map but **no decoder** — printed raw hex only, so the replay timeline/viewer silently **dropped every static** (the oil pump `Pmpjk01`, both armories, ~70 oil-field decorations were invisible — the reported "why isn't Pmpjk01 showing up"). Ported §5.9 to `decode_static_entity_batch` (`ingame_decode`): header `[u16 startIndex][u16 count]`; per record `[u16 itemTypeId (0 = empty-slot sentinel)][u16 fieldFlags][i32 posX/Y/Z]` + flag-gated vel / sectionMask / team@+354 / parentSlot, **unconditional** `ammoCount` + `weaponByte`, and `attachRef` when `weaponByte != 0 || flags & 0x200`. Wired a `0x10` branch into `build_replay_timeline` (pool-2 handle `(2<<12)|slot`), a `nw_pp` `print_tag_10`, the catalog (`0x10` → Decoded), the coverage gate, and a new `nw_dvxi3_groundtruth_test` asserting every authored static. Byte-exact full-consume on all 4 capture batches; the replay JSON went **31 → 106 entities** (75 statics surfaced). Also landed the already-documented player decoders `decode_player_list` (§5.20) / `decode_player_sync` (§5.21) — catalog → Decoded, byte-exact on the capture (TestPlayer → handle `0x0004`, FooPlayer → `0x0005`). [orig: NapiNPClientMsg_0x010 @ 0x433400 / NapiNPClientMsg_PlayerList @ 0x42FAE0 / NapiNPClientMsg_PlayerSync @ 0x431370]
- **D-NET-72** [FIXED] Tags present in the probe2 capture but uncharacterized (dispatch-table one-liners, no field map) — now IDA-witnessed and landed (D-NET-73 + D-NET-74): S2C `0x5A` weapon-loadout (`0x4290E0`), `0x6E` roster (`0x429880`), `0x7B` full-player-info (`0x429BB0`), the `0x0F` world-state-load body (`0x42E200`); C2S bursts `0x22`/`0x23`/`0x28`/`0x29` (`0x514C90`/`0x514D50`/`0x51A550`/`0x514F10`) + `0x4C` (`0x5111B0`); and the `0x64`/`0x60` mission-transfer "inner codec" (`0x432410`/`0x432350`). Field maps §5.28-§5.33; decoders + `nw_pp` printers + catalog flips + coverage all landed below. [orig: addresses inline]
- **D-NET-73** [HIGH, FIXED] Field-mapped + decoded the uncharacterized in-game tag bodies (closing the structured half of D-NET-72) from the retail handlers, each byte-exact-validated against the probe2 capture. **S2C `0x5A`** weapon-loadout (§5.30): `[u8 avatarClass]` + a `{u8 typeId, u8 ammoP, u8 ammoS, u8 ammoAlt}` slot chain to a `0xFF` terminator (`typeId` = AdmDef index; the handler's AdmDef-validity drop is runtime, not wire — the decoder keeps all slots). **S2C `0x6E`** roster (§5.31): `[u8 teamCount]` + per-team `{u16 entityHandle (0xFFFF=none), u16 slotIdx, u8 memberCount, u16 slotHandle, u16 members[]}`. **S2C `0x7B`** full-player/session-info (§5.32): 5 cstrings + `[u32 extra]` + 2 cstrings = name / **playerId** / serverName / missionName / mapFile / motd / gameName (probe2: `FooPlayer` / `00000003` / `biggy` / `ON RE Probe TDM Dvxi3` / `probe2.bms`). **String 2 is the NovaWorld player/account ID, not a clan tag** — witnessed from the landing globals + `PunkBuster_GetCvarValue @ 0x4D96A0` cvar map (`name`/`sv_hostname`/`mapname`/`gamename`) and the slot string 2 shares with the S2C 0x7A name handler (`@ 0x429B40`); cross-capture it is persistent per player and empty on LAN joins (where the display name is used instead). The Hex-Rays `clan/squad/rank` auto-comment is wrong on every string. **S2C `0x0F`** world-state-load (§5.29): `i32 sessionTick` + 3×i32 spawn + 3×i16 angles + `u8 gameFlags` + a **fixed 128-i32 score block** (`(data−outTable)/4` @ 0x42e324) + `u16 waypointCount` + waypoint records (gated by the off-wire `g_GameType` waypoint test → decoder hint, default false; TDM sends 0) + `u16 teamNameCount` + cstring names. **C2S bursts** (§5.33), field-mapped from the authority server read-handlers: `0x22` `[u8 slot][u16 fieldFlags]`→S2C 0x46, `0x23` empty→S2C 0x4C, `0x28` `[u32][u32][u16]`→S2C 0x4E, `0x29` `[u16 bufferIndex]`→S2C 0x51, `0x4C` `[u8 value]` (clamp 0..4). Landed `decode_weapon_loadout`/`decode_roster_sync`/`decode_full_player_info`/`decode_world_state_load`/`decode_burst_*` (`ingame_decode`), `nw_pp` printers, catalog → Decoded (25 Decoded tags), and `nw_message_coverage` checks. Wire-validated: `nw_pp` full-consumes every occurrence in probe2 (`0x0F`×1, `0x5A`×8, `0x6E`×43, `0x7B`×2, `0x22`×11, `0x23`×1, `0x28`×1, `0x29`×1, `0x4C`×60) with zero leftover bytes; the `0x0F` `waypoints=0` confirms both the TDM gate-off default and the 128-entry score-block count. [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290E0 / NapiNPClientMsg_HandleSquadRosterSync @ 0x429880 / NapiNPClientMsg_HandlePlayerInfoFull @ 0x429BB0 / NapiNPClientMsg_0x00F @ 0x42E200 / NapiNPServerMsg_0x022 @ 0x514C90 / _0x023 @ 0x514D50 / HandleWeaponLoadoutRequest @ 0x51A550 / _0x029 @ 0x514F10 / _0x04C @ 0x5111B0]
- **D-NET-74** [HIGH, DOC+CODE] **S2C `0x60`/`0x64` are a genuine chunked file transfer — refines D-NET-69.** The IDA grill of `NapiNPClientMsg_HandleFileTransferChunk @ 0x432350` (0x60) and `NapiNPClientMsg_HandleMissionDataChunk @ 0x432410` (0x64) shows both read an identical 12-byte header `[u32 transferId/checksum][u32 totalSize][u32 chunkOffset]` then `len−12` **raw file bytes**, reassembled by offset; on `chunkOffset + chunkSize >= totalSize` the transfer completes, else the client re-requests the next chunk (`0x60`→C2S `0x33`, `0x64`→C2S `0x37`, payload `[transferId][nextOffset]`, 8 B). 0x60 reassembles into a `CDataStream`; 0x64 into a buffer whose completion extracts 3×32-B mission-name strings. **There is no compression codec** — the payload is literal file content (resolves the deferred "0x64 inner codec"). D-NET-69 read the header as `[type=1][bodyLen][reserved=0]` and called 0x60 a parsed `SERVERNAME`/`MISSIONNAME` VarList; that was a **single-chunk artifact** — probe2's transfers each fit in one chunk, so `transferId=1` looked like `type=1`, `totalSize` equalled the remaining bytes, and `chunkOffset=0` looked like `reserved`, and the C2S 0x33/0x37 re-requests didn't fire because the transfers *completed*, not because delivery is stream-only. The `SERVERNAME`/`MISSIONNAME` text is the transferred file's content (a downstream-parsed VarList), not a 0x60 field layout. Landed the shared `decode_file_transfer_chunk` (`ingame_decode`) + `nw_pp` printer + catalog (`0x60`/`0x64` → `file-transfer-chunk`, Decoded) + coverage; corrected §4 (`0x60`/`0x64`/`0x33`/`0x37` rows) and rewrote §5.28. Wire-validated against probe2 (`0x60`: id=1 total=163 offset=0 [FINAL]; `0x64`: id=1 total=180 offset=0 [FINAL]; both consume to the byte). [orig: NapiNPClientMsg_HandleFileTransferChunk @ 0x432350 / NapiNPClientMsg_HandleMissionDataChunk @ 0x432410 / CDataStream_Write @ 0x455480]

Controlled-capture validation (probe mission "ON RE Probe COOP Dvxc1", probe 3 / **Co-op** — the unique mode whose `g_GameType` unlocks BOTH deferred gates; host + joiner on a separate install, JOX expansion, 2026-06-18):
- **D-NET-75** [HIGH, DOC+CODE] **The two off-wire-gated in-game sub-bodies (0x0F waypoint records, 0x0A objective sub-block 3) are now WITNESSED, and the 0x60 multi-chunk transfer fired for the first time.** Probe3 is a Co-op mission whose header attrib `0x01000000` resolves to `g_GameType = 0x30020` (`AI_GetTaskTypeFromFlags @ 0x40DAE0` → task index 2 → `Game_StartMission @ 0x524360`), the unique value passing both `(g & 0xFFFDFFFF)==0x10020` (waypoint gate) and `(g & 0x20000)` (objective gate). Findings:
  - **S2C `0x0F` waypoint records (§5.29) — first wire witness.** probe3 carries `waypointCount=4` (records reference pool-3 marker slots 6-9), `teamNameCount=0`. The §5.29 / D-NET-73 field map (`{u16 slotId, u16 nameId, u8 pad}` × count) is byte-exact; the decoder was already correct, but the off-wire gate hint (`is_waypoint_gametype`) was never supplied, so the records misparsed as team-name data ("DECODE INCOMPLETE"). Fixed by sourcing `g_GameType` from the **0x7B `extra` field** (below) and threading it as the hint — `decode_world_state_load` now consumes byte-exact.
  - **S2C `0x0A` objective sub-block 3 (§5.9) — first wire witness.** 771 frames carry `flags2 & 3 == 3` with the 16-B objective body (4× i32, all zero in this capture). `decode_frame_update` previously read 0 B for sub-block 3 (the gate is off-wire); added the `is_objective_gametype` hint (gate `g_GameType & 0x20000`, sourced from 0x7B `extra`) — the body now consumes. The `flags2 & 0xF0` high bits (e.g. `0x7b`) don't affect sub-block selection. [orig: NapiNPClientMsg_0x00A @ 0x42FEC0 gate @ 0x430361, body @ 0x430363..0x4303D0]
  - **S2C `0x60` MULTI-CHUNK transfer + C2S `0x33` re-request — first witness (confirms D-NET-74).** A >200-B host VarList (a long MOTD) forced 2 chunks: `id=1 total=239 offset=0 chunk=200 [more]` → joiner re-requests **C2S `0x33` `[id=1][nextOffset=200]`** → `id=1 total=239 offset=200 chunk=39 [FINAL]`. Payload is the session VarList (`SERVERNAME` "…server of biggy" / `MISSIONNAME` / `…ENAME`=`probe3.bms` / `EXP_FANFARE`), confirming 0x60 carries the VarList **content**, not the `.bms`. 0x64 stayed single-chunk (fixed 180 B < 200). This closes D-NET-74's "didn't fire because single-chunk" caveat.
  - **S2C `0x7B` `extra` = `g_GameType` (§5.32).** probe3 `extra=0x00030020` equals `g_GameType` (0x30020) exactly — the wire independently confirms the Co-op gametype derivation, and makes `extra` a reliable decoder source for the two off-wire gates above. `gameName` = the expansion id (`"jox01"`), `serverName` non-empty, `motd` empty. (The 0x429BB0 handler's Hex-Rays `clan/squad/label/rank` field names are menu-context; the in-game roles are name/id/server/mission/map — see §5.32.)
  - **Pool routing (extends D-NET-70).** The "Change Team & Spawn Volume" objects (`0x0575` tent / `0x0576` HQ) ride **pool-1 S2C `0x0D`** (team-gated `spawn_flags & 0x10`), NOT pool-2 statics; the four pool-2 **armories** (handles `0x2000`-`0x2003`) carry the S2C `0x40` capture-zone overlays + drive the S2C `0x1E` OBJECTIVE events (`STRCND_PSP_BLUEWARNING` type 41 / `STRCND_PSP_BLUETAKEN` type 43).
  - **Landed:** `is_objective_gametype` hint + objective body in `decode_frame_update`; `nw_pp` g_GameType tracking (from 0x7B) + the `is_waypoint_gametype`/`is_objective_gametype` wiring + 0x0F waypoint-record printing; `fixtures/novaworld/dvxc1_manifest.txt` (37 authored entities) + `nw_dvxc1_groundtruth_test` (every 0x10/0x0D/0x0C/0x20 entity field-for-field + the four deferred witnesses). `nw_pp` full-consumes the entire probe3 capture with **zero leftover bytes on every tag**; all 30 net ctests green. [orig: NapiNPClientMsg_0x00F @ 0x42E200 / NapiNPClientMsg_0x00A @ 0x42FEC0 / NapiNPClientMsg_HandlePlayerInfoFull @ 0x429BB0 / AI_GetTaskTypeFromFlags @ 0x40DAE0 / Game_StartMission @ 0x524360]
- **D-NET-55 / D-NET-64 — probe3 did NOT surface them (still OPEN).** Mid-game S2C `0x20` (D-NET-55): probe3 sent only the load-batch 0x20; the Co-op AI stayed static (no waypoint patrol), so no mid-game pool-3 stream. Guided-weapon record (D-NET-64): the player fired only `adm=74` rifle + a few grenades (zero C2S `0x0c` guided sub_ops; all 3048 are sub_op `0x0a`), and the placed Stinger AI never fired a tracked missile — no guided traffic. A follow-up capture must have the player equip+fire a Stinger/Javelin/AT4 and fly the rocket Little Bird, and the AI must actually patrol (investigate the dormant-AI cause first).

Controlled-capture validation (probe mission "ON RE Probe COOP Dvxc1" re-run — `probe3_again`, **3 human players** + 15 ballistic weapons + more movement + a second client; host + 2 client `/PROFILE` recordings, 2026-06-19):
- **D-NET-76** [MED, DOC+CODE] **The high-volume transport / anti-cheat control pings are now decoded — the largest hex-only hole in the §4 catalog (§5.34).** The richer 2-client session carried enough of each to field-map them. **RTT ping/pong S2C `0x57` ⇄ C2S `0x2C`** (×10,679 each — the single biggest channel by datagram count): identical 5-B `[u32 timestamp][u8 echoFlag]`; bidirectional — both peers ping, `echoFlag != 0` bounces the stamp back with the flag cleared, `echoFlag == 0` measures `rtt = GetTickCount() - timestamp` into a 10-sample ring (the server side also enforces `g_MinPing`/`g_MaxPing`, kicking >20× violators). **Periodic request trio S2C `0x68`/`0x43`/`0x39`** (×141 each, ~every 335 frames): each a single `[u32]` → fixed reply — `0x68` start_index → C2S `0x3D` frozen loaded-model page; `0x43` server_timestamp → C2S `0x08` time-sync; `0x39` challenge_seed → C2S `0x1C` charattr CHARACTER-row CRC (seed constant `0x3D5D`). Landed `decode_rtt_sample` + `decode_u32_scalar` (`ingame_decode`), `nw_pp` printers, catalog flips (`0x57`/`0x2C`/`0x68`/`0x43`/`0x39` → Decoded; `0x08`/`0x1C`/`0x3D` reply labels), and `nw_message_coverage` checks (**30 Decoded tags**). **`0x2C` is direction-overloaded** — S2C `0x2C` (`@ 0x427E10`) is a chat-history entry, only the C2S direction is RTT. Wire-validated: RTT timestamps pair across the two directions; the trio full-consumes. [orig: NapiNPClientMsg_0x057_RTT @ 0x432210 / NapiNPServerMsg_HandlePingResponse @ 0x515070 / NapiNPClientMsg_0x068 @ 0x42DAA0 / NapiNPClientMsg_0x043 @ 0x42FA90 / NapiNPClientMsg_HandleChecksumChallenge @ 0x42E6D0]
- **D-NET-77** [MED, DOC+CODE] **S2C `0x6B` is a minimap-overlay batch, not the objective/HUD timer the census guessed (§5.35).** `[u8 count]` + `count × 12-B records`; the handler reads only the `[u16 handle]` at each record+0 (pool-resolved) and **rebuilds that entity's minimap blip from its own engine-side state** (position, type, team @ `entity+354` → icon + team color via `update_minimap_overlay_entity @ 0x5BEC10`). The 10 trailing bytes per record are not consumed by the handler — so the `1e→1d` "countdown" the census flagged is just a byte inside a per-record blob the engine ignores, not a global timer. Landed `decode_minimap_overlay_batch` + printer + catalog + coverage. probe3_again ×266 (`count=1`, blip = the active player), full-consume. [orig: NapiNPClientMsg_0x06B @ 0x425520 → update_minimap_overlay_entity @ 0x5BEC10]
- **D-NET-78** [MED, DOC+CODE] **Weapon-reload / second death path / entity-checksum + misc client scalars decoded (§5.35).** **S2C `0x49`** weapon-reload `[u16 handle][u16 reloadParam]` → `WeaponSlot_ReloadAmmo` — and the **IDB name `handle_camera_sync_packet_0x049` is WRONG** (no camera code; reloads ammo). **S2C `0x13`** is a SECOND entity-death path beside `0x26`: `[u16 handle][i16 killerSource]` acts directly on the entity (`Health=0` + death cb), where `0x26` routes through `Entity_KillBySlotId`. **S2C `0x30`** entity-checksum request `[u8 entityId][u16 checksum]` replies **C2S `0x20`** — correcting the §4 catalog row that read "→ C2S 0x21" (0x21 is the *0x31* weapon-loadout CRC reply; the census confirms the 0x30↔0x20 pairing, ×174 each). Plus the misc scalars **`0x42`** input/state-flags `[u16]`→`Input_UnpackStateFlags`, **`0x79`** spectator flag `[u8]`, **`0x2A`** chat-history `[i32][i32][i16]`. Landed `decode_weapon_reload`/`decode_entity_death`/`decode_entity_checksum_request`/`decode_input_state_flags`/`decode_spectator_flag`/`decode_chat_history_entry` + printers + catalog (→ Decoded; `C2S 0x20` reply label) + coverage (**37 Decoded tags**). probe3_again: `0x49` ×84 (`reloadParam=195` on both players), `0x13` ×18, `0x30` ×174, `0x42` ×143, `0x79` ×355, `0x2A` ×12; `nw_pp` decodes the whole capture with **zero decode failures**. [orig: handle_camera_sync_packet_0x049 @ 0x42C0A0 (misnamed) / NapiNPClientMsg_EntityDeath @ 0x42EB50 / NapiNPClientMsg_HandleChecksumRequest @ 0x431170 / NapiNPClientMsg_0x042 @ 0x4281A0 / _0x079 @ 0x429B00 / _0x02A @ 0x425BA0]
- **D-NET-79** [MED, DOC+CODE] **Deployed-item spawn 0x59 + entity-routed sub-packet 0x44 (§5.36).** **S2C `0x59`** is the deployed-item / weapon-overlay channel — a fixed 32-B record (item ids + owner + slot + parent + 3×i32 16.16 pos + 3×u16 Euler) the host streams for placeables a player drops; one record carries a friend/foe item-id pair so the same deployable shows a different model per team (owner team @ `+354` vs local player). Witnessed in probe3_again as a `Rifle-sized Crate` (`itemId=0x0362`) dropped by player slot 5; landed `decode_deployed_item_spawn` (→ Decoded, **38 Decoded tags**). **S2C `0x44`** is an entity-routed sub-packet: a 5-B sub-header `[u16][i16 netId][u8 subtype]` whose class-dependent body the dispatcher routes to the entity's per-class `def+356` callback — the same per-class path as the C2S `0x0C` uplink (§5.10b), with `subtype` playing the field-group role. Decoded the sub-header (PrinterOnly; body left raw — class-specific, same deferral as the §5.15 guided record). `nw_pp` decodes the whole capture with zero failures. [orig: Entity_SpawnOrUpdateFromSlotPacket @ 0x546770 / NetPacket_DispatchToEntityByNetId @ 0x4D6960]
- **D-NET-80** [INFO, VALIDATED] **Multi-client lifecycle cross-validated against the `/PROFILE` .sph value-oracle.** probe3_again ran 3 Blue players (TestPlayer / TestPlayer1 / FooPlayer) through a full play session with deaths and clean leaves. The new `nw_probe3again_lifecycle_test` decodes the wire and cross-checks it against the host `.sph` (`decode_server_log`): the `.sph` reports a **3-player roster + 7 DEATH + 2 DISCONNECT** (frames 7186 / 7228), and on the wire the **2 clean disconnects coincide with the 2× S2C `0x5D`** (entity destroy-list `[i16 slot]×N` → `Entity_Destroy` + `PlayerSlot_ClearAndUnlink`) — the clean-leave channel distinct from the death-driven 0x26/0x4E despawn of D-NET-66. (The `0x5D` bodies were *empty* in this capture. **Corrected 2026-07-25 — the earlier reading that "the per-entity removal itself rides the `0x46` player-sync `0x8000` removal bit" is REFUTED by the handlers.** `0x46` bit 15 carries NO entity byte and `PlayerSlot_ClearAndUnlink @0x434730` clears BOOKKEEPING ONLY — the active flag, names, team and entity ref (@0x431411..0x43144c); the entity-field wipes @0x431437 are dead code on that leg because `entitySlotPtr` is null there, so **the entity is never destroyed by `0x46`**. Entity destruction happens ONLY in the `0x5D` sweep, whose sole witnessed trigger is a client C2S `0x32` request (`NapiNPServerMsg_SendEmptySlots @0x51a600`) — an empty body simply means the host had no empty pool-0 slots at that moment. See D-NET-176.) The wire death tags (0x26 ×18 + 0x13 ×18) cover the `.sph` death count. The test also pins the high-volume transport channels (RTT `0x57`==`0x2C`==10,679; trio `0x68`/`0x43`/`0x39` ×141 each ⇄ replies `0x3D`/`0x08`/`0x1C` ×141), the weapon-heavy session (260 C2S `0x06` across **15 distinct adm indices**, both shooters `0x0006`/`0x0007`), the `0x59` deployed-item channel (×12), AND the confirmed negatives **as assertions** (every C2S `0x0C` sub_op `0x0A` → no guided; S2C `0x20`=2 load-batch only → AI static; `0x6E` teams==0 → Co-op single team). Gated on `NW_PROBE3AGAIN_PCAP` / `NW_PROBE3AGAIN_HOST_SPH`; skips clean when absent. [orig: CServerLog_WriteDeathMarker @ 0x4e1e00 / CServerLog_WriteDisconnectMarker @ 0x4e1c50 / NapiNPClientMsg_DestroyEntityList @ 0x429730]
- **D-NET-81** [INFO, DECISION] **Weapon-variety witnessed across the fire/hit/loadout decoders; AdmDef→name resolution deferred (runtime table, not wire).** probe3_again drove `decode_client_fired_round` (§5.16) across **15 distinct `adm_index` values** in 260 fires from 2 shooters (full list + counts in §5.16) — all ballistic (none guided). The `adm_index` is a runtime `AdmDefs` table key (`@ 0x24E7FE0`, 1120 B/entry, loaded from `.adm` action-descriptor data; `AdmDef_GetEntryByIndex @ 0x53FC80`); the weapon NAME is not on the wire. Resolving it offline needs a `.adm`/weapon-def game-data parser, not a wire decoder, and the faithful-port rule forbids a hand-built name map — so `nw_pp` prints `adm` raw and a name resolver is a tracked future item (shared with §5.9.1 / §5.30). No code divergence.
- **D-NET-55 / D-NET-64 — probe3_again ALSO did NOT surface them (still OPEN).** Despite 3 players, 15 distinct weapon `adm` indices, and 260 fire events, all 9,049 C2S `0x0c` uploads are sub_op `0x0a` (zero guided field-groups), nobody used a vehicle / rocket Little Bird (every uplink `vehHdl=0xFFFF`), and the AI stayed static (mid-game S2C `0x20` = the 2-record load batch only). The 15 weapons fired were all ballistic; roster `0x6e` stayed `teams=0` (Co-op single-team). A dedicated guided + patrolling-AI capture is still required for D-NET-64 / D-NET-55.

Stock-content validation (the FIRST capture of a normal retail Co-op session — `operation_whitenoise.pcapng`, a JOX Co-op mission played to completion; 3304 datagrams; **wire-only**, no `.sph` / no authored manifest, 2026-06-19):
- **D-NET-82** [INFO, VALIDATED] **Every in-game decoder holds byte-for-byte on stock retail content.** Prior in-game findings all rode authored RE probes; this is organically-authored mission traffic at scale. The new `nw_whitenoise_coverage_test` reads the capture directly (`decode_capture_to_messages`) and asserts that **every catalogued `Decoded` tag present consumes each body EXACTLY across all occurrences** — S2C `0x40` ×1286, `0x0A` ×1485 (counted; its compact loop needs the items.def class table), `0x20` ×29, `0x10` ×41, `0x16` ×63, …; C2S `0x0C` ×1463 / `0x2C` ×1730 / `0x06` ×89, plus ~30 lower-volume tags — all clean. The §5.17 C2S `0x21` reply's 4 trailing framing zeros are validated as such (not under-read). It also records the **44-tag uncharacterized §4 tail** stock Co-op exercises (`S 0x01/0x04/0x05/0x11/0x2c/0x7e/…`, `C 0x33/0x37/…`) as a future-work surface. No divergence — the curated catalog's decoders are stock-correct. [orig: §4 S2C table `0x82AE28` / CNapiNPConnection_DispatchMessage @ 0x622570]
- **D-NET-83** [MED, DOC+CODE] **S2C `0x45` is a terrain-tile load batch, NOT "empty payload" (§5.37).** The §4 dispatch row read `_0x045 @ 0x422890` = "empty payload"; the grill shows the handler forwards the message body to `PolyTrn_LoadTileData @ 0x6081D0` (Hex-Rays renders the body arg as a separate `buffera`, but it is `[ebp+8]` = the same incoming pointer). The body is a **paged terrain-tile stream** the host sends during a client's initial-state load: `[u16 startWord][u16 endIndex]` then, on the first chunk (`startWord==0xFFFF`), a 16-B `'til0'` header (`magic`+`tileCount`+2 dwords), then `(endIndex−startIndex)` × **12-B opaque tile entries** the loader copies verbatim into `g_TerrainTileData`. Witnessed byte-exact from BOTH the writer (`serialize_terrain_tiles @ 0x6080F0`) and the reader, and against operation_whitenoise ×8 (header chunk `tileCount=381`, tiles `[0,52)` = 644 B; pages `[52,105)`… = 640 B). Landed `decode_terrain_load_batch` + struct + `nw_pp` printer + catalog flip (PrinterOnly→Decoded, **39 Decoded tags**) + `nw_message_coverage` check; `nw_whitenoise_coverage_test` consumes all 8 bodies exactly. [orig: serialize_terrain_tiles @ 0x6080F0 / PolyTrn_LoadTileData @ 0x6081D0 / NapiNPClientMsg_0x045 @ 0x422890]
- **D-NET-84** [INFO, DOC] **S2C `0x20` (and `0x45`) are LOAD-ONLY — reframes the D-NET-55 "mid-game patrol" expectation.** `serialize_entity_pool_to_packet @ 0x503460` (the pool-3 `0x20` serializer) has **exactly one caller** — `Server_SendInitialGameStateToPlayer @ 0x51BBA0`, its state-4 **sub-phase 4** (the per-joining-client load sequence: phase 1 `0x10` pool-2 → 2 `0x0D` pool-1 → 3 `0x0C` pool-0 → **4 `0x20` pool-3** → 5 `0x45` terrain → 7 `0x1A` + `SetGameState(9)`). So pool-3 `0x20` is emitted ONLY at join, never on a gameplay tick. operation_whitenoise confirms it on the wire: all 29 `0x20` fall in frames 959–1092, **before** the first gameplay `0x0A` (frame 1201). Pool-3 holds static markers (player starts / air-spawn / nav / waypoint); moving AI replicate via pool-0 (`0x0C`/`0x0A`). Therefore D-NET-55's anticipated "patrolling-AI mid-game `0x20`" **does not exist** — the load batch IS the witness, and stock content makes it a rich one (the 29-payload / 714-record paged stream exercises every flag-gated optional `0x01`/`0x02`/`0x04`/`0x08`/`0x10`/`0x20`, vs the authored probes' 2-record batch). D-NET-55's still-open half is purely the **runtime wiring** (no inbound `handle_tag_20`), and its builder spec is now confirmed load-only. [orig: serialize_entity_pool_to_packet @ 0x503460 / Server_SendInitialGameStateToPlayer @ 0x51BBA0 (phase 4)]
- **D-NET-85** [INFO, VALIDATED] **High-table `H:0x00` CS-config sparse update confirmed on stock content.** The `0x1100` 9-byte pairs in operation_whitenoise parse exactly as the documented §4 high-table `H:0x00` format `[u8 direction][u32 fieldMask][u32 value]` (e.g. `dir` 0 then 1, `mask=0x2000`, `value=0x514`; and `mask=0x08`, `value=0x0c`) — the runtime connection-settings sync, the sparse form of the `0x82` CS block. The shared `decode_capture_to_messages` correctly surfaces these as `settings_update` (the NAPI high-table control namespace, D-NET-65), so they never appear as gameplay low-table tags (the coverage test sees `high_table=0` low-table, by design). No new decoder needed — stock play validates the existing `H:0x00` map. [orig: CNapiNPConnection_HandleCSConfigUpdate @ 0x621940 / CNapiNPConnection_DispatchMessage @ 0x622570]
- **D-NET-86** [MED, DOC+CODE] **Spawn-batch `entity+16/+20/+24` is the orientation Euler triple, NOT velocity — static/spawn entities now carry their facing on the wire (§5.9, §5.11).** Watching the net spectator, every static (armory, oil pump/tower) and pool-1 spawn faced the same way (east) regardless of authored facing, while ONED placed them correctly. Root cause: the `0x10` (`NapiNPClientMsg_0x010 @ 0x433400`) and `0x0D` (`NapiNPClientMsg_0x00D @ 0x432c40`) spawn handlers write their `0x01/0x02/0x04`-gated fields to `entity+16/+20/+24`, which Hex-Rays names `velX/Y/Z` — but those offsets are the entity's **orientation Euler**, fed by `Entity_UpdateOrientationMatrix @ 0x43b440` → `Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40` as `euler[3..5]` (the same matrix builder, and the same correction D-NET-63 made for the vehicle compact record's `entity+576/584/580`). `entity+16` is the yaw heading (32-bit BAM) = `(90 − bms_yaw)` deg — identical to the `0x0C` `orientation` field (§5.18) and the `0x20` `movement_val` (§5.12). The reimpl decoded all three as `velX/Y/Z` and dropped them, so `replay_timeline` left `heading_deg` unset (0) → the spectator rendered every static at heading 0 (`bms_to_godot_basis(90 − 0)`, facing east). Fix: renamed `PoolSpawnRecord` / `StaticEntityRecord` `vel_x/y/z → euler_z/euler_x/euler_y` (`ingame_decode.h/.cpp`, `ingame_encode.cpp`), decoded `euler_z` into the spawn sample's `heading_deg` in `replay_timeline` for both `0x0D` and `0x10`; the existing `net_world_view` `90 − heading` then matches ONED placement exactly. **Cross-validated against authored truth:** `nw_dvxc1_groundtruth` now asserts each `0x10`/`0x0D` record's `euler_z == expected_bam(90 − authored_facing)` — all 6 statics + 9 pool-1 entities in probe3 reproduce the manifest facings field-for-field (`blue_armory` 90°→heading 0 gate-clear; `red_armory` 270°→`0x80000000`; `oil_pump`/`oil_tower` 0°→`0x40000000`). Plus a `nw_pool_decode_unit` regression that the spawn yaw round-trips the full S2C stack and surfaces as `(90 − heading) == authored bms_yaw`. Wire bytes / read order / sizes unchanged — label + decode-surfacing only. [orig: NapiNPClientMsg_0x010 @ 0x433400 / NapiNPClientMsg_0x00D @ 0x432c40 / Entity_UpdateOrientationMatrix @ 0x43b440 → Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40]
- **D-NET-55 / D-NET-64 — operation_whitenoise ALSO did NOT surface them (still OPEN).** Even a full stock Co-op session: all 1463 C2S `0x0c` uploads are sub_op `0x0a` (zero guided), **no** S2C `0x44` entity-routed, **no** S2C `0x59` deployed-item, and a single human shooter firing 2 ballistic weapons (`adm` 9 / 74). The `0x20` stream is the join-time load batch (D-NET-84), not a mid-game patrol. Net: D-NET-55 is no longer blocked on a capture (its remaining work is the inbound runtime wiring, spec now confirmed load-only); **D-NET-64 still requires a dedicated capture** of a player equipping+firing a guided weapon (Stinger / Javelin / AT4) or flying the rocket Little Bird.

`CNapiNPConnection_*` IDB-hygiene grill (decomp cleanup + naming validation of the whole connection-node family, 2026-06-26; IDB-only — no reimpl code change):
- **D-NET-128** [INFO, IDB] (renumbered from D-NET-116 on 2026-06-27 — the IDB-hygiene ID collided with the §5.43 behavior entry **D-NET-116** "pending-spawn loop gates on the mission-load flag", which is cited in code and keeps the number) **The `NapiNPConnection_*` family decomp was cleaned up and its names validated against the bytes.** Scope: all 47 prefixed methods + 2 unprefixed high-table handlers + 1 unprefixed teardown sibling. Changes (Jointops.exe.kong.i64):
  - **Prefix normalized to the C++ method style `CNapiNPConnection_*`** (49 functions), matching the pre-existing C-prefixed siblings (`CNapiNPConnection_LogHostStarted`/`_HandlePingResponse`/`_NetworkThreadProc`/`_SendChatMessage`). Addresses unchanged; the `[orig:]` citations in this doc were migrated in lockstep.
  - **`SendClientHello @ 0x61fe20` → `CNapiNPConnection_SendClientJoin` (MISNOMER fixed).** It emits opcode **0x42**='B' (the client JOIN/auth leg, server analog `NapiNPProtocol_HandleClientJoin @ 0x62b750`), writing the full identity block (NVS/CO/AP/BDAT/PG/PV2/CI/HK/CK/NA/PW/SIP/SPN/CU/SCRK/NF/DCNT/RCNT), not the 0x41 hello. A stale repeatable comment ("previous name confirmed correct") was misattributed (it belongs to `InitFromSession`); the function's own `[OpenNova NW-S2]` note already proposed this rename. Reimpl = `client_auth_to_bytes`/`build_client_auth`.
  - **Three functions un-misfiled to `NapiNPEnumerator_*`** — `FindTimerById @0x621f20`, `DestroyAllTimers @0x6220c0`, `DrainTimerList @0x622140` operate on the `NapiNPEnumerator` (96 B, tag "NapiNPEnumerator", `[orig: NapiNPEnumerator_Create @ 0x625f50]`, stored in `conn->session_keys.unk2C` @+0x178), reading list heads at enum+0x3C/+0x4C — offsets that on a real `NapiNPConnection` land inside `net_state`. Confirmed: every caller of `DrainTimerList` dereferences `conn+0x178` first; none passes a raw connection. (`[orig: NapiNPEnumerator_DestroyTarget @ 0x624d40]` drains both lists.)
  - **`sub_6253C0 → CNapiNPConnection_TeardownActiveConnection`** — the leave/teardown invoked by `SetState` when departing `conn_state` 1/5 (and by `Destroy`/`InitFromSession`): sends throttled goodbye/disconnect packets (`SendDisconnectPacket`, 0x46/0x86, count clamped by `cs_dir0.recv_max_per_tick`≤32), fires server/client leave callbacks, rebuilds the quick-connection list, **clears `net_state.tx_crypto_key` + `crypto_key`/SCRK**, resets `connection_id`/session keys, clears DSP queues. The prior "processes up to 32 received messages/tick" comment was WRONG (the 32 is the send clamp). NOTE: Hex-Rays fails on this function (the `add esi,0x384` `this`-reassignment); read via disasm. Fixing its bogus auto-prototype also **un-stuck the Hex-Rays failure on `CNapiNPConnection_SetState @ 0x626250`** (a caller) — both were collateral of the corrupt callee type.
  - **Struct `NapiNPConnection` filled out (1960→1984 B = the real `[orig: CNapiNPConnection_Create @ 0x62acb0]` alloc).** New `NapiNPDisconnectEvent` (184 B @ +0x654) = `{valid, role(DS), reason_code(DC), param1(DP1), param2(DP2), char message[128](DSTR), dpc(DPC), char extra[32](DDSTR)}` — field→TLV map cross-validated against `SendDisconnectPacket` (emit) + `HandleDescriptionPacket`/`PumpStateMachine` (populate). Heartbeat/keepalive block named (+0x710..+0x730: `heartbeat_enabled`, `keepalive_idle_ms`=10000, `heartbeat_step_ms`=1000, `heartbeat_min_ms`, `heartbeat_max_ms`=60000, `heartbeat_cur_ms`, `heartbeat_next_tick`) validated against the `PumpStateMachine` clamp logic. Plus send/recv tick + flag fields (+0x63c..+0x650: `last_send_tick`, `last_send_interval_tick`, `last_recv_activity_tick`, `send_holdoff_countdown`, `send_flush_counter`, `has_pending_out`), `pending_disconnect`/`desc_sent` flags, and the trailing seq fields `out_packet_seq`(+0x7ac)/`recv_ack_seq`(+0x7b8). Param typing normalized on `SendPing`/`SendDisconnectPacket`/`PumpStateMachine` so connection fields render by name. (`SendEncryptedPayload`'s param is a DSP outgoing descriptor from `NapiNPDSPQueue_PumpOutgoing @ 0x625050`, NOT a bare connection — left `NapiNPConnection**` + a clarifying comment rather than force a false field map.)
  - **Globals named/typed:** `g_txkey_charset` (`char*`→"0123456789BCDFGHJKLMNPQRSTVWXYZ", the vowel-free key alphabet) + `g_txkey_charset_len` (lazy strlen cache, 3 xrefs all in `GenerateTxKey`), `aNwuSessionKey` (the `"asdfj…"` outer NWU key), `aDpc` ("DPC" disconnect TLV tag), `g_empty_str` (the misnamed `font_name` `""` blank-fill, 407 binary-wide refs), and 17 ClientJoin/SessionInit TLV tag strings (`aNpNvs`/`aNpCi`/`aNpHk`/… defined as C strings). Opcode legs confirmed in passing: **0x44/0x84** missing-seq (`SendMissingSeqList`, NACK-style — unwitnessed in the reimpl, candidate future decoder), **0x45/0x85** ping (`SendPing`, WR/MS TLVs), **0x47/0x87** DSP encrypted-fragment session-data (`SendEncryptedPayload`). Adversarially re-verified (independent pass): every renamed field re-checked against its witnessing access; struct singletons confirmed (no duplicate types); all 49 functions decompile except the documented `TeardownActiveConnection` Hex-Rays edge case. [orig: CNapiNPConnection_Create @ 0x62acb0 / CNapiNPConnection_SendClientJoin @ 0x61fe20 / CNapiNPConnection_SendDisconnectPacket @ 0x61f2a0 / CNapiNPConnection_PumpStateMachine @ 0x6292e0 / CNapiNPConnection_TeardownActiveConnection @ 0x6253c0 / NapiNPEnumerator_Create @ 0x625f50]

`CNapiNetwork_*` / `CNapiServer*` IDB-hygiene grill (decomp cleanup + naming validation of the whole
network-context method family, 2026-06-27; IDB-only — no reimpl behaviour change):
- **D-NET-129** [INFO, IDB] (renumbered from D-NET-117 on 2026-06-27 — the IDB-hygiene ID collided with the §5.43 behavior entry **D-NET-117** "world-path pose look-pitch", which is cited in code and keeps the number) **The `CNapiNetwork_*`/`CNapiServer*` family (42 functions, 0x4a8040-0x4ca4a0) was cleaned up and its names validated against the bytes.** Changes (Jointops.exe.kong.i64):
  - **Receiver type consolidated onto `NapiNPServerCtx`** (user-approved). The duplicate `CNapiNetwork` struct (4432 B, `field_*` placeholders) was **deleted**; all 42 method `this`/ctx params now type as `NapiNPServerCtx *`, matching the type already on `g_napi_np_ctx`. `NapiNPServerCtx` grown from 4520 to its true **5232 B (0x1470)** (witnessed by `ClearState`'s `memset` and `OnPlayerDisconnected`'s +0x11A8 write); `field_5C`→`connection_mode`, new `active_connection_id`@0x1190, `randomized_timeout_ms`@0x1194, `disconnect_reason_buf`@0x1270; four interior auto-named globals folded back in as ctx fields. See §6.2/§6.3.
  - **Calling-convention fixes:** `CheckPlayerTimeouts`, `DisconnectActiveConnection`, `ProcessPendingPlayerSpawns`, `DisconnectPendingSpawnBans`, `ClearState`, `SerializeToSession` were `__thiscall` mis-detected as `__cdecl`/no-args (used `ecx` as the object base); corrected so fields render by name.
  - **Misnomers fixed (6):** Kong `PumpProtocolType25/737/26/738` → `PumpServerProtocolRecv`/`PumpServerProtocolSend`/`PumpClientProtocolRecv`/`PumpClientProtocolSend` (the suffix was the literal `flags` value; recv/send from the `NapiNPProtocol_Pump` 0x8/0x3F0 decode, server/client from the connection-type low-bit selector + caller split); `PumpManagerType4` → `PumpManagerReceive`; `SendPunkBusterChat @0x4c9140` → `DisconnectActiveConnection` (no chat — builds a `NapiNPDisconnectEvent` + `RequestDisconnect`); `CNapiServerInfo_Init @0x4c8690` → `CNapiNetwork_ClearState` (resets the whole ctx, not a sub-struct); `CNapiNetwork_GetConnectionParams @0x4a8040` → `VideoConfig_GetResolution` (**not a network function** — reads display width/height/AA from `off_840960`).
  - **`RandomizeTimeout` retail address pinned: `0x4c4d80`.** The `0x4a6d50` cited in `libs/napi/include/napi/session.h` and `libs/novaworld/include/novaworld/connection/manager.h` is the **jodemo** image, not retail Jointops — migrated in lockstep.
  - **Globals named/typed:** `g_is_dedicated_server` (0xB5F4E4), `g_server_join_locked` (0xC94794), `g_local_net_address_str` (0x7CA298), `g_net_spawn_suspended` (0x24D1DE0, formerly `dword_24D1DE0`, the mission-loading spawn gate, §5.42).
  - Adversarially re-verified (independent pass): all renames/types re-checked against witnessing accesses; no duplicate types; `connection_mode`@0x5C flagged WEAK (writer `CGameSession_SetConnectionMode @0x4c49f0` confirmed, no in-family reader). [orig: CNapiNetwork_Init @ 0x4ca4a0 / CNapiNetwork_ClearState @ 0x4c8690 / CNapiNetwork_RandomizeTimeout @ 0x4c4d80 / CNapiNetwork_DisconnectActiveConnection @ 0x4c9140 / NapiNPProtocol_Pump @ 0x62a650 / CNapiNPConnection_PumpFlags @ 0x629780]

NapiNP dispatch-surface + crypto IDB-hygiene grill (decomp cleanup, name validation, global
typing across the whole `*napinp*` roster, 2026-06-27; IDB-only — no reimpl behaviour change):
- **D-NET-118** [INFO, IDB] **The full `*napinp*` dispatch/message-table + crypto surface was
  validated and its remaining naming errors fixed (Jointops.exe.kong.i64).** Roster: 356 functions;
  a typed-prototype scan found **352/356 already clean** (the 4 "rough" — `DestroyEntityList`,
  `NapiNPClientMsg_0x00C`, `NapiNPPlayer_Destroy`, `NapiNPManager_Shutdown` — are accurate
  `__usercall`/register-arg Hex-Rays representations with named args, not dirty). The prior
  D-NET-128/129 grills had already cleaned the connection/network-context families; this pass
  audited the dispatch tables, the message handlers, the opcode legs and the crypto helpers.
  - **All four dispatch tables cross-checked by reading the data and resolving every handler.**
    `g_np_msginfo_client @ 0x82AE28` (123+sentinel) and `g_np_msginfo_server @ 0x82B5D8`
    (72+sentinel): **every generic `NapiNP{Client,Server}Msg_0xNNN` suffix equals its wire
    `msg_id` (hex) — zero mismatches.** `g_np_opcode_handlers @ 0x849D90` (14+sentinel) and
    `g_np_msginfo_highbit @ 0x849E80` (4+sentinel, all `CNapiNPConnection_Handle*`) fully named.
  - **Three genuine misnomers fixed** (each single-xref'd from the C2S table — bogus auto-applied
    names, not shared utilities): `server_broadcast_entity_kill @ 0x515390` →
    **`Server_BroadcastMedicRequest`** (anchored: the handler formats `GameText "Server"/"STRSRV_MEDREQ"`
    and sends opcode 0x14 param 310 — it is the medic-request broadcast, matching D-NET-108 which the
    IDB had drifted from; comment corrected from "kill message"); `Path_ReplaceExtension @ 0x500ec0`
    → **`NapiNPServerMsg_0x03D`** (body is an authority-gated entity write `slot+352→entity+192`,
    stores `dword_A87060`→`entity+97560`; role uncharacterized — `unknown`); `ErrorLog_Write @ 0x500e10`
    → **`NapiNPServerMsg_0x03E`** (single `retn` no-op stub). Plus one convention-normalize:
    `napi_np_server_msg_0x049_parse_player_status @ 0x510f40` → `NapiNPServerMsg_0x049_ParsePlayerStatus`.
  - **Opcode legs `0x44`–`0x47`/`0x84`–`0x87` confirmed named + behaviourally correct** (resolving
    the §4 open question): `Nwu_HandleClientResendList @ 0x6241f0` delegates `NapiNP_HandleResendList(…,1)`
    (NACK/resend), `Nwu_HandleClientProbe @ 0x624280` = `!disable_processing && FindConnection(...)`.
    Opcode-handler prototype: `int __cdecl(NapiNPProtocol*, NapiNPOpcodeInfo*, int addr, int port,
    u8* data, int len, int arg6)`.
  - **Magic `0x7C08C6` resolved** = `&g_empty_str` (the empty-string default), carried by every
    valid msginfo/opcodeinfo entry; sentinel = 0. A non-null validity marker, not a stamp.
    `handler2` confirmed `0` across all gameplay-table entries (only high-bit H:0x02 → `nullsub_280`).
  - **Net-owned global typed:** `g_napi_prng_state @ 0x31C1078` set to the existing **`LCGState`**
    struct (reused — no new type): `seed` / `multiplier = 78665521` (NWU_LCG_MAGIC) / `counter`;
    the NapiNP connection-entropy LCG, seeded by `PRNG_InitFromTimestamp @ 0x794260`, drawn 4× in
    `NapiNPServer_HandleNewConnection @ 0x4c8040`. The ~76 remaining auto-named globals in the net
    core are per-function static scratch (`stru_80EAxx` family), log-record dwords, and rodata
    format pointers — not net-semantic state; left unnamed per the net-owned-only scope.
  - **Crypto confirmed MATCHING (read-only):** `CNapiNPConnection_SendSessionPacket @ 0x61edd0`
    two-layer NWU encrypt (per-connection key `conn+204` then static `aNwuSessionKey`
    "asdfj2349857…"); naming/typing of the `NapiNP_*` crypto/TLV family unchanged from the prior
    byte-exact grills. Behavioural handler spot-checks found no drift beyond the medic case.
    The 0x41 handler remains distinct from `SpawnEffect` 0x27 / `HandleSpawnEffect` 0x21,
    but its IDB name `ClearAnimSlot` is corrected above: it clears a charattr property.
  - Adversarially re-verified (independent pass): renames re-checked against witnessing accesses
    and single-xref provenance; no duplicate types introduced (`LCGState`/`NapiNPMsgInfo` reused).
    `idb_save`d. [orig: Server_BroadcastMedicRequest @ 0x515390 / NapiNPServerMsg_0x03D @ 0x500ec0 /
    g_np_opcode_handlers @ 0x849D90 / g_empty_str @ 0x7C08C6 / g_napi_prng_state @ 0x31C1078 /
    CNapiNPConnection_SendSessionPacket @ 0x61edd0]

C5 joi-regurl (PARTIAL): documentation only — NK separator ':' and HOSTKEY trim ('&' then ']')
confirmed; `parse_joi_connection_string`'s NI/NP-presence gate is a defensible live-path choice;
over-length field clamp is low-priority. No code change required.

**Refuted (evaluated, NOT divergences — do not "fix"):** A4 DE/PV3/PW (dead-in-retail for any
NW join, byte-identical); A7 verify-cookie carries locale+identity and already matches the .204
capture; A1 `0x04B0ED91` (decompiler comment misread; real immediate 0x04B05731 already matches);
A1 demo-vs-retail anchor (doc-cite only); A3 CI/EIP/EPN/PN gating (cited the ANNOUNCE path, not
ClientHello); A8 `[domainname]` guard (byte-identical on the real-NW path); B6 decoder bounds
(both memory-safe, identical consumed bytes); B7 signedness (host/CRT type artifact, identical
for reachable inputs); C1 ×3 (EPASK query-vs-form, GET-vs-POST, per-field-vs-per-widget — all
byte-identical / already settled NW-S5/B); C2 cookie-name trim + subnet jar (behavior-preserving
for the witnessed flow); C3 remember-login (unimplemented feature, unverified); D1 jointoperations_pg
mechanism (sub_62E750 has no in-match caller); D2 ServerVar order (0x4d0660 is the client
serializer; no Server serializer exists in this binary); B4 url-cipher i≥22 (unverified — IDA was
down; unreachable for realistic payloads).

**Fixes applied** (build clean; scoped net ctest green incl. `nw204_lobby_decode` + the new
`gsb_real204_decode`):
- Wave-7 grill commit: D-NET-1, D-NET-2, D-NET-4 (`session_hello.cpp`), D-NET-19 (`client_session.cpp`).
- GSB rewrite: D-NET-32..36 (`gsb.cpp`/`gsb.h`, server emit + binding consume) — retail chunk
  framing + row layout, byte-verified against the genuine `.204` blob via the new
  `gsb_real204_decode_test` oracle (fixtures/novaworld/nw204_jop_2.gsb). Server browse against
  real NovaWorld now produces a retail-parseable list.

**High-value follow-up backlog (tracked rewrites):** JO game PG discovery (D-NET-49);
~~pool-3 0x20 runtime wiring (D-NET-55)~~ **RESOLVED 2026-06-27** — `encode_pool3_sync_batch`
is now emitted from the §5.2a world-stream phase 4 (`Server_SendInitialGameStateToPlayer`,
the D-NET-84 load-only path) and decoded inbound by `NetClientView::apply` case 0x20; and giving
`make_client_host_request` the same `ClientVarList` wrapping `make_client_play_request`
now has (D-NET-38). Each carries its `[orig]` anchor and corrected behavior above.

NetPacket serializer + client-loop/scoreboard-state IDB-hygiene grill (decomp cleanup, naming
of remaining `sub_*` net helpers + net-state globals, name validation, 2026-06-27; IDB-only — no
reimpl behaviour change):
- **D-NET-130** [INFO, IDB] (renumbered from D-NET-119 on 2026-06-27 — the IDB-hygiene ID collided with the §5.43 behavior entry **D-NET-119** "the C2S 0x0C drain enforces the per-connection owner gate", which is cited in code and keeps the number) **The remaining unnamed `sub_*` helpers in the in-match packet/serializer
  band (0x500000-0x509000) and the client per-frame net-loop / scoreboard-message state globals were
  named and validated against the bytes (Jointops.exe.kong.i64).** The prior D-NET-128/129/118 passes
  had cleaned the connection / network-context / dispatch-table families; this pass took the leaf
  `NetPacket_Write*` serializers, the `CNetQuality` client tracker, and the decoded net-message
  globals. Changes:
  - **Functions named (24, all were `sub_*`).** Packet serializers (all single-purpose `Write` leaves,
    `NetPacket_*`-family convention): `NetPacket_WriteEntityHandleAndTeam @ 0x503770`
    (pool<<12|slot + team, 4 B), `NetPacket_WritePositionTeamAndName @ 0x503800` (3×i32 pos + i16 team
    + cstr name), `NetPacket_WriteOverlayAction @ 0x505D50`, `NetPacket_WriteTerrainTiles @ 0x506570`
    (via `serialize_terrain_tiles`), `NetPacket_WriteBriefingText @ 0x506620` (briefing3 + briefing2/
    briefing cstrs), `NetPacket_WriteReplayStreamChunk @ 0x506F60`, `NetPacket_WriteType6SlotStates @
    0x507100`, `NetPacket_WriteReplayDataBlock @ 0x5071F0`, `NetPacket_WriteFixed180Block @ 0x507300`.
    Net state/util: `CNapiNetwork_StartClientConnection @ 0x4CA160` (begin client connect to a selected
    discovered session — wires `OnConnectedToServer`/`OnDisconnectedFromServer` callbacks, auth ticket,
    2000/30000 timeouts, parses the NovaWorld `.joi` join tokens ni/np/bk/nk into the connection, then
    `CNapiNPConnection_InitFromSession`; only caller `UI_JoinSelectedSession @ 0x5699d0`),
    `CNetQuality_Reset @ 0x4C58C0` (resets the `g_netQuality` link-quality/anti-cheat tracker — domain
    confirmed by `CNetQuality_SetCheatFlag @ 0x4c34f0` reading the same +7 flags/+8 display-timer/+12
    cooldown offsets), `Network_DrawDebugScreen @ 0x500EF0` (Server/Client debug overlay: FPS, CPU,
    local/remote QuantumSize = `cs_dir*.send_holdoff_ticks`), `NetSync_IsEntityEligibleInWindow @
    0x507AA0` (pool-handle validity gate: pool bounds + timestamp window + state-flag mask, used to
    filter entities for a net update), `Server_InitServerTicks @ 0x5018C0` (constructs the two
    `CServerTick` instances from CC.BIN), `ItemPoolIterator_Advance @ 0x501740` (cat<<12|slot pool
    iterator). MP/server gameplay siblings reached through the same serializer band, named for
    completeness: `Spawn_FindNearestMarkerByTypeAndTeam @ 0x501000` (team@entity+354, matching §5.x),
    `EntityLimit_InitTable @ 0x509A70` / `EntityLimit_SetEntry @ 0x500E50` (per-entity-type, 70-per-team
    MP spawn-limit table), `Game_AccumulateTeamScores @ 0x508D70`, `Game_CountAlivePlayersPerTeam @
    0x5001C0`, `Player_ComputeScore @ 0x500A80`, `PlayerSlot_FindByEntityTypeName @ 0x5009E0`,
    `Server_DumpPuntLogToFile @ 0x500260` (batch-dumps the in-memory kick log to `punt.log` — distinct
    from the live `Server_WritePuntLog @ 0x4f9c70` which appends one event to `_PUNT.TXT`),
    `CNapiVarEntry_AddToIntValue @ 0x6305F0` (NW container var helper).
  - **Globals named/typed (23).** Net state: `g_netQuality @ 0x82BF88` (CNetQuality tracker),
    `g_netMsgLen @ 0xB5CBB4` (shared outgoing message-length scratch, set by every `NetPacket_Write*`
    then passed to `CNapiNetwork_QueueReliableMessage`), `g_lastKeepaliveTick @ 0xA822A0` (msg 0x34
    timestamp keepalive, ~480 s), `g_netQualityReportTimer @ 0xA85B84` (msg 0x4C every 310 ticks),
    `g_slotRefreshTimer @ 0xA85B80` (62-tick), `g_tag2CSendCooldown @ 0xA860D8`, `g_netPlayerCount @
    0x24C1A90`, `g_poolListEnd @ 0xA8933C`, `g_replayBlockMagic @ 0xC86FC4`. Scoreboard-message (0x056)
    decode outputs: `g_scoreGameType @ 0x24C1970`, `g_scoreTeamScore0/1 @ 0x24C1974/78`,
    `g_scoreTeamCount @ 0x24C197C`, `g_scorePlayerStats @ 0x24C1AD0`, `g_scoreReassemblyStream @
    0xA82324` (CDataStream chunk reassembly), `g_scoreboardDirty @ 0xA81B28`. MP/server: `g_serverFps
    @ 0xC8FC64`, `g_serverCpuPct @ 0xC8FC68`, `g_entityLimitTable @ 0xC7B480`, `g_entityLimitCount @
    0xC84680`, `g_puntLog @ 0xC74480`, `g_puntReasonStrings @ 0x82F148`.
  - **Wire-critical names re-validated MATCHING (read-only):** `Network_CompressFixedPoint @ 0x4c2780`
    / `Network_DecompressFixedPoint @ 0x4c27e0` — bit layout (sign=bit0 via ROR1+ASR31, exp=bits1-3
    used as the shift, mantissa=bits4-15) is self-consistent compress↔decompress and matches the §5
    position-compression record. Generic `NapiNP{Client,Server}Msg_0xNNN` suffix↔msg_id correctness
    was already re-confirmed by D-NET-118; not re-derived.
  - **One suspected misnomer flagged, NOT renamed (insufficient proof):** `Network_CalcSendRateTiers @
    0x5b7c50` is called ONLY from the scoreboard/playerlist net handlers `NapiNPClientMsg_0x01D`/`_0x056`
    (right after `g_netPlayerCount` is refreshed) and writes `dword_28E4DE8/DEC/DF0` from player-count +
    dedicated-flag thresholds (17/25/34/50/51). The same three globals are consumed by `sub_5BAF20` as a
    clamped position and zeroed on respawn (`sub_5B71B0`), so the name may describe an overlay/scoreboard
    display tier rather than a UDP send-rate throttle. The `Network_` prefix is *plausible* (player-count-
    driven send throttling is real) and the name is human-curated, so per the shared-state rule it was
    left in place with an IDB comment marking it NEEDS VERIFICATION; `28E4DE8/DEC/DF0` left unnamed.
  - **Adversarial false-positives kept OUT of the net picture:** address-range proximity alone is not
    evidence — `sub_4CD460` (terrain/water/environment mission setup), `sub_5B71B0` (mission/respawn
    state reset), and `sub_5BAF20` (the clamp helper above) sit inside the net bands but are not net code
    and were deliberately left unnamed / not net-tagged. Every rename in this pass was gated on a
    witnessed access or single-xref provenance; no duplicate types introduced (CNetQuality left as the
    named global without forcing a speculative struct — layout only partially witnessed). `idb_save`d.
    [orig: CNapiNetwork_StartClientConnection @ 0x4CA160 / CNetQuality_Reset @ 0x4C58C0 /
    CNetQuality_SetCheatFlag @ 0x4c34f0 / NetPacket_WritePositionTeamAndName @ 0x503800 /
    NetSync_IsEntityEligibleInWindow @ 0x507AA0 / Network_DecompressFixedPoint @ 0x4c27e0 /
    NapiNPClientMsg_0x056 @ 0x431d10 / Network_CalcSendRateTiers @ 0x5b7c50]

**D-NET-131** [reimpl divergence, **FIXED 2026-07-24**] **"Serve only" now selects the
original's mode-1 host-only row and creates no local client/player.** Witnessed in
`[orig: UI_HandleHostSessionStart @0x556d00]`: the LAN host branch reads the `SERVERTYPE`
spinlist value `[orig: HostDialog_ReadSettings @0x555940,
dword_2550AB8 = CSpinListWnd_GetSelectedValue]`; `HG_SERVEONLY` (value 1) calls
`[orig: CGameSession_SetConnectionMode @0x4c49f0]` with mode **1**
(`is_host=1,is_client=0`), while `HG_SERVEPLAY` (value 0) uses mode **3**
(`is_host=1,is_client=1`). `start_host_session` now makes the same selection:
`serve_and_play=false` installs `ConnectionMode::HostOnly`, passes no type-2 loopback to
`create_session`, and therefore cannot create the former phantom local player;
`serve_and_play=true` retains `ConnectionMode::HostClient`, the type-2 loopback, and the
host's real local player. The helper and 62-Hz owner pump remain shared; only the role row
and local-client registration differ. Pinned by `npruntime_server_session` in both modes.

**D-NET-132** [reimpl consolidation, IMPLEMENTED 2026-06-30] **One `GameConfig` server-state struct +
roster identity read THROUGH `link.owned_entity`, not a per-connection cache (ADR 0013 §6.9).** Two
faithful-port findings, both wire-neutral on the byte-parity goldens:
(1) **The three diverging config structs are merged into one `GameConfig`** (`libs/npruntime/include/
npruntime/game_config.h`), mirroring the `CAdminServer SET` field set (§6.9). The reimpl had modeled the
one `g_GameType @0x24D2128` as THREE copies feeding different serializers with different values
(`rules.game_type`=0 → 0x08 dword[3]; `session_config.gametype`=0x10010 → 0x7B; `game_settings.game_type`
=0 → BuildFlags) — an artifact. IDA proves `[orig: ServerConfig_SerializeToPacket @0x505bd0]` (0x08 dword[3]
@0x505c2b) and `[orig: NapiNPMsg_0x7B_BuildPayload @0x507740]` (0x7B gametype @0x5078ce) read the SAME
`g_GameType`; `[orig: CNapiServerConfig_BuildFlags @0x4c4dc0]` reads a `game_settings.game_type` copy
(ctx+0xCC) equal to it in a live session. So the reimpl collapses to one `game_type` field feeding 0x08 +
0x7B + BuildFlags + `assign_player_team`. On the dev/test default (`game_type`=0) the 0x7B/0x60 gametype
byte shifts `0x10010`→0 (unpinned, and now self-consistent with the 0x08 block); on a real host `game_type`
seeds the mission gametype so 0x08 dword[3] and 0x7B agree — the faithful behavior (retail frame-146 0x08
carries the gametype, not 0). (2) **The reactive-reply roster identity collapses onto `link.owned_entity`.**
`[orig: CAdminServer_HandleStatus @0x402e30]` proves the roster IS the player-slot entity array and reads
name/**team (@entity+344)**/class/kills/deaths/ping OFF each entity (`sprintf @0x403182`), so the §5.1 reply
builders (`build_reply_tag_16`/`rep_for_slot`/`make_rep_state`) now read the wire handle off
`owned_entity.packed` and the team off the live `world::EntityRegistry` Entity through it — the former
`SessionReplyState.{binding_valid, player_entity_handle, team}` cache is removed. `player_slot` (roster
ORDER) + the echoed `player_name` stay on `conn.reply`. The pre-World reactive path
(`bind_session_reply_player`, a World-less session-responder / `handshake_server_test`) stamps the bare wire
handle onto `owned_entity` so it resolves the same way (team defaults when no live entity backs it). All 21
net ctests + the byte-parity goldens (`npruntime_golden_lan_join`, `npruntime_golden_gameplay`,
`nw_golden_diff`, `nw_message_coverage`, `nw_capture_decoder`) stay green. [orig: ServerConfig_SerializeToPacket
@0x505bd0 / NapiNPMsg_0x7B_BuildPayload @0x507740 / CNapiServerConfig_BuildFlags @0x4c4dc0 /
NetPacket_WriteServerNameAndMapFile @0x505780 / CAdminServer_HandleStatus @0x402e30 /
CAdminServer_HandleSetCommand @0x405a60]
**D-NET-133** [reimpl residual, PARTIALLY FIXED 2026-07-21] **`build_full_entity_spawn` now reads the
modeled retail sources for several formerly deferred fields** (§5.46): a null `has_item_def` emits zero
`item_type_id`/`item_type` and an empty name (preserving the client's destroy-and-stop gate); a resolved
def supplies its raw `item_type`, and `item_attrib & 0x100000` is the exact name gate.
`primary_occupant` / `ground_target` / `mount_target` source entity+368/+40/+364. Seat discovery now keeps
gameplay's dense vector while assigning the retail fixed slots (passengers 0..7, control/driver 8,
UseGun 9), and empty live slots serialize as `0xFFFF`. `pitch`, `ref_num`, and `sub_type` source the other
modeled pose/tail fields. The rich extractor test pins these values through encode/decode, including
sparse fixed seats, low-byte, and BAM high-word truncation.

This facet remains broader than a single-byte residual: entity+340 is unmodeled; the builder reads
`world::Entity::ai_state`, but production AI does not yet mirror the live `AiBrain` current state into it;
and the non-player entity+36 low-16 flags source is still zero. The player-specific flag and minimap-ID
approximations remain separately tracked as D-NET-136/137. The 0x0F handler also over-answers when the
requester has no session player (retail gates on session+192 non-null @ 0x5141a8). An in-capacity EMPTY
slot deliberately remains a zeroed type-0 record rather than retail's possibly stale slot memory; the
client-observable destroy+clear effect is identical and the permanent garbage-policy facet remains
registered under ADR 0022.

**D-NET-134** [reimpl divergence, DOCUMENTED 2026-07-01] **The per-frame S2C 0x0A sub-block phase counter
cycles a SAFE 3-value subset `{1,0,3}` instead of the original's free-running 4-value counter (§5.47).**
The original `NetPacket_WritePlayerState @0x4ff6b0` writes `flags2 = playerSlot+100566` (a free byte
counter), so `flags2 & 3` cycles all four sub-blocks — including **2 (env)** — evenly, and `flags2 & 0xF
== 8` emits a passenger block every 16th frame. Our `emit_connection_s2c` advances the same counter
(`netsim::Connection::s2c_phase`) but maps it to `{1 server-status, 0 weapon, 3 gametype}`, omitting env
and passenger. Phase 3 is now faithful within that subset: objective game types carry the four live
`World::subgoals` masks, with the off-wire gate learned from S2C `0x08`/`0x7B`; other modes
carry zero bytes. Reason for the remaining omission: env sub-block 2 authoritatively OVERWRITES the
client's `Env_FogDistTarget` /
`Env_CurTimeFixed24`, and our headless host does not populate `world.env` (it is set only by WAC
`TOD`/`fogdist` commands; ASH_I5A drives TOD from its `.env` file, which the netsim world does not load) —
so emitting it would darken/de-fog the client's correctly mission-loaded sky. Sending nothing leaves the
client's own env intact (the correct visual). Weapon sub-block 0 is emitted with the golden-witnessed
co-op steady value (all-zero slots + zero uniform mask) pending a recipient-weapon-slot model. Both env
and passenger slot back into the
free counter unchanged once `world.env` authoring and vehicle-mount modeling land. First send is phase 1
so the load-bearing `C6EAE4` fall-damage tolerance reaches the client on frame 1 (matches the original,
which increments to 1 before its first write).

**D-NET-135** [FIXED 2026-07-20] **World-stream batch paging now uses retail's 650-byte budget with
the witnessed per-pool headroom margin checked AFTER each record.** The pool serializers self-limit
inside a 4096-B caller buffer: 0x0C
organics break on `written + 100 > 650` (`serialize_entity_states_to_buffer @ 0x5030a0`, guard
@ 0x50340d — margin 100 covers the variable name string), 0x20 pool-3 on `written + 30 > 650`
(`serialize_entity_pool_to_packet @ 0x503460` @ 0x503694), 0x10 pool-2 static on `written + 40 > 650`
(`serialize_pool2_static_to_buffer @ 0x5042F0` @ 0x504687-region). `np::slice_batch_pages`
now accepts an explicit pre-write or post-write policy. The four entity-pool call sites select the
post-write policy with their retail margins, retaining the crossing record and the existing
at-least-one-record guarantee, page cursor, and paced resume behavior.
**Complete witness (2026-07-05), scoping the fix precisely:** all four world-stream pool margins are
now pinned — 0x0C = **100** (`@ 0x5030a0`), 0x20 = **30** (`@ 0x503460`), 0x10 = **40** (`@ 0x5042F0`),
and 0x0D = **110** (`serialize_entity_pool_to_packet_0 @ 0x503940`, guard `write_ptr - buffer_start +
110 > 650`) — all against the common **650** budget, checked AFTER each record (the crossing record IS
included). **The 0x45 tiles are NOT part of this divergence:** `serialize_terrain_tiles @ 0x6080F0`
uses a different model — fill a caller `buf_size` while `remaining >= 12` (a PRE-check that EXCLUDES the
crossing tile). The production 0x45 call uses the named 650-B pre-write policy, reproducing the
D-NET-83 stock boundaries exactly: 52 tiles / 644 B on the 20-B-header first page and 53 tiles /
640 B on 4-B-header continuations. The four entity pools use their named post-write policies.
`npruntime_batch_chunker` pins the real four-byte pool-3 header boundary (624/34 B), strict
greater-than comparison, all five named policies, and both tile page shapes;
`npruntime_initial_state_burst` covers the production initial-state path.

**D-NET-136** [reimpl divergence, DOCUMENTED 2026-07-01] **The 0x0C player-record `entity+36` bit
0x01 is computed PER-RECIPIENT ("this is your own entity"); retail sets it ONCE per entity at add
time.** `Server_PlayerAdd @ 0x51cbc0` does `entity+36 |= 1` (@ 0x51d0da) gated on `add_event+108`
(= a dword at `NapiNPPlayer+0x37`, low byte; set only on the remote-add path — the host's LOCAL
player takes the early-return path @ 0x51cc31 and never gets it), and the serializer copies
`entity+36` verbatim with no recipient-conditional logic (`@ 0x5030a0` @ 0x50324c) — per-recipient
variation is impossible in retail. The same-map ASH_I5A capture (0x0101 only on the joiner's record)
is fully explained by remote-vs-local add. Our `player_wire_flags` (`entity_wire_bridge.cpp`) is
wire-identical for a host+1-joiner session but diverges for ≥3 players (retail sends 0x0101 for
OTHER remote players too). Kept until the `NapiNPPlayer+0x37` gate semantics is witnessed (open
question: its writer — JIP flag? always-set-for-remote?); then the faithful port is a per-entity
flag stamped at player add.

**D-NET-137** [reimpl divergence, DOCUMENTED-TOLERABLE 2026-07-01] **The player wire net_id
(entity+348, the per-team MINIMAP slot id) is an invented encoding shim, not the retail packing —
tolerable because the client self-heals unmatched ids.** Retail allocates the id from the minimap
slot array: seeded from the joining client's own JSP fields (jsp[56]/jsp[58],
`Server_BuildPlayerInfoAndAdd @ 0x51d560`), validated by `MinimapSlot_HasEntity @ 0x57b140` and
(re)allocated by `lookup_entity_slot_and_pack_entry @ 0x57ad40`, whose packing (@ 0x57ae47, decoder
`MinimapSlot_FindByPackedId @ 0x57a270`) is `type(bits 0-4) | subtype(5-8) | index(9-14) |
SIDE(15)` over the 288-byte-stride registry (bit 15 = side-B, matched against entry+280;
CORRECTED 2026-07-02 from the earlier "alive" reading — §5.59) — golden `0x0200` = side A index 1,
`0x8207` = side B type 7 index 1. Our `player_minimap_net_id` emits
`(team==2?0x8000:0)|0x0200|(slot&0x1F)` — its "team bit" happens to land on the real SIDE bit, but
`slot` lands in the `type` field. Interop-safe because `NapiNPClientMsg_0x00C @ 0x42eadb`
reallocates a fresh minimap slot and OVERWRITES `entity->NetId` whenever `MinimapSlot_HasEntity`
fails — any per-entity-distinct id that resolves to NOTHING self-heals; an id that resolves to the
WRONG entry does not (that failure mode is D-NET-148). ~~The 0x51 spawn-confirm's NetId/animSlot
zeros are the same family (client treats 0x51 as a spawn signal, does not field-parse it)~~ —
REFUTED 2026-07-02: the client FULLY field-parses 0x51 and rebinds `CharacterEntity` from its
packed char id (`NapiNPClientMsg_HandlePlayerSpawn @ 0x431BB0` @ 0x431cad/@ 0x431cf3; §5.59,
D-NET-148 — retail never sends 0x51 outside the team-change flow). Faithful fix: model the minimap
slot array and allocate/pack for real.
Exonerated for the C2S 0x0F flood (2026-07-02 recon): in the 0x18 apply the record's net_id feeds
ONLY the minimap path (`@ 0x433ddd`, gated on `Flags & 0x100` + person type) — it is never an input
to the `@ 0x4307c4` itemDef/ItemTypeIndex cross-check that queues 0x0F (the flood was D-NET-138's
field-17 byte). UPDATE 2026-07-25 (D-NET-146): the #300 ClientAuth omission is repaired, so the
joiner path is retail-faithful again — the id is SEEDED from the joiner's mounted-`Avatars.def`
CI0/CI1 join vars, picked per assigned team and echoed via `Entity::minimap_net_id` (stock
side-B 0x8207 reproduced exactly). The shim now only backstops truly var-less callers (the host's
own player emits its 0x0200 through it). Residual gap = host-side character-table validation.
Corrected attribution 2026-07-25: the substitution is NOT in `MinimapSlot_HasEntity @0x57B140`
— that is an 11-instruction boolean probe over `MinimapSlot_FindByPackedId @0x57A270` and
substitutes nothing. It lives in `Server_BuildPlayerInfoAndAdd @0x51D560`, which gates EACH
uploaded CI field on that probe (`@0x51d6a4` for CI0/jsp[56], `@0x51d6dd` for CI1/jsp[58]) and,
on failure, RE-PACKS the field through `lookup_entity_slot_and_pack_entry @0x57AD40`
(`@0x51d6ba` / `@0x51d6f4`, called with the alignment index 0/1): the first 288-byte slot whose
`+280` matches that alignment, falling back to slot 0 (`@0x57ad91..0x57adda`) and to a packed 0
on an empty table. OpenNova currently trusts any nonzero uploaded id. Duplicate selections are
valid reusable appearance keys, not per-player collisions (`MinimapSlot_FindByPackedId @0x57A270`).

**D-NET-138** [reimpl divergence, FIXED 2026-07-02] **The 0x0A player compact-record field-17
byte was sent as raw clamped health; retail packs `[bits 4-5 health tier | bits 0-3 playerClass]`.**
The server-side quantizer is now witnessed: `Entity_GetHealthClassification @ 0x4AD4E0`, called
from the case-1 compact write in `NetPacket_SerializePlayerState @ 0x4C09C0` (call @ 0x4c0d71, byte
store @ 0x4c0d89 — the LAST byte of the compact record). Formula: `ratio = (Health<<16) /
max(itemDef->healthMax, 1)` (16.16; Health @ entity+0x11E, healthMax @ itemDef+0x17C); tier 2 if
`ratio > 49152` (0.75), tier 1 if `ratio > 28671` (0.4375), else tier 0; `byte = (tier << 4) |
(entity->playerClass & 0xF)` (playerClass @ +0x294). Null entity → 0x20; null itemDef → `0x20 |
(playerClass & 0xF)`. The client apply `Entity_SetHealthFromDifficultyByte @ 0x4AD580` is the exact
inverse — it reconstructs the tier MIDPOINT (87.5 % / 59.375 % / 21.875 % of healthMax) from the
same two constants with 0x8000 rounding; the LOCAL player skips the apply (`@ 0x4c11ac`). Ported as
`netsim::health_classification_byte` (`libs/netsim/entity_wire_bridge.cpp`), fed by
`GameEntitySnapshot::player_class` (the [5,9]-else-8 clamp) and `health_max` (class-8 150 stopgap
until items.def healthMax is resolved onto the world entity); boundary-exact unit tests in
`netsim_two_peer_fanout`. **This byte was also the C2S 0x0F flood root cause** (§5.46): the raw
byte re-classed remote players every applied frame. Live retail-join verification 2026-07-02
(v11 capture, full join+deploy+move): **0 C2S 0x0F** vs 1,526 in the pre-fix v10 session.

**D-NET-139** [reimpl approximation, DOCUMENTED 2026-07-02] **The 0x0A priority score ports the
distance/age/own-boost terms; the view-interest terms contribute 0.** `select_frame_entities`
(`libs/netsim/connection_fan.cpp`) ports from `Server_BuildEntityPriorityList @ 0x50e590` +
`serialize_entity_states_to_packet @ 0x50f070`: the saturating age sweep (@0x50e60f), the
`sqrt(dx²+dy²+(dz/2)²)>>16` distance metric with the 1124-tile gate and age≥50 force-admit
(@0x50e925), the `(entity+36 & 1) >> 4` damp, the +1000 own-entity boost, the
`age + v + ((age*v)>>8)` key, descending sort (shell sort @0x526cf0 ≙ stable_sort), and the
600-byte soft budget (g_entity_send_budget @0xC8FC50, checked after each record @0x50f34b, age
reset on selection @0x50f168; round-robin is EMERGENT from aging — no cursor). NOT modeled (0
contribution): angleScore (recipient view yaw), the LOS raycast (@0x50eadb), enemy/team bonuses,
velocity/heading delta caches (slot+91434/+92890), the +200 view-distance bonus (word_26C681E),
the tracked-handle priority floors + 0x12 despawns (slot+94346/+94356), the projectile chain
(type-2 records @0x4ffee0/@0x504820), and the budget halving (slot+89876 congestion flag /
uptime>2000 @0x517c62). Interop-safe: ordering differs, the record set converges via aging.

**D-NET-140** [reimpl divergence by design, DOCUMENTED 2026-07-02] **The listen host's OWN
loopback connection receives the full 0x0A record set; retail sends its local player header-only
frames.** Retail: the priority build is skipped for the local player (@0x517c1b) and
`serialize_entity_states_to_packet` returns immediately (@0x50f07e) — the local client reads
process memory. Our serve-and-play local view RENDERS FROM the loopback 0x0A fold (ADR 0011), so
the loopback gets full records. That frame never leaves the process — retail interop unaffected.

**D-NET-141** [reimpl divergence, FIXED 2026-07-02] **The S2C 0x5A ammo bytes echoed the
request's 255 ("default") instead of resolved counts — degenerate when the weapon's
`startrounds` is the shipped −1 default.** Witnessed resolve semantics in §5.57; the client
apply clamps a non-negative byte identically, but 255 → signed −1 → `entry[23] (startrounds)`
fallback, and weapon.def leaves startrounds −1 for most entries → the count degenerated (part
of the live-witnessed retail-join v15 "ammo issues"). FIXED by the witnessed pipeline: the
index-allocation rule closed (§5.57 "Index numbering" — null@0 + by-name-reuse-else-lowest-free
= file order; the anchor contradiction was the fixture-vs-live weapon.def, not the rule), the
host parses ITS OWN resolved weapon.def (`NovaSimulation::load_weapon_table` →
`np::build_weapon_table` → `world::WeaponTable`), and `build_tag_5a_weapon_loadout` resolves
per accepted entry like `Server_SendWeaponSlotListToPlayer @ 0x502550`: mask filter
(@0x502716), ammoPrimary/ammoSecondary via the `WeaponSlot_GetTotalClips @ 0x5425F0` port
(`resolve_loadout_ammo`, libs/npruntime/weapon_table_build.cpp), slot-combo reply order.
Table-less hosts (no resource root) keep the echo — tracked for that configuration only.
Pinned by npruntime_weapon_table + the handshake armory cases. LIVE-VERIFIED retail-join v18
(2026-07-02): the 0x5A reply is byte-for-byte golden — slots {2,3,21,76,77,78,83}, primaries
{255,10,10,1,2,3,3}, secondaries 255, restriction 0, slot-combo order — resolved from the
host's own 126-weapon VFS view.

**D-NET-142** [reimpl gap, FIXED 2026-07-02] **The host ignored C2S 0x25 (reload request), so
a retail joiner's reload animation repeated without refilling the clip.** The reload FSM sends
0x25 and sets a transient 0x80 entry guard that the begin-active shim overwrites with phase 2;
the server's S2C 0x49 broadcast is the client's clip-refill event (§5.58,
`NapiNPServerMsg_HandleReloadRequest @ 0x514DF0` →
`NapiNPClientMsg_WeaponReload_0x049 @ 0x42C0A0`). Live-witnessed as retail-join v15 "cannot
reload". FIXED: dispatch case 0x25 decodes + re-encodes the `[u16 handle][u16 weaponSlotCombo]`
body (ADR 0003) and stages S2C 0x49 on EVERY in-match connection's transport INCLUDING the
requester (the original's two filtered sends @0x4C87E0), each framed with its own sequencing at
the flush boundary (`server_message_dispatch.cpp`; npruntime_reload_relay_test). LIVE-VERIFIED retail-join v17
(2026-07-02): the joiner reloads normally (user-confirmed; the v15 wedge is gone). TAIL
CLOSED 2026-07-03 (D-NET-152): the host-side `WeaponSlot_ReloadAmmo @ 0x541720` refill is
witnessed (remote-requester-only, refund + capacity clamp — §5.58) and the dispatch 0x25
case now refills the same per-slot clip the 0x06 fire pipeline decrements
(npruntime_client_fire_test; the ammo-pool refund/clamp stays deferred with the pool model);
v28 holds live — fire→empty→0x25→fire across 5 reloads, params echoed, no wedge (D-NET-152).

**D-NET-143** [reimpl divergence, FIXED 2026-07-02 (defaults + echo); body motor tracked]
**The 0x0A player records sent anim_state_id 0 (the null clip — the v15 flicker/spazz) and a
permanent anim_def_index 0xFF (starving the client's weapon-action layer); the retail host
produces live values by RUNNING THE BODY MOTOR for every remote player.** Witnessed (§5.10
apply map): the extended uplink carries the input byte (+0x12C), stance-xor, anim-def triple
(+0x130..132) and the equipped-weapon adm index (+0x2B0, case-4 store @ 0x4C20A3 gated
`AdmDefs[idx].category < 11` — the ex-`reserved_24` byte 24, renamed `equipped_adm_index`
across codec/intent/entity/snapshot); +0x2BC (anim state) and +0x377 (channel ratio) come from
the host's own `Entity_UpdateInfantryPlayerBody @ 0x4B40E0` run over the replicated input.
FIXED to the witnessed defaults + echo: off-14 = 0x2B (43, idle — the spawn default [orig:
PlayerClass_InitEntity @ 0x4B1116]), off-16 = the entity's equipped adm index (uplink ingest
with the category gate against the armory table; host-spawned players default to the armory's
WPN_M4AUTO index resolved by name, the engine re-stamping the pre-feed host player). RESIDUAL
(tracked): remote players render idle-posed while moving until the infantry body FSM runs for
net-snapped peers off the replicated input — the faithful completion. LIVE-VERIFIED
retail-join v17/v18 (2026-07-02): the host-player flicker/spazz is gone (user-confirmed);
wire shows animState=43 / animDef=9 / the joiner's uplinked equipped index echoed; diff_0a
reports player.animDef + animState populated exactly like the golden.

**D-NET-146** [reimpl divergence, FIXED 2026-07-02; #300 REGRESSION FIXED 2026-07-25]
**The joiner's S2C 0x0C organic-spawn
record carried the wrong `animSlot` byte and a mis-packed `netId` — ours echoed the
body-anim CLIP slot (`Entity::anim_slot`, 1 for every standing player) and the D-NET-137
netId shim (0x8201, pool slot leaking into the TYPE bits); the golden retail host sends the
joiner's own uploaded per-side character selection (animSlot 4, netId 0x8207).** The
live-witnessed symptom was a DBuggy1 SHADOW blob (mesh correct) under the joiner's own
player (v17/v18): the client's 0x0C apply binds every minimap-registered entity to a
character-slot registry entry keyed by the packed NetId (`entity->CharacterEntity =
MinimapSlot_FindOrAllocByEntityId(&count_and_entries, entity, NetId)` [orig:
NapiNPClientMsg_0x00C @ 0x42E730, alloc @ 0x42eb1d, re-alloc-on-miss @ 0x42eafb]), so a
type-1-packed id + a wrong avatar byte resolve a vehicle-archetype entry's shadow decal
while the mesh (resolved from playerClass) stays correct.

The #300 regression had the same presentation failure one stage earlier: its joiner
ClientAuth omitted the entire character-profile CU block. The host consequently spawned the
joiner with `animSlot=0` and let the organic encoder's fallback produce `netId=0x0200`.
That exact malformed row is present in `join_r10.pcapng`; the same capture places the two real
dune-buggy pool rows hundreds of metres away with no player parent. Together with the earlier
retail witness, this identifies the reported floating vehicle as the wrong character-registry
shadow association, not a replicated vehicle pose.

The same-gun firing report is a separate identity invariant, not evidence that the profile
omission caused both symptoms. Retail parses each round's packed shooter handle and executes
the action callbacks against that resolved entity (`NetPacket_DeserializeRoundEvent @0x42F270`);
ordinary rifle fire does not stamp the player-body secondary channel
(`WeaponAction_Fire @0x542BBC`), and `Server_BuildRoundEventListForPlayer @0x4FFEE0` excludes
the shooter's own round from its S2C echo. The two-peer regression now equips both players with
the M4 and proves the joiner's shot leaves the host weapon FSM and body state unchanged, while
both predicted and authoritative presentation events retain the joiner's handle and muzzle
origin. A rendered-model regression also drives two actors through one shared parsed ADM/BAD
definition and proves their clip selection, playhead, and Skeleton3D pose remain entity-local.
This pins fire presentation to entity identity rather than the shared weapon/ADM key.

The witnessed chain, end to end: (1) the retail joiner uploads its per-SIDE character
selection as CU chunks in the game-session 0x42 ClientAuth — `CI0`/`CI1` = per-side
minimap/character-slot ids (u16 of atol), `TR` = requested side (0/1, else clamps 0xFF
auto @ 0x4c752f), `CTA`/`CTB` = per-side soldier class, `VCA`/`VCB` = per-side avatar byte
[orig: client emit CNapiServerInfo_SerializeToSession @ 0x4c3650 (each tag omitted when 0);
wire: golden retail-ashi5a f=199140 and retail_join_v18 f=47676 both carry CI0=512(0x0200)
CI1=33287(0x8207) TR=-1 CTA=CTB=8 VCA=1 VCB=4 — one captured profile selection, not
universal protocol defaults]. (2) The host
parses them into the connection's NapiNetConfig [orig: NapiNPProtocol_HandleClientJoin
@ 0x62b750 CU loop (type-2 gate) -> NapiNetConfig_LoadFromConnTags @ 0x4c7260 ->
jsp[56..63] + ci0.lo, Napi_StrCaseEqual names, atol values]. (3) `Server_BuildPlayerInfoAndAdd
@ 0x51d560` copies jsp[56..64] into the add-event (validating the ids via MinimapSlot_HasEntity
@ 0x57b140 / lookup_entity_slot_and_pack_entry @ 0x57ad40 against the character-slot
registry `count_and_entries` — the 288-byte-stride table whose entry+284 is the avatar byte,
see sub_57AE60). (4) `Server_PlayerAdd @ 0x51cbc0` assigns the team, then picks per ASSIGNED
team — side A when team ∈ {1,3} or the session gametype is non-team-based ((g_GameType &
0x10000) == 0), side B otherwise [@ 0x51cff7] — stamping entity+0x374 = the picked avatar
byte [@ 0x51d0b1] and entity+0x15C = the picked char id [slot+440]; playerClass = TR ? CTB :
CTA (absent -> 8 in-session [@ 0x51d02b], outside [5,9] -> 8 [@ 0x51d102]) -> entity+0x294.
`Server_InitAllPlayerEntitiesForRound @ 0x516aa0` re-stamps entity+0x374 = slot+89857 every
round [@ 0x516b8e]; `Server_ChangeEntityTeam @ 0x518d70` (ex-`Server_ChangePlayerTeam`) re-picks on a team change
[@ 0x518e8c]; the host's OWN player takes the LOCAL path instead: animSlot = g_avatarTeam1/2
by team split {1,3}/{2,4} [orig: Player_InitPlayer @ 0x4e15f0 @ 0x4e1843], sourced from the
profile avatar byte with a not-found default of 1 [orig: apply_session_settings_to_globals
@ 0x551500 -> sub_57AE60 default-return 1] — the golden host record's animSlot 1. (5) The
0x0C/0x18 serializers read entity+0x374/+0x15C raw [orig: serialize_entity_states_to_buffer
@ 0x5030a0 (+0x374 read @ 0x5032b8) / serialize_object_to_buffer @ 0x504d10]. g_GameType
itself is the HOST's chosen session setting, seeded at host start [orig: g_GameType =
session gametype setting @ 0x4a6657 / ServerConfig_ApplyHostSetting @ 0x4a6000 @ 0x4a6587;
golden ASH_I5A 0x08 advertises gameType=0x10010 — team-based bit 0x10000 set].

REIMPL: `Entity::anim_slot` was a semantic conflation and is SPLIT — the body-anim clip is
now `Entity::body_anim_slot` (present-pass state, never wire), and `Entity::anim_slot` is
the retail +0x374 character selector, plus `Entity::minimap_net_id` = the +0x15C wire id
(players; 0 -> the D-NET-137 shim fallback). handle_client_join parses the CU character
vars onto the connection (npruntime napi_np_protocol.cpp), Server_BuildPlayerInfoAndAdd
stamps the spawn per assigned team (server_spawn.cpp), the 0x0C/0x18 builders echo the new
fields (entity_wire_bridge.cpp). For a real LAN join, `GameWorld` now resolves both
alignment entries from the mounted `Avatars.def`, applies the active PLAYER_INFO selection
to its side, packs `[nat:5 | division:4 | combo:6 | alignment:1]`, derives the avatar byte
from the selected head voice, and installs that profile on `ClientRuntime` before the first
ClientHello. `JoinerConnection` serializes `CI0/CI1/TR/CTA/CTB/VCA/VCB` in the witnessed
ClientAuth order. When the named S2C 0x0C row identifies the joiner's own player,
`JoinerConnection::SelfSpawn` carries that row's `animSlot` and packed minimap id into the
local L entity as well; the local self-spawn path no longer drops the character-registry
identity after receiving it. Direct/headless clients retain the stock-table fallback
`0x0200/0x8207`, class `8/8`, avatar `1/10`. BOTH host entry points — the `NW_LAN_HOST` dev
boot and the `mp.mnu` host config — seed the Co-op gametype **0x30020**, the value retail
derives from a Co-op mission's `ATTRIB_COOP` header attrib (`AI_GetTaskTypeFromFlags
@ 0x40DAE0` → `Game_StartMission @ 0x524360`; the D-NET-75 witness chain, and the only value
that passes both off-wire sub-body gates). `NW_LAN_GAMETYPE` remains a diagnostic override;
0x10010 was the ASH_I5A capture's value, not what this build seeds. DEFERRED (tracked here): the
host-side character-table validity check and invalid-id fallback (`Server_BuildPlayerInfoAndAdd
@0x51D560` probes each uploaded CI field with `MinimapSlot_HasEntity @0x57B140` and re-packs the
failures through `lookup_entity_slot_and_pack_entry @0x57AD40` — see D-NET-137 for the addresses;
duplicate selections are allowed), the
BMS `AnimSlot` spawn property for mission AI (the mission promote does not carry it yet —
AI now sends the retail memset default 0 instead of a body clip), and the WAC
`set_ssn_anim` command still drives the body clip (its retail target — +0x374 vs the clip
channel — is unwitnessed).

**D-NET-163** [reimpl gap, WITNESSED-READY-DEFERRED — the golden-diff baseline, minted
2026-07-05] **The dev golden-harness host does not emit retail's full S2C tag set.** The
tier-1 `nw_golden_diff` self-test, run against the attested v35 baseline (game-server
`retail_join_v35_game` vs the retail gameplay golden), enumerates 21 tags a retail↔retail
session carries that our host↔retail-client join capture does not. They share one root
cause: `nw_server` is a minimal in-match *golden-harness* host (ADR 0013, never shipped),
and a join-scope capture never exercises the traffic these tags belong to. Three families:
(1) **periodic integrity / anti-cheat challenge-response** the harness drives none of —
time-sync (S2C `0x43` ping / C2S `0x08` reply), charattr CRC (`0x39`/`0x1c`), entity-checksum
(`0x30`/`0x20`), loadout-CRC (`0x31`/`0x21`), loaded-model paging (`0x68`/`0x3d`); (2)
**gameplay-event traffic** a join-only capture never produces — kill-sync `0x26`,
kill-by-slot `0x4e`, play-sound `0x34`, score-delta-sound `0x81`; (3) **low-frequency
session / roster / control tags** the harness does not model — target-assignment `0x4c`,
team-assign `0x50` (the emit path; `Server_AssignPlayerTeam @ 0x4fe310` itself is MATCHING,
D-NET-113), session-status `0x58`, deployed-item `0x59`, destroy-list `0x5d`
(`_DestroyEntityList @ 0x429730`), spectator-flag `0x79`, server-config-strings `0x7e`.
Each is a documented message shape whose emit is deferred; `nw_golden_diff_test.cpp`'s
`kDeferredGaps` now maps every tag to this ID, so the diff PASSES with each deferral named
and a NEW gap (any tag not on this baseline) still fails. The lone allowed spurious
(S2C `0x18` full-entity-spawn, which our host emits and the retail reference session did
not) is the tracked D-NET-133 repair-path residual.

**D-NET-162** [reimpl gap, PORTED 2026-07-04 (slice 2; verify v35)] **The AS capture loop
now runs on our host** — the §5.61 1 Hz block was witnessed round 13 but unported (v33: no
map colors, no LFP capture). Ported: `world::zone_capture_tick`
(libs/world/zone_capture.{h,cpp}) — the per-second secure/control pass (the enemy-frontier
latch + `calculate_capture_zone_control_delta @ 0x501120` verbatim incl. the small-server
boost, the 20/40/60 soft caps, the 12/24/48 base table, the shared-zone-number divide, and
the ±1 minimum), secure edges, the instant numbered-zone flips (owned → neutral → capturer,
control zeroed — the Advance-and-Secure beat), mask rebuilds, and non-trigger zone-object
team enforcement `[orig: Server_UpdateCaptureZoneEntities @ 0x519690;
Server_UpdateCaptureZones @ 0x53B8F0 drain; GameEvent_FlagCapture @ 0x50F6F0;
Server_EnforceZoneEntityTeams @ 0x519600]` — plus the npruntime 1 Hz wire block
(server_tick.cpp): 0x6F (15 B, change-gated to all + the full set to deploy-pending/dead
recipients), the 0x1E zone events (0x3B/0x3C edges; flips 50/51/52/53 team-filtered + the
56/57 banner), 0x53 on flips, and the 0x40 minimap-overlay feed (persistent zone entries
icon 0 + transient vehicle blips by items.def `unit_type`, chunked ×16
`[orig: Server_BuildOverlayStateForPlayer @ 0x517FC0 → Entity_ClassifyForMinimap
@ 0x50FA70 → the staging flush @ 0x50FE20]`). Pinned by `zone_chain_test`
(control-delta formula pins; the full flip→secure→contest→neutralize→retake cycle).
Tracked divergences: our flip-request source is the same 1 Hz proximity sample the drain
consumes (retail queues per-touch through the physics pass); the 0x1E attacker byte uses
the chain-vector index (retail: `SpawnZoneList_IndexOf @ 0x43B990` over the client-sorted
registry); 0x6F is change-gated (the golden's 268 non-periodic emits refute a steady
per-second stream; the exact retail emit filter is unwitnessed); the 0x40 walk covers
zones + vehicle blips only (players/emplacements/CTF-flag entries + the resumable
per-slot budget walk deferred); the timed-capture engine's ACTIVE entries + 0x6C presence
counts (un-numbered flag zones — none authored on ASH_I5A), spawn-wave resets, the
underdog catch-up term (needs the round clock), proximity scoring/0x81, and the
`def+88 & 2` in-radius team conversion are all deferred.

**D-NET-161** [reimpl gap, PORTED 2026-07-04 (ground-family core; verify v35)] **The host
never simulated vehicles** — the whole v33 "second model + can't drive" defect (see the
§5.13 drive-authority subsection for the witness). Ported: the items.def physics-property
block (libs/def, scaled at parse per `ItemDef_ParsePhysicsProperty @ 0x49d870` — turn rates
deg/s×192426 BAM/tick, player_speed km/h×293 16.16-u/tick, slopes deg×11930464, accel/decel
×4 with the 2×accel decel default) → `world::VehicleTraits` (stamped per item by
`NovaSimulation::resolve_item_traits`, `attrib & 0x40` PlayerControl gate) →
`world::tick_vehicle_motor` (libs/world/vehicle_motor.cpp — the authority drive core of
`Entity_UpdateVehiclePhysics @ 0x48af00`: the occupant resolve/stale-clear, the input block
with the 8-way dir switch + modifier bits + key-steer ramp (+0x16C16C0/tick cap 0x238E38C0)
+ the analog leg (±192426·axis>>1 steer, playerSpeed·axisX>>7 throttle), the speed-scaled
steering chase (min rate turn_rate2 else turn_rate/4; wheel state `aiState += (4 − 32·Δ −
aiState) >> 3`; yaw rate = −speed·(wheel>>2)>>16 applied while grounded), the cos²(pitch)
slope factor, the accel branch tree (±accel same-direction, ±decel zero-target, the
UNCLAMPED 1/32 launch step on direction changes, ±decel/2 airborne coast, the airborne
command inversion), gravity −324/tick, the |speed|<48 zero-target deadzone) run per pool-1
traits entity in the AiSystem tick. The driver's input arrives via the already-ported 0x0C
apply; `Entity::net_analog_x/y/z` (entity+0x130..) now ride `PlayerIntent`. Pinned by
`vehicle_motor_test` (the def-scaling pins incl. the JOX buggy block; launch 861 = the
unclamped (27542+16)>>5 first step; the +60/tick clamp; reverse −playerSpeed/2;
turn-in-place holds a standing buggy; the steer chase toward the driver yaw; coast-to-stop;
the dead gate; the `physics` selector gate) + `netsim_two_peer_fanout`
(vehicle_drive_authority: a remote driver's 0x08 move input spins the host vehicle to
speed 861+60·61 in 62 ticks and the streamed 0x0A vehicle record pose goes LIVE).
2026-07-16 update (the vehicle pass, world-wac-ai-re §23.3): the **AI-driver leg**
(`@ 0x48bc12-0x48c034`) and the **no-controller parked stamp** (`@ 0x48c002-0x48c02d`,
SM state 22 + the 22→16 hand-back) are PORTED — `AiSystem::vehicle_ai_drive` stages a
`VehicleDriveCmd` the motor consumes (turn budget `32·|Yaw−bearing|/((brain[35]>>15)+32)`,
±budget delta clamp, the ×0.75 sharp-leg damps at 357913920/715827840 when
`turnRate2<<6 < budget`, steer `Yaw+Δ+Δ/8`, cmd speed `min(brain outSpeed, playerSpeed)`);
the SM's kinematic `apply_locomotion` retires for `physics != 0` vehicles (the motor is
the only integrator, matching the original split). Pinned by ctest `vehicle_mount`.

Tracked deferrals: the air/helicopter family (`move_function chel` — Super Pumas stay
parked; the buggy-family ground core is what landed), the skid/tire-slip model, pool-1
vehicle-vs-vehicle collision + the collision-avoid damping (`@ 0x48bd8f-0x48bf26` incl.
the DcbId-seeded 0.25–0.75 yield factor), water drag/drowning drain, the wait-for-boarders
stop (`@ 0x48bf6f-0x48bff9`, aiComp mode 125), the minAI crew health clamp
(`@ 0x48bc4e-94`), the handbrake byte-973 latch + aim-lock stop (`@ 0x48c03a/0x48c086`),
`EntityAI_ProcessVehicleStateMachine @ 0x4583c0`'s non-drive states, the engine sound
state machine, husk/section damage, the wheel-contact pitch/roll solver
(`Entity_ProcessTrackedVehiclePhysics @ 0x47c1c0` — substituted by the shared 5-tap
bilinear terrain clamp), the above-water drive gate, the ground/carrier-follow
grounded-on-entity block, and the driver-yaw analog write-back for remote drivers
(their yaw is wire-owned on our host). 2026-07-17 update: the hull-vs-WORLD collision
half of `Entity_CheckCollisionState @ 0x462a30` is PORTED (world-wac-ai-re §23.3
addendum — `CollisionWorld::resolve_vehicle_hull`, one mid-hull point, the wall-like
full-force severity-3 class + the def-torque speed decay `@ 0x47cc13-0x47ccc1`);
still deferred here: the per-wheel point array/radii, the v84/v85 slope-threshold
grading (caller locals, unwitnessed), the graded ¼/⅛ bands, the size-class crush leg
(`@ 0x462e94`), the severity-3 authority damage block (`@ 0x47cd00`), and the second
averaged-suspension pass (`@ 0x47d213`).

**D-NET-160** [reimpl gap, FIXED 2026-07-03 (ported; verify v34)] **A killed client never
learned it died — no death screen, no redeploy (v33: 2 kills routed, 0x13 + 0x1E on the wire
to everyone, death anims visible to OTHERS via the D-NET-159 motor, but the VICTIM kept
playing).** The victim's own death rides two per-recipient channels we hardcoded: (1) the
0x0A TAIL health — the client STORES it as its own Health [orig: @ 0x4305df], and ours sent a
constant 150 (the D-NET-144-era stopgap), so the victim's Health never hit 0; (2) the record
byte13 dead bit 0x02 — the LOCAL apply's dead path stores the death anim + zeroes Health
[orig: @ 0x4c1005-0x4c1027] and its 1→0 edge is the client SPAWN HOOK (pose snap +
Entity_ResetToSpawnState [orig: @ 0x4c1109]) — and nothing server-side ever set entity+36
bit1 on death. FIXED: route_round_deaths marks the victim entity dead (`flags |= 2`,
`alive = false`; lifted by entity_reset_to_spawn_state at the deploy/respawn — the wire edge
then fires the client spawn hook, closing the death→redeploy cycle through the existing
dead-or-pending 0x0E gate + the D-NET-156 release bundle), and the 0x0A tail now carries the
recipient's LIVE health (`FrameHeaderState::tail_health`; damage also reads as the retail
red-flash via the decrease detector @ 0x43059a). Pinned by `netsim_two_peer_fanout`
(dead-state frame: tail 0 + byte13 bit 0x02) and the byte-identity sub-case's aligned tails.

**D-NET-159** [reimpl divergence, FIXED 2026-07-03 (ported; verify v32) + tracked stand-ins]
**The 0x0A player record's anim bytes were hardcoded (off-14 = 43, off-15 = 0) and the
move-input echo idled at 0 for the host's own player — retail RECOMPUTES the body anim on
the authority for EVERY player from the replicated input** (v31: remote bodies frozen at
idle; the "off-14/15 are echoed from the uplink" premise was WRONG — the extended uplink
carries NO anim state and NO stance, §5.10). Witnessed: `Entity_UpdateInfantryPlayerBody
@ 0x4B40E0` runs the selection under `g_local == e || is_authority` (@ 0x4b70a3-0x4b70b2)
every 4th tick, consuming MoveOrder bits 0-3 (dir + moving) + bits 8/9 (stance — fed by the
**C2S 0x1D stance-change**, dispatch table) + Flags bits (0x10 scope suppresses the run
promotion); bases 1/11/19 + the {0,7,6,5,4,3,2,1} dir offsets, idles 43→44 (62 passes) /
45 / 48, run/jog 9/10 by the ADM gait class, prone lean 41/42, commit via
`g_animStateFlagsTable @ 0x8139E8`; ratio = the +0x188 channel's elapsed-ticks-in-loop
(`AnimChannel_AdvancePlayback @ 0x40B140`), clamp 255. PORTED: `AiSystem::
remote_player_body_anim` (the selection subset: bases + 8-dir + stance idles + the 43/44
idle counter + arbitration commit) + `mirror_wire_anim` (Entity::net_anim_state/pending/
phase → the record bytes; the local player also exports its packed MoveOrder low byte);
the 0x1D dispatch case writes `Entity::net_stance_bits`; the 0x0A header TAIL state byte
now echoes the recipient's own stance bits 0-1 (a hardcoded 0 force-STOOD a crouched
retail client every frame — the long-standing crouch/prone bug, §5.9 tail row). Tracked
stand-ins (this entry): run/jog promotion deferred (the ADM gait class
`dword_24E808C[adm*0x460]` is not in our weapon table), prone lean 41/42 deferred (lean
bits uplink fine but the selection leg is unported), the no-anim-data channel phase
self-advances on a 62-tick loop (faithful source = the .adm loop rate; the Godot shell
feeds real data via IRootMotionSource), and the death leg reuses the motor's generic
torso-forward pick (deathAnim +0x2C0 variants unmodeled). Tests:
`infantry_test` (remote selection walk/crouch/prone/idle-promote),
`netsim_two_peer_fanout` (pending-wins byte 14, ratio byte 15).

**D-NET-158** [reimpl gap, FIXED 2026-07-03 (ported; verify v32)] **The HUD "Number of
players" broke on three fronts, all in the 0x46/0x22/0x16/0x04 roster contract** (v31: 14
repeated C 0x22s = the unknown-slot retry churn; D-NET-155 had aimed at the wrong tag —
the 0x16 re-push was necessary but not sufficient): (1) the 0x04 slot-config hardcoded
`(slot 1, capacity 2, team 2)` — byte 18 feeds `g_max_player_slots @ 0xA860D1`, the
client's 0x46/0x22 walk TERMINATOR, so slot 2+ never got walked (fixed: live slot/
capacity/team from the connection + `GameConfig.max_players`); (2) no join-time 0x46
broadcast — existing clients DROPPED the new player's 0x16 row (rows are accepted only for
0x46-known slots) and churned 0x22 retries (fixed: `broadcast_player_sync_on_join`,
fieldFlags 0x1CF7 [orig: Server_PlayerAdd @ 0x51D296]); (3) the 0x16 trailer
`[inGameCount][spectatorCount]` was hardcoded `{2, 0}` — the HUD count = accepted rows −
spectatorCount (fixed: live counts; §5.20 rewritten — byte 0 is a FLAGS byte, row bit0 =
spectator). Also: the 0x46 reply now serializes the REQUESTED fieldFlags verbatim
(encode_player_sync(rep, flags)), and 0x16 rows carry only in-match players (the golden
31→39 grow timing). The 0x32 name broadcast stays deferred with D-NET-149.

**D-NET-157** [reimpl gap, FIXED 2026-07-03 (ported; verify v32) + tracked divergences]
**C2S 0x26/0x27 (vehicle attach/detach) were undispatched — the two v31 buggy-enter
attempts (`02 00 04 10 01 00`, bone 1) fell on the floor and no player could ever mount.**
The full retail flow is §5.10's "C2S 0x26/0x27" subsection (HandleVehicleAttach @ 0x502390
word0 anti-spoof → Entity_ProcessVehicleAttach @ 0x435AA0 validation order →
Entity_AttachToVehicleSlot @ 0x4946D0 writes; detach @ 0x4FC980/@ 0x4355F0; NO confirm tag
— the 0x0A mounted branch is the confirmation). PORTED: dispatch cases 0x26/0x27 →
`world::entity_process_vehicle_attach / entity_detach_from_vehicle`
(libs/world/vehicle_attach.cpp) + `Entity::mount_bone` (+0x157) + the record byte0 echo +
the header-tail mount handle. The former seat-classification divergence is resolved: the
model USRP enumeration has the witnessed 48-byte runtime row shape (name at +32), and the
wire index is 1-based. Production extraction preserves that exact index and rejects
unmatched rows instead of substituting a free seat. Tracked divergences (ours vs retail):
(a) the 0x27 detach subject is CLAMPED to the sender's own entity
(retail trusts wire word0 with no range guard — a hardening divergence); (b) the
weapon-busy gate (EquippedSlot currentAction 0/1/11 @ 0x435b29), the ATTR_PlayerControl/
ATTR_EWeap ctrl-seat def gates, gun-carrier traversal in the enemy-occupant scan, and the
record's gun seatType byte (needs carrier +0x326/+0x312 modeling) are unmodeled. Test:
`netsim_two_peer_fanout` 0x26 attach → mounted echo → detach round-trip.

**D-NET-156** [reimpl gap, FIXED 2026-07-03; **v32 LIVE: the hold/pick/release chain WORKS
(picker appears, pick lands, C 0x0E on the wire from both joiners) but the session found the
MISSING RELEASE BUNDLE** — the 0x0E pick sets the client's `dword_81474C` wait-gate (Input
case 12 @ 0x49b17b) and the client STOPS its C2S 0x0C uplink at the pick frame (v32: both
joiners' uplinks ceased exactly at their 0x0E, forever — the host-side entity pinned at the
deploy spot with input=0 = the live rubber-band). The retail release is the deploy leg's
bundle: **0x5A loadout re-send + 0x61 seed + 0x1E hint in ONE datagram** (golden f=240018,
after the wave countdown; the 0x5A apply is the un-latcher — NapiNPClientMsg_
HandleWeaponLoadoutSync @ 0x4290E0 resets 81474C on completion, §5.30). FIXED same day: the
0x0E success path emits the retained granted 0x5A body + the session-seed 0x61 before the
0x1E (`SessionReplyState::last_loadout_reply`; `npruntime_round_sim` pins the bundle +
no-bundle-when-alive-deployed). Also witnessed off the v32/golden diff: the retail body
motor skips HIDDEN entities entirely (@ 0x4b411b `test dl,1`), freezing the pending player's
anim channel (golden ratio constant 40; ours swept to 255) — the remote anim pass now gates
on the hidden bit. Golden also confirms fresh-join deploys produce NO byte13 bit-0x02 edge
(pre-deploy byte13 = 0x01 exactly), and ratio 255 on long idle is retail-correct. Verify
v33.] **The deploy screen never
appeared on our host (v31 root cause #1: ZERO C2S 0x0E all session) — the picker is HELD
open by the 0x0A header flags1 bit1 re-asserted EVERY frame, and ours hardcoded flags1 =
0x00** (golden pre-0x0E flags1 histogram {0x02: 54}, post {0x00: 293, 0x02: 3}; our
0x0F-driven flash died < 16 ms later). The full chain is §5.61's "deploy-screen HOLD
chain" + the §5.9 flags1 row: join sets slot bit4 iff `SpawnZoneList_GetCount() > 0`
[orig: Server_OnPlayerJoin @ 0x51a6f2], the header writer emits bit1 + ORs the entity
hidden bit0 while pending [orig: NetPacket_WritePlayerState @ 0x4ff7bd/@ 0x4ff7dd — the
golden pre-deploy record byte13 = 0x01], the 0x0E gate accepts dead-OR-pending
[orig: @ 0x519cc7], and the deploy leg clears bit4 [orig: @ 0x517791]. PORTED:
`netsim::Connection::respawn_pending` (join-set via `world_has_spawn_zone`, host loopback
exempt; 0x0E-cleared with the hidden bit) → per-connection flags1; the 0x0E dispatch gate;
S2C 0x6E empty-group form at 1 Hz to pending/dead players; optional parity landed with it:
the 0x0F location-name block (def-2044 markers, §5.29) + the 0x0D zone byte/radius fields
(§5.11) + the 0x04 live slot bytes. Tests: `netsim_two_peer_fanout` (flags1 hold +
byte13 + tail clear on deploy), `zone_chain_test` (spawn-zone presence + the packed zone
byte). Death does NOT set the flag — the death screen is client-local (flags1 bit0 edges
drive it; §5.9).

**D-NET-155** [reimpl gap, FIXED 2026-07-03 (v29)] **The 0x16 player-list re-push was
one-shot-per-connection to the JOINING client only — every existing client's roster (and
its HUD player count) went stale when a later joiner arrived** (v29: three players in, the
count stuck at 2). FIXED: a roster generation on `NapiNPProtocol` (bumped on every
observed spawn and on the disconnect teardown) + a per-connection `roster_seen_gen`; every
spawned type-1 connection re-receives the framed 0x16 whenever its generation is stale.
The golden single-joiner 31→39 grow is preserved byte-for-byte (one push, same tick;
golden ctests green). [orig: the retail broadcast is `Server_BuildAndBroadcastScoreboard
@ 0x50DE00` — its exact trigger set is a tracked follow-up.] **v30 wire caught the first
cut incomplete**: the version check lived below tick_connections' spawned-peer skip, so it
only ever ran on each connection's OWN burst-completion tick — joiner 1 got its 39-B list
twice and never the grown 47-B list when joiner 2 spawned (HUD still 2). Fixed: spawned
type-1 connections evaluate the roster version every tick before the skip; re-verify v31
(v31 outcome folded into D-NET-158 — the re-push alone was not sufficient).

**D-NET-154** [reimpl divergence, FIXED 2026-07-03 (v29)] **Tag-2 round events starved to
ZERO on the wire: the grouped-order port gave `select_round_events` only the budget
LEFTOVER after the tag-1 entity walk.** A real-world entity set (players + ~21 vehicles)
fills the 600-B frame budget alone, so the round budget was 0 every frame — and the sweep
still advanced the per-connection watermark, permanently discarding every round echo
(v29: 35 C 0x06 with two live observers, 0 tag-2 anywhere). FIXED: rounds select FIRST
under the shared budget (each ≤ 20 B, 255-cap), entities absorb the remainder — the
`@0x50f312` interleave's OUTCOME without the interleave [orig: the one-budget loop
`@0x50f070`]. The positive tag-2 wire witness therefore remains pending → v30.

**D-NET-153** [reimpl divergence, FIXED 2026-07-03 (v29)] **The round sim's fire
direction used the 0x0A euler_z heading frame (90° − yaw); the C2S 0x06 dir yaw is the
MISSION BEARING directly** (a 16.16 turn fraction; `<<16` = BAM32, §5.16). Wire-proof from
the v29 duel: joiner 2 at `(-396.6, 413.1)` firing wire yaw −122.0° at joiner 1 standing
at `(-402.7, 403.5)` = true shooter→victim bearing −122.4° (0.4° residual); joiner 1's
kill line 45.6° vs true 44.2°. The 90°−yaw mapping missed by ~26° everywhere EXCEPT the
45° diagonal where the two frames coincide — joiner 1's duel line sat exactly there, so
"the first joiner could kill the second but the second could not kill the first". FIXED in
`world::RoundSim::spawn`: bearing = the wire yaw; consistent with the spawner component
form X=sinYaw·cosPitch, Y=cosYaw·cosPitch, Z=sinPitch in engine axes [orig:
`RoundData_SpawnRound @0x4ec5e9` / `Weapon_SpawnSingleProjectile @0x4ebf51`]. Pinned:
`npruntime_round_sim_test` (wire yaw 0 → +X). The pitch SIGN is still unverified by wire
(v29 shots were level) — tracked.

**D-NET-152** [reimpl gap, FIXED 2026-07-03, **live-verified v28 (2026-07-03)**: 95/95 C 0x06
uplinks decoded (fixed 45 B, zero failures), shooter = the joiner's own `0x0001` throughout
(anti-spoof-valid), `hit_part` strictly monotonic 513→607, and every fired adm resolves to a
granted 0x5A armory slot (19/24/29 = the successive rebought primaries; 62 = the 2-round 7th
slot, fired exactly twice, alt flag 0x10); firing continued after every mid-session reload —
5× C 0x25 each drew the S 0x49 relay back to the requester with the param echoed (215 ×4,
461 ×1), no clip wedge (the D-NET-142 tail holds live); ZERO tag-2 on the wire is the
EXPECTED single-observer outcome (the only other player is the idle host on the in-process
socketless connection, and own rounds are skipped `@0x4fff97`) — the positive fan-out stays
pinned by `netsim_two_peer_fanout` (round_event_fanout); a second observer retail client is
the v29+ topology for the positive wire witness. Sweep clean: 0 C 0x0F, no 0xC9, 60.7/s 0x0A
cadence, header value-sets match golden, 0x5A byte-shape golden incl. the double-send (the
in-game "no spare mags at spawn" report is NOT a wire defect — we grant `(10,255,0)` for the
primary exactly like golden; only the send moment differs: golden's 0x5A pair rides the
deploy bundle, ours rides join — noted, not chased); diff-vs-golden gaps unchanged (env
sub-block = D-NET-134 deferred; `player.animRatio` zero = the tracked body-motor off-15
channel-ratio item; parked-vehicle euler/flags zero = session content — nothing was driven
in v28).]
**The host had NO dispatch case for C2S 0x06 (client fired round) — every shot a retail
joiner fired was silently dropped: no ammo authority, no fire echo, other observers saw
nothing (retail-join v26: 51 C 0x06 uplinks, zero host response; client prediction hid the
shooter's own view).** The full original pipeline is now witnessed end-to-end (§5.16 the
validate/fire chain, §5.9.1 the per-recipient tag-2 round-event echo): handler
`@ 0x513310` (anti-spoof: claimed shooter == the connection slot's own entity `@0x51358d`;
`PlayerSlot_IsActive @ 0x4FC760` fire-rate gate on `slot+96472/96480`, stamped
`tick + AdmDef[276]` on success `@0x513740`) → `Server_ClientFiredRound @ 0x50BAA0` (guards
−3/−8/−9/−10/−12/−14/−15; ammo via `WeaponSlot_CanFire @ 0x541BA0` — clip u16 slot+16 /
pools; warp compensation `savedLivePose − Position` for shooter + carrier; equipped-adm +
fire-target stamps) → net primary latches the client's origin/direction into the weapon
slot (+64..84, +94 bit0) and runs the adm 'fire' ACTION (`admEntry+684` →
`WeaponAction_Fire @ 0x542B10` → `Entity_FireWeaponAndSendPacket @ 0x42BD80` → LOCAL
re-entry → `RoundData_AddRound @ 0x4FDB40`; `consume_weapon_ammo @ 0x540850` decrements) →
the 256-record ring `g_round_ring @ 0xC8D848` fans per recipient
(`Server_BuildRoundEventListForPlayer @ 0x4FFEE0`: watermark `playerSlot+97544`, own-rounds
skip `@0x4fff97`, line-of-fire proximity score; `NetPacket_SerializeRoundEvent @ 0x504820`
interleaved `@0x50f312`). PORTED at the reimpl altitude: dispatch case 0x06
(`server_message_dispatch.cpp` — decode, anti-spoof, armory adm lookup, per-combo clip
seed/decrement on `world::WeaponTable.clipsize` (−1 = no-clip weapons free), equipped-adm +
`Entity::last_fire_target` stamps, `world::RoundRing` append of the PRE-SPREAD claimed
pose); the 0x25 case refills the same clip (the D-NET-142 tail, `WeaponSlot_ReloadAmmo
@ 0x541720` remote-only `@0x514f03`); netsim `select_round_events` ports the per-connection
watermark + arm gate + own-shooter skip + the x87 line-of-fire scoring (2π/2^32 BAM,
2^22 trig scale, 1000u projection clamp, z half-weight, `0x4000 − lateral>>12`) and
compresses origins against the recipient anchor; `build_0a_frame` emits the tag-2 records.
Codec rename sweep rides along (§5.9.1): `RoundEventRecord` /
`decode_round_event_record` / `encode_round_event_record` / `FrameUpdate::round_events` with
the corrected field semantics (`shooter_handle` mandatory — ex "target"; optional 0x40 word
= the shooter's live TARGET — ex "weapon_handle"; `shot_seq` — ex "damage_extra"; origin +
direction — ex "impact"); IDB renames `NetPacket_DeserializeWeaponHit →
NetPacket_DeserializeRoundEvent`, `serialize_projectile_to_packet →
NetPacket_SerializeRoundEvent`, `compute_entity_angular_priority →
Server_BuildRoundEventListForPlayer`, `should_send_entity_update → WeaponSlot_CanFire`,
`RoundData_ProcessHit → RoundData_SpawnRound`, globals `g_round_ring{,_cursor,_count}` +
`g_round_event_refs`. Pinned by `npruntime_client_fire_test` (anti-spoof, NULL-wpn, clip
seed/decrement/exhaustion/0x25-refill, alt-fire no-decrement, loopback no-op, table-less
accept, ring field sources) and `netsim_two_peer_fanout` (round_event_fanout: arm gate
swallows the pre-join backlog, observer gets exactly-once with anchor-correct origin +
intact direction BAMs + the live 0x40 target word, shooter never echoed its own round).
DEFERRED (tracked here): the authoritative round SPAWN + damage
(`RoundData_SpawnRound @ 0x4EC0D0` — projectile-pool entity, weapon spread, tracer
interval; impact handlers `Projectile_Handle*Impact @ 0x4e93xx` → health → the death
family) — **witnessed end-to-end AND PORTED at the MVP altitude 2026-07-03 → §5.60**
(`world::RoundSim` + ammo.def plumbing + the 0x13/0x1E death routing;
`npruntime_round_sim_test`; live verify = v29); the cease-fire gate (`g_InCeaseFire` unmodeled); the
`+96472` fire-rate stamp (the adm[276] cooldown dword is not in `WeaponTableEntry` — needs
its weapon.def token witnessed); the savedLivePose warp compensation (our net-snapped peers
have zero intra-tick motion, so the delta is 0 by construction until a peer motor lands);
the moving-carrier re-anchor (`@0x50bc41`); the shared ammo POOLS (adm+220 belt / adm+216
ammo-point classes — clips only for now, refill to capacity without the pool clamp); and
the tag-1/tag-2 INTERLEAVE order (ours groups rounds after entities under the same budget —
the retail decode loop is tag-driven, so grouped order reads identically).

**D-NET-151** [reimpl divergence, FIXED 2026-07-03, **live-verified v27, wire re-verified v28
(2026-07-03)**: v27 was user-confirmed only (its dumpcap window expired before the session);
the v28 trace supplies the wire evidence — 105 grounded C 0x0C uplinks across FOUR carriers
(pool-2 statics `0x200c`/`0x2243`/`0x2012` + the pool-1 vehicle deck `0x1004`), and our 0x0A
player records echo each carrier back with compressed CARRIER-LOCAL pos + the local yaw byte
(1,261 grounded records, e.g. `carrier=0x1004 pos=(0x9441,0x1b01,0xd4d0) yaw=0x76`); every
grounded→on-foot edge resumes WORLD coords within a few units of the step-off (e.g. `-406.5
→ local 0.0…1.1 → -404.0`), incl. a direct carrier→carrier handoff (`0x2243→0x200c`) — zero
origin-teleport signature across all 923 uplinks] **The
grounded-on-entity player replication loop was unported on BOTH host sides — the joiner's
carrier-local uplink was applied as world coordinates and the 0x0A echo never returned the
carrier — snapping a retail client to ~map origin the moment it stood ON another entity
(building floor, vehicle deck).** Wire-witnessed retail-join v26 (both cases, same shape):
on-foot at world `(-388.8, 439.4, 11.9)` → grounded uplink `carrier=0x1004` (Dune Buggy,
pool 1) `pos=(2.3, 0.4, 1.1)` LOCAL → next on-foot uplink at world `(2.4, 0.5, 39.3)` — the
local coords as world, z terrain-clamped; identically `carrier=0x227e` (pool-2 STATIC —
building s638) `pos=(8.1, 6.9, 1.0)` → world `(8.3, 6.7, 42.3)`. Golden retail↔retail runs
the same maneuver for 106 uplinks with NO snap: the retail host echoes the carrier back in
the player's own compact record (`vehBone=0 seat=0 carrier=0x1034` + compressed LOCAL pos)
while the 0x0A header refs stay WORLD (the recipient-eye anchor). The witnessed contract
(§5.10, all four ops of `NetPacket_SerializePlayerState @ 0x4C09C0`): the client uplinks
`carrier = groundEntity (entity+0x28)` — maintained by the movement resolver's final
CB/terrain ground probe [orig: `Entity_RaycastGroundHeightAndObject @ 0x414370`
unconditionally stores the hit entity or null; CL's 0x100000 write is separate] — with
`Entity_TransformWorldToLocal @ 0x43BB50` pose (local pos + relative
heading); the host apply lifts it back via `Entity_TransformLocalToWorld @ 0x43BD00`
(`@0x4c1de1`, heading re-add `@0x43be7e`) and REPLACES flags bits 2-4 from the raw wire
byte (`@0x4c1e4d`); the host echo selects mount-else-groundEntity (`@0x4c0a08`) and writes
carrier + compressed LOCAL pos (`@0x4c0b07`) + local-heading yaw byte (`sar 24 @0x4c0b85`);
the client stores the echoed carrier into its own groundEntity UNCONDITIONALLY (`@0x4c1353`)
and hard-applies its own record's position only on an `Entity_TryAttachOrDetach @ 0x436610`
attach-state change (`@0x4c1329-0x4c1345`) — which our 0xFFFF/world echo provoked. FIXED
end-to-end: `apply_player_intent` lifts the grounded uplink through the carrier pose and
mirrors the carrier into `world::Entity::ground_target`; `snapshot_world` resolves the
carrier pose from the registry (a pool-2 static has no 0x0A snapshot of its own);
`build_0a_frame` emits the witnessed grounded record form; `NetClientView` lifts
carrier-form records via the carrier's view-state pose; `Entity_TransformWorldToLocal`
ported as `network_transform_world_to_local` (exact 22-bit transpose; the binary folds the
inverse-rotation sign into a −2^22 sine scale, `dbl_7C57B0`). Decoder corrections ride
along (§5.10 tables): `carrier_handle` (was vehicle_handle), `state_flags_byte`
replace-bits apply (was flags_xor XOR-delta — the xor corrupted already-set crouch/prone
bits), `anticheat_flags` (was reserved_18), priority `(handle,score)` pairs (were
"weapon/fire counters"). DIVERGENCE NOTE (tracked here): retail's host re-derives
groundEntity from its own collision ground probe each tick (it re-simulates remote players
from replicated input); our read-applied peer model does not re-simulate that probe, so the
owner's uplinked carrier is mirrored instead — wire-identical in steady state (the client
reports exactly what it grounds on) but self-corrected a tick later by retail when they
disagree. Locally simulated entities do run the model-aware ground probe. RESIDUAL:
read-applied disagreement is not host-corrected. Relationship attach/detach and requester-local
S2C `0x0A` confirmation are now live, but the production C2S `0x0C` builder still emits
`carrier_handle=0xFFFF` plus world pose for a mounted local player. Static emplacements hide
that omission; moving or rotated carriers still need carrier-local mounted uplink encoding.
Pinned by
`netsim_two_peer_fanout` (grounded_uplink_apply_and_echo: carrier-pose lift, ground_target
mirror, flags replace, carrier echo + local pos + local yaw byte, free-standing regression;
pose_transform_roundtrip: identity-exact + arbitrary-pose round-trip) and the §5.10 codec
tests (`nw_ingame_c2s_uplink`, `nw_ingame_encode`, `nw_ingame_compact_records`). IDB changes
this session: struct member `GamePlayerEntity+0x157 weaponType → attachBoneId` (anchored:
the op1 bone-byte source + the `Entity_TryAttachOrDetach` compare); witness comments at
`0x4c0a08` / `0x4c0b07` / `0x4c1353` / `0x4c1de1` / `0x4c1e4d` / `0x4b3291` / `0x517bf5`.

**D-NET-150** [reimpl divergence, FIXED 2026-07-02, **VERIFIED live 2026-07-03 (v25)**: cold
join = arms + human shadow (user-confirmed, first cold pass ever clean), warm rejoin = arms +
shadow good; wire (retail_join_v25.pcapng) shows every join parking after S 0x11 until the
client's C 0x0A (cold: 0x11 f=172064 → 828-frame park → C 0x0A f=172892 → S 0x19 +1 → first
0x10 +2, batches in ack-window clumps; rejoins f=394137/583879 park 701/597 frames the same
way)] **The host started the §5.2a world stream unrequested and unthrottled —
racing a COLD client's mission build; the 0x0C organic batch then landed mid-`Game_StartMission`
and the client's self-bind (§5.59) resolved against a HALF-BUILT character registry (the
cold-join DBuggy blob shadow + missing first-person arms).** Live-witnessed v22–v24: 5/5 data
points cold=broken / warm=ok; the v24 armless-vs-armed wire diff was byte-identical except the
join-order dcb (3 vs 4) — the defect is TIMING, not bytes. The original gates the stream twice:
(1) **the client-request gate** — the player-sync track ends at sync-state **3** (`[orig:
Server_SendInitialGameStateToPlayer @ 0x51bba0]` tail `@ 0x51c134`, game state 9 `@ 0x51c11c`,
subphase reset `@ 0x51c13c`) and PARKS; only the client's empty C2S 0x0A spawn-menu request
advances 3 → 4 and resets the world-stream phase to 0 (`[orig:
NapiNPServerMsg_HandlePlayerSpawnRequest @ 0x513260]`: game state 9 `@ 0x513295`, sync-state 4
`@ 0x5132b1`, phase reset `@ 0x5132f6`, S2C 0x19 reply `@ 0x5132f1`; golden retail-ashi5a:
S 0x11 f=201572 → C 0x0A f=201573 → first S 0x10 f=201713); (2) **the sent-unacked throttle** —
BOTH burst tracks stall while the connection's sent-but-unacknowledged reliable-message count
`conn+0x768` is >= 20 (`[orig: @ 0x51bbfd (world-stream) / @ 0x51bf14 (player-sync)]`). The
counter is the sent-list `NapiListHead.count` (list heads at conn+0x74C queued / +0x75C sent /
+0x76C free — kong's `NapiNPMessageQueueState` carve mis-slices these; retype proposal open):
messages enqueue via `[orig: NapiNPMessage_Create @ 0x627fc0]`, and the inbound session-packet
parser retires every sent message with `msg_seq <= hdr.ack_seq` (`[orig:
CNapiNPConnection_ParseMessages @ 0x625bc0, sweep @ 0x625d9b]`, header dword +4; peer-ack
high-water kept monotonically at conn+0x7b0 `@ 0x625dbe`). That backpressure is what stretched
the golden world stream across the cold client's whole build (0x10 phase alone f=201713..217422,
~15,700 frames of client-paced trickle) so 0x0C arrives only once the client is nearly done
loading. FIXED as the faithful structural translation: the burst tail parks at sync_state 3, the
dispatch 0x0A case ports the @ 0x513260 advance, the type-2 loopback self-advances (retail's
host-local client sends its 0x0A from the shared in-process loop `[orig:
NapiClient_WaitForGameStart @ 0x42cc10]`), and the emitter stalls while
`next_outbound_seq-1 − peer_acked_seq >= 20` (npruntime server_initial_state.cpp /
server_message_dispatch.cpp / napi_np_protocol.cpp `peer_acked_seq`; JoinerConnection now sends
the 0x0A on receiving 0x11). Pinned by `npruntime_initial_state_burst` (parks at 3, no
world-stream tag before the 0x0A, resumes on the advance) + `npruntime_client_runtime` (full
join round-trip through both gates). DEFERRED (tracked here): retail RE-RUNS the full world
stream on EVERY later spawn-menu 0x0A (state 5 → 4 + phase reset — the respawn map-screen
refresh); our tick loop skips spawned connections, so the reset is applied pre-spawn only.
Retail counts outstanding MESSAGES; our transport frames the burst 1 message : 1 datagram, so
counting unacked datagram seqs is the same basis. Closes the TASK-2 dcb lead: the dcb is
SERVER-ASSIGNED join order (`[orig: CNapiNPConnection_Create @ 0x62acb0]` seeds unk_14 from the
protocol join counter → `[orig: CNapiNPConnection_OnStateChange @ 0x626060]` stamps
connection_id(+0x18) → shipped to the client in the 0x82 MI TLV `[orig:
CNapiNPConnection_SendSessionInit @ 0x620ef0]`; the client-uploaded CI/CK are reconnect-dedup
keys only `@ 0x62bee6`) — the cold/warm 3-vs-4 delta was join order, not a divergence.

**D-NET-149** [reimpl divergence, FIXED 2026-07-24 (all teardown entries); broadcasts partially deferred]
**A disconnecting player's world entity leaked forever — the goodbye path erased only the
connection node, so the body kept streaming (0x0A/0x20) and re-entered every future joiner's
0x0C batch as a ghost player.** Live-witnessed retail-join v23: after two leave/rejoin cycles
the 0x0C batch carried FOUR TestPlayer organics (slots 1-3 = the previous sessions' corpses,
pool slots and eFlags/dcb accumulating 3→4→5) and the user saw the older bodies standing at
their previous positions. The original tears the player down on disconnect [orig:
Server_HandlePlayerDisconnect @ 0x51B5C0]: team spawn-token return (@ 0x51b661..0x51b67a),
S2C 0x32 minimap-slot removal (`serialize_minimap_slot @ 0x505a60` mode 2, mask 128), squad
S2C 0x6A, per-player slot struct memset, and a 0x46 broadcast re-serialized over the now-empty
slot (fieldFlags 0x1CF7 @ 0x51b8ad → the 0x8000 REMOVAL record @ 0x505ecb..0x505ee0, mask 128
@ 0x51b8bc; client apply = `PlayerSlot_ClearAndUnlink @ 0x431420`). FIXED:
`teardown_connection` is now the one player/entity/roster/node path used by every exit:

- a C2S 0x46 is accepted only when its leading receiver-local key equals the current
  connection's `server_sk`, so a malformed leave or a delayed leave from a prior occupant
  cannot destroy the live endpoint;
- each successfully authenticated/decrypted packet resets `receive_inactive_ms`; the JO
  timer sweep tears a silent peer down once it exceeds the witnessed 120000-ms limit;
- a different CI/CK reusing the same address first runs the complete old-player teardown,
  then creates the fresh connection. Owner cleanup no longer drops that replacement by
  address, and fixed-table roster allocation reuses the first free slot rather than
  colliding after a non-tail leave;
- every path despawns `conn.link.owned_entity`, stages S2C 0x46 removal to surviving
  in-match peers, advances the roster generation, and erases the connection node.

The client leave remains the retail four-datagram burst from
`JoinerConnection::disconnect()`: `[u32 remote session key]` plus zeroed
`DS/DC/DP1/DP2/DSTR/DPC/DDSTR` TLVs, shipped from ESC/watchdog/menu/free teardown
[orig: `CNapiNPConnection_TeardownActiveConnection @0x6253c0` →
`SendDisconnectPacket @0x61f2a0`; receiver `Nwu_HandleDisconnect @0x623ce0`].
`npruntime_handshake_server` pins keyed goodbye, same-address replacement, slot reuse,
wrong-key timeout non-activity, and the 120001-ms reap. DEFERRED: the 0x32/0x6A
broadcasts and team spawn-token return.

**D-NET-148** [reimpl divergence, FIXED 2026-07-02] **The host replied an invented,
unconditional S2C 0x51 "spawn-confirm" (packed char id 0) to the joiner's deploy-time C2S
0x29 — the retail-join DBuggy1 shadow.** The original `[orig: NapiNPServerMsg_0x029
@ 0x514F10]` replies 0x51 ONLY when the C 0x29's `[u16 team_change_index]` resolves to a
pending entity in `g_team_change_entity_list @ 0xC947C8` (gated `!g_net_spawn_suspended &&
!g_spawn_success_gate`), with a real `write_entity_packet @ 0x506bb0` record — a plain
join-deploy 0x29 draws NO reply (golden retail-ashi5a: zero S2C 0x51 in the session; the
deploy-time C 0x29 at f=225705 is answered by nothing but the normal stream). The client
FIELD-PARSES 0x51 — `[orig: NapiNPClientMsg_HandlePlayerSpawn @ 0x431BB0]` stamps team
(+354) and NetId (`@ 0x431cad`) and REBINDS `entity->CharacterEntity`
(`@ 0x431cf3`, §5.59) — so our zero-id confirm re-bound the joiner's own player to the
registry's fallback archetype (ASH_I5A: the `d_buggy` mission archetype), drawing a
Dune-Buggy blob shadow under a correct mesh from the moment of deploy. Live-witnessed
retail-join v21 (2026-07-02): joiner 0x0C record already golden-identical
(net=0x8207/animSlot=VCB echo, D-NET-146 fix verified live) yet the shadow persisted;
wire diff isolated our `C 0x29 (f=55518) → S 0x51 (f=55519)` against golden's no-reply.
FIXED by porting the faithful no-reply: the dispatch case consumes the ack (team change is
unmodeled — when it lands, port the @ 0x514F10 list lookup + write_entity_packet record,
never an echo); `encode_player_spawn` and the `player_spawn_confirmed` echo-guard flag are
REMOVED (npruntime server_message_dispatch.cpp / novaworld ingame_encode). Pinned by
`npruntime_handshake_server` ("0x29 draws NO 0x51 on a plain join"). The C2S 0x29 decoder
is renamed `decode_team_spawn_ack` (was `decode_burst_entity_request` — the "entity
request" reading was wrong).

**D-NET-147** [reimpl divergence, FIXED 2026-07-03 (fields witnessed + streamed); live
re-verify v26: fields stream golden-shaped, teleport NOT cured — root cause continues as
D-NET-151] **The S2C 0x10 static-entity records omitted the flag-0x20 dword,
subType and ammo byte — golden building records carry `field_flags 0x0A1` + `ammo_count 0xFF`;
ours sent `0x001` with ammo 0.** Wire-witnessed on ASH_I5A (golden vs v18, building + Armory
records); live symptom: entering a building / touching an armory SOMETIMES teleported the
joiner to the map origin (v18; no 0x18/0x0F traffic involved). The 2026-07-03 serializer
witness (`[orig: serialize_pool2_static_to_buffer @0x5042F0]`) CORRECTED the field identity:
the flag-0x20 i32 is **the entity FLAGS dword (entity+36) streamed raw** — the old
"parentSlot" reading was wrong (and the old "plumb static parent links" theory with it; BMS
`blink_parent` remains parsed-but-unconsumed, see below). Golden values decode as composed
spawn flags: buildings `0x04020400` = Indestructible-def (healthMax 0 → 0x4000000 `[orig:
Entity_InitFromModel @0x40dc8e]`, which ALSO sets subType = 0xFF — the flag-0x80 byte) +
type-Building (0x20000 `[orig: @0x40e105]`) + BMS `Reflective` attribute (1<<23 → 0x400, the
per-placement pier/water bit); bridges add BMS `NoShadow` (1<<24 → 0x1000000 → `0x05020400`);
`ammo_count` ← BMS record byte 81 and `refNum` ← byte 153 `[orig: Entity_SpawnFromBMSRecord
@0x40e9f0]`. FIXED: `world::Entity` gains `engine_flags`/`ammo_count`/`sub_type`/`ref_num`;
`promote_mission` composes the BMS-attribute bits + Building bit and carries bytes 81/153
(bms::Entity::gen_reserved1 RENAMED `ref_num`, dropped from the reserved-zero parse assert);
the def-sourced half (hp==0 → 0x4000000 + subType 0xFF) lands in NovaSimulation::
resolve_item_traits; `build_pool2_static_batch` streams all four;
`StaticEntityRecord::parent_slot` RENAMED `entity_flags` (encoder/decoder/nw_pp/§5.9 map).
Pinned by `npruntime_initial_state_burst` (building record emits field_flags 0x0A1 +
eflags 0x04020400 + ammo/subType 0xFF) and `nw_ingame_encode` round-trip. DEFERRED (tracked
here): the sectioned-destructible `sectionMask` rebuild (def attrib sign bit → per-section
state table `[orig: @0x50443f..0x5044a4]` — ASH_I5A golden has one such record, Power
Generator Housing flags 0x0A9), the armory `weaponByte`/`attachRef` via
`ZoneSlotChain_GetZoneInfo @0x4a2750` (ex-`CWeaponSlotManager_GetEntitySlotInfo`; golden ASH_I5A statics all carry weap 0x00),
and the `scoreFlag` def-callback gate `[orig: @0x504554]`. LIVE RE-VERIFY retail-join v26
(2026-07-03) NEGATIVE for the symptom: the 0x10 statics now stream golden-shaped (buildings
eflags `0x04020400`, ammo/subType 0xFF), yet the stand-on-entity snap (building floors,
vehicle decks) persisted unchanged — these record fields were not the cause. The real cause
was D-NET-151 (grounded-on-entity player replication), fixed + live-verified v27.

**D-NET-145** [reimpl divergence, FIXED 2026-07-02] **The initial-state burst's phase-8
loadout gate had a reimpl-invented ~10 s timeout fallback that force-started the game-start
bundle + the per-frame 0x0A stream at a client that had not sent its C2S 0x2F — i.e. was
still LOADING.** The golden retail host emits NOTHING in-match until the joiner's 0x2F: its
first S2C 0x0A directly follows the 0x5A reply (retail-ashi5a f223117–f223118), because the
0x2F is the client's load-complete signal (a client cannot build a loadout submit before its
own weapon.def/AdmDef table exists; deploy — C 0x0E — comes much later, f238947). Harmless
through v15 while the stream's anim fields were inert (off-16 = 0xFF, the apply-skip
sentinel), the fallback became a hard wedge once D-NET-143 landed real anim-def bytes: the
client's UNGATED off-16 apply (§5.10) resolved AdmDef entries against its MID-BUILD table and
its loader died at ~25% (live-witnessed retail-join v16 "stuck loading"; the slow load that
exposed the race was host-machine contention). FIXED: the phase-8 wait has no timeout
(`server_initial_state.cpp`); a joiner that never selects never deploys, exactly like retail
(the session-level timeout reaps true zombies). LIVE-VERIFIED retail-join v17/v18
(2026-07-02): slow loads complete cleanly; the v18 wire shows the game-start bundle strictly
following the joiner's 0x2F.

**D-NET-144** [reimpl divergence, FIXED 2026-07-02] **A late-spawned (joining) player's
field-17 health byte read tier 1 (0x18) instead of the golden tier 2 (0x28).** The items.def
health stamp (`resolve_item_traits`) runs at mission load; joiners spawned later and kept the
spawn-seed default health 100 with no health_max, so the tier ratio landed at 100/150. Retail
spawns every entity at `Health = itemDef->healthMax` [orig: Entity_InitFromItemDef @ 0x49e550].
Cosmetic for the joiner's own client (local apply skipped @0x4c11ac) but a wire divergence any
other observer decodes. FIXED: `resolve_item_traits` caches the Player-template hp on the world
(`World::player_item_hp`, class-8 = 150) and `spawn_player_entity` seeds
`health = health_max = hp` (item-less worlds fall back to the seed and still spawn at full) —
the structural port of the retail spawn init. Pinned by player_spawn + two_peer_fanout (spawn
byte 0x28). LIVE-VERIFIED retail-join v16/v18 (2026-07-02): the joiner's field-17 reads the
golden 0x28 on the wire; diff_0a header health = 150 matches golden.

## IDB type-sync session (2026-07-30)

A net/entity type-sync pass over `Jointops.exe.kong.i64` (net-side changes;
entity/AI-side changes in `docs/world/world-wac-ai-re.md`):

- **`NapiNPClientMsg_HandleBatchSpawn @ 0x431870` → `NapiNPClientMsg_HandleBatchKill`.**
  The function loops `Entity_KillBySlotId(slot, 0, 1)` over the u16 slots and
  replies C2S `0x28` — it kills, never spawns. The Kong name was actively
  misleading; renamed in the IDB and propagated to every `[orig:]` citation
  (§4 table, §5.25, the D-NET-66 record, `correspondence.md`, and the three
  `libs/npwire` sites), each keeping a "Kong: `HandleBatchSpawn`" breadcrumb so
  the symbol still resolves against a stock IDB.
- **`PlayerSlotEntry`** (0x40) declared (`active_flag +13`, `health +40`) and
  applied to `PlayerSlotTable_GetActiveSlot @ 0x434780` alongside
  `PlayerSlotTableHeader` (the decomp now reads `table->maxSlots` /
  `table->slotsBase`).
- **`sub_43C290 → Entity_RaiseHealthToMax`** (raises `Health +0x11E` toward
  `Entity_GetMaxHealthWithDifficulty`), typed `int(GamePlayerEntity *)`.
- Typed the two remaining untyped helpers: `PlayerInfo_SaveAndRepopulate
  @ 0x5608f0`, `PlayerInfo_HandleClassSelect @ 0x560910`. IDB saved.
