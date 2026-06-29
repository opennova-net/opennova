# NovaWorld networking - protocol + struct RE record

> **Status**: the NovaWorld web/UDP stack (libs/novacrypto, libs/napi, libs/novaworld, the
> standalone server app, and the web portal) landed in PR #37, was reverted in PR #50 while
> it matured, and is now **relanded on the `web-nw-for-real-master` integration branch**
> (see `plan/`). This protocol RE remains the durable wire record; §8 will accumulate
> per-system equivalence verdicts as the IDA grill proceeds.

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

| Layer | Contents | Reimpl home (reverted stack) |
|---|---|---|
| 4 — message sets | Selected by the `PN` field at CLIENT_HELLO. `PN=NOVAWORLDUDP` → container-based browser/session services (ClientHostRequest, ClientPlayRequest, ServerVerifyResult, ...; §3). `PN="JointOperations"` etc. → in-match TLV game traffic dispatched via the NAPI msginfo tables (§4). | `libs/novaworld` (PN dispatch inside the lib) |
| 3 — session + framing | Per-(ip,port) session state (`CK`, `SK`, `SCRK`, fragment buffers); the opcode `0x43`/`0x83` protocol-message envelope (flags/len/seq/frag). | `libs/napi` |
| 2 — NWU wire framing | 4-byte LSB CRC32 header; 1-byte opcode `0x41` HELLO / `0x42` JOIN / `0x43` SESSION / `0x46` GOODBYE (server replies `0x81`/`0x82`/`0x83`/`0x86`); NWU stream-cipher payload encryption. | `libs/novacrypto` + `libs/napi` |
| 1 — UDP sockets | Owned by the app, not a lib. | server app / Godot client |

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
| `H:0x03` | `0x621AE0` | `CNapiNPConnection_HandleDescriptionPacket` | Connection description packet. |

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
| 0x02 | 0x42E0F0 | `_0x002` | game-start confirm + position ACK |
| 0x03 | 0x425390 | `_0x003` | u8 + 2×u16 sync tick |
| 0x04 | 0x425410 | `_0x004` | session config (max players, flags) |
| 0x05 | 0x42E180 | `_HandleGameStart` | the actual GAME-START UI signal: reads u8 flag; if non-zero, client replies C2S 0x4E and resets game-state dwords; toggles `dword_A86C28`, queues UI notification, resets game timers |
| 0x06 | 0x432BC0 | `_HandleChatCommand` | server→client chat |
| 0x07 | 0x422730 | `_0x007` | per-frame keep-alive stub |
| 0x08 | 0x4281D0 | `_0x008` | game-state snapshot / delta entity updates (~2 KB) |
| 0x0A | 0x42FEC0 | `NapiNPClientMsg_0x00A` | **per-frame local-player + world-state update** (multiplexed player/timer/env/gametype + health + weapon-hit loop); full field map §5.9. Defined 2026-06-16 (was undefined — data blob mis-marked at 0x430000) |
| 0x0B | 0x422660 | `_0x00B` | copies the 616-byte BMS header into `byte_A761D0` (field map §5.4) |
| 0x0C | 0x42E730 | `_0x00C` | pool-0 organic spawn batch (AI infantry + players); `[u16 count]` header + per-record FLAT layout (slotId-first, no flag-gated optionals) per the §5.23 field map; parses name inline (crash-safe on `0x14B9` where 0x0D is not, §5.6); team → entity+354 |
| 0x0D | 0x432C40 | `_0x00D` | pool-entity spawn batch; sets `dword_A82370=3`; `[u16 count]` header + per-entity record per the §5.11 field map (always: 2×u16 flags+slot, u16 type, cstr name, 3×i32 pos, u8 team byte → entity+354 (gate 0x10) + u8 bone byte → entity+290 always; D-NET-58; conditional fields gated by every flag bit 0x01-0x8000); AI-flagged item defs (`ItemDef[+84] & 0x100000`) require the `flags & 0x800` trailer = **`[u32][u32][cstring ai_name]`** (§5.6/§5.11) |
| 0x0F | 0x42E200 | `_0x00F` | **WORLD-STATE-LOAD** (no descriptive Kong name; any "game-start" label is misleading): i32 sessionTick + 3×i32 spawn pos, 3×i16 angles, u8 flags, **fixed 128-i32 score block**, then waypoint records (off-wire gametype gate) + team names; sets `dword_81474C=0` (load-bearing input/heartbeat gate); client replies with the C2S burst 0x22 0x23 0x28 0x29 0x2D 0x32; ~624 B. Full field map **§5.29** (decoded) |
| 0x10 | 0x433400 | `_0x010` | static entity batch (pool 2): u16 start_idx, u16 count, flag-driven per-entity records; 612-644 B in retail, every frame; **full field map §5.9** |
| 0x11 | 0x4226E0 | `_0x011` | one-line stub: `dword_A82358=1` (unblocks WaitForDisconnect); retail only ever ships it bundled last with 0x0B (§5.5) |
| 0x12 | 0x425EE0 | `_0x012` | |
| 0x13 | 0x42EB50 | `_EntityDeath` | entity death (2nd path, beside 0x26) `[u16 handle][i16 killerSource]` → Health=0 + death cb (§5.35) |
| 0x14 | 0x42F240 | `_0x014` | |
| 0x16 | 0x42FAE0 | `_0x016` | PLAYER-LIST — full layout verified §5.20 (controlled capture 2026-06-17) |
| 0x17 | 0x4226F0 | `_0x017` | |
| 0x18 | 0x433780 | `_0x018` | does not fire in normal multiplayer (§5.7); an early "EntitySpawn" label is unverified |
| 0x19 | 0x425E80 | `_0x019` | |
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
| 0x29 | 0x427D00 | `_0x029` | |
| 0x2A | 0x425BA0 | `_0x02A` | chat-history entry `[i32][i32][i16]` (10 B) → Chat_AddToHistory (§5.35) |
| 0x2B | 0x427DF0 | `_0x02B` | |
| 0x2C | 0x427E10 | `_0x02C` | chat entry |
| 0x2D | 0x427E90 | `_0x02D` | |
| 0x2E | 0x427F80 | `_0x02E` | |
| 0x2F | 0x430E10 | `_0x02F` | |
| 0x30 | 0x431170 | `_HandleChecksumRequest` | entity-checksum request `[u8 entityId][u16 checksum]` → reply **C2S 0x20** (NOT 0x21; §5.35) |
| 0x31 | 0x4311E0 | `_0x031` | CRC request for weapon loadout → C2S 0x21 |
| 0x32 | 0x428060 | `_0x032` | |
| 0x33 | 0x425FA0 | `_0x033` | |
| 0x34 | 0x4283A0 | `_0x034` | |
| 0x35 | 0x4261A0 | `_0x035` | |
| 0x36 | 0x426120 | `_0x036` | |
| 0x37 | 0x431250 | `_0x037` | |
| 0x38 | 0x4260B0 | `_0x038` | |
| 0x39 | 0x42E6D0 | `_0x039` | anti-cheat anim-map CRC challenge `[u32 seed]` → C2S 0x1C (§5.34) |
| 0x3A | 0x422680 | `_0x03A` | |
| 0x3B | 0x431340 | `_0x03B` | |
| 0x3D | 0x422870 | `_0x03D` | |
| 0x3E | 0x4226D0 | `_0x03E` | ack-style |
| 0x3F | 0x42BB20 | `_0x03F` | |
| 0x40 | 0x425A50 | `_0x040` | minimap-overlay update / capture-zone state — `[u8 count][N×6B entry]`, full map §5.19 (controlled capture 2026-06-17) |
| 0x41 | 0x4254C0 | `_0x041` | |
| 0x42 | 0x4281A0 | `_0x042` | input/state-flags `[u16]` → Input_UnpackStateFlags (§5.35) |
| 0x43 | 0x42FA90 | `_0x043` | time-sync ping `[u32 serverTs]` → C2S 0x08 (§5.34) |
| 0x44 | 0x422710 | `_0x044` | entity-routed sub-packet: `[u16][i16 netId][u8 subtype]` + class body → entity def+356 callback (§5.36; body partial) |
| 0x45 | 0x422890 | `_0x045` | terrain-tile load batch (§5.37, D-NET-83 — NOT "empty payload"); clamps `g_loading_progress`→6; forwards the body to `PolyTrn_LoadTileData()` (paged tile-array load) |
| 0x46 | 0x431370 | `_0x046` | PLAYER-SYNC — full layout + field read order verified §5.21 (controlled capture 2026-06-17) |
| 0x48 | 0x4284B0 | `_0x048` | |
| 0x49 | 0x42C0A0 | `_0x049` | weapon-reload `[u16 handle][u16 reloadParam]` → WeaponSlot_ReloadAmmo (IDB name `handle_camera_sync_packet_0x049` is wrong; §5.35) |
| 0x4C | 0x428570 | `_0x04C` | target-assignment list (squad/AI orders) |
| 0x4D | 0x4317B0 | `_HandleSpawnSlot` | reads u8 slot; local slot → tip event; otherwise client replies C2S 0x22+0x23; reads `dword_24C1928` as a skip-tip-if-already-spawned guard (never writes it) |
| 0x4E | 0x431870 | `_HandleBatchSpawn` (misleading) | u16 count + per-slot u16; calls `Entity_KillBySlotId` (kill, not spawn), then replies C2S 0x28 |
| 0x4F | 0x4286C0 | `_0x04F` | |
| 0x50 | 0x431910 | `_0x050` | |
| 0x51 | 0x431BB0 | `_0x051` | |
| 0x52 | 0x428A80 | `_0x052` | |
| 0x53 | 0x428AE0 | `_0x053` | |
| 0x54 | 0x429040 | `_0x054` | |
| 0x56 | 0x431D10 | `_0x056` | touches `dword_24C1928` (write unconfirmed; decomp on demand) |
| 0x57 | 0x432210 | `_0x057_RTT` | RTT ping/pong `[u32 ts][u8 echoFlag]` (§5.34); ⇄ C2S 0x2C |
| 0x58 | 0x4228C0 | `_0x058` | texture loader (terrain assets) |
| 0x59 | 0x4228E0 | `_0x059` | deployed-item / weapon-overlay spawn (32 B): item ids + owner + slot + parent + 3×i32 pos + 3×u16 ang (§5.36) |
| 0x5A | 0x4290E0 | `_0x05A` | weapon-loadout sync: `[u8 avatarClass]` + a `{typeId, ammoP, ammoS, ammoAlt}` slot chain to a `0xFF` terminator (typeId = AdmDef index); resets `dword_81474C=0`. Field map **§5.30** (decoded) |
| 0x5B | 0x4322B0 | `_0x05B` | |
| 0x5C | 0x425200 | `_0x05C` | |
| 0x5D | 0x429730 | `_DestroyEntityList` | entity-destroy list (clean despawn) `[i16 slot]×N` → Entity_Destroy + PlayerSlot_ClearAndUnlink; pairs with 0x46 0x8000 removal (D-NET-80) |
| 0x5E | 0x4297B0 | `_0x05E` | |
| 0x5F | 0x4228F0 | `_0x05F` | |
| 0x60 | 0x432350 | `_HandleFileTransferChunk` | **chunked file transfer** (decoded §5.28): `[u32 transferId][u32 totalSize][u32 chunkOffset]` + raw file bytes → `CDataStream`; re-request **C2S 0x33** when incomplete. probe2 completed in one 163-B chunk (transfer content = the `SERVERNAME`/`MISSIONNAME` VarList), so 0x33 never fired — **D-NET-74 corrects the D-NET-69 "announce VarList / type=1,bodyLen,reserved" reading** (a single-chunk artifact). **probe3 forced multi-chunk** (a >200-B MOTD → total=239: chunk0 200 `[more]` + chunk1 39 `[FINAL]`), so **C2S 0x33 fired** — first multi-chunk witness (D-NET-75) |
| 0x61 | 0x4297C0 | `_HandleSessionKey` | u32 session key → `g_sessionKey`; **disables `_connectlog.txt`** (source of the "DISABLING CONNECTLOG" log line) |
| 0x62 | 0x42D200 | `_0x062` | |
| 0x63 | 0x42D450 | `_0x063` | |
| 0x64 | 0x432410 | `_HandleMissionDataChunk` | **chunked file transfer** (decoded §5.28): same 12-B `[transferId][totalSize][chunkOffset]` header + raw file bytes → buffer (completion extracts 3×32-B mission-name strings); re-request **C2S 0x37** when incomplete. probe2 completed in one 180-B chunk (D-NET-74). **No compression codec** — raw file content (resolves the deferred "0x64 inner codec") |
| 0x65 | 0x429870 | `_0x065` | |
| 0x66 | 0x42D4C0 | `_HandleWeaponRestrictions` | count + (slot, restriction) pairs |
| 0x67 | 0x42D570 | `_0x067` | |
| 0x68 | 0x42DAA0 | `_0x068` | entity-index list request `[u32 startIdx]` → C2S 0x3D (§5.34) |
| 0x6A | 0x432510 | `_0x06A` | |
| 0x6B | 0x425520 | `_0x06B` | minimap overlay batch `[u8 count]`+count×12B (handle@+0; blip rebuilt from entity state, 10 trailing B unused) (§5.35) |
| 0x6C | 0x428FC0 | `_0x06C` | |
| 0x6D | 0x430C50 | `_HandleEntityDeath` | |
| 0x6E | 0x429880 | `_0x06E` | team/squad roster sync: `[u8 teamCount]` + per team `{u16 entityHandle, u16 slotIdx, u8 memberCount, u16 slotHandle, u16 members[]}`. Field map **§5.31** (decoded) |
| 0x6F | 0x428D60 | `_0x06F` | cinematic camera assignment |
| 0x70 | 0x429A30 | `_0x070` | |
| 0x71 | 0x425600 | `_0x071` | |
| 0x72 | 0x425710 | `_0x072` | |
| 0x73 | 0x425770 | `_0x073` | |
| 0x74 | 0x4258B0 | `_0x074` | |
| 0x75 | 0x4259E0 | `_0x075` | spectator-mode flags (2 B); sets `byte_A860EC`, `dword_24D1DF4` |
| 0x76 | 0x42D540 | `_0x076` | u16 → `dword_24D59FC` |
| 0x78 | 0x4259070 | `_0x078` | (address as recorded in the source note has 7 hex digits — likely a typo; re-verify in the IDB) |
| 0x79 | 0x429B00 | `_0x079` | spectator-mode flag (1 B → `dword_82BEE4`) (§5.35) |
| 0x7A | 0x429B40 | `_0x07A` | player name (max 64 chars) → server-info struct |
| 0x7B | 0x429BB0 | `_0x07B` | full player/session info: 5×cstring + u32 + 2×cstring. Field map **§5.32** (decoded) — roles witnessed from the landing globals + PunkBuster cvars (the Hex-Rays "clan/squad/rank" comment is wrong): name / **playerId** (NovaWorld account id, **not** a clan tag) / serverName / missionName / mapFile / … / gameName |
| 0x7C | 0x426020 | `_0x07C` | |
| 0x7D | 0x432690 | `_0x07D` | |
| 0x7E | 0x425E20 | `_0x07E` | two cstrings → `byte_A86520` / `byte_A86120` (server config strings) |
| 0x7F | 0x429E60 | `_0x07F` | |
| 0x80 | 0x42A070 | `_0x080` | |
| 0x81 | 0x42A0B0 | `_0x081` | |
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
| 0x03 | 0x501BE0 | |
| 0x04 | 0x5199D0 | |
| 0x06 | 0x513310 | `_0x006_ClientFiredRound` |
| 0x07 | 0x4FC970 | |
| 0x08 | 0x502210 | time-sync / anti-speedhack — `[u32 sessionId][u32 gameTimestamp]`; host checks the client's reported game-time deltas stay within 3% of wall-clock (`GetTickCount`). NOT movement. [orig: `validate_time_sync @ 0x502210`] |
| 0x09 | 0x513200 | client checksum response |
| 0x0A | 0x513260 | |
| 0x0B | 0x51AB10 | |
| 0x0C | 0x501C30 | entity sub-packet: `[u16 handle][u16 itemTypeId][u8 sub_op][payload]` → per-type callback at `entity_def+356`; §5.9 |
| 0x0D | 0x513760 | replication frame ACK |
| 0x0E | 0x519AF0 | |
| 0x06 | 0x513310 | client-fired-round — fixed 45 B (§5.16); host validates shooter authority + ammo via Server_ValidateAndFireRound and may emit S2C 0x0A trailing weapon-hit (§5.9.1) |
| 0x0F | 0x514180 | player/entity-info request — `[u16 pool-0/1 handle]`; host serializes that entity's info + broadcasts it as S2C 0x18. The fallback spawn-menu "query loop" (pool-1 slots `0x10NN`) is this — NOT an input/movement frame (JO has no raw-input channel; see D-NET-68). [orig: `NapiNPServerMsg_HandlePlayerInfoRequest @ 0x514180`] |
| 0x13 | 0x514330 | |
| 0x14 | 0x501E00 | |
| 0x16 | 0x511A70 | client→server chat |
| 0x17 | 0x514850 | |
| 0x18 | 0x51A020 | |
| 0x19 | 0x514250 | |
| 0x1A | 0x514B20 | |
| 0x1B | 0x501D90 | |
| 0x1C | 0x501D40 | anim-map CRC reply (to S2C 0x39, §5.34) |
| 0x1D | 0x501C60 | |
| 0x20 | 0x501F70 | entity-checksum reply (to S2C 0x30, §5.35) |
| 0x21 | 0x502050 | anti-cheat CRC reply (§5.17) — reads u8 player_index + u32 expected_crc; host XORs computed CRC against per-connection salt at `playerCtx+89924`, mismatch logs "ACRC" + sends "PUNT ACRC" |
| 0x22 | 0x514C90 | player-sync request `[u8 slot][u16 fieldFlags]` → host serializes & replies S2C 0x46 (§5.33); the client queues it on a 0x46 `0x4000`-ack and as a 0x0F/0x4D reply-burst member |
| 0x23 | 0x514D50 | visible-players request (empty body) → host replies S2C 0x4C snapshot (§5.33); 0x0F/0x4D reply-burst member |
| 0x24 | 0x514DC0 | |
| 0x25 | 0x514DF0 | weapon-reload request (mid-game) — note the direction asymmetry vs S2C 0x25 (§5.3) |
| 0x26 | 0x502390 | |
| 0x27 | 0x4FC980 | |
| 0x28 | 0x51A550 | weapon-loadout request `[u32 loadoutFilter][u32 flags][u16 extra]` → host replies S2C 0x4E (§5.33); 0x0F reply-burst member |
| 0x29 | 0x514F10 | entity-packet request `[u16 bufferIndex]` → host writes that entity's packet & replies S2C 0x51 (§5.33); reply-burst member |
| 0x2B | 0x514FE0 | |
| 0x2C | 0x515070 | RTT ping/pong consumed `[u32 ts][u8 echoFlag]` (§5.34); ⇄ S2C 0x57 [HandlePingResponse, enforces min/max ping] |
| 0x2D | 0x502430 | burst-member receiver |
| 0x2E | 0x515390 | |
| 0x2F | 0x515790 | |
| 0x30 | 0x5029B0 | |
| 0x31 | 0x5024A0 | |
| 0x32 | 0x51A600 | burst-member receiver |
| 0x33 | 0x515230 | next-chunk request for the S2C 0x60 file transfer — payload `[u32 transferId][u32 nextOffset]` (8 B). Fires only when a transfer spans >1 chunk; probe2's 0x60 fit in one chunk so it didn't fire — NOT because the semantic is unverified (D-NET-74 corrects D-NET-69) |
| 0x34 | 0x5024B0 | |
| 0x35 | 0x500DF0 | |
| 0x36 | 0x500E00 | |
| 0x37 | 0x5152E0 | next-chunk request for the S2C 0x64 file transfer — same `[transferId][nextOffset]` 8-B payload as 0x33. Didn't fire in probe2 because that transfer fit in one chunk, NOT because the protocol is re-request-free (D-NET-74) |
| 0x38 | 0x502510 | |
| 0x39 | 0x500E20 | |
| 0x3C | 0x519110 | |
| 0x3D | 0x500EC0 | entity-index list reply (to S2C 0x68, §5.34) |
| 0x3E | 0x500E10 | |
| 0x3F | 0x518F10 | |
| 0x40 | 0x51C4C0 | |
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
  pcap. Remaining C2S candidates without field maps yet: 0x22 / 0x23 / 0x28 / 0x29 (the
  "burst-member" replies; tiny 3 B payloads in capture), 0x0F (no samples in the 3-player
  capture), 0x33 / 0x37 (file-chunk re-request replies; need a C2S 0x60 / 0x64 flow).

## 5. Tag-level findings (audited against retail captures)

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
  echoes it in a `0x48` (bundled with the `0x37` mission request). The existing `0x48`-learning path
  is retained as the override for the NovaWorld case, where the dcb is gate-assigned, not
  host-assigned (TODO(P6): gate the host-assignment on the LAN network type once NovaWorld transport
  lands). This refines, but does not contradict, D-NET-92 (the client self-ID is numeric in retail;
  the opennova `JoinerConnection` keeps its name-match per the ROADMAP).

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
   | 1 | 0x10 | `[orig: sub_5042F0]` (static batch; §5.9) |
   | 2 | 0x0D | `[orig: serialize_entity_pool_to_packet_0 @ 0x503940]` (pool spawn; §5.11) |
   | 3 | 0x0C | `[orig: serialize_entity_states_to_buffer @ 0x5030a0]` (entity states) |
   | 4 | 0x20 | `[orig: serialize_entity_pool_to_packet @ 0x503460]` (bulk pool-3; §5.12) |
   | 5 | 0x45 | `[orig: sub_506570]` (terrain; repeats until it returns 0) |
   | 6 | 0x7E | `[orig: sub_506620]` |
   | 7 | 0x1A | `[orig: NetPacket_WriteTimestamp @ 0x5046c0]`; then `CNetPlayer_SetGameState(9)` |

   The per-entity payload bodies these serializers emit are the inverse of the witnessed decoders: the
   player class via `[orig: NetPacket_SerializePlayerState @ 0x4C09C0]` (mode 1 = write compact / mode 3
   = write extended; §5.10 gives both directions, write side = `Network_CompressFixedPoint` instead of
   decompress, `Entity_TransformWorldToLocal` for the vehicle-mounted branch), AI infantry via
   `NetPacket_SerializeInfantryEntityState @ 0x4C0320` (§5.14), vehicles via
   `Entity_SerializeMountedVehicleState @ 0x460560` (§5.13).

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
| 0x2C | `NetPacket_WriteServerNameAndMapFile @0x505780` (Kong-misnamed `WriteTypeNameAndBaseName` — FIXED) | `g_server_name_str` ("Untitled") + `g_map_file_name` ("TDH_I5A.BMS"), two NUL C-strings | `SessionReplyConfig.server_name`/`mission_file` |
| 0x08 | `ServerConfig_SerializeToPacket @0x505bd0` | 51 B = 10 rule dwords [respawn 30, timelimit 10, _, gametype, _, score 50, _, startdelay, _, _] + 7 bytes + flags dword (`CNapiServerConfig_BuildFlags @0x4c4dc0`) | `NapiNPServerCtx.rules` (ServerRules) + `build_server_config_flags` |
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
- **Anim state** — `animStateId` +0x2bc (entity[175]; indexes the state→name table `dword_8139E8` /
  `off_8135F0`), `animSlotIndex` +0x2c0 (`Entity_ComputeAnimSlotIndex`), `prevAnimStateId` +0x2c8
  (entity[178]).
- **Aim / torso / head** (incremental `+= chase` writes; reset to body yaw on spawn) — `aimPitch`
  +0x2d0 (entity[180]), `torsoYaw/Pitch/Roll` +0x2d4/+0x2d8/+0x2dc (entity[181-183]; feed the render
  Yaw/Pitch/Roll bones), `headLookYaw/Pitch` +0x2e4/+0x2e8 (entity[185/186]), `aimHeading` +0x2ec
  (entity[187]; render yaw chases it), `pitchBlend` +0x380 (added to render Pitch), `headLookDecay`
  +0x36c.
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
`[orig: CMap_SetupSpawnCamera @ 0x50cf60]` (Kong's name is a misnomer — it positions the player
ENTITY, not just a camera), called on join `[orig: Server_OnPlayerJoin @ 0x51a680 → 0x51a786]`
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
starts. `[orig: Server_OnPlayerJoin @0x51a680]` calls `CMap_SetupSpawnCamera` with spawn-param
low-word **0** (not 0xFFFF), so the engine first tries `[orig: sub_4FE110 @0x4fe110]` — which is a
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
resolution (`g_GameType`-driven order, the `sub_4FE110` handle pre-check, the cycling-vs-farthest
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
`byte_A761D0`). Cross-capture diff over three maps (dvxi5 / dvxc1 / g11), sourced from the
fragment-aware dissector (`tools/wireshark/jointops_udp.lua`); never diff from
non-reassembled per-fragment extracts:

| Offset (dec) | Width | Field | dvxi5 | dvxc1 | g11 | Category |
|---|---|---|---|---|---|---|
| 0-3 | 4 | Signature | `42 4d 53 13` | same | same | structural (invariant) |
| 4-35 | 32 | Mission display name | "AS - Dormant Volcano Isle" | "TD - Tenaga Delta" | "AS - Kendari Airport" | map-specific |
| 36-67 | 32 | Designer | "Brophy / Brent / BB" | "Brent Houston / James Payne" | "Brent" | map-specific |
| 68-99 | 32 | Map basename | "dvxi5" | "dvxc1" | "g11" | map-specific — **the BMS file the client must load** |
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
| 276-307 | 32 | Atlas key | "trntile10" | "trntile10" | "trntilea1" | atlas-specific |
| 308-615 | 308 | tail metadata + zeros | sparse non-zero | mostly zeros | mostly zeros | map-specific |

Consequence: any fixture blob of this tag is **map-locked** (the basename at +68 names the BMS
the client loads); a server must synthesize it from the actual mission selection.

### 5.5 Tag 0x11 bundling policy (retail)

Retail emits S2C 0x11 **only** inside the BMS-state bundle, always last, never standalone —
verified in both capture3 (frames 197227/197228, the only S2C 0x11 emissions) and capture4
(frames 163797/163798): bundle signature `[0x1C(0), 0x0B(616), 0x66(1), 0x76(2), 0x11(0)]`.

Why it matters: 0x11 sets `dword_A82358 = 1`, which unblocks WaitForDisconnect; the client
then immediately runs `Game_LoadTerrainDuringConnect`, which reads the mission basename out of
the tag-0x0B header copy (`byte_A761D0+0x44`) via `sub_610940`. Sending 0x11 before the 0x0B
header is delivered makes that load fail → unnumbered "Mission loading aborted" → disconnect
~2-3 s later. A reimplementation must deliver 0x0B and 0x11 in the same packet (0x11 last) or
at minimum strictly after 0x0B.

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

### 5.7 Spurious tag 0x18

S2C 0x18 does not fire in normal multiplayer (the `NapiNPServerMsg_0x00F → 0x18` reply path is
inert); the reverted stack emitted it speculatively and it was marked for removal.

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
| parentSlot | i32 | `flags & 0x20` | entity+36 |
| ammoCount | u8 | **always** | entity+290 (u16) |
| refNum / subType | u8 | `flags & 0x40 / 0x80` | entity+533 / +532 (D-NET-94; not bones) |
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
| flags1 | u8 | `0x04`→loadprog `dword_A8235C=10`; `0x02`→`byte_A860DC`; `0x01`→death/spectator (camera reset, `CameraOffset.Z=0xD000`, spawn-gate `dword_24C1928`) |
| flags2 | u8 | low 2 bits select the sub-block AND its ROLE: **0** = aim/player-view (client-authoritative — the working golden does NOT send it in gameplay), **1** = server-status/timer, **2** = ENV, **3** = objective (gametype-gated). The golden CYCLES 1/2/3. bit 3 (`0x08`) gates a 6 B vehicle-passenger record after the fixed tail (joiner-as-passenger; only the `(flags2 & 0xF) == 8` exact value triggers it — i.e. sub-block 0 + passenger bit) [orig: 0x430459] |
| sub-block 0 | `==0`: 6× u8 + u8 (`0xFF` sentinel) + i32, 11 B | player aim/state → dword_A85B5C… [orig: 0x430054..0x43012E] |
| sub-block 1 (server-status/timer) | `==1`: 4× u8 + i16, 6 B | `[u8→dword_C6EAE0][u8→dword_C6EAE4][u8→g_serverFps (0xC8FC64)][u8→g_serverCpuPct (0xC8FC68)][i16 timer]`; each u8 is `movzx`-widened from one wire byte; `dword_24C1958 = 62 × i16` (62 Hz timer; `-1` if negative) [orig: C6EAE0@0x4301a1, C6EAE4@0x4301bc, g_serverFps@0x4301e0, g_serverCpuPct@0x430200, timer@0x430210]. Init defaults [orig: 0x4f638b C6EAE0=20, **C6EAE4=13**, C6EAE8=10]. **`dword_C6EAE4` is the fall-damage tolerance (§5.38d)** — a server that only ever sends sub-block 0 leaves the client's C6EAE4 at 0 → per-frame fall damage [orig: read @0x4b7d0d] |
| sub-block 2 (ENV) | `==2`: u16,u16,u16,u8,u8,u8,u8,u8, 11 B | `Env_FogDistTarget=u16<<16`, `Env_FogDistAccelClamp=u16<<8`, `Env_CurTimeFixed24=u16<<13` (TOD), `Env_QuakeTicks`, `Env_CloudScrollRateTarget=u8<<10`, u8<<8, `Env_OvercastBlendTarget=u8<<8`, u8 [orig: 0x430253..0x430341] |
| sub-block 3 | `==3 && g_GameType & 0x20000`: 4× i32, 16 B (else 0 B) | dword_AC86E8… — gate is wire-invisible, so `decode_frame_update` reads the body only when its `is_objective_gametype` hint is set. **First witnessed in probe3** (Co-op, `g_GameType 0x30020`; 771 frames, body all-zero); the `flags2 & 0xF0` high bits don't change sub-block selection (D-NET-75) [orig: 0x430361..0x4303C8] |
| state_flag_byte | u8 | bit 0→`dword_B76484`, bit 1→`dword_B76480`, bits 0/1→`g_local_player_entity.pad7[12]` bits 8/9 (the `<<8` of older notes was the receiver's internal shift, not a wire-format detail) [orig: 0x4303E5] |
| mountHandle | u16 | vehicle-mount handle (`pool<<12\|slot`; `0xFFFF`=none) [orig: 0x430408] |
| health | i16 | read @0x430428; applied late in the handler — compares the new value to the stored `Health` (`cmp dx,[entity+0x11E]` @0x43059a, `jge` skip @0x4305a1) and, ONLY when it DROPPED, fires INLINE COSMETIC feedback (red flash `dword_B764B4+=0x78` @0x4305a3, camera-shake `dword_B764B0+=0x0A` @0x4305c1; both capped 0xFF; **no `Radar_AddBlip`** — distinct from the body motor's `Player_OnDamageReceived`, §5.38d) — then stores `Health` @0x4305df. ⇒ stream this at full health (`healthMax`) or any below-stored tail self-triggers the flash [orig: 0x430428 / 0x43059a / 0x4305df] |
| state_word | i16 | → `*(WORD*)g_local_player_entity->pad7` (packed state) — bytes 6/7 of the 7 B tail; previously misread as two separate `hdr_trail_a/b` bytes [orig: 0x430442] |
| **passenger record (conditional, `(flags2 & 0xF) == 8`)** | 6 B (or 2 B early-skip) | — |
| passenger_handle | u16 | passenger entity handle; `0xFFFF` early-skips the next 4 B [orig: 0x430474] |
| seat_yaw | u16 | rider body yaw [orig: 0x4304C3] |
| seat_pitch | u16 | rider body pitch [orig: 0x4304DC] |
| event loop | trailing `[u8 tag]…` | tags exactly `{0,1,2}` — `cmp eax,2 / jg` at `0x4306DA` treats any tag ≥3 as silent terminator (same exit as `tag==0`); `tag==1`→`[u16 handle][u16 typeId]` then per-class callback (§5.10b); `tag==2`→§5.9.1 weapon-hit; ends at `tag==0`/EOB/tag≥3 [orig: 0x4306A1, handle@0x43070C, typeId@0x43076B] |

(The handler was undefined in the IDB — a data blob mis-marked at the `0x430000` page boundary;
defined 2026-06-16. Sub-block + tail field maps fully witnessed 2026-06-16c via
`NapiNPClientMsg_0x00A` grill: the previously empirical `hdr_trail_a/b` 2-byte trailer is refuted
— it's the high half of the `state_word i16`. The `(flags2 & 0xF) == 8` vehicle-passenger record
appears 4× in the 2026-06-16b loopback capture, all on sub-block 0 frames.)

#### 5.9.1 Weapon-hit record (event-loop `tag==2`) — wire decoded 2026-06-16d

The trailing event loop's `tag==2` branch is the projectile/melee impact record. Decoded by
`NetPacket_DeserializeWeaponHit @ 0x42F270` — sole receiver, called from `0x4306EF` inside
`NapiNPClientMsg_0x00A`. The record is 17-20 B, variable by `flags` gate bits `0x80` / `0x40`:

| Field | Bytes | Gate | Landing |
|---|---|---|---|
| `flags` | u8 | always | local; bits 0x80 → `parent_byte` present, 0x40 → `weapon_handle` present, low bits 0x01/0x02 gate downstream damage-processing branches (not wire reads) [orig: 0x42f2a8] |
| `adm_index` | u8 | always | → `AdmDef_GetEntryByIndex(adm_index)` resolves the action-descriptor entry [orig: 0x42f2ca] |
| `hit_subtype` | u8 | always | → `dword_A822E0` (last-hit subtype global; categorises the hit) [orig: 0x42f2e2] |
| `parent_byte` | u8 | `flags & 0x80` | `pos_z_decompressed` local → `hitDataPtr[5]` low byte; bone/seat index for the parent of the hit [orig: 0x42f30a] |
| `target_handle` | u16 | always | `(pool<<12)\|slot` of the hit entity; `0xFFFF` = no target (early `return result`); validated against `g_pool_list` capacity [orig: 0x42f337] |
| `weapon_handle` | u16 | `flags & 0x40` | parent-weapon sub-handle stored at `entity_link+12` for AI damage attribution; `0xFFFF` = sentinel (no parent weapon) [orig: 0x42f359] |
| `damage_extra_raw` | u16 | always | raw u16 → `word_B7C670` global (weapon-extra slot; observed as a monotonic per-shot counter in the 2026-06-16d capture) [orig: 0x42f37e] |
| `pos_x_compressed` | u16 | always | `Network_DecompressFixedPoint(.) + dword_A822E4` → `position[0]` (impact world X) [orig: 0x42f39c] |
| `pos_y_compressed` | u16 | always | `+ dword_A822E8` → `position[1]` [orig: 0x42f3c7] |
| `pos_z_compressed` | u16 | always | `+ dword_A822EC` → `position[2]` [orig: 0x42f3f2] |
| `yaw_bam_high` | u16 | always | raw u16 reinterpreted as the high 16 bits of a 32-bit BAM (`raw << 16`); impact heading [orig: 0x42f41f] |
| `pitch_bam_high` | u16 | always | raw u16 reinterpreted as BAM high word; impact pitch [orig: 0x42f43c] |

**Closed byte-sum table by flags combination:**

| flags & 0xC0 | parent_byte? | weapon_handle? | total |
|---|---|---|---|
| `0x00` | no | no | **17 B** |
| `0x80` | yes (+1) | no | **18 B** |
| `0x40` | no | yes (+2) | **19 B** |
| `0xC0` | yes (+1) | yes (+2) | **20 B** |

After deserialization the receiver dispatches into the action-descriptor execution path:
`flags & 1` enters projectile-impact, `flags & 2` enters direct-damage; both ultimately call
`RoundData_ProcessHit` with the (`position`, `entity_ptr`, `adm_index`, `flags`, `hit_subtype`,
`parent_byte`) tuple. The yaw/pitch BAM words feed FOV-cone sound playback at the impact site.

**Cross-witness (capture `host_and_join_game_on_opennovaworld_loopback_threeplayers_more_gameplay.pcapng`,
2026-06-16d).** 20 weapon-hit records across 672 0x0A frames, all decoded byte-exact, zero
walker halts. Observed flag bytes: `{0x02, 0x12, 0x22, 0x32}` (none with 0x80/0x40 set — all
17 B minimum). Two distinct (`adm_index`, `hit_subtype`) tuples: `(68, 12)` × 13 hits, target
`p0/s1`; `(7, 12)` × 7 hits, target `p0/s0`. `damage_extra_raw` increments monotonically per
shot in each weapon's sequence (`0x020b…0x0217`, `0x0005…0x0006`), suggesting a per-weapon
shot-id counter rather than damage value. Sample wire bytes — first record (17 B):

```
02 44 0c 01 00 0b 02 9a 44 38 6d d6 ea bc 73 02 f9
flags=02 adm=44(=68) sub=0c(=12) target=0x0001 dmgExtra=0x020b
pos=(0x449a,0x6d38,0xead6) yaw_BAM=0x73bc pitch_BAM=0xf902
```

The decoder is bounds-checked end-to-end (libs/novaworld §5.9.1
`decode_weapon_hit_record`) and the nw_pp walker advances past tag==2 records to keep decoding
the rest of the frame — previously the walker halted on the first hit per frame, masking
subsequent records.

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
`dword_C867A4 / C867A8 / C867AC` (map origin); when mounted, position is vehicle-LOCAL
instead (the case-3/4 vehicle branch transforms via `Entity_TransformWorldToLocal` before
the cursor write).

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
Ported as `network_decompress_fixedpoint` + `decode_frame_update` (libs/novaworld) and folded
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
| 0 | 1 | vehicleBone | entity+0x157 |
| 1 | 1 | seatType (0/1/2) | local seat-type byte |
| 2 | 2 | vehicleHandle (`0xFFFF`=none) | pool resolve |
| 4 | 2 | posX *compressed* | `DecompressFixedPoint` → entity+4 (vehicle-local if mounted, else world + `C867A4`) |
| 6 | 2 | posY | → entity+8 (+ `C867A8`) |
| 8 | 2 | posZ | → entity+0xC (+ `C867AC`) |
| 10 | 1 | yaw byte | high byte of a 32-bit BAM → `entity+0x10` (heading) on read [D-NET-57] |
| 11 | 1 | pitch byte | same shape → `entity+0x14` (pitch) on read; the write sources `(entity+0x14 + 0x800000) >> 24` [D-NET-57] |
| 12 | 1 | anim slot low | → entity+0x12C |
| 13 | 1 | state flags | bit `0x02` = spawning, bit `0x04` = mounted; → entity+0x24 |
| 14 | 1 | weapon-anim state | → entity+0x2B8 / 0x2BC |
| 15 | 1 | priority | → entity+0x377 |
| 16 | 1 | anim def index | → entity+0x2B0 |
| 17 | 1 | health classification | `Entity_SetHealthFromDifficultyByte @ 0x4AD580`: `tier=(byte>>4)&3` @0x4ad596 → `Health = {tier2 ≈0.875×, tier1 ≈0.594×, tier0/3 ≈0.219×} × ItemDef.healthMax` (@0x4ad5f4 / 0x4ad65b / 0x4ad68c), low nibble → `playerClass` +0x294 @0x4ad5a2. **Don't-care for the LOCAL player**: `NetPacket_SerializePlayerState` SKIPS this apply for `g_local_player_entity` (`cmp edi,g_local_player_entity; jz` @0x4c11ac → local branch only floors `Health` at 1 @0x4c11c4); the local player's health comes from the §5.9 0x0A tail, not this byte |

**Spawn hook (load-bearing):** when `state flags & 2` is set AND the entity is the local
player, the engine fires `Game_InitNewRound @ 0x422740` + `Entity_ResetToSpawnState @ 0x4B9610`
— the "actually spawned" signal the client interprets. The case-1 write side is the inverse:
position via `Network_CompressFixedPoint(entity+4..C − mapOrigin)`, vehicle-local via
`Entity_TransformWorldToLocal` when a parent vehicle is attached.

#### Tag 0x0C body — extended (type-10) [orig: `NetPacket_SerializePlayerState` case 3/4]

Fixed **43 B body** (5-B header + 43 B = 48 B total — every captured C 0x0C frame in
2026-06-16b is exactly 48 B). The joiner's per-frame uplink for its own player entity,
including a host-validated anti-cheat block:

| off | bytes | field | landing (host receiver, case 4) |
|---|---|---|---|
| 0 | 2 | vehicleHandle (`0xFFFF`=none; `(h&0xF000)>=0x5000` invalid) | pool resolve via `g_pool_list` |
| 2 | 4 | posX (i32 LE, 16.16; vehicle-local if mounted) | smooth-target entity+0x234 (ABSOLUTE world — NO map-origin add on receive; mounted = vehicle-local lift via `Entity_TransformLocalToWorld @0x43BD00`; §5.38a / D-NET-91) |
| 6 | 4 | posY | entity+0x238 |
| 10 | 4 | posZ | entity+0x23C |
| 14 | 2 | heading (i16 LE, sign-ext ×0x10000) | entity+0x240 / +0x10 (32-bit BAM) |
| 16 | 2 | pitch (i16 LE, sign-ext ×0x10000) | entity+0x244 / +0x14 |
| 18 | 1 | RESERVED — cursor advance, no read | — |
| 19 | 1 | anim slot low | entity+0x12C (low byte only) |
| 20 | 1 | flagsXor (mask `0x1C` — bits 2-4 only) | XOR'd into entity+0x24 |
| 21 | 1 | anim def 1 | entity+0x130 |
| 22 | 1 | anim def 2 | entity+0x131 |
| 23 | 1 | anim def 3 | entity+0x132 |
| 24 | 1 | RESERVED — read into AL, discarded | — |
| 25 | 1 | stat byte 0 | playerSlot+0x15F78 |
| 26 | 1 | stat byte 1 | playerSlot+0x15F79 |
| 27 | 2 | weapon id 0 | playerSlot+0x1708A |
| 29 | 2 | counter 0 (u16 → zero-ext u32) | playerSlot+0x17094 |
| 31 | 2 | weapon id 1 | playerSlot+0x1708C |
| 33 | 2 | counter 1 (u32) | playerSlot+0x17098 |
| 35 | 2 | weapon id 2 | playerSlot+0x1708E |
| 37 | 2 | counter 2 (u32) | playerSlot+0x1709C |
| 39 | 2 | weapon id 3 | playerSlot+0x17090 |
| 41 | 2 | counter 3 (u32) | playerSlot+0x170A0 |

The 8 trailing u16s form **4 pairs of `(weapon_id, fire_counter)`** — the host's anti-cheat
ground truth for shot/hit tallies. `playerSlot` = `packetCtx+0x20` = the joiner's
per-connection player struct (`connectionCtx+0x160 → playerObj+0xC0`, set by
`NapiNPServerMsg_0x00C`). The case-3 send path mirrors this layout from the joiner's local
state.

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
| `CHel` / `cveh` / `cbot` / `cpln` / `ctrn` | `Entity_SerializeMountedVehicleState @ 0x460560` | vehicles + AI ground/air units | compact only — §5.13 |
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
| — | 2 | parentHandle | `spawnFlags & 0x0100` | resolved → entity+368 (pool ptr) |
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
| — | 1 | healthByte | `spawnFlags & 0x2000` | entity+538 |
| — | 2 | healthShort | `spawnFlags & 0x2000` (extra read) | entity+350 |
| — | 2 | healthShort (alt) | `(spawnFlags & 0x8000) && !(0x2000)` | entity+350 |
| — | 1 | difficultyByte | `spawnFlags & 0x4000` | entity+624 |

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

`[orig: Entity_SerializeMountedVehicleState @ 0x460560]`. Used by every item
whose entity class tag is `CHel` / `cveh` / `cbot` / `cpln` / `ctrn` (per
§5.10b dispatch table). Both write (mode 1) and read (mode 2) paths handle
format type 11 only — the callback rejects modes 3/4 (extended), so vehicles
never appear in a C2S 0x0C body. They're host-pushed inside the S2C 0x0A
trailing event-loop `tag==1` record.

The write side branches first on whether the entity has an attached parent
(`entity+40`) — if so, position is vehicle-LOCAL (via
`Entity_TransformWorldToLocal`), otherwise world-relative to the map origin
`dword_C867A4..AC`. Then on `flagsByte & 4` (mounted bit): if set, only a
small heading block follows; if clear, the full weapon/turret block follows.

| off | bytes | field (new name) | gate | write-source | read-dest |
|---|---|---|---|---|---|
| 0 | 2 | parentSlotHandle | always | `(pool<<12)\|slot` from entity+40 (`0xFFFF`=none) | resolves parent entity |
| 2 | 2 | posX compressed | always | entity+4 (vehicle-local if parent ≠ none) | entity+4 (local→world) |
| 4 | 2 | posY compressed | always | entity+8 | entity+8 |
| 6 | 2 | posZ compressed | always | entity+12 | entity+12 |
| 8 | 2 | eulerZ (i16 BAM `(v+0x8000)>>16`) | always | entity+16 | entity+576 |
| 10 | 1 | flagsByte | always | entity+36 (low byte) | entity+36 |
| 11 | 2 | eulerY (i16 BAM) | `flagsByte & 4` | entity+24 | entity+584 |
| 13 | 2 | eulerX (i16 BAM) | `flagsByte & 4` | entity+20 | entity+580 (mounted case ends here) |
| 11 | 2 | weaponX compressed | NOT `flagsByte & 4` | entity+160 | entity+160 |
| 13 | 2 | turretPitch raw i16 | NOT `flagsByte & 4` | entity+286 | entity+286 |
| 15 | 2 | weaponAimY compressed | NOT `flagsByte & 4` | vehicleData[136] | vehicleData[177] |
| 17 | 2 | weaponAimZ compressed | NOT `flagsByte & 4` | vehicleData[135] | vehicleData[178] |
| 19 | 2 | weaponHeading (i16 BAM high) | NOT `flagsByte & 4` | vehicleData[132] | vehicleData[179] |

Total: **15 B** when mounted (`flagsByte & 4`), **21 B** when not.

`vehicleData` is `*(_DWORD **)(entity + 100)` — an auxiliary state buffer
attached to mounted vehicles for weapon-aim tracking. Note the write side reads
weapon-aim from `vehicleData[136/135/132]` while the read side lands the
decompressed values into a *different* slot triple `vehicleData[177/178/179]`
(write-source ≠ read-dest — the earlier single "landing" column conflated them).

**Field labels corrected 2026-06-17 (D-NET-63).** The table above now reflects the
witnessed semantics: `eulerZ/eulerY/eulerX` are the orientation / rider Euler
triple (Z read pre-branch always; X/Y only when mounted) fed to
`Math_BuildFixedPointMatrixFromEulerAngles`, and the unmounted block is a
turret-pitch raw i16 + weapon-aim Y/Z + a weapon-heading BAM. The reimpl
`VehicleCompactRecord` (`ingame_decode.h`) uses these names. Because the original
write side has no shared trailing field, the formerly-shared `finalHeading` is
split per branch into `euler_x` (mounted) / `weapon_heading_bam` (unmounted). Wire
byte counts, read order, and sizes (15 B / 21 B) are unchanged — the rename is
label-only and the round-trip + byte-witness tests stay green.

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
`dword_8139E8[]` indexed by animState — non-zero bits 4 / 0x20 in the table
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
joiner reports a discrete weapon-fire event: origin, direction, target, body part hit,
and a 5-u16 muzzle-offset block the host applies to the shooter entity's local
coordinate frame (entity+1..6) before running `Server_ValidateAndFireRound @ 0x50BAA0`.
On success, when `fire_flags & 1 == 0` (primary fire), the host advances the shooter's
ammo-tick counter at `playerSlot+0x178D8` by `AdmDef_GetEntryByIndex(adm_index)[276]`
(reload-cooldown ticks).

| off | bytes | field | landing (host receiver) |
|---|---|---|---|
| 0 | 4 | `current_tick` (u32 LE) | tick anchor — compared against `playerSlot+0x178D8` for cooldown gate |
| 4 | 2 | `shooter_handle` (u16 LE, `pool<<12\|slot`) | pool resolve via `g_pool_list`; `0xFFFF` or `(h&0xF000)>=0x5000` rejects |
| 6 | 1 | `fire_flags` (u8) | bit 0 = "alt fire" (skips ammo decrement); → `dest[3]` |
| 7 | 1 | `adm_index` (u8) | `AdmDef_GetEntryByIndex @ 0x53FC80` key (action-descriptor — same index space as §5.9.1 weapon-hit `adm_index`) |
| 8 | 4 | `pos_x` (i32 LE, 16.16) | shooter world position at fire moment (entity+4 + map origin); → `dest[4]` |
| 12 | 4 | `pos_y` | → `dest[5]` |
| 16 | 4 | `pos_z` | → `dest[6]` |
| 20 | 4 | `dir_x` (i32 LE) | fire direction — host applies `<< 16` (`dir_x_shifted`); wire is raw i32 LE; → `dest[7]` |
| 24 | 4 | `dir_y` | same `<< 16` shift; → `dest[8]` |
| 28 | 2 | `target_handle` (u16 LE) | hit entity; `0xFFFF` = no specific target; → `dest[16]` |
| 30 | 2 | `hit_part` (u16 LE) | body-part / collision sub-section index; → `dest[17]` |
| 32 | 1 | `extra_byte1` (u8) | → `dest[18]` |
| 33 | 1 | `extra_byte2` (u8) | → `dword_C86FB4` (last-fire global) → `dest[19]` |
| 34 | 1 | `misc_byte` (u8) | → `LOBYTE(dest[20])` |
| 35 | 2 | `base_offset` (u16 LE) | applied to `shooter_entity[1]` before validate — relative muzzle x-base |
| 37 | 2 | `offset_x` (u16 LE) | applied to `shooter_entity[2]` |
| 39 | 2 | `offset_y` (u16 LE) | applied to `shooter_entity[3]` |
| 41 | 2 | `offset_z` (u16 LE) | applied to `shooter_entity[4]` |
| 43 | 2 | `offset_w` (u16 LE) | applied to `shooter_entity[5]` |

**Cross-witness against `host_and_join_game_on_opennovaworld_loopback_threeplayers_more_gameplay.pcapng`:**
- f=2057 (adm=7, fire_flags=0x02): `tick=15532061 pos=(-444.8, -413.2, 14.5) hit_part=1025`.
- f=2061 (adm=7, +7 ticks ≈ 113 ms): `tick=15532068`, hit_part increments to 1026 — a monotonic
  fire-counter / shot-sequence carried in `hit_part`.
- f=2278: weapon switch → `adm=61, fire_flags=0x32` (alt fire bit set), distinct muzzle-offset
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

**Open follow-up:** `Server_ValidateAndFireRound` (sub_50BAA0) decompile would resolve how
`current_tick` gates the ammo-cooldown check and what `dest[2]` / `dest[9]` are seeded for.
Not pursued in this round.

### 5.17 C2S 0x21 — anti-cheat CRC reply (3-player loopback 2026-06-16d)

`[orig: handle_anti_cheat_crc_check @ 0x502050]`. Sent in response to S2C 0x30 (`0x5029B0`) /
S2C 0x31 (`0x5024A0`) anti-cheat challenges. Effective wire shape is **5 B** (`u8 player_index
+ u32 expected_crc`), but every observed reply has 4 trailing zero bytes the handler never
reads — `len=9 B` is the protocol layer's framing minimum, not a payload requirement.

| off | bytes | field | landing (host receiver) |
|---|---|---|---|
| 0 | 1 | `player_index` (u8) | record index into the 276-stride `dest[]` player array; out-of-range early-returns |
| 1 | 4 | `expected_crc` (u32 LE) | client's claim for `CRC_ComputeCustomTable(dest+idx*276, 276) ^ playerCtx[89924]` |
| 5 | 4 | (trailing zeros) | observed-zero — handler does not advance the cursor past byte 5 |

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

**Witness:** the A&S probe "ON RE Probe AS dvxi5" (two human players, 699 0x40 records) has two
capture points — Rebel HQ (handle 0x1000, bms(0,0)) and JO Tent (handle 0x1001, bms(0,-40)), both
authored neutral. Rebel HQ's iconColor evolves `0x0c→0x09→0x0c→0x0a` (neutral → Red captures →
neutral → Blue captures); JO Tent stays `0x0c` all game. Per-handle histogram: 0x1000 = 310
neutral / 217 Red (0x09) / 172 Blue (0x0a), 0x1001 = 699 neutral. Six `count==3` records append a transient truck blip (handle 0x1003,
flags `0x00` ≠ 0x10 — distinguishes a blip from a zone). Decoded by `decode_capture_zone_overlay`
in `libs/novaworld/include/novaworld/ingame_decode.h`.

### 5.20 Tag 0x16 — PLAYER-LIST (controlled capture 2026-06-17)

`[orig: NapiNPClientMsg_PlayerList @ 0x42FAE0]`. All 33 records in the probe capture decode to a
2-byte trailer remainder.

```
[u8 max_players][u8 player_count (clamp 252)]
player_count × { [u8 slot_id][u16 ping LE][u16 score1 LE][u16 score2 LE][u8 flags] }   // 8 B/row
[u8 team_count]
(team_count+1) × { [u16 score1 LE][u16 score2 LE][u8 player_count][u8 alive_count] }    // 6 B/row
[u8 extra1][u8 extra2]                                                                   // trailer
```

- Row flags: `alive = flags & 1` (scoreboard `is_alive` → score_entry+52; observed 0 in the probe —
  not a per-row liveness), `team = flags >> 1`. Probe: host slot0 flags=0x02 (team1/Blue), joiner
  slot1 flags=0x04 (team2/Red).
- Team table: `team_count`=2 ⇒ **3 rows** (T0 neutral / T1 Blue / T2 Red); per-player score2 mirrors
  into T1/T2, T0 stays 0. Probe score2 accrues with A&S play (T1 0→47, T2 0→57); ping=0 (loopback).
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
`libs/novaworld/serverlog_decode.{h,cpp}`; `tests/novaworld/nw_serverlog_decode_test` witnesses the
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

Decoder: `libs/novaworld/ingame_decode.{h,cpp}` `decode_organic_spawn_batch` /
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
`libs/novaworld/wire_capture.{h,cpp}` (`decode_capture_to_messages` → `InGameMessage{frame, dir,
tag, payload}`). `libs/novaworld/replay_timeline.{h,cpp}` then assembles those messages into an
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
- **The C2S `0x0C` uplink lands in the spawn's WORLD frame.** §5.10 notes the unmounted uplink
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
one-host/one-client loopback capture (`apps/nw_pp` printers + `libs/novaworld/ingame_decode`
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
`game_event_strcnd_key` / `game_event_kind` (libs/novaworld/ingame_decode.cpp). **Wire-confirmed:**
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
`NapiNPClientMsg_HandleBatchSpawn @ 0x431870`] — Kong-misnamed "spawn": it kills every u16 slot
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

### 5.29 Tag 0x0F — world-state-load (joiner spawn + scores + waypoints; probe2, 2026-06-18)

After the mission transfer (§5.28) the host sends the joiner its spawn pose, the game flags, the
team-score table, and the waypoint / team-name lists. ~624 B.
[orig: `NapiNPClientMsg_0x00F @ 0x42E200`]:

| off | type | field | notes |
|---|---|---|---|
| 0 | i32 | sessionTick | → `dword_A82368` |
| 4 | i32 ×3 | posX/Y/Z | local-player spawn (16.16); → entity+4/8/12 when `!is_authority` |
| 16 | i16 ×3 | yaw/pitch/roll | each `<< 16` to 16.16 → entity Yaw/Pitch/Roll |
| 22 | u8 | gameFlags | bit0 → `byte_A860DC` (gated on `byte_A860EC==0`); bit1 → `A860DD`; bit2 → `A860DE`; bit3 → ceasefire |
| 23 | i32 ×128 | teamScores | **FIXED 128-entry block** — the loop fills `[outTable, data)` @ 0x42e324 (512 B; the bulk of the body) |
| 535 | u16 | waypointCount | |
| 537 | … | waypointRecords | `{ u16 slotId, u16 nameId, u8 pad }` × waypointCount — **present ONLY for a waypoint gametype** `(g_GameType & 0xFFFDFFFF) == 0x10020`; that gate is **not on the wire** (off-wire, like the §5.9 0x0A objective block), so the decoder takes the `is_waypoint_gametype` hint. **First witnessed in probe3** (Co-op `g_GameType 0x30020`): `waypointCount=4` (slots p3/6-9), `teamNameCount=0`; byte-exact once the hint is supplied (D-NET-75). TDM/A&S send count 0 |
| … | u16 | teamNameCount | |
| … | cstring × teamNameCount | teamNames | each copied 3-bytes-at-a-time into a 64-byte slot; wire advance = `strlen+1` |

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

### 5.34 Session/transport control pings — RTT 0x57/0x2C + request trio 0x68/0x43/0x39 (probe3_again, 2026-06-19)

The NAPI transport / anti-cheat keepalives — distinct from gameplay replication: each carries a single
scalar and triggers a fixed reply. They dominate the wire by volume (the RTT pair alone is **~10.7 K each**
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
| `0x68` | `[u32 start_index]` | entity-index list page cursor (observed paging 50, 100, 150…) | C2S `0x3D` (entity-index list) | `NapiNPClientMsg_0x068 @ 0x42DAA0` |
| `0x43` | `[u32 server_timestamp]` | time-sync / anti-speedhack stamp | C2S `0x08` (`[u32 server_ts][u32 GetTickCount]`) | `NapiNPClientMsg_0x043 @ 0x42FA90` |
| `0x39` | `[u32 challenge_seed]` | anti-cheat anim-map CRC seed (constant `0x3D5D` in probe3) | C2S `0x1C` (`AnimMap_GetSlotChecksum`) | `NapiNPClientMsg_HandleChecksumChallenge @ 0x42E6D0` |

**Witness:** probe3_again — `0x57`/`0x2C` ×10,679 each (every body 5 B, timestamps pair across the two
directions); `0x68`/`0x43`/`0x39` ×141 each (`0x39` seed constant `0x3D5D`, `0x43` serverTs a slow coarse
tick, `0x68` startIdx paging 50/100/150…); all full-consume via `decode_rtt_sample` / `decode_u32_scalar`.

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
entity → `WeaponSlot_ReloadAmmo(entity, reloadParam)`; a vehicle entity instead arms an 80-tick timer.
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
  restriction/avoidance probes (`[orig: sub_4142C0 @ 0x4142C0]`), the anim flag table
  `dword_8139E8` gating velocity application. This is the SAME mover the AI uses (OpenNova
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
    `NapiNPConnection.unk_18` (its ConnectionId / dcb, via `sub_4C6D40`). In `host_and_join_lan.pcapng` the
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
- **The witnessed joiner C2S in-match sequence:** `0x00`(JOIN, VERSIONCRCSTRING) → `0x01` → `0x02` (256 B) →
  `0x4E/0x03/0x48/0x47/0x33` → `0x22` → `0x37` → `0x09` → `0x0A` → `0x2F ×2 / 0x0B` (the gate-tripping
  loadout/status burst). The NW-service auth (`0x00/0x01/0x02`) is the `0x41/0x42` Hello/Auth handshake; the
  `0x37/0x09/…/0x2F/0x0B` burst is the in-match spawn-gate drive (it trips the host's
  `loadout_synced`/`mission_status_received` gate, §5.2a). The joiner sends NO C2S `0x0C` before it knows H
  — there is no pre-spawn pose uplink on the wire.
- **Unpinned (low-risk):** the exact store that writes `g_local_player_entity` on the name-match — the
  `0x42E730` handler exceeds a clean single decompile — is not byte-anchored; the mechanism is empirically
  certain from the wire (H == the named record's `slot_id`, set before any C2S `0x0C`).

**Port (libs/novaworld + godot/engine, this session; verdict MATCHING, unit-tested by `joiner_session` +
`netsim_build_player_uplink`):**
- `JoinerSession` (`libs/novaworld/src/joiner_session.cpp`) — the CLIENT MIRROR of `HostSessionAccept`:
  ClientHello/Auth handshake (the joiner's player name rides `ClientHello.co`, the free/unvalidated field
  the host echoes into the organic-spawn `entity_name`), then `pump()` drives the in-match spawn-gate burst,
  then the S2C `0x0C` handler name-matches `entity_name == player_name` → adopts `slot_id` as the wire
  handle **H**. It does NOT compose `ClientSession` (whose post-`0x82` path is the matchmaking lobby-verify
  flow, the wrong channel for the in-match game connection).
- Two-handle reconciliation: the joiner simulates its OWN local player (handle L, motor-driven per §5.38)
  and stamps **H** (the wire identity) in its C2S `0x0C` sub-header so the host's `apply_player_intent`
  (§5.38a) resolves the right peer; the wire present is self-filtered on H (render local L, not the host's
  SNAP of self). The joiner-side `NovaSimulation` mode is the next increment.
- Host side: `NovaSimulation::announce_joiner_organic_spawn` builds a 1-record `OrganicSpawnBatch
  {slot_id = the admitted handle, entity_name = the joiner's `ClientHello.co`, type 0x14B9, pose}` →
  `encode_organic_spawn_batch` → `HostSessionAccept::frame_in_match_s2c(peer, 0x0C, …)`, so the joiner can
  name-match. `HostSessionAccept` now captures `ClientHello.co` into `PeerState.player_name` and surfaces it
  on the `PeerSpawned` event.
- ctests: `tests/novaworld/joiner_session_test` (drives a real `JoinerSession` against a real
  `HostSessionAccept` in-process: handshake → name-match → InMatch with H → C2S `0x0C` uplink →
  `PeerC2SInMatch` → `NetSystem` apply SNAP; plus a wrong-name decoy that must NOT match);
  `tests/netsim/build_player_uplink_test` (the joiner-side uplink body builder).

**Port status — D.2 (joiner `NovaSimulation` mode + wire-direct present, 2026-06-23).** The joiner-side
runtime is built and green (`godot/tests/net/coop_two_sim_test` — a host listen server + a joiner in one
process, each on a real loopback `NovaUdpPump`, free-running their own `advance_frame`; asserts the joiner
reaches InMatch, the host admits it, and the two-handle present resolves both ways):
- `NovaSimulation::enable_join(host_ip, port, name)` mirrors `enable_host_listen`: dial a pump, drive a
  `JoinerSession`, feed the host's S2C `0x0A` into the SAME `NetClientView`/`ClientState` the listen server
  uses (via an identity-framed `UdpSessionTransport` conduit). The joiner runs `run_logic_tick(false)`,
  never emits S2C, never registers `NetSystem`; on the name-match it `spawn_player`s **L** at the
  H-learned pose and per-frame builds the C2S `0x0C` uplink stamped with **H**.
- **Remote entities render WIRE-DIRECT** (`wire_present_pass.gd`), the faithful client model (§5.23/§5.25):
  a non-authority client cannot resolve the host's entities through its local `MissionEntityRegistry` (the
  wire handles live in the host's handle space), so it builds one model per wire handle, keyed by the wire
  `type_id`, posed from the decoded `0x0A` position + coarse yaw. The host keeps the registry-resolved
  `MissionPresentPass` for its placed NPCs and adds the wire pass ONLY for un-placed spawned players (an
  admitted joiner has no `.bms` node) — so co-op is bidirectional on both sides. Each side excludes its own
  local player from the wire pass (drawn by `LocalPlayerHost`); the joiner keys that exclusion on **H**,
  not L (L collides with a host-side slot). The present buffer gained `PF_TYPE_ID`/`PF_WIRE_HANDLE`.
- **Refinement [D-NET-96]:** the host surfaces `PeerSpawned` REACTIVELY, from `handle_datagram` on an
  incoming SESSION packet (`host_session_accept.cpp` — not from `tick_handshakes`), so once the
  late-spawn gate opens (which takes ~tens of server ticks of world streaming) there must be an inbound
  `0x43` to surface it on. `JoinerSession::pump` therefore keeps streaming a per-frame keepalive (the
  witnessed `0x22` player-sync) after the spawn-gate burst instead of going silent — the real client never
  goes quiet mid-join. Equivalent in effect to the original server proactively pushing the organic-spawn
  when its gate opens.

**Live confirmation + open issues (2026-06-23).** The two-instance localhost demo now WORKS end-to-end
(beyond the headless `coop_two_sim_test`): a host and a joiner on one machine, and the joiner sees the
remote player rendered in-game. Launch contract (the host listens on the witnessed `32768`): host =
`NW_LAN_HOST=<m.bms>`; joiner = `NW_LAN_JOIN=<ip>:32768 NW_LAN_MISSION=<m.bms>`. **Footgun:** the joiner
must pass the mission via `NW_LAN_MISSION`, NOT `NW_LAN_HOST` — `main_game.gd` tests `NW_LAN_HOST` first and
takes the HOST path before it ever reads `NW_LAN_JOIN`, so a joiner with `NW_LAN_HOST` set silently becomes a
second host (its `32768` bind fails, it falls back to a socketless listen server, and never dials).
Two faithful-render gaps remain (the next work):
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
2. **Remote bodies glide — body animation is not carried over the wire.** The `0x0A` motion stream moves the
   model but no anim-state rides it, so remote soldiers slide in their rest pose (the "soldier-glide" gap).
3. **The joiner's local player uses the NPC motor.** On a non-authority client, L does not route through the
   `is_local_player` infantry-motor branch (§5.38) the SP host uses, so the joiner's own movement reads as
   NPC locomotion. Investigate the joiner `spawn_player` + motor gating under `run_logic_tick(false)`.

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
every live entity by the host's `NovaSimulation::resolve_item_ai_capability` post-load pass (called from
`MissionRuntime` alongside `resolve_infantry_adm_ids`). Because our emit gate is now the SAME predicate as the
decoder's own gate (`attrib & 0x100000`), an AI-capable record ALWAYS carries the `0x0800` flag + a valid
in-packet NUL-terminated name → byte-faithful (retail emits the trailer iff AI-capable) AND crash-safe. The
earlier dc90f64f stopgap (force `0x0800` on EVERY pool-1 record) is removed — no remaining divergence on the
trailer. The pool-0 (`0x0C`), pool-2 (`0x10`) and pool-3 (`0x20`) handlers are crash-safe (`0x0C` reads its
name inline/always-present; `0x10`/`0x20` have no name/AI branch). Byte-matching a specific retail mission
also wants the item-def flag gate from `serialize_entity_pool_to_packet_0 @0x503940` (the pool ROUTING split,
still by `EntityKind` here — the residual D-NET-97 simplification).

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
- **The spawn-select↔deploy VISUAL toggle is `byte_A860EC`, driven per-frame by `0x0A` flags1 bit0 — not by
  the `0x0F` handler.** `NapiNPClientMsg_0x00A` sets `byte_A860EC=1` (enter spawn-select) when flags1 bit0 = 1
  `[orig: @0x42ffa0]` and clears it to 0 the first frame flags1 bit0 = 0 `[orig: @0x43001e]`. The `0x0F`
  handler only READS `byte_A860EC`. (Corrects the prior note pinning the gate to the `0x0F` handler.) Note
  this is the camera/UI toggle; the deploy UPLINK is separately gated on `g_spawn_success_gate` above.
- **In-match S2C `0x1A` is a secondary liability.** `NapiNPClientMsg_0x01A @0x425EB0` feeds
  `NapiClient_WaitForGameStart @0x42cc10`, which re-sets `dword_81474C = 1` `[orig: @0x42cc14]`. Retail does
  not send `0x1A` post-load; our `GameSession` leaks it (`game_session.cpp:442/458/483`). Hygiene, not the
  blocker.
- **Ruled out (IDA-verified red herrings):** `0x4E FF FF` is a zero-iteration no-op kill loop
  `[orig: NapiNPClientMsg_HandleBatchSpawn @0x431891]`; the joiner's pool-0 slot index is irrelevant (self-ID
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
- **Spectator / spawn-select divert.** `cmp byte_A860EC, 0` @0x4b40f8: while `byte_A860EC` is SET, if
  `entity == g_local_player_entity` the motor calls `Camera_UpdateFreeFly @ 0x4b2980` and RETURNS @0x4b410d-
  0x4b411a — look-only, no body simulation. `byte_A860EC` is the spawn-select / spectator flag, set by S2C
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
(`[orig: Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0]`, returns the ground delta) @0x4b7cf4, when
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
- `g_view_pos {x,y,z}` ← entity `Position` (+4/+8/+12); then **`g_view_pos_z += 0x10000`** = **+1.0
  world-unit eye height** (a fixed standing-infantry bump, NOT CameraOffset) `[orig: @ 0x437e8f]`.
- `g_view_rot {yaw,pitch,roll}` ← entity `Yaw/Pitch/Roll` (+16/+20/+24, 32-bit BAM) `[orig: @ 0x437d92]`.

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

**OpenNova port (Phase 2.5).** The host first-person camera places the `Camera3D` at the player's
Godot position + 1.0u eye, oriented by the player's authoritative Yaw/Pitch
(`NovaSimulation::get_local_player_yaw_deg`/`get_local_player_pitch_deg`); **F4** swaps to a
behind+above third person; the mouse drives Yaw + Pitch (clamped ±80°). The "AI in the ground" symptom
was a two-store bug (the motor's grounded `AiEntity.pos` was mirrored to the registry `Entity` only
for the local player) — now every motor entity mirrors. **Tracked deferrals:** 3P follow
smoothing/collision (`@0x437af0`), the weapon view-bias/bone/velocity-lead/prone-drop (`@0x4dd380`),
the exact `CameraOffset@+0x6C`, and the FOV source. (The FP arms viewmodel placement is now §5.40.)

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
per-weapon `pos`/`tpos` from a weapon.def binding; the `pos`→`tpos` ADS swap (entity `Flags & 2`); the
small per-weapon `Bone.rot`; velocity lead + prone drop; the model-facing basis and the two small
lateral/forward signs are dialed by drive (the `pos[2]→down` term is the certain one).

### 5.41 `Player_*` family — naming validation + decomp cleanup grill (2026-06-26)

A full read-only grill of the **32 `Player_*` functions** (the local-player input / weapon / camera /
net-identity cluster, `0x42a550`–`0x5cf780`) plus their player-subsystem neighbors. Method: per-function
decompile / disasm / xref + struct-field witnessing, with every proposed rename adversarially re-derived
from its address by two independent skeptic passes (read-only multi-agent refutation). Verdict:
**MATCHING (read-only grill)** — the family is faithfully named after the corrections below; this is a
naming/typing grill of original engine code, not a reimpl-equivalence claim. IDB names/types/comments
were updated this session (log at the end).

**Naming corrections (the misnomers the grill caught; each UPHELD by the adversarial pass):**

| Addr | Old name | → New name | Why (witness) |
|---|---|---|---|
| `0x4c6d40` | `Player_MaybeGetLocalSessionId` | `NapiNP_GetLocalConnectionId` | returns `NapiNPConnection.connection_id` (@+0x18) — the ConnectionId/dcb, an int; return type was wrongly `NapiNPConnection*` (D-NET-100) |
| `0x4dff60` | `Player_BuildNetIdLookupOrFatalError` | `Player_FatalPlayerDcbNotFound` | `__noreturn`; loop never matches, always `MessageBoxA("Could not find player dcb…")`+crash; the "lookup" tables are dead (D-NET-102) |
| `0x4b1060` | `Player_InitLocalPlayer` | `PlayerClass_InitEntity` | sole xref = the `"plyr"` entity-class descriptor table @`0x813054`; inits the passed entity, not specifically "local" |
| `0x4a3d30` | `Player_ResetTerrainPosition` | `Camera_ResetToLocalPlayer` | `Camera_ClearViewState` + `Camera_SetTrackedEntity(local)` + cam-height/offset globals; nothing terrain |
| `0x4dc6b0` | `Player_GetCurrentWeaponAmmoCapacity` | `Player_GetClampedWeaponElevation` | reads/clamps `MountSlot.Elevation` → `WeaponDef.MaxElevation`; feeds the FOV zoom divisor; no ammo |
| `0x4dcc80` | `Player_GetVehicleAutoAimRange` | `Player_IsEquippedWeaponScoped` | returns `g_weaponScopeActive` gated on `Def->Flags&1`; not a range |
| `0x4dcd30` | `Player_IsGunnerInVehicle` | `Player_IsVehicleGunnerScoped` | returns `g_weaponScopeActive` gated on gunner seat (`Flags&2`, `Type!=7`); not a clean bool |
| `0x51cbc0` | `player_ServerAdd` | `Server_PlayerAdd` | own string `"server_PlayerAdd():"`; server subsystem |
| `0x59b280` | `sub_59B280` | `Radar_AddBlip` | bearing(atan2) + compass-edge marker + 128-slot blip array (pos/type/lifetime 62/color); `OnDamageReceived` uses it for damage direction |
| `0x541690` | `sub_541690` | `WeaponOverlay_BuildTypeLookup` | memset 0x200; iterate 780 slots; index by slot-type byte +216; action-specific overlays (state 5-9) |

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
  request (ammo, distance, ownership, weapon CRC) and queues it via `RoundData_AddRound`. Signature `()` →
  `(int fireRequest)` (the body's `teamIndex` local was a misnamed pointer to the fire-request descriptor).
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

**D-NET-117** [reimpl deferral, DOCUMENTED] **World-path pose look-pitch is not yet sourced.** The
legacy `pose_from_session` filled `HostJoinerPose.pitch` from `gss.client_pitch` — the HIGH 16 bits
of the BAM32 look-pitch (the wire/0x0C uses `ae.pitch >> 16`). The World-path `pose_for_conn` has no
faithful source: `world::Entity::pitch` is unset for a net-snapped remote peer (the C2S apply writes
`AiEntity::pitch`, not the world entity) and is the LOW 16 bits for the local player (`infantry.cpp`
narrows `look_pitch`). So the field is left 0 until the AiEntity look-pitch is threaded into the pose
builder. Impact is minimal: pitch is ~0 at spawn and `HostJoinerPose.pitch` rides only the in-process
PeerSpawned/F3 event — it never reaches the 0x0C/0x20/0x0A wire (verified: `encode_organic_spawn_batch`
has no pitch field; the per-frame 0x0A pitch comes from the AiEntity). `[orig: pose_from_session
gss.client_pitch; entity_wire_bridge ae.pitch >> 16]`

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
production driver fans to an owned-entity-less connection. `[orig: replication_min.h dvxi5 defaults]`
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
     self-decremented at `@0x42c386`, but the only three xrefs to it are this fn's read/decrement/set, so
     it is **never read as a send gate**; the `0x2C` fires **every deployed frame** (gated only by the
     deploy condition above + `!send_holdoff_countdown`). The earlier "62-tick holdoff via `dword_A860D8`"
     phrasing was an over-inference from the set-and-decrement pattern; the cooldown is vestigial in this
     function. `[orig reads/dec/set @0x42c380/0x42c388/0x42c412]`
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
correction above), gated by `send_holdoff_countdown_` (default 0 = open; the `NapiNPConnection+0x648`
field is not yet modeled — a `GetSendHoldoffTicks @0x4C4AB0` follow-up). `seed_session` sets a
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
| 0x22 | `NapiNPServerMsg_0x022 @0x514C90` | 0x46 | `NetPacket_SerializeWeaponOverlaySlotState @0x505e80` (reads `[u8 slot][u16 fieldFlags]`, `slotPtr[25146*slot]`) | `build_tag_46_player_sync` |
| 0x29 | `NapiNPServerMsg_0x029 @0x514F10` | 0x51 | `write_entity_packet @0x506bb0` (`CBufferList_GetAtIndex(g_team_change_entity_list, idx)`; gated `is_authority && !g_net_spawn_suspended && !g_spawn_success_gate`) | `build_tag_51_player_spawn` (8 B) |

**D-NET-127** [reimpl divergence, DOCUMENTED] **The reactive §5.1/spawn-confirm reply bodies are
captured-from-observation, structurally faithful but byte-divergent from the witnessed serializers.**
`game_session.cpp`'s `build_tag02_push`/`build_tag7b_session_summary`/`build_tag60_server_info`/
`build_tag64_mission_metadata` and the `replication_min` `build_tag_46`/`build_tag_51`/`build_tag_5a`
fixtures, plus the `0x0E`→`add_game_start_bundle` (`0x5A×2/0x42/0x0A/0x0F/0x4D/0x61/0x3E/0x40…/0x6F…/
0x6E/0x57/0x4E/0x58/0x5D/0x4C`), reproduce the *expected reply tags in the witnessed order* but with
captured bytes. The witnessed join burst is **leaner** — `Server_OnPlayerJoin @0x51a680` (§5.43,
D-NET-114) emits only `0x42`(`NetPacket_WriteInputStateFlags @0x505ba0`) → world-stream
(`Server_SendEntityStateToPlayer @0x517ba0`) → `0x0F`(`serialize_player_state_to_packet @0x502d10`) →
`0x4D`(player index byte) → seed (`Server_SendRandomSeedToPlayer @0x5101a0`, `0x61`) → `[0x14`
cease-fire `NetPacket_WriteTwoBytesAndCString @0x5047a0]` → `[0x1D` weapon overlay
`WeaponOverlay_SerializeToBuffer @0x505280` + game-state 11, when in-progress`]` → `0x3E`(empty
terminator). **P8 moves the reply machine to `npruntime` carrying these fixture bodies verbatim (zero
wire regression); the faithful per-body port — emitting the serializers cited above — is the deferred
grill wave** (mirrors the §5.2a serializer wave that P3–P6 left deferred). `[orig: Server_OnPlayerJoin
@0x51a680; NapiNPServerMsg_0x002 @0x512FD0; NapiNPServerMsg_0x022 @0x514C90; NapiNPServerMsg_0x029
@0x514F10]`

**D-NET-127 UPDATE (2026-06-27, D-NET Wave 2 partial).** The §5.2a serializer wave this entry cross-references is CLOSED (Wave 1 / §5.2a serializer-grill, MATCHING). For the reactive-reply bodies: the `0x7A` PCID and `0x7B` session-info bodies are now FAITHFUL ports (the invented `"DEV-A02-0001"` literal removed; `0x7A` was wrongly writing the player name instead of the PCID — golden frame 134 proves len 1 / empty). The witnessed field maps for `0x46` (`NetPacket_SerializeWeaponOverlaySlotState @0x505e80` — `[u8 type][u16 fieldFlags]` then per-bit fields: 0x1=name, 0x2=team-string, 0x4=score@slot+416, 0x8=damage/alert, 0x10=vehicle-name, 0x20=score2, 0x40=squad, 0x80=side, 0x400=weaponType, 0x800=timer-dword, 0x1000=alert2) and `0x51` (`write_entity_packet @0x506bb0` — `[u16 header][u16 handle=pool<<12|slot][u8 team][u16 NetId-if-Flags&0x100-else-0][u8 animSlot-if-Flags&0x100-else-0]`) are now landed (no longer "unwitnessed"); they carry approximations pending slot-state / entity-handle modeling (the `0x46` flag-driven body reads ~10 slot/entity fields the headless host does not yet model; tracked, not invented). **0x51 layout FIXED (2026-06-27):** `build_reply_tag_51` now emits the witnessed `[u16 requested_index (echoed from C2S 0x29)][u16 handle][u8 team][u16 NetId][u8 animSlot]` (was wrongly `[team][handle][team][0][0]`); the index is sourced from the 0x29 payload. NetId/animSlot still default 0 pending the spawned entity's net_id/anim threaded into the reply binding. **0x46 confirmed structurally FAITHFUL (2026-06-27):** `build_reply_tag_46` already emits the witnessed flag-driven format — `[u8 slot][u16 fieldFlags][u8 entitySlot]` then the bit-gated fields in source order — and **round-trips through `decode_player_sync` (@0x431370, the client inverse)**. The invented `"A-A02-000000"` literal in field 0x10 (vehicle-name) is removed (empty for an on-foot player). The remaining gap is only per-field slot-state VALUES (score/squad/side/timer) for fields the headless host doesn't model — defaults, the wire SHAPE is faithful. Remaining D-NET-127 work (all LOW/cosmetic): the `0x46` per-field slot-state values, the `0x51` NetId/anim binding plumbing, and the `0x02`-handler extra `0x00`/`0x05`/`0x04` owner attribution.

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
(`sub_62B5E0`), `StopServer @ 0x62a820`, `NapiNPProtocol_Create @ 0x625a10`,
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
`send_throttle_ms`. Stale `sub_` names identified: `sub_62FE50` = `NapiPingManager_Start`,
`sub_62FD10` = `NapiPingManager_Pump` (100 ms throttle), `sub_62FE10` =
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

## 7. Landed architecture

The design that shipped in PR #37, was reverted in PR #50, and is relanded on the
`web-nw-for-real-master` integration branch. `web/` and `apps/novaworld_server/` exist
there (not yet on master).

- **One standalone C++ server binary, three listeners**: gate UDP :7597, HTTP :8080 (the
  `/api/*` routes plus the bundled Vue web portal from `web/dist/`), NW UDP :64206. Shared
  in-memory connection registry; SQLite state at `backend/data/state.db` (schema kept abstract
  enough to swap libpq later). HTTP framework was decided as Drogon but implemented with Crow
  + standalone Asio.
- **Protocol code lives once** in Godot-free libs: `libs/novacrypto` (Layer 2 cipher),
  `libs/napi` (Layer 2-3 framing/session/TLV), `libs/novaworld` (all Layer-4 PN message sets —
  browser/session services and the in-match GameSession runtime — plus connection registry and
  db wrapper). PN dispatch happens inside `libs/novaworld`; the NovaWorld server routes
  `NOVAWORLDUDP` to lobby containers and `JointOperations`/`JOINTOPERATIONS` to the experimental
  in-match `GameServerRuntime`.
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
builder `CNapiNPConnection_SendClientJoin @ 0x61fe20` (NW-S2, §8) — confirmed
live: the client reaches `session_join`, and after NW-S2 the join carries the
identity block real NW requires for `ServerAuth`. The verify framing is still
inferred from the container set (§3). Live-smoke past AUTH and the host/join
legs are ADR 0010 Phases 3-5.

## 8. Equivalence verdicts (grill log)

Per-system verdicts from grilling the reimplementation against retail
`Jointops.exe` (Kong IDB). Each row cites the original entry point. Verdict:
**matching** | **divergent → fixed** | **divergent (accepted)** | **unknown**.

### Wave 1 — gate protocol + session envelope (2026-06-11)

| System (reimpl) | Original | Verdict | Notes |
|---|---|---|---|
| Gate response emit (`apps/novaworld_server/gate_listener.cpp::build_gate_response`) | `CNapiGateManager_ProcessResponse @ 0x4ced20` | **matching** | Emits the required POSTIPADDRESS/POSTIPPORT (see NW-G1); the wire shape (`GATEPROTOCOL "1.0"` + `VAR "k" "v"` CRLF lines) is what the retail parser consumes. |
| Gate response parse (`libs/novaworld/gate_response.cpp`) | `CNapiGateManager_ProcessResponse @ 0x4ced20`, tokenizer `String_TokenizeQuotedToArray @ 0x616d60` | **divergent → fixed (×2)** | (1) The parser was missing 6 of retail's 19 keys (LOBBYNAME, USEJUNCTION, CLEARJUNCTION, GLSVSSREQUEST, GLSVSSRIMS, GLSVSSAGRMS) and carried 2 non-retail keys (CUS, PVT). Missing keys added; CUS/PVT kept as flagged tolerant extras. The header's address citation was wrong (`0x4ad330` is `SaveFile_WriteFullState`); corrected to `0x4ced20`. (2) **2026-06-11**: the tokenizer split on whitespace only and left the quotes attached, so the real gate's quoted lines (`VAR "POSTIPADDRESS" "127.0.0.1"`) matched no key (`var_count==0`) and the Godot client rejected every real-NW reply as "bad gate response". Retail's tokenizer `String_TokenizeQuotedToArray @ 0x616d60` toggles on `"` and never copies it (quotes stripped, whitespace inside quotes kept); our `tokenize_line` now mirrors it and uses `tokens[2]` as the value (= retail's `tokenValue`). The envelope was never the issue (the symptom was the post-envelope parse, not "bad gate envelope"). Covered by a quoted-format case in `gate_response_test` + a quoted `gate_server_loopback_test` body. |
| Gate manager defaults (`§6.7` struct) | `CNapiGateManager_InitDefaults @ 0x4d1460` | **matching** | hostname `gs.novaworld.net` @ +8, port 7597 @ +72, tag `jop:cus2` @ +76 — exactly the §6.7 layout. Note: the base `CNapiGateManager_Init @ 0x633f90` defaults to `novaworld.net` @ +64 / port @ +192; the game layer's `InitDefaults` overrides it, so the effective retail gate host is `gs.novaworld.net`. |
| Session HELLO TLV (`libs/novaworld/session_hello.cpp`) | `NapiNPProtocol_HandleClientHello @ 0x6213B0` | **matching** | Flat TLV tag set confirmed: NVS, CO, AP, BDAT, PN (game id), PG (16-byte key), PV1, PV2 — plus retail-only validated/echo tags PV3/PM/CI/EIP/EPN/ET. Retail validates NVS == the Milota version string `"NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic"`, PN == server game id, PG == server key (16 B), PV1 == server build. SESSION NWU key `"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj"` @ 0x7DFC50 confirmed. |
| ClientHello identity (`libs/novaworld/client_session.cpp::build_client_hello`) — **client direction** | `HandleClientHello @ 0x6213B0` validation + `CNapiGameSession_InitNPConnection @ 0x4d3be0` constants | **divergent → fixed** | NW-S1 (**2026-06-11**): the Godot client sent `NVS="OpenNova Godot Client 0.1"` and **no PG**. Our own server's `parse_client_hello` doesn't validate, so OpenNova accepted it — but real NW's `HandleClientHello` does the LABEL_68 check: if `NVS != Milota` **or** `PN != game id` **or** the 16-byte `PG != proto+284` **or** `PV1 != proto+300`, it `return 0`s and **sends no ServerInfo/ServerHello** → the client times out in `session_hello`. The client now sends the retail-faithful identity from `InitNPConnection @ 0x4d3be0`: `NVS` = Milota, `PN` = `NOVAWORLDUDP`, `PV1` = `"0.0.0 2/10/2004 EM"`, and `PG` = the 16-byte `NOVAWORLDUDP` protocol GUID built by `sub_62E750 @ 0x62e750` from `(-655487758, 58574, 17549, 144,180,29,66,179,100,171,113)` → bytes `F2 0C EE D8 CE E4 8D 44 90 B4 1D 42 B3 64 AB 71` (`[u32 LE][u16 LE][u16 LE][8B]`). CO/AP/BDAT are read but not validated, kept as our identity. Pinned in `client_session_loopback_test`. (Reference is IDA only — `opennova-int` is server-only and never modeled the client direction.) |
| ClientAuth identity (`libs/novaworld/session_hello.cpp::client_auth_to_bytes` + `client_session.cpp::build_client_auth`) — **client direction** | `HandleClientJoin @ 0x62B750` validation + client builder `CNapiNPConnection_SendClientJoin @ 0x61fe20` | **divergent → fixed** | NW-S2 (**2026-06-11**): after the NW-S1 hello fix the client advanced `session_hello → session_join` but timed out — real NW never sent `ServerAuth(0x82)`. `HandleClientJoin @ 0x62B750` re-runs the **same** identity gate as the hello and silently `return 0`s (no ServerAuth) unless `NVS==Milota && PN==proto+220 && PG==proto+284(16 B) && PV1==proto+300`; its `is_server` branch additionally requires `HK==proto+1332` (the echo), `PV2==proto+364` (`"1"`), and a non-empty `NA`. Our `client_auth_to_bytes` emitted **only** `CI/HK/CK/NA/SIP/SPN/SCRK/CU` — the entire identity block was missing, so the gate failed. Retail's own `0x42` builder is `CNapiNPConnection_SendClientJoin @ 0x61fe20` (a Kong **misnomer** — `packet_type=66='B'`=0x42, not the hello), which emits `NVS/CO/AP/BDAT/[DE]/PN/PG/PV1/PV2/[PV3]` ahead of `CI/HK/CK/NA/[PW]/SIP/SPN/CU/SCRK/[NF/DCNT/RCNT]`, identity sourced from `CNapiGameSession_InitNPConnection @ 0x4d3be0` (`CO="NovaLogic Inc, Calabasas CA U.S.A."`, `BDAT="Jul 21 2009 18:54:41"`, `PV2="1"` @ +364). The fix emits the identity block in retail order (each tag gated on non-empty/non-zero, as retail does), reusing the same `Config` values that already pass the hello gate. `parse_client_auth` made symmetric. Pinned in `client_session_loopback_test` (identity-block assertions) + a `client_auth_to_bytes`↔`parse_client_auth` round-trip in `session_hello_roundtrip_test`. (Reference is IDA only — `opennova-int` is server-only.) Proposed IDB rename recorded: `0x61fe20 → CNapiNPConnection_SendClientJoin`. |
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
| EPASK login-form encrypt (`libs/novacrypto/src/epask.cpp`) | `sub_6669A0 @ 0x6669a0` (core) ← edit-widget vtable `+0x38` `build_form_field_query_string @ 0x657760` ← `build_url_and_submit_request @ 0x63e3f0` | **matching (byte-exact, retail)** | NW-C2: polymorphic dispatch resolved. Core = NWU-add → modexp `pow(byte+2,exp,mod)` 4-byte LE (`sub_666600 @ 0x666600`) → NWU-add → A-P low-first (`NapiNP_EncodeToHexAlpha @ 0x666570`); `exp:mod:key` split (`parse_colon_delimited_string @ 0x666710`); the NWU copy `NapiNP_EncryptBufferAlt @ 0x6668e0` is byte-identical to `0x6187b0`. Golden vectors equal the test's own ciphertext fixtures. |
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
(= modexp ×4 · A-P ×2) and calls the EPASK core `sub_6669A0 @ 0x6669a0`:

1. `NapiNP_EncryptBufferAlt @ 0x6668e0` — NWU ADD chain (key-add, reverse, progression-add,
   LCG-add; multiplier `0x5731`, reverse-flag mult `0x31`), byte-identical to `0x6187b0`.
2. `sub_666600 @ 0x666600` — per byte, `modular_exponentiation(byte + 2, exp, mod)`
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
- **Timing divergence.** Retail does **not** send `ClientConnected` on `ServerAuth`. The only
  caller is `CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400`, gated on
  `np_conn_state == 5 && session_state == 2` — i.e. only after the NP layer's `ServerSessionInit`
  has completed and `CNapiGameSession_OnNovaWorldConnected @ 0x4d1570` has run (it sets session
  state 2 at its tail). Our `ClientSession` fires it the instant it parses `ServerAuth` (state
  `Verifying`). Permissive against the OpenNova server; premature against live NW.
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
cookie capture) and the panel's credential fields. Rename proposals from this grill:
`notes/grill-nws5-ida-renames.md`.

> **SUPERSEDED by Wave 5 (below).** Wave 4's "the verify leg needs the HTTP login first" /
> "the verify `Cookie` var-list is lifted from the login cookie jar" conclusion was an
> *inference* (the verify server gate is not in `Jointops.exe`). A full packet capture of a
> **successful** retail session against the real `.204` then refuted it: login is **not** a
> verify prerequisite, and the verify `Cookie` var-list is CD-key/hardware identity (with the
> CD-key fields **empty**), not login cookies. The Phase-3 HTTP login work above is still
> correct and still needed — for the **account/GSB** leg that follows VALIDATE — just not for
> reaching VALIDATE. Same correction applies to NW-S3's "blocked on auth" verdict (§8 Wave 1).

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
out-game flow ran end to end — `GET /jop_2.gsb?a=1` (unauth on connect, 7 real servers) →
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
| C2 cookie-jar | partial | one-`Cookie:`-header-per-cookie (D-NET-30) |
| C3 login-orchestration | partial | markup-derived URLs (D-NET-31) + shared cookie fix |
| C4 gsb-parse | **matching** | format fixed + byte-verified vs genuine `.204` (D-NET-32..36) |
| C5 joi-regurl | partial | documentation only; ':' separator + HOSTKEY trim confirmed |
| D1 join-handoff | partial | JO PV1 + dial-from-NK fixed; JO PG remains open (D-NET-49) |
| D2 client-play-request | **matching** | CurrentlyPlaying + ClientVarList wrap fixed; parseable by our server (D-NET-37..39) |
| D3 host-registration | partial | text blob corrected (D-NET-40..46); host request VarList wrapping remains tracked |

The cryptographic + framing foundation re-confirmed byte-exact (NW-C1..C4, CRC32, NAPI
TLV/envelope) — no code change. Defects concentrate in GSB (C4), the join/host client-direction
builders (D1/D2/D3), 0x0A runtime replacement, and remaining ServerHello/cookie edge cases.

**Divergence catalog (D-NET-n; stable IDs, never renumbered).** Status: FIXED = applied this
session (green ctest); TRACKED = confirmed, fix specified, not yet applied.

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

`gate_response.cpp` (A2):
- **D-NET-9** [LOW, WITNESSED — deferred low-value] `NapiScript_ParseLiteralValue @0x62db00` tries, in order: char-literal `'x'` → `NapiScript_ParseHexValue @0x62d610` → `parse_octal_integer @0x62d7b0` → `parse_binary_literal @0x62d900` → `NapiScript_ParseDecimalIntegerB @0x62da20`. The reimpl parses gate port/literal fields as DECIMAL only. Real gate responses are decimal, so the other four radixes are unexercised robustness — re-prioritized MED→LOW; the 4-radix port is witnessed-and-ready but deferred (poor value/risk). [orig: NapiScript_ParseLiteralValue @ 0x62db00]
- **D-NET-10** [MED, FIXED] store full 32-bit port; drop the [0,65535] reject (retail stores verbatim, presence = non-zero). [orig: CNapiGateManager_ProcessResponse @ 0x4ced20 (@ 0x4cf1ae)]
- **D-NET-11** [MED, FIXED] IPv4 octets >255 accepted (mask to uint8), not rejected. [orig: Network_ParseIPv4AddressOctets @ 0x62dc10]
- **D-NET-12** [LOW, FIXED] IPv4 parse stops after the 4th octet, ignores trailing chars. [orig: 0x62dc10]
- **D-NET-13** [LOW, FIXED] remove CUS/PVT phantom keys (exactly 19 real keys; CUS/PVT counted-but-ignored). [orig: 0x4ced20]
- **D-NET-14** [LOW, FIXED] `is_ws` should match `isspace` (add \v 0x0B, \f 0x0C). [orig: String_TokenizeQuotedToArray @ 0x616d60]
- **D-NET-15** [LOW, FIXED] `atoi_loose` must skip leading whitespace (atol semantics). [orig: 0x4ced20 (atol @ 0x76ab0a)]

`session_hello.cpp` (A3 ClientHello/ServerHello):
- **D-NET-16** [MED, FIXED 2026-06-27] `server_hello_to_bytes` (`session_hello.cpp`) is now the witnessed FLAT builder: SF emitted UNCONDITIONALLY, P1/P2/NP/MP each only when nonzero, NO PL tag, SUS1/SUS2 gated on non-empty — the `is_game_server`/PL two-branch is removed from the encoder (the parser stays lenient so decoders still read a stray PL). Grilled vs the full decompile @0x6204b0 (CI/CO/AP/BDAT/DE/UT/PN/PG/PV1/PV2/PV3/HK/SN/SF/P1/P2/P3-P8/NP/MP/NPW/NC/RIP/RPN/SUS1-4/RIPE/EPN/ET, each individually gated). Tests: `session`/`client_session_loopback`/`golden_lan_join_session` green. [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0]
- **D-NET-17** [LOW, TRACKED] ClientHello DE/PV3/PM/ET fields unmodeled (gated off for the stock client, so byte-correct for the common case). The SERVER side of these (DE/PV3/ET) is now witnessed @0x6204b0; the ClientHello PARSER modeling them is the remaining work. [orig: NapiNPSession_SendAnnouncePacket @ 0x61fa00]
- **D-NET-18** [LOW, FIXED 2026-06-27] `server_hello_to_bytes` field order/gating now matches @0x6204b0 for the modeled field set (SF unconditional, never PL, count fields nonzero-gated, SUS non-empty-gated). Remaining nicety: UT could also be nonzero-gated (currently unconditional; cosmetic, low value).

`client_session.cpp` (A5/A7):
- **D-NET-19** [MED, FIXED] `Success` compared as exact "1"; retail uses `atol(Success) != 0`. [orig: CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800]
- **D-NET-20** [MED, TRACKED] `build_verify_request` must emit the `ClientVarList(VarList="Cookie")` parent unconditionally (retail SerializeVarList includeAll=1). [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 / NapiStatement_SerializeVarList @ 0x4d0660]
- **D-NET-21** [LOW, TRACKED] ClientConnected emitted synchronously; retail waits one periodic tick (conn_state==5 && session_state==2). [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400]
- **D-NET-22** [LOW, BINDING] the verify Cookie var-list is data-driven (locale + NW* identity) from client env — registry/Win32 glue belongs in the Godot binding, not `libs/`. Also fix the `client_session.h:99-110` comment. [orig: CNapiSession_ReadLocaleInfo @ 0x4ce390 / CNapiGameSession_SendLocaleAndVerify @ 0x4d57e0]

`napi/session.{h,cpp}` (A9):
- **D-NET-23** [MED, FIXED 2026-06-27] `SESSION_CONNECT_TIMEOUT_MS` corrected 20000→**60000** (0xEA60 — the ConnectOrHost connect/host poll, witnessed as the immediate in BOTH GetTickCount loops @0x4d4f10); the 20000ms (0x4E20) periodic-update timeout is now its own constant `SESSION_PERIODIC_UPDATE_TIMEOUT_MS`. (`libs/napi/session.h`; `napi session` test.) [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10 / ProcessPeriodicUpdate @ 0x4d4400]
- **D-NET-24** [LOW, FIXED 2026-06-27] `SESSION_HANDSHAKE_RETRANSMIT_MS` was a misnomer (a 1300-BYTE message chunk size, not a ms interval) — renamed `SESSION_MESSAGE_CHUNK_BYTES`. [orig: CNapiNPConnection_QueueMessage @ 0x628640]
- **D-NET-25** [LOW, FIXED 2026-06-27] added `Reject1009`→NWEC14; `novaworld_error_from_code` unknown-NONZERO-reject default now → `UnknownReject`→NWEC13 (was wrongly TimeoutPoll→NWEC02; NWEC02 is the code -1 poll-timeout path). (`libs/napi/session.{h,cpp}`.) [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10 dword_B60110 switch]

`novacrypto/epask.cpp` (B2, edge-case only):
- **D-NET-26** [LOW, FIXED] `epask_from_string` uses `_atoi64` semantics (return 0, no throw). [orig: parse_colon_delimited_string @ 0x666710]
- **D-NET-27** [LOW, FIXED] `epask_encrypt` truncates plaintext at first NUL (strlen). [orig: sub_6669A0 @ 0x6669a0]

`napi` tlv/envelope (B6/B7):
- **D-NET-28** [LOW, FIXED 2026-06-27] The statement-param limits are WITNESSED real at `NapiStatementParam_Create @0x632b30` (name `strlen-1 > 0x3E` ⇒ 1..63; `dataSize >= 4096` ⇒ 0..4095; reject = error flag + null). `make_client_var_list` (`libs/napi/session.cpp`) now skips a ClientVar whose name/value data exceeds 4095 (the param names are the fixed VarFNum/VarName/VarValue literals, always in [1,63]) — the faithful reject. Note: this is the gate STATEMENT layer, distinct from the in-match TLV codec `NapiNP_WriteTLV @0x61dd60`, which uses a plain u16 length (0xFFFF) with no such limit — our `libs/napi/tlv.cpp` already matches that. [orig: NapiStatementParam_Create @ 0x632b30]
- **D-NET-29** [LOW, SCOPE] envelope variable-header (first-dword==0) decode mode unsupported — documented scope decision. [orig: NapiNP_UnpackPacket @ 0x62ca20]

`http_login.cpp` + binding (C2/C3):
- **D-NET-30** [MED, TRACKED] emit one `Cookie:` header per cookie (retail `Cookie: name=value;` per entry); jar keyed by subnet-truncated host. [orig: CUIBrowser_SendHTTPRequest @ 0x658840 / Network_TruncateIPToSubnet @ 0x62dfe0]
- **D-NET-31** [LOW, DOC] login URLs/params are markup-derived (`nw_startup.mnx`), not C literals; `[CC]`/`[GT]` tokens and the `[domainname]` lower-casing are non-retail. [orig: gate STARTUPURL via 0x4ced20]

`gsb.cpp` (C4) — FIXED, **byte-verified against the genuine `.204` blob**
(`fixtures/novaworld/nw204_jop_2.gsb`, `gsb_real204_decode_test` — 8 servers, 26
FLDS columns, decoded through the XXXX terminator):
- **D-NET-32** [HIGH, FIXED] no bare "GSB " file header — "GSB " (0x20425347) is the FIRST chunk's TAG (reset/init; payload dword0==0x00010000). [orig: NapiGameList_ProcessEncryptedResponse @ 0x63d740]
- **D-NET-33** [HIGH, FIXED] chunk layout is `[magic:4 @+0][len:u32 @+4][payload @+8]`, advance len+8 — magic is a PREFIX, not the suffix we emitted. [orig: 0x63d740 (@ 0x63d78b / 0x63d76c / 0x63d781)]
- **D-NET-34** [HIGH, FIXED] tags: GSB =init, FLDS=field-names, SVRS=rows, XXXX=terminator; dropped the bogus FLDS-as-summary/TotalServers chunk. The 26 FLDS column names+order are confirmed IDENTICAL to retail. (`.204` also sends an "IVAR" chunk between GSB and FLDS, but the retail parser — and ours — ignore unknown tags, so the builder omits it harmlessly.) [orig: 0x63d740]
- **D-NET-35** [HIGH, FIXED] SVRS row = `[u32 rid][u32 port]` then 26 positional NUL-term values (FLDS-keyed) then `[u16 playerCount]` then player names. The first u32 is the host id / join `rid` — retail's "serverIP" is a misnomer; the `.204` values (e.g. 0x0A0027A0 = 167782304, in the Wave-6 join-`rid` range) are NOT IPv4, the host IP arrives via the NK join token. We previously misread it as an IP and dropped the player list. [orig: 0x63d740 (@ 0x63da60..)]
- **D-NET-36** [LOW, DISPLAY] GSB strings are Latin-1 — transcode to UTF-8 at the Godot display layer, not the parser. [orig: 0x63d740]

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

`replication_min.cpp` + `game_session.cpp` (in-match player state — §5.10):
- **D-NET-50** [HIGH, FIXED] `build_tag_0a_world_reference` shipped a 623-byte verbatim retail blob (`kRetailTag0aPayload`, only bytes 0-11 patched) on a 300 ms gameplay cadence — an ADR-0003 raw-passthrough violation. Replaced with a **field-driven builder**: it constructs a `FrameUpdate` from the host's `PlayerReplicationState` + `config_.replicated_entities` (anchor = subject world position; one tag=1 compact record per replicated entity, positions 16-bit compressed relative to the anchor via the new `network_compress_fixedpoint`, classed by `GameEntitySnapshot.entity_class`) and emits it via the new `encode_frame_update` — the exact inverse of `decode_frame_update`. Ported `network_compress_fixedpoint` (with a documented zero-guard divergence) + added `encode_frame_update` / `encode_weapon_hit_record` (ingame_encode). Validated by encode↔decode round-trips (compressor + whole-frame) and a `game_session` end-to-end assertion that the tick's 0x0A `decode_frame_update`-cleans. Guided/Unknown classes are skipped (no 0x0A compact form). Vehicle-local (mounted) compression + env/timer sub-block rotation are tracked follow-ups. [orig: NetPacket_SerializePlayerState @ 0x4C09C0 case 1 / NapiNPClientMsg_0x00A @ 0x42FEC0 event loop / Network_CompressFixedPoint @ 0x4C2780]
- **D-NET-51** [HIGH, FIXED] `handle_tag_0c_player_input` now uses the shared 5-byte entity sub-header + 43-byte extended (type-10) decoder from §5.10 instead of raw offsets. [orig: NetPacket_SerializePlayerState @ 0x4C09C0 case 4 / dispatch_entity_packet_callback @ 0x4D6A80]

`replication_min.cpp` + `game_session.cpp` (pool-entity spawn/sync — §5.11/§5.12):
- **D-NET-52** [DOC, FIXED] §5.6 trailer layout previously read `[u16][u32][cstring]`. The retail handler reads `aiProfile1` and `aiProfile2` with `cursor += 2` on a `uint16_t*` — both fields are **4 wire bytes** (the Hex-Rays render shows `uint16_t*` as the value type, but the cursor advance and the destination slot writes are `_DWORD`). Cross-witnessed against 195/437 trailer-carrying 0x0D records in the 2026-06-16b loopback. Update §5.6 + §5.11 (this commit). [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x43311e / 0x433131)]
- **D-NET-53** [HIGH, FIXED] `build_tag_0d_spawn_points` no longer emits ungated weapon-slot zeros before the always-read bone/other byte; multi-record batches decode without leftover/misalignment. [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 — weapon block gated by `spawnFlags & 0x400`)]
- **D-NET-54** [LOW, DOC] In-source field-table comments at `replication_min.cpp:417-422` (and the mirror at line 524-526) label the always-byte at +290 "bone_attach byte" and `flags&0x10` as "team". Per §5.11 the always-byte is unnamed in retail (Hex-Rays calls it `teamByte`; field is at +290), and `flags&0x10` writes `orientByte` to +354. Update the in-source comments to match §5.11. Wire-emitted bytes are unchanged by this fix — comment-only. [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x432e29 = flags&0x10 → +354; @ 0x43310a = unconditional u8 → +290)]
- **D-NET-55** [HIGH, TRACKED] No `build_tag_20_pool3_sync` builder exists; `game_session.cpp` dispatch (around lines 1023-1075) has no inbound `handle_tag_20_*` either — every S2C 0x20 falls through to `handle_unknown_or_passive_tag`. Pool-3 markers / waypoints / nav-nodes are therefore not registered into the client's pool 3, which blocks AI navigation, target markers, and any spawn-select markers that resolve via pool 3. §5.12 has the full record map; the builder needs a `[u16 start_idx][u16 count]` header + per-entity flag-driven serializer matching the witnessed 29-payload / 792-entity loopback shape. **Reframed (D-NET-84):** the host emits `0x20` ONLY from `Server_SendInitialGameStateToPlayer @ 0x51BBA0` phase 4 (per-join, paged, load-only) — there is no mid-game patrol stream, so the open work is purely the inbound runtime wiring, and the operation_whitenoise stock 29-payload load batch is the reference shape. [orig: NapiNPClientMsg_0x020 @ 0x425C00 / serialize_entity_pool_to_packet @ 0x503460]
  **RESOLVED (2026-06-27, verified):** the npruntime rework wired BOTH halves the retired game_session.cpp lacked. Builder: `encode_pool3_sync_batch` (`libs/novaworld/ingame_encode`) over `netsim::build_pool3_spawn_marker_batch`, emitted from the §5.2a world-stream phase 4 in `Server_SendInitialGameStateToPlayer` (`server_initial_state.cpp`) — exactly the D-NET-84 per-join load-only shape. Inbound: `NetClientView::apply` case `0x20` → `apply_pool3_batch` → `decode_pool3_sync_batch` registers each marker into the client `ClientState` pool (`net_client_view.cpp`). Tested end-to-end by `npruntime_initial_state_burst` (asserts the 0x20 body decodes + carries the 6002 spawn marker).
- **D-NET-56** [MED, FIXED] `decode_pool_spawn_batch` (ingame_decode.cpp) read `extra_handle_0/1` only inside `if (weapon_mask)`, under-reading by 4 B on the (`0x400` set, mask==0) path. The handler's mask==0 branch (`goto LABEL_110`) skips the per-bit loop but still consumes both extras unconditionally once `0x400` is set; moved the extras read outside the mask!=0 guard. **Latent:** retail's encoder `serialize_entity_pool_to_packet_0 @ 0x503940` only sets `0x400` when its mask (`itemDef+604`) is non-zero, so the byte-witness capture never produced mask==0 and `nw_ingame_pool_records_test` stayed green — but the client handler reads it regardless, so the port must match. Found by grilling the encode side for the Phase-1 host world-stream (trust-but-verify of already-written code). [orig: NapiNPClientMsg_0x00D @ 0x432C40 (@ 0x4330b1 LABEL_110)]
- **D-NET-57** [DOC, FIXED] §5.10 player compact record: the yaw_byte landing was cited as `entity+0x14`. Validated against the actual `NetPacket_SerializePlayerState` case 1 (write) + case 2 (read) — the read lands **yaw (byte 10) at `entity+0x10`** and **pitch (byte 11) at `entity+0x14`** (case-2 spawn branch writes `entity+0x10/0x14/0x18` = the heading/pitch/roll Euler triple). The decompiler's `pitchPacked`/`rollPacked` slot names are reused-stack artifacts, not the field semantics. **Wire layout, field widths, and yaw@10/pitch@11 ORDER are unchanged and confirmed correct** — `encode_player_compact_record` and `decode_player_compact_record` need no change; only the doc/struct landing-offset comment is corrected. This was the validation pass the player-compact encoder needed (the case-switch function exceeds a single decompile, so the case-1 write + case-2 read were extracted via Hex-Rays `py_eval`). [orig: NetPacket_SerializePlayerState @ 0x4C09C0 (case 1 write; case 2 read @ ~0x4c0730 entity+0x10/0x14/0x18 stores)]

Controlled-capture validation (probe mission "ON RE Probe AS dvxi5", dvxi5 / A&S 0x10000, host + "TestPlayer", 2026-06-17):
- **D-NET-58** [HIGH, DOC+CODE] §5.11 0x0D team/orient labels were CROSSED (inherited from D-NET-54 trusting the handler-side Hex-Rays name). The `spawnFlags&0x0010`-gated byte at **entity+354 is TEAM** (1=Blue/2=Red); the unconditional post-weapon byte at **entity+290 is a bone/other byte, NOT team**. entity+354 is the unified team landing shared with the 0x20 path (§5.12 flag 0x08). Renamed `ingame_decode.h PoolSpawnRecord.orient_byte→team_byte` (+354, gate 0x10) and `team_byte→bone_byte` (+290), with matching `ingame_encode.cpp`/`nw_pp` updates. Controlled witness: trucks authored team 1/2 → +354 = 0x01/0x02, +290 = 0x00. [orig: serialize_entity_pool_to_packet_0 @ 0x503940 (team_byte=*(entity+354); bone_byte=*(entity+290))]
- **D-NET-59** [HIGH, DOC+CODE] §5.12 0x20 `flags&0x01` field is the engine's `entry[4]` **`movement_val` @ entitySlot+16**, written RAW (no pool-resolve) — a 32-bit BAM heading for pool-3 start markers, NOT a `pool<<12\|slot` parent handle. Renamed `ingame_decode.h Pool3SyncRecord.parent_handle→movement_val`. Controlled witness: Blue starts 0x40000000 (90°), Red starts 0xc0000000 (270°), team-correlated. [orig: serialize_entity_pool_to_packet @ 0x503460 (movement_val=entry[4], written raw) / NapiNPClientMsg_0x020 @ 0x425C00]
- **D-NET-60** [LOW, DOC] §5.4 0x0B icon-key offset: "full_00" observed at off **220-226**, not the documented 246-253. Signature(0-3)/name(4-35)/designer(36-67)/basename(68) all matched their documented offsets, so only the icon row is suspect — re-diff against more retail maps or annotate as header-variant-dependent. Note: the synthesized header title-cases the basename to "Dvxi5" at +68 (client terrain lookup is case-insensitive). [orig: byte_A761D0 @ §5.5]
- **D-NET-61** [INFO, VALIDATED] The `/PROFILE` `.sph` server-log (§5.22) — the engine's own decoded per-frame view of the SAME probe session — was decoded (`libs/novaworld/serverlog_decode.{h,cpp}`, `nw_pp` `.sph` mode, `nw_serverlog_decode_test`) and cross-validated against the `.pcapng`: FooPlayer (Red, pool-0 handle 0x0005) spawn state `(70.0, 25.0, 56.306)/0xc0000000` matches **byte-for-byte** across `.sph` `PDAT`, C2S 0x0C extended uplink (§5.10), and the S2C 0x0A header `refs` triple — independently confirming the 0x0C decoder, the 16.16/-Z + 32-bit-BAM conventions, pool-0=players (the recorder iterates `g_pool_list[0]`), and team@entity+354 (re-confirms D-NET-58 via the `FEDP` roster: TestPlayer=Blue/1, FooPlayer=Red/2). No code divergence — a validation pass + new oracle tooling. Two IDB-fidelity fixes were required to read the recorder: `sub_522350`→`Game_TeardownMission` decompilation was blocked by phantom-arg prototypes on 0-arg callees (`Database_GetFieldValue` is actually `void __thiscall Database_FreeFieldEntries`; `File_Seek`/`Terrain_RenderSectorsWithWhiteFog`/`CEffectWorld_IsNameAvailable` retyped to 0 args — each 1 xref, 0 stack-arg reads). [orig: Game_ProcessMainFrame @ 0x5263f0 / CServerLog_WritePositionRecord @ 0x4e1b00 / CServerLog_WritePlayerNameRecord @ 0x4e1cc0]
- **D-NET-62** [INFO, VALIDATED] Authored-mission cross-validation of pools 1/2/3 (§5.24) — the dvxi5 probe's *known* `mission.bms`, serialized by the retail host, decoded field-for-field on the wire (the sibling of D-NET-61 for the pools the `.sph` can't see). Lands the **S2C 0x0C organic-spawn field map + decoder** (`decode_organic_spawn_batch` / `OrganicSpawnRecord`, §5.23) — byte-exact consume on the probe's 6-organic batch (4 AI `0x0816` + 2 players `0x14B9`); the shared pcap reader (`apps/common/pcap_reader`, nw_pp factored onto it); and two tests (`nw_pool_groundtruth_test` reads the real `.scratch` pcap directly; `nw_pool_decode_unit_test` inline-pcap round-trips 0x0D/0x20 through the full S2C stack). Confirms: type_id/position/team reproduce (posX/posY lossless i32 16.16; posZ re-grounds ≤1u for vehicles/AI, markers keep authored z); the heading convention **`wire_BAM = 90 - facing`** (pinned by AI authored at facing {0,90,180,270} → wire {90°,0°,270°,180°}; the 0x20 markers at facing {0,180} alone could not distinguish it from `facing+90`); and team @ **entity+354** — the onhook PoC's `+146`/`+196` reads are inside `GamePlayerEntity.pad5`, a runtime/display mirror, NOT the BMS team (same mislabel class as the PDAT `+42` STAT byte, §5.22). No divergence in the pool decoders — a new field map + validation oracle. [orig: NapiNPClientMsg_0x00C @ 0x42E730 / serialize_entity_pool_to_packet_0 @ 0x503940 / CServerLog_WritePlayerNameRecord @ 0x4e1cc0]
- **D-NET-63** [MED, DOC+CODE] §5.13 vehicle compact record field labels corrected (the rename the 2026-06-16d footnote deferred). Re-grilled the mode-2 (read) path of `Entity_SerializeMountedVehicleState @ 0x460560`: the pre-branch i16 (`yaw_high`) and the two mounted-branch i16s are the **orientation / rider Euler triple Z/Y/X** landing at **entity+576/584/580** (fed to `Math_BuildFixedPointMatrixFromEulerAngles`), and the unmounted block is **turret-pitch raw i16 (entity+286) + weapon-aim Y/Z (read-dest `vehicleData[177/178]`) + weapon-heading BAM (`vehicleData[179]`)** — distinct from the genuine weapon-X compressed u16 (entity+160). The write side has NO shared trailing field, so the reimpl's formerly-shared `final_heading` is split per branch into `euler_x` (mounted) / `weapon_heading_bam` (unmounted). Renamed `ingame_decode.h VehicleCompactRecord` (`yaw_high→euler_z`, `secondary_heading→euler_y`, `final_heading→euler_x|weapon_heading_bam`, `weapon_x_compressed→weapon_x`, `weapon_y_raw→turret_pitch_raw`, `weapon_z_compressed→weapon_aim_y`, `weapon_heading_compressed→weapon_aim_z`) with matching `ingame_encode.cpp` / `nw_pp.cpp` / `replay_timeline.cpp` / `nw_ingame_compact_records_test` / `nw_ingame_encode_test`. Also split the §5.13 table's "landing" column into write-source vs read-dest (it had conflated write `vehicleData[136]` with read-dest `vehicleData[177]`). **Wire bytes, read order, and sizes (15 B mounted / 21 B not) are unchanged** — label-only; round-trip + byte-witness tests stay green. [orig: Entity_SerializeMountedVehicleState @ 0x460560 (read path @ 0x4605a3..0x460aff; Euler matrix build @ 0x460a0f → Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40)]
- **D-NET-64** [PARTIAL, DOC+CODE] §5.15 guided weapon record upgraded from "TBD" to a documented per-(mode, field-group) matrix + structural port. `Entity_SerializeGuidedMissileState @ 0x447C50` is a `mode (packetCtx[6] ∈ {1..4}) × field-group (packetCtx[7] ∈ {1..6})` codec (write-full/read-full/write-delta/read-apply across status / clear-target / target+pos / type+pos / pos / attach-offsets), NOT a fixed compact. **Framing resolved:** `dispatch_entity_packet_callback @ 0x4D6A80` copies the 5-byte entity sub-header's `sub_op` byte into `packetCtx[7]`, so the field-group selector rides the wire as `sub_op` (1..6 for guided; 10/11 = extended/compact for the §5.10b classes), and hardwires `packetCtx[6]=4` (read-apply) on the host C2S-receive path. The serializer rejects format 11, confirming guided never legitimately appears as a 0x0A compact — `decode_frame_update`'s fail-closed on `EntityClass::Guided` is correct. Landed `GuidedRecord` + `encode_guided_field_group`/`decode_guided_field_group` (`ingame_encode.cpp`/`ingame_decode.cpp`) + `nw_ingame_guided_test` (per-(mode,group) round-trip; the write-side 1-B `0x00` status/clear marker is the dispatcher's framing, read side reads 0 B). **DEFERRED:** wiring into the 0x0C entity-packet dispatch + per-group field validation — no capture carries guided traffic (the 2026-06-16b loopback fired no rockets). Verdict partial (IDA-structural, round-trip-pinned, wire-unvalidated). [orig: Entity_SerializeGuidedMissileState @ 0x447C50 / dispatch_entity_packet_callback @ 0x4D6A80]
- **D-NET-65** [HIGH, DOC] High-bit protocol-message packets are a separate NAPI high-table control namespace, not low-table gameplay tags and not generic "unknown settings." Retail registers only `H:0x00..H:0x03` in `g_np_msginfo_highbit @ 0x849E80`: `H:0x00` is CS config update, `H:0x01` is connection name/tag update, `H:0x02` is data-transfer control, and `H:0x03` is description packet. `H:0x00` is the sparse runtime update form of the opcode-`0x82` `CS` TLVs: payload `[direction:u8][mask:u32le][u32 per set field]`, with the same 15 `NapiCSConfig` field indexes documented under §6.5. Observed masks match IDA callers: `0x2000` -> field 13 `max_packet_bytes=1300` from `NapiNPServer_HandleNewConnection`, and `0x0008` -> field 3 `send_holdoff_ticks=12` from `NapiNPServer_UpdateHoldoffTicks`. Also corrected the terminology trap: `NA=jop:cus2` is connection/game/gate tag state in the NOVAWORLDUDP path, not the player display name (`NWHANDLE`/`CHAR`). No source change in this commit; this records the finding and implementation implication. [orig: NapiNPProtocol_InitMsgInfoIndex @ 0x61E400 / NapiNPProtocol_FindMsgInfo @ 0x61E380 / CNapiNPConnection_DispatchMessage @ 0x622570 / CNapiNPConnection_HandleCSConfigUpdate @ 0x621940 / CNapiNPConnection_SendSessionInit @ 0x620EF0 / NapiNP_HandleServerJoinResponse @ 0x629840 / CNapiNPConnection_SendConfigUpdate @ 0x6286E0 / NapiNPServer_HandleNewConnection @ 0x4C8040 / NapiNPServer_UpdateHoldoffTicks @ 0x4C5F40 / NapiNPServer_GetSendHoldoffTicks @ 0x4C4AB0]
- **D-NET-66** [HIGH, FIXED] The replay timeline (§5.25) had **no death/respawn lifecycle** — an entity was one monotonically-accumulating track, so a kill followed by a respawn-elsewhere read as two consecutive samples and `interp_pos` / the viewer's `posAt` **linearly interpolated a glide** from the death spot to the spawn point (the reported "players drift when they die"). This is unfaithful: the engine never interpolates across a death — `Entity_KillBySlotId @ 0x42BCE0` sets the dead flag `Flags & 2`, and the dead→alive transition relocates the entity and calls `Entity_ResetToSpawnState @ 0x4B9610` (a SNAP). The read path gates on this exact bit: `NetPacket_SerializeInfantryEntityState @ 0x4C0320` branches on `flagsByte & 2` (the wire dead/spectator bit), and `NetPacket_SerializePlayerState @ 0x4C09C0` does `test [entity+0x24], 2` → set position directly + `Entity_ResetToSpawnState`. Modeled from BOTH wire signals: the per-record dead bit (S2C `0x0A` compact `flags & 0x02`, set while the ragdoll is broadcast and cleared at the respawn record — empirically brackets victim `0x4`: dead f=1998→2142, respawn snap f=2216) AND the kill stream (S2C `0x26`/`0x4E` — the only signal when a victim drops out of the `0x0A` set, e.g. victim `0x5`: records stop f=1934, killed f=2344, reappears at spawn f=3651). Added `ReplaySample.dead/respawn` + `mark_lifecycle` (flags the dead→alive transition `respawn`, run on the full timeline AND each projected per-participant view); `interp_pos`, the viewer `posAt`, and the trail polyline never bridge a `respawn` sample (hold at the death spot, styled dead, then teleport); `nw_pp` emits `dead`/`respawn`; the previously-missing S2C `0x4E` batch-despawn fold (`decode_batch_kill`) now emits a Kill per slot. CI: `nw_replay_timeline_test::test_death_respawn` (ragdoll path + records-stop path). Non-death disconnect/cull gaps (no kill, no flag — e.g. the `0x46` `0x8000` player-leave / `0x5D` destroy list) remain a separate despawn-channel grill. [orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320 / NetPacket_SerializePlayerState @ 0x4C09C0 / Entity_KillBySlotId @ 0x42BCE0 / Entity_ResetToSpawnState @ 0x4B9610 / NapiNPClientMsg_HandleBatchSpawn @ 0x431870]
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
- **D-NET-76** [MED, DOC+CODE] **The high-volume transport / anti-cheat control pings are now decoded — the largest hex-only hole in the §4 catalog (§5.34).** The richer 2-client session carried enough of each to field-map them. **RTT ping/pong S2C `0x57` ⇄ C2S `0x2C`** (×10,679 each — the single biggest channel by datagram count): identical 5-B `[u32 timestamp][u8 echoFlag]`; bidirectional — both peers ping, `echoFlag != 0` bounces the stamp back with the flag cleared, `echoFlag == 0` measures `rtt = GetTickCount() - timestamp` into a 10-sample ring (the server side also enforces `g_MinPing`/`g_MaxPing`, kicking >20× violators). **Periodic request trio S2C `0x68`/`0x43`/`0x39`** (×141 each, ~every 335 frames): each a single `[u32]` → fixed reply — `0x68` start_index → C2S `0x3D` entity-index list; `0x43` server_timestamp → C2S `0x08` time-sync; `0x39` challenge_seed → C2S `0x1C` anim-map CRC (seed constant `0x3D5D`). Landed `decode_rtt_sample` + `decode_u32_scalar` (`ingame_decode`), `nw_pp` printers, catalog flips (`0x57`/`0x2C`/`0x68`/`0x43`/`0x39` → Decoded; `0x08`/`0x1C`/`0x3D` reply labels), and `nw_message_coverage` checks (**30 Decoded tags**). **`0x2C` is direction-overloaded** — S2C `0x2C` (`@ 0x427E10`) is a chat-history entry, only the C2S direction is RTT. Wire-validated: RTT timestamps pair across the two directions; the trio full-consumes. [orig: NapiNPClientMsg_0x057_RTT @ 0x432210 / NapiNPServerMsg_HandlePingResponse @ 0x515070 / NapiNPClientMsg_0x068 @ 0x42DAA0 / NapiNPClientMsg_0x043 @ 0x42FA90 / NapiNPClientMsg_HandleChecksumChallenge @ 0x42E6D0]
- **D-NET-77** [MED, DOC+CODE] **S2C `0x6B` is a minimap-overlay batch, not the objective/HUD timer the census guessed (§5.35).** `[u8 count]` + `count × 12-B records`; the handler reads only the `[u16 handle]` at each record+0 (pool-resolved) and **rebuilds that entity's minimap blip from its own engine-side state** (position, type, team @ `entity+354` → icon + team color via `update_minimap_overlay_entity @ 0x5BEC10`). The 10 trailing bytes per record are not consumed by the handler — so the `1e→1d` "countdown" the census flagged is just a byte inside a per-record blob the engine ignores, not a global timer. Landed `decode_minimap_overlay_batch` + printer + catalog + coverage. probe3_again ×266 (`count=1`, blip = the active player), full-consume. [orig: NapiNPClientMsg_0x06B @ 0x425520 → update_minimap_overlay_entity @ 0x5BEC10]
- **D-NET-78** [MED, DOC+CODE] **Weapon-reload / second death path / entity-checksum + misc client scalars decoded (§5.35).** **S2C `0x49`** weapon-reload `[u16 handle][u16 reloadParam]` → `WeaponSlot_ReloadAmmo` — and the **IDB name `handle_camera_sync_packet_0x049` is WRONG** (no camera code; reloads ammo). **S2C `0x13`** is a SECOND entity-death path beside `0x26`: `[u16 handle][i16 killerSource]` acts directly on the entity (`Health=0` + death cb), where `0x26` routes through `Entity_KillBySlotId`. **S2C `0x30`** entity-checksum request `[u8 entityId][u16 checksum]` replies **C2S `0x20`** — correcting the §4 catalog row that read "→ C2S 0x21" (0x21 is the *0x31* weapon-loadout CRC reply; the census confirms the 0x30↔0x20 pairing, ×174 each). Plus the misc scalars **`0x42`** input/state-flags `[u16]`→`Input_UnpackStateFlags`, **`0x79`** spectator flag `[u8]`, **`0x2A`** chat-history `[i32][i32][i16]`. Landed `decode_weapon_reload`/`decode_entity_death`/`decode_entity_checksum_request`/`decode_input_state_flags`/`decode_spectator_flag`/`decode_chat_history_entry` + printers + catalog (→ Decoded; `C2S 0x20` reply label) + coverage (**37 Decoded tags**). probe3_again: `0x49` ×84 (`reloadParam=195` on both players), `0x13` ×18, `0x30` ×174, `0x42` ×143, `0x79` ×355, `0x2A` ×12; `nw_pp` decodes the whole capture with **zero decode failures**. [orig: handle_camera_sync_packet_0x049 @ 0x42C0A0 (misnamed) / NapiNPClientMsg_EntityDeath @ 0x42EB50 / NapiNPClientMsg_HandleChecksumRequest @ 0x431170 / NapiNPClientMsg_0x042 @ 0x4281A0 / _0x079 @ 0x429B00 / _0x02A @ 0x425BA0]
- **D-NET-79** [MED, DOC+CODE] **Deployed-item spawn 0x59 + entity-routed sub-packet 0x44 (§5.36).** **S2C `0x59`** is the deployed-item / weapon-overlay channel — a fixed 32-B record (item ids + owner + slot + parent + 3×i32 16.16 pos + 3×u16 Euler) the host streams for placeables a player drops; one record carries a friend/foe item-id pair so the same deployable shows a different model per team (owner team @ `+354` vs local player). Witnessed in probe3_again as a `Rifle-sized Crate` (`itemId=0x0362`) dropped by player slot 5; landed `decode_deployed_item_spawn` (→ Decoded, **38 Decoded tags**). **S2C `0x44`** is an entity-routed sub-packet: a 5-B sub-header `[u16][i16 netId][u8 subtype]` whose class-dependent body the dispatcher routes to the entity's per-class `def+356` callback — the same per-class path as the C2S `0x0C` uplink (§5.10b), with `subtype` playing the field-group role. Decoded the sub-header (PrinterOnly; body left raw — class-specific, same deferral as the §5.15 guided record). `nw_pp` decodes the whole capture with zero failures. [orig: Entity_SpawnOrUpdateFromSlotPacket @ 0x546770 / NetPacket_DispatchToEntityByNetId @ 0x4D6960]
- **D-NET-80** [INFO, VALIDATED] **Multi-client lifecycle cross-validated against the `/PROFILE` .sph value-oracle.** probe3_again ran 3 Blue players (TestPlayer / TestPlayer1 / FooPlayer) through a full play session with deaths and clean leaves. The new `nw_probe3again_lifecycle_test` decodes the wire and cross-checks it against the host `.sph` (`decode_server_log`): the `.sph` reports a **3-player roster + 7 DEATH + 2 DISCONNECT** (frames 7186 / 7228), and on the wire the **2 clean disconnects coincide with the 2× S2C `0x5D`** (entity destroy-list `[i16 slot]×N` → `Entity_Destroy` + `PlayerSlot_ClearAndUnlink`) — the clean-leave channel distinct from the death-driven 0x26/0x4E despawn of D-NET-66. (The `0x5D` bodies were *empty* in this capture, so the per-entity removal itself rides the `0x46` player-sync `0x8000` removal bit; `0x5D` is the paired flush.) The wire death tags (0x26 ×18 + 0x13 ×18) cover the `.sph` death count. The test also pins the high-volume transport channels (RTT `0x57`==`0x2C`==10,679; trio `0x68`/`0x43`/`0x39` ×141 each ⇄ replies `0x3D`/`0x08`/`0x1C` ×141), the weapon-heavy session (260 C2S `0x06` across **15 distinct adm indices**, both shooters `0x0006`/`0x0007`), the `0x59` deployed-item channel (×12), AND the confirmed negatives **as assertions** (every C2S `0x0C` sub_op `0x0A` → no guided; S2C `0x20`=2 load-batch only → AI static; `0x6E` teams==0 → Co-op single team). Gated on `NW_PROBE3AGAIN_PCAP` / `NW_PROBE3AGAIN_HOST_SPH`; skips clean when absent. [orig: CServerLog_WriteDeathMarker @ 0x4e1e00 / CServerLog_WriteDisconnectMarker @ 0x4e1c50 / NapiNPClientMsg_DestroyEntityList @ 0x429730]
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
- **D-NET-129** [INFO, IDB] (renumbered from D-NET-117 on 2026-06-27 — the IDB-hygiene ID collided with the §5.43 behavior entry **D-NET-117** "world-path pose look-pitch not yet sourced", which is cited in code and keeps the number) **The `CNapiNetwork_*`/`CNapiServer*` family (42 functions, 0x4a8040-0x4ca4a0) was cleaned up and its names validated against the bytes.** Changes (Jointops.exe.kong.i64):
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
    byte-exact grills. Behavioural handler spot-checks found no drift beyond the medic case
    (`ClearAnimSlot` 0x41, `SpawnEffect` 0x27 vs `HandleSpawnEffect` 0x21 all accurate + distinct).
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

**D-NET-131** [reimpl divergence, DOCUMENTED] **A "serve only" (dedicated) host is represented as a
mode-3 listen server with `serve_and_play=false`, not the original's mode-1 host-only.** Witnessed in
`[orig: UI_HandleHostSessionStart @0x556d00]`: the LAN host branch reads the `SERVERTYPE` spinlist value
`[orig: HostDialog_ReadSettings @0x555940, dword_2550AB8 = CSpinListWnd_GetSelectedValue]` and, for
`HG_SERVEONLY` (value 1, dedicated), calls `[orig: CGameSession_SetConnectionMode @0x4c49f0]` with mode
**1** → `is_host=1, is_client=0` (a genuine host-only with NO local client role); `HG_SERVEPLAY` (value 0)
→ mode **3** (host+client). Transport: LAN host → `[orig: CNapiNetwork_SetTransportMode @0x4c8750]`
mode **3**, NovaWorld host → mode **4** (both real UDP; the NovaWorld `=4` is witnessed in the dead-code
reference `[orig: Game_HostMultiplayerSession @0x4a65a0]`, not yet pinned to the live call site reached
after HTTP registration). **Our choice:** opennova keeps the single mode-3 in-process listen-server path
(ADR 0011 — the host always runs the client role) and expresses "dedicated" as `HostOwner.serve_and_play
= false` (`bringup_host_runtime`): the host's own player is not spawned and `host_session_pump` discards
the host loopback (step 5), so there is no local view. This is **wire-equivalent from a joiner's
perspective** — mode 1 vs 3 changes only whether the host maintains its OWN client bookkeeping, never the
S2C stream a peer receives (and with `serve_and_play=false` that bookkeeping is discarded anyway) — so no
byte divergence reaches a connected client. We diverge to reuse one listen-server bring-up rather than add
a second host-only path. Cited at `nova_simulation.cpp bringup_host_runtime`. Follow-up if true mode-1
fidelity is ever needed (e.g. an exact host-internal-state match): thread `ConnectionMode` from the UI
server-type and teach `HostOwner`/`host_session_pump` a no-loopback-client mode.
