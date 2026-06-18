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
| `0x41` | ClientHello | C→S | Accepts only `PN=NOVAWORLDUDP`, replies ServerHello. | `CI`, `PN`, client address. |
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

`[orig: NapiNPConnection_SendEncryptedPayload @ 0x61F5A0]` writes opcodes `0x47`/`0x87` — a
different category of encrypted packet, **not** the ProtocolMessage path.

### Message dispatcher — `[orig: NapiNPConnection_DispatchMessage @ 0x622570]`

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
the `+0x04` high-table bit. `[orig: NapiNPConnection_DispatchMessage @ 0x622570]` gets the
selector from the protocol-message flags/raw type high bit (`0x80`). If a high-table index has no
entry, retail does **not** fall through to the normal low msg_id callback.

Only four high-table entries are registered in the retail JO binary:

| High tag | Handler | Working name | Purpose |
|---|---|---|---|
| `H:0x00` | `0x621940` | `NapiNPConnection_HandleCSConfigUpdate` | Runtime connection-settings sync; sparse update form of the `CS` entries sent in opcode `0x82`. |
| `H:0x01` | `0x6219F0` | `NapiNPConnection_HandleNameTagUpdate` | Connection name/tag update, not a player display name. |
| `H:0x02` | `0x62A040` | `NapiNPConnection_ProcessDataTransferControl` | Data-transfer side channel control. |
| `H:0x03` | `0x621AE0` | `NapiNPConnection_HandleDescriptionPacket` | Connection description packet. |

`H:0x00` payload format:

```
u8  direction
u32 field_mask_le
for each set bit i in field_mask, low to high:
    u32 value_le_for_cs_field_i
```

The same 15 CS field indexes appear in opcode `0x82` SessionInit as repeated `CS` TLVs with
payload `[direction:u8][field_index:u8][value:u32le]` (`[orig:
NapiNPConnection_SendSessionInit @ 0x620EF0]`; receiver `[orig:
NapiNP_HandleServerJoinResponse @ 0x629840]`). `H:0x00` is therefore the runtime sparse-update
form of the SessionInit CS block, not an unknown gameplay message. Observed captures line up with
the IDA callers:

| Caller | Mask | Field | Meaning |
|---|---|---|---|
| `[orig: NapiNPServer_HandleNewConnection @ 0x4C8040]` | `0x00002000` | 13 | `max_packet_bytes`; observed value `1300` (`0x514`). |
| `[orig: NapiNPServer_UpdateHoldoffTicks @ 0x4C5F40]` | `0x00000008` | 3 | `send_holdoff_ticks`; observed value `12`. |

Direction is peer-relative and mirrored. On receive, `[orig:
NapiNPConnection_HandleCSConfigUpdate @ 0x621940]` applies `direction != 0` to local
`conn+0x17C` and `direction == 0` to local `conn+0x1B8`; the SessionInit receiver uses the same
pair after applying the `CS` TLVs. The send side (`[orig:
NapiNPConnection_SendConfigUpdate @ 0x6286E0]`, gated by `[orig:
NapiNPConnection_SendConfigUpdateIfEnabled @ 0x629730]`) flips the outbound direction byte so the
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
| 0x0F | 0x42E200 | `_0x00F` | **WORLD-STATE-LOAD** (no descriptive Kong name; any "game-start" label is misleading): 4×i32 (sessionId, X, Y, Z), 3×i16 fixed-point angles, u8 flags, team scores, player count, waypoint + team names; sets `dword_81474C=0` (load-bearing input/heartbeat gate); client replies with the C2S burst 0x22 0x23 0x28 0x29 0x2D 0x32; ~624 B, sometimes fragmented in retail |
| 0x10 | 0x433400 | `_0x010` | static entity batch (pool 2): u16 start_idx, u16 count, flag-driven per-entity records; 612-644 B in retail, every frame; **full field map §5.9** |
| 0x11 | 0x4226E0 | `_0x011` | one-line stub: `dword_A82358=1` (unblocks WaitForDisconnect); retail only ever ships it bundled last with 0x0B (§5.5) |
| 0x12 | 0x425EE0 | `_0x012` | |
| 0x13 | 0x42EB50 | `_0x013` | |
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
| 0x2A | 0x425BA0 | `_0x02A` | chat-history entries |
| 0x2B | 0x427DF0 | `_0x02B` | |
| 0x2C | 0x427E10 | `_0x02C` | chat entry |
| 0x2D | 0x427E90 | `_0x02D` | |
| 0x2E | 0x427F80 | `_0x02E` | |
| 0x2F | 0x430E10 | `_0x02F` | |
| 0x30 | 0x431170 | `_HandleChecksumRequest` | anti-cheat CRC challenge → client replies C2S 0x21 |
| 0x31 | 0x4311E0 | `_0x031` | CRC request for weapon loadout → C2S 0x21 |
| 0x32 | 0x428060 | `_0x032` | |
| 0x33 | 0x425FA0 | `_0x033` | |
| 0x34 | 0x4283A0 | `_0x034` | |
| 0x35 | 0x4261A0 | `_0x035` | |
| 0x36 | 0x426120 | `_0x036` | |
| 0x37 | 0x431250 | `_0x037` | |
| 0x38 | 0x4260B0 | `_0x038` | |
| 0x39 | 0x42E6D0 | `_0x039` | |
| 0x3A | 0x422680 | `_0x03A` | |
| 0x3B | 0x431340 | `_0x03B` | |
| 0x3D | 0x422870 | `_0x03D` | |
| 0x3E | 0x4226D0 | `_0x03E` | ack-style |
| 0x3F | 0x42BB20 | `_0x03F` | |
| 0x40 | 0x425A50 | `_0x040` | minimap-overlay update / capture-zone state — `[u8 count][N×6B entry]`, full map §5.19 (controlled capture 2026-06-17) |
| 0x41 | 0x4254C0 | `_0x041` | |
| 0x42 | 0x4281A0 | `_0x042` | spawn-gate-adjacent; retail emits during load |
| 0x43 | 0x42FA90 | `_0x043` | |
| 0x44 | 0x422710 | `_0x044` | |
| 0x45 | 0x422890 | `_0x045` | empty payload; sets `dword_A82370=6`; calls `PolyTrn_LoadTileData()` (terrain texture rebuild) |
| 0x46 | 0x431370 | `_0x046` | PLAYER-SYNC — full layout + field read order verified §5.21 (controlled capture 2026-06-17) |
| 0x48 | 0x4284B0 | `_0x048` | |
| 0x49 | 0x42C0A0 | `_0x049` | weapon-reload notification |
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
| 0x57 | 0x432210 | `_0x057_RTT` | RTT echo |
| 0x58 | 0x4228C0 | `_0x058` | texture loader (terrain assets) |
| 0x59 | 0x4228E0 | `_0x059` | |
| 0x5A | 0x4290E0 | `_0x05A` | weapon-loadout sync; resets `dword_81474C=0` |
| 0x5B | 0x4322B0 | `_0x05B` | |
| 0x5C | 0x425200 | `_0x05C` | |
| 0x5D | 0x429730 | `_0x05D` | entity-destroy list (cleanup) |
| 0x5E | 0x4297B0 | `_0x05E` | |
| 0x5F | 0x4228F0 | `_0x05F` | |
| 0x60 | 0x432350 | `_HandleFileTransferChunk` | u32 sessionId, u32 totalSize, u32 offset, payload; incomplete → client requests the next chunk via C2S 0x33 |
| 0x61 | 0x4297C0 | `_HandleSessionKey` | u32 session key → `g_sessionKey`; **disables `_connectlog.txt`** (source of the "DISABLING CONNECTLOG" log line) |
| 0x62 | 0x42D200 | `_0x062` | |
| 0x63 | 0x42D450 | `_0x063` | |
| 0x64 | 0x432410 | `_0x064` | mission-file chunk transfer (like 0x60); incomplete → C2S 0x37 |
| 0x65 | 0x429870 | `_0x065` | |
| 0x66 | 0x42D4C0 | `_HandleWeaponRestrictions` | count + (slot, restriction) pairs |
| 0x67 | 0x42D570 | `_0x067` | |
| 0x68 | 0x42DAA0 | `_0x068` | |
| 0x6A | 0x432510 | `_0x06A` | |
| 0x6B | 0x425520 | `_0x06B` | |
| 0x6C | 0x428FC0 | `_0x06C` | |
| 0x6D | 0x430C50 | `_HandleEntityDeath` | |
| 0x6E | 0x429880 | `_0x06E` | team/squad roster sync |
| 0x6F | 0x428D60 | `_0x06F` | cinematic camera assignment |
| 0x70 | 0x429A30 | `_0x070` | |
| 0x71 | 0x425600 | `_0x071` | |
| 0x72 | 0x425710 | `_0x072` | |
| 0x73 | 0x425770 | `_0x073` | |
| 0x74 | 0x4258B0 | `_0x074` | |
| 0x75 | 0x4259E0 | `_0x075` | spectator-mode flags (2 B); sets `byte_A860EC`, `dword_24D1DF4` |
| 0x76 | 0x42D540 | `_0x076` | u16 → `dword_24D59FC` |
| 0x78 | 0x4259070 | `_0x078` | (address as recorded in the source note has 7 hex digits — likely a typo; re-verify in the IDB) |
| 0x79 | 0x429B00 | `_0x079` | spectator-mode (1 B → `dword_82BEE4`) |
| 0x7A | 0x429B40 | `_0x07A` | player name (max 64 chars) → server-info struct |
| 0x7B | 0x429BB0 | `_0x07B` | full player info: 5×cstring(32), u32, 2×cstring(512) (MOTD) |
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
| 0x08 | 0x502210 | entity movement/state delta |
| 0x09 | 0x513200 | client checksum response |
| 0x0A | 0x513260 | |
| 0x0B | 0x51AB10 | |
| 0x0C | 0x501C30 | entity sub-packet: `[u16 handle][u16 itemTypeId][u8 sub_op][payload]` → per-type callback at `entity_def+356`; §5.9 |
| 0x0D | 0x513760 | replication frame ACK |
| 0x0E | 0x519AF0 | |
| 0x06 | 0x513310 | client-fired-round — fixed 45 B (§5.16); host validates shooter authority + ammo via Server_ValidateAndFireRound and may emit S2C 0x0A trailing weapon-hit (§5.9.1) |
| 0x0F | 0x514180 | client input frame (movement + buttons; ~33 ms cadence); len-2 form is the spawn-point query seen in the fallback spawn-menu loop |
| 0x13 | 0x514330 | |
| 0x14 | 0x501E00 | |
| 0x16 | 0x511A70 | client→server chat |
| 0x17 | 0x514850 | |
| 0x18 | 0x51A020 | |
| 0x19 | 0x514250 | |
| 0x1A | 0x514B20 | |
| 0x1B | 0x501D90 | |
| 0x1C | 0x501D40 | |
| 0x1D | 0x501C60 | |
| 0x20 | 0x501F70 | |
| 0x21 | 0x502050 | anti-cheat CRC reply (§5.17) — reads u8 player_index + u32 expected_crc; host XORs computed CRC against per-connection salt at `playerCtx+89924`, mismatch logs "ACRC" + sends "PUNT ACRC" |
| 0x22 | 0x514C90 | member of the client reply burst to S2C 0x0F / 0x4D |
| 0x23 | 0x514D50 | burst member |
| 0x24 | 0x514DC0 | |
| 0x25 | 0x514DF0 | weapon-reload request (mid-game) — note the direction asymmetry vs S2C 0x25 (§5.3) |
| 0x26 | 0x502390 | |
| 0x27 | 0x4FC980 | |
| 0x28 | 0x51A550 | burst member; also the ack reply to S2C 0x4E |
| 0x29 | 0x514F10 | burst member |
| 0x2B | 0x514FE0 | |
| 0x2C | 0x515070 | burst-member receiver |
| 0x2D | 0x502430 | burst-member receiver |
| 0x2E | 0x515390 | |
| 0x2F | 0x515790 | |
| 0x30 | 0x5029B0 | |
| 0x31 | 0x5024A0 | |
| 0x32 | 0x51A600 | burst-member receiver |
| 0x33 | 0x515230 | next-chunk request (reply to S2C 0x60) |
| 0x34 | 0x5024B0 | |
| 0x35 | 0x500DF0 | |
| 0x36 | 0x500E00 | |
| 0x37 | 0x5152E0 | mission-chunk re-request (reply to S2C 0x64) |
| 0x38 | 0x502510 | |
| 0x39 | 0x500E20 | |
| 0x3C | 0x519110 | |
| 0x3D | 0x500EC0 | |
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
| 0x4C | 0x5111B0 | |
| 0x4D | 0x518F70 | |
| 0x4E | 0x511210 | client reply when the S2C 0x05 flag byte is non-zero |
| 0x4F | 0x514A40 | |
| 0x50 | 0x5112B0 | |
| 0x51 | 0x51C840 | |

### Open questions

- Magic `0x7C08C6` in every dispatch entry — guessed a build/version stamp, but `0x7C08C6` is
  also the **address** of the global `font_name` (an empty/default C-string used widely as a
  default arg; see §5.0). Re-examine whether the field is a pointer to that string rather than a
  stamp.
- `handler2` of `NapiNPMsgInfo` — always zero in observed entries; possibly the
  `cb_server_3`/`cb_client_0` callback slots per the dispatcher decompile.
- High-table payload depth beyond `H:0x00` — registered entries and routing are witnessed
  (§4 high-table control messages), but `H:0x01..H:0x03` still have only purpose-level names.
- Opcode handlers `0x6213B0`..`0x624340` mostly lack descriptive names.
- msg_id values `0x86`/`0x87`/`0x88+` observed in the client-table tail; dispatch assignment
  unclear (possibly dead entries).
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
connection** with `[orig: NapiNPConnection_Create @ 0x62acb0]` (connection type **2**) and blocks
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
  → `[orig: NapiNPServer_SendToConn @ 0x4c4f20]` → `[orig: NapiNPConnection_QueueMessage @ 0x628640]`
  — i.e. each S2C is **built as a wire message on the connection's `msg_queue`**, never handed to the
  client handler by pointer. The receive side `[orig: NapiNPProtocol_PumpRecvQueues @ 0x6266a0]` (under
  `[orig: NapiNPProtocol_Pump @ 0x62a650]`) drains a **byte circular-buffer FIFO** (`recv_buf[55]` via
  `CCircularBuffer_Read2`), dispatches by the first opcode byte through `g_np_opcode_handlers`, then
  `[orig: NapiNPConnection_ParseMessages @ 0x625bc0]` runs the msg_id dispatch — the identical path for
  socket and in-process datagrams. `[orig: CNapiNetwork_SetTransportMode @ 0x4c8750]` opens **no
  socket** for mode 1 (`[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` is reached only for modes
  2/3/4), so mode-1 delivery must loop the serialized datagram back into that same recv FIFO in-process.
  **Consequence for the reimpl:** the faithful SP path is a literal in-process byte loopback (serialize
  real entity state → datagram → recv FIFO → decode), *not* a direct snapshot hand-off — this is what
  ADR 0011 records. **Remaining open (narrow):** (a) the exact mode-1 transmit substitution that writes
  the datagram into the local recv FIFO in place of `sendto`; (b) whether the SCRK stream cipher runs on
  that in-memory datagram — the `0x43`/`0x83` recv dispatch decrypts with SCRK, so crypto **likely** runs
  end-to-end in-process, but the transmit-side encrypt on the loopback is not yet byte-witnessed.
- `font_name @ 0x7C08C6` is the empty/default-string global that `SinglePlayer_StartMission` and
  `CreateSession` copy (via `[orig: Napi_CopyString @ 0x617e10]`) into the unused password/config
  fields. Its **address** coincides with the dispatch-table `magic` constant `0x7C08C6` (§4 open
  items, §6.1) — the "magic = build/version stamp" guess should be re-examined as a possible
  pointer to this default-string global.

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
   `[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]` (builds the `GamePlayerEntity`, stored at
   `CGameSession+4512`), sends spawn msgs `3` (weapon-restriction flag) / `5` (bool true) / `4` /
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

**IDB names applied (2026-06-16, this grill; addresses are the join key, so older prose keeps the
`dword_*` spellings):** `sub_62B5E0 → NapiNPProtocol_StartServer @ 0x62b5e0`;
`dword_24C1928 → g_spawn_success_gate`; `dword_24C1878 → g_loading_timeout_flag`;
`dword_24C187C → g_loading_cancel_flag`; `dword_A82370 → g_loading_progress`. Each carries a
one-line entry comment in the IDB. Probable-only and left as-is: `dword_24C0CA0` (local-player-ctx
guard), `dword_24D1DE0` (spawn-processing gate), `dword_A82364` (reconnect/ready flag, set by S2C
0x1A).

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
  at +36.

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
| velX / velY / velZ | i32 | `flags & 0x01 / 0x02 / 0x04` | entity+16/+20/+24 |
| sectionMask | i32 | `flags & 0x08` | entity+308 |
| team | u8 | `flags & 0x10` | entity+354 |
| parentSlot | i32 | `flags & 0x20` | entity+36 |
| ammoCount | u8 | **always** | entity+290 (u16) |
| boneA / boneB | u8 | `flags & 0x40 / 0x80` | entity+533 / +532 |
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
| flags2 | u8 | low 2 bits select sub-block; bit 3 (`0x08`) gates a 6 B vehicle-passenger record after the fixed tail (joiner-as-passenger; only the `(flags2 & 0xF) == 8` exact value triggers it — i.e. sub-block 0 + passenger bit) [orig: 0x430459] |
| sub-block 0 | `==0`: 6× u8 + u8 (`0xFF` sentinel) + i32, 11 B | player aim/state → dword_A85B5C… [orig: 0x430054..0x43012E] |
| sub-block 1 | `==1`: 4× u8 + i16, 6 B | `dword_24C1958 = 62 × i16` (62 Hz timer; `-1` if negative) [orig: 0x430191..0x430210] |
| sub-block 2 (ENV) | `==2`: u16,u16,u16,u8,u8,u8,u8,u8, 11 B | `Env_FogDistTarget=u16<<16`, `Env_FogDistAccelClamp=u16<<8`, `Env_CurTimeFixed24=u16<<13` (TOD), `Env_QuakeTicks`, `Env_CloudScrollRateTarget=u8<<10`, u8<<8, `Env_OvercastBlendTarget=u8<<8`, u8 [orig: 0x430253..0x430341] |
| sub-block 3 | `==3 && g_GameType & 0x20000`: 4× i32, 16 B (else 0 B) | dword_AC86E8… — gate is wire-invisible; receivers without the bit set skip these 16 bytes entirely [orig: 0x430361..0x4303C8] |
| state_flag_byte | u8 | bit 0→`dword_B76484`, bit 1→`dword_B76480`, bits 0/1→`g_local_player_entity.pad7[12]` bits 8/9 (the `<<8` of older notes was the receiver's internal shift, not a wire-format detail) [orig: 0x4303E5] |
| mountHandle | u16 | vehicle-mount handle (`pool<<12\|slot`; `0xFFFF`=none) [orig: 0x430408] |
| health | i16 | → `g_local_player_entity->Health` (drop triggers damage flash) [orig: 0x430428] |
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
into the replay timeline (§5.25); the unmounted case is implemented, mounted (vehicle-local) is
a tracked follow-up.

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
| 17 | 1 | health classification | `Entity_SetHealthFromDifficultyByte @ 0x4AD580` |

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
| 2 | 4 | posX (i32 LE, 16.16; vehicle-local if mounted) | smooth-target entity+0x234 (+ map origin if no vehicle) |
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
carries live projectile traffic — D-NET-64). Compact-record velocity
fields witnessed for tag 0x10 (§5.9, entity+16/20/24 with flag gating) but not exercised in
the 0x0A trailing record — confirmed class-specific (no vehicle / infantry callback writes
them; player callback's compact path doesn't either).

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
| — | 4 | velX | `spawnFlags & 0x0001` | entity+16 |
| — | 4 | velY | `spawnFlags & 0x0002` | entity+20 |
| — | 4 | velZ | `spawnFlags & 0x0004` | entity+24 |
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
| — | 1 | alertByte | `spawnFlags & 0x0040` | entity+533 |
| — | 1 | actionByte | `spawnFlags & 0x0080` | entity+532 |
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
| 19 | u8 | alertLevel | entity+533 (0x215) | hasBody |
| 20 | u8 | subType | entity+532 (0x214) | hasBody |
| 21 | u8 | weaponType | entity+343 (0x157) | hasBody |
| 22 | u8 | parentSlot | entity+360 (0x168) | hasBody |
| 23 | u16 | parentHandle `(pool<<12)\|slot` | resolved → entity+364 (0x16C) | hasBody `[@ 0x42ea6e]` |

Decoder: `libs/novaworld/ingame_decode.{h,cpp}` `decode_organic_spawn_batch` /
`OrganicSpawnRecord`. **Both spawn paths land team at entity+354** (the unified team landing,
D-NET-58); 0x0C orientation at entity+16 is the same 32-bit BAM as 0x20 `movement_val` (§5.12).

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
`nw_pp <cap> --replay-json <out>` emits a self-contained JSON the standalone
`tools/net/replay_viewer.html` (top-down canvas, timeline scrubber) loads directly. Walking `0x0A`
needs items.def (the per-record width is class-dependent, §5.10b) — pass `--items`.

Two facts worth recording, both observed on the dvxi5 probe + a 3-player capture:

- **Multi-entity motion is decoded straight from the wire (no `.sph`, no hook).** Folding the
  `0x0A` compact records via the now-solved decompression + header anchor (§5.10) turns the replay
  from "static layout + the one uplink player" into every nearby entity moving: the dvxi5 probe
  yields 8 dynamic entities (2 vehicles, 4 AI, 2 players) with 663–816 per-frame samples each, and
  the 3-player capture yields 54 moving entities (51 vehicles + 3 players). Validation is
  **wire-only** (per the project rule that the viewer / reimplementation never depend on the `.sph`
  profiling recording): every dvxi5 entity's first `0x0A` sample lands on its `0x0D`/`0x0C` spawn
  position exactly (Δ = 0.00000 m). (Mounted/vehicle-local records are skipped pending the parent
  transform; markers carry no `0x0A` motion.)
- **The C2S `0x0C` uplink lands in the spawn's WORLD frame.** §5.10 notes the unmounted uplink
  position is "world + map_origin"; on the dvxi5 capture the first uplink sample equals the
  player's `0x0C` spawn position exactly (Δx = Δy = 0), i.e. the map-origin contribution is zero
  (or pre-folded) here. The exporter emits raw values and tags each sample's source; the viewer's
  "anchor tracks to spawn" toggle reconciles the two frames for display and is a no-op when Δ = 0.

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
  The env stream is the `0x0A` case-2 snapshots over time. `nw_pp --replay-json` emits `events[]`
  + `env[]`; `tools/net/replay_viewer.html` renders a time-synced kill feed / event log (with a
  `snd:` filter), projectile tracers (fire rays) + hit bursts, live capture-zone rings, an
  environment readout, and a click-to-inspect entity panel.

**Wire-confirmed** on the one-host/one-client capture (957 datagrams): 151 events — 19 fire
(pool0/s5, `adm 24`), 125 hit (→pool0/s3, weapon pool0/s4, `adm 18`, positions decompressed), 1
kill (s5 ✖ s4, `STRCND04` — matching the `0x1E` byte-witness above), 2 game-events, 4 capture-zone
state changes (deduped from 336 `0x40` syncs) — plus 98 env snapshots. CI: `nw_replay_timeline`
gains `test_event_stream`, which crafts an inline `0x1E` kill + C2S `0x06` fire + `0x0A` env+hit
through the shared pipeline and asserts the assembled events (kill source/target/`STRCND04`, fire
origin/dir/adm, hit world pos = decompress + anchor) and the env snapshot.

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

### 6.2 `CNapiNetwork` (~4524 B; methods 0x4a8040-0x4ca4a0; 32/33 typed)

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | `list_heads[5]` | 80 | five linked-list head sentinels |
| 80 | `transport_mode` | 4 | 0=down, 1=host, 2=client_relay, 3=client_direct (per method dispatch) |
| 84 | `socket_state` | 4 | state machine 0..4 |
| 88 | `field_58` | 4 | gates OpenTransportSocket |
| 92-100 | `connection_mode` / `is_authority` / `is_mp_session_peer` | 12 | the host/client config sub-struct, resolved: written by `[orig: CGameSession_SetConnectionMode @ 0x4c49f0]` from the connection mode (§5.0, §6.3) |
| 104 | pad | 3372 | unaccounted NAPI internals (queues / logging / per-session state) |
| 3476 | `disconnect_event_buf` | 184 | `NapiNPDisconnectEvent` buffer; cleared by Shutdown |
| 3660-3668 | `field_E4C/E50/E54` | 12 | |
| 3672 | `np_manager` | 4 | `NapiNPManager *` |
| 3676 | `np_protocol` | 4 | `NapiNPProtocol *` |
| 3680 | `field_E60` | 4 | |
| 3684 | `ping_manager` | 4 | `NapiPingManager *` |
| 3688 | `game_settings` | 216 | inline `NapiGameSettings` (§6.4) |
| 3904 | `server_info_buf` | 72 | |
| 3976 | `net_config` | 520 | inline `NapiNetConfig` |
| 4496 | `field_1190` | 4 | |
| 4500 | pad | 20 | |
| 4520 | `field_11A8` | 4 | |

Key witnesses: `[orig: CNapiNetwork_Init @ 0x4ca4a0]` (all list heads + manager pointers +
settings init; also configures the ping manager to 3000/2/10, §6.6),
`[orig: CNapiNetwork_OpenTransportSocket @ 0x4c6a40]` (transport-mode dispatch),
`[orig: CNapiNetwork_CheckPlayerTimeouts @ 0x4c8ad0]` (walks `np_protocol`'s connection list),
`[orig: CNapiNetwork_Shutdown @ 0x4ca440]`. Open: one method missed in the typing batch
(probably `CNapiNetwork_GetConnectionParams @ 0x4a8040`); the 3372-byte interior gap.

### 6.3 `NapiNPServerCtx` — `g_napi_np_ctx @ 0xB5CBC8` (4520 B, 473 xrefs)

The game-level singleton is a **`CNapiNetwork`-shaped header** plus game-specific trailing
fields: field offsets line up byte-for-byte (CNapiNetwork is 4524 B, the singleton 4520 B; the
difference is one trailing pointer), and every call site passes `&g_napi_np_ctx` cast to
`CNapiNetwork *`. Applied (conservative) layout:

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
| 0x1190 | `field_1190` | 4 | NAPI-internal only |
| 0x1194 | pad | 4 | |
| 0x1198 | `send_mask` | 4 | bitmask used by `NapiNPServer_SendFiltered` (preserved) |
| 0x119C | `send_target_player` | 4 | preserved |
| 0x11A0 | `send_target_state` | 4 | preserved |
| 0x11A4 | `send_filter_416` | 4 | preserved |

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
NapiNPConnection_Create @ 0x62ACB0]` copies those protocol templates into each connection with
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
  db wrapper). PN dispatch happens inside `libs/novaworld`; the NovaWorld server rejects
  non-`NOVAWORLDUDP` PN, leaving in-match traffic to a (deferred) per-match game server.
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
builder `NapiNPConnection_SendClientHello @ 0x61fe20` (NW-S2, §8) — confirmed
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
| ClientAuth identity (`libs/novaworld/session_hello.cpp::client_auth_to_bytes` + `client_session.cpp::build_client_auth`) — **client direction** | `HandleClientJoin @ 0x62B750` validation + client builder `NapiNPConnection_SendClientHello @ 0x61fe20` | **divergent → fixed** | NW-S2 (**2026-06-11**): after the NW-S1 hello fix the client advanced `session_hello → session_join` but timed out — real NW never sent `ServerAuth(0x82)`. `HandleClientJoin @ 0x62B750` re-runs the **same** identity gate as the hello and silently `return 0`s (no ServerAuth) unless `NVS==Milota && PN==proto+220 && PG==proto+284(16 B) && PV1==proto+300`; its `is_server` branch additionally requires `HK==proto+1332` (the echo), `PV2==proto+364` (`"1"`), and a non-empty `NA`. Our `client_auth_to_bytes` emitted **only** `CI/HK/CK/NA/SIP/SPN/SCRK/CU` — the entire identity block was missing, so the gate failed. Retail's own `0x42` builder is `NapiNPConnection_SendClientHello @ 0x61fe20` (a Kong **misnomer** — `packet_type=66='B'`=0x42, not the hello), which emits `NVS/CO/AP/BDAT/[DE]/PN/PG/PV1/PV2/[PV3]` ahead of `CI/HK/CK/NA/[PW]/SIP/SPN/CU/SCRK/[NF/DCNT/RCNT]`, identity sourced from `CNapiGameSession_InitNPConnection @ 0x4d3be0` (`CO="NovaLogic Inc, Calabasas CA U.S.A."`, `BDAT="Jul 21 2009 18:54:41"`, `PV2="1"` @ +364). The fix emits the identity block in retail order (each tag gated on non-empty/non-zero, as retail does), reusing the same `Config` values that already pass the hello gate. `parse_client_auth` made symmetric. Pinned in `client_session_loopback_test` (identity-block assertions) + a `client_auth_to_bytes`↔`parse_client_auth` round-trip in `session_hello_roundtrip_test`. (Reference is IDA only — `opennova-int` is server-only.) Proposed IDB rename recorded: `0x61fe20 → NapiNPConnection_SendClientJoin`. |
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
  `NapiNPConnection_OnStateChange @ 0x626060` (state 1) calls `cb_server_1` (`proto+724`) and, if
  it returns ≥ 0, `NapiNPConnection_SendSessionInit @ 0x620ef0`. Earlier, `HandleClientJoin`
  itself runs `cb_server_0` (`proto+720`); on `< 0` it destroys the connection and stores the
  reply tag **`NP.C:PCCR:ILC`** ("Illegal Login Callback"). These two callbacks are the
  account-auth gate and live in the NW **server** binary — not witnessable here.
- The retail client carries the auth/session context the server expects:
  `CNapiGameSession_ConnectToNovaWorld @ 0x4d4640` builds the connection's CU var list via
  `CNapiVarList_SetOrCreate` — **`Application`, `BuildDateAndTime`, `Debug`, `CountryName`,
  `Language`, `TimeZoneBias`, `GateTag`, `MetTag`, `UdpCode1`, `UdpCode2`, `MaxPacketSize`** —
  which `NapiNPConnection_SendClientHello @ 0x61fe20` emits as `CU` chunks in the 0x42. `MetTag`
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

**The auth chain, end to end (de-risk grill, 2026-06-11):**
1. `UDPCODE1`/`UDPCODE2` — the session-auth codes the 0x42 join carries as `UdpCode1`/`UdpCode2`
   CU chunks — are **gate-response VAR keys**, parsed in `ProcessResponse @ 0x4ced20`
   (`UDPCODE1 → CMissionInfo_SetGateTag`, `UDPCODE2 → CMissionInfo_SetMetTag`). Our
   `gate_response.cpp` already parses both. So they come from the **gate**, not a separate endpoint.
2. The gate issues them only to an **authenticated** request. Authentication is a **web-form login**
   through the in-game browser (`CUIBrowser` @ `dword_2551100`): `load_persistent_login_credentials
   @ 0x557330` fills the `NAME` (username) + password fields; persisted creds live in
   `PERSISTENTREMEMBERLOGINDATA`, decrypted by `NapiNP_DecodeEncryptedKeyValue @ 0x619470` with key
   `"SLHALI289SZ79210987ZS:OCV789YHK2QJ3HSKDJHVS978THYG23"`. (EPASK crypto = NW-C2.)
3. So **live-NW Phase 3 = web login → gate issues `UDPCODE1/2` → emit them (+ env vars) as CU chunks
   in the 0x42.** Our gate parser already captures `UDPCODE1/2`; the missing pieces are (a) the web
   login that makes the gate issue them and (b) attaching the CU-chunk set to `ClientAuth`.

**Scope note — product vs parity.** This entire auth chain is required only to impersonate a retail
client against **live NovaLogic NW** (parity testing). The OpenNova **product** path is OpenNova
client ↔ the OpenNova server (`apps/novaworld_server`), whose `cb_server_0`/`cb_server_1` are
permissive — there the full handshake already reaches `Verified` (`client_session_loopback_test`).
The `session_join` timeout is therefore expected against live NW and is **not** a blocker for the
OpenNova-server path; it gates only the retail-impersonation parity scenario.

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
  `NapiNPConnection_QueueMessage @ 0x628640`. Our `client_session.cpp` `on_server_auth` emits the
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
2. **0x82 is `ServerSessionInit`, not "ServerAuth".** `NapiNPConnection_SendSessionInit @
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
| A2 gate-response | partial | literal parser still tracked (D-NET-9); leniency fixes landed (D-NET-10..15) |
| A3 clienthello | partial | ServerHello two-branch model + PL fiction (D-NET-16..18) |
| A4 clientauth | partial | CS-table/gating/JFC rejection fields fixed; remaining issues are outside A4 |
| A5 client-session-fsm | partial | Success atol, Cookie parent, ClientConnected timing (D-NET-19..22) |
| A6 protocol-message | partial | 0x80 selector + LEN8/16 fixed; frag reset/truncated stream remain (D-NET-7/8) |
| A7 verify-containers | partial | Success atol (= D-NET-19); verify-cookie claim refuted |
| A8 web-domain | **matching** | SessionInit CU web-domain install confirmed |
| A9 session-timing | partial | timeout value/citation, NWEC13 default (D-NET-23..25) |
| B1 nwu | **matching** | byte-exact (NW-C1) |
| B2 epask | **matching** | byte-exact (NW-C2); edge cases fixed (D-NET-26/27) |
| B3 pubcrypto | **matching** | byte-exact (NW-C3) |
| B4 url-cipher | **matching** | byte-exact (NW-C4) |
| B5 crc32-table | **matching** | table identical @0x849938, check 0x0376E6E7 |
| B6 napi-tlv | **matching** | builder-layer length guard only (D-NET-28) |
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
- **D-NET-1** [HIGH, FIXED] CS field default tables were onnet guesses, wrong at idx 4/8/9/10/12/13. Engine template (IDENTICAL both directions): `{0:240000,1:4,4:60000,5:1000,6:0xFFFFFFFF,8:2048,9:128,10:100,11:500,12:1,13:MTU(1300),14:0xFFFFFFFF}`. [orig: CNapiGameSession_InitNPConnection @ 0x4d3e1f / NapiNPConnection_Create @ 0x62acb0 / NapiNPConnection_SendSessionInit @ 0x620ef0]
- **D-NET-2** [LOW, FIXED] CI/HK/CK were emitted unconditionally; retail gates each on non-zero (like SIP/SPN). [orig: NapiNPConnection_SendClientHello @ 0x61fe20]
- **D-NET-3** [LOW, FIXED] `parse_server_auth` now parses JFC/JFP/JFS rejected-join fields (failure code/param/string); SCRK-less rejections are valid auth packets and surface as rejections, not malformed. [orig: NapiNP_HandleServerJoinResponse @ 0x629840]
- **D-NET-4** [LOW, FIXED] RIP/RPN were emitted unconditionally; retail gates on peer_addr/peer_port != 0. [orig: NapiNPConnection_SendSessionInit @ 0x620ef0]

`protocol_message.cpp` (A6):
- **D-NET-5** [HIGH, FIXED] high-table/include-seq selector is flag bit **0x80**, not 0x01; `full_tag = (flags&0x80?0x100:0)|tag`; wire bit 0x01 is unused/reserved. [orig: NapiNPConnection_DispatchMessage @ 0x622570 / NapiNPProtocol_FindMsgInfo @ 0x61e380 / NapiNP_WriteMessageRecord @ 0x61da90]
- **D-NET-6** [HIGH, FIXED] LEN8(0x20)/LEN16(0x40) parse precedence inverted — retail tests LEN8 FIRST. [orig: NapiNPConnection_ParseMessages @ 0x625bc0]
- **D-NET-7** [MED, TRACKED] reassembly must clear the buffer on the FIRST fragment (`(flags&6)==4`) before appending. [orig: NapiNPConnection_DispatchMessage @ 0x622570 / NapiBuffer_SetLength @ 0x634220]
- **D-NET-8** [LOW, TRACKED] truncated inner stream: retail substitutes 0 for missing fields and still dispatches; we bail. [orig: NapiNPConnection_ParseMessages @ 0x625bc0]

`gate_response.cpp` (A2):
- **D-NET-9** [MED, TRACKED] port fields need full literal parse (char/hex/octal/binary/decimal, in order). [orig: NapiScript_ParseLiteralValue @ 0x62db00]
- **D-NET-10** [MED, FIXED] store full 32-bit port; drop the [0,65535] reject (retail stores verbatim, presence = non-zero). [orig: CNapiGateManager_ProcessResponse @ 0x4ced20 (@ 0x4cf1ae)]
- **D-NET-11** [MED, FIXED] IPv4 octets >255 accepted (mask to uint8), not rejected. [orig: Network_ParseIPv4AddressOctets @ 0x62dc10]
- **D-NET-12** [LOW, FIXED] IPv4 parse stops after the 4th octet, ignores trailing chars. [orig: 0x62dc10]
- **D-NET-13** [LOW, FIXED] remove CUS/PVT phantom keys (exactly 19 real keys; CUS/PVT counted-but-ignored). [orig: 0x4ced20]
- **D-NET-14** [LOW, FIXED] `is_ws` should match `isspace` (add \v 0x0B, \f 0x0C). [orig: String_TokenizeQuotedToArray @ 0x616d60]
- **D-NET-15** [LOW, FIXED] `atoi_loose` must skip leading whitespace (atol semantics). [orig: 0x4ced20 (atol @ 0x76ab0a)]

`session_hello.cpp` (A3 ClientHello/ServerHello):
- **D-NET-16** [MED, TRACKED] drop the is_game_server two-branch ServerHello model; emit SF UNCONDITIONALLY (0/1 flag); remove the fabricated PL tag; gate P1/P2/NP/MP on nonzero. [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0]
- **D-NET-17** [LOW, TRACKED] ClientHello DE/PV3/PM/ET fields unmodeled (gated off for the stock client, so byte-correct for the common case). [orig: NapiNPSession_SendAnnouncePacket @ 0x61fa00]
- **D-NET-18** [LOW, TRACKED] `server_hello_to_bytes` order: gate UT on nonzero, SF unconditional, never PL. [orig: 0x6204b0]

`client_session.cpp` (A5/A7):
- **D-NET-19** [MED, FIXED] `Success` compared as exact "1"; retail uses `atol(Success) != 0`. [orig: CNapiGameSession_HandleConnectVerifyResponse @ 0x4d5800]
- **D-NET-20** [MED, TRACKED] `build_verify_request` must emit the `ClientVarList(VarList="Cookie")` parent unconditionally (retail SerializeVarList includeAll=1). [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 / NapiStatement_SerializeVarList @ 0x4d0660]
- **D-NET-21** [LOW, TRACKED] ClientConnected emitted synchronously; retail waits one periodic tick (conn_state==5 && session_state==2). [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400]
- **D-NET-22** [LOW, BINDING] the verify Cookie var-list is data-driven (locale + NW* identity) from client env — registry/Win32 glue belongs in the Godot binding, not `libs/`. Also fix the `client_session.h:99-110` comment. [orig: CNapiSession_ReadLocaleInfo @ 0x4ce390 / CNapiGameSession_SendLocaleAndVerify @ 0x4d57e0]

`napi/session.{h,cpp}` (A9):
- **D-NET-23** [MED, TRACKED] `SESSION_CONNECT_TIMEOUT_MS` mis-cited: ConnectOrHost poll is 60000ms (0xEA60); 20000ms (0x4E20) is the periodic-update background timeout. [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10 / ProcessPeriodicUpdate @ 0x4d4400]
- **D-NET-24** [LOW, TRACKED] `SESSION_HANDSHAKE_RETRANSMIT_MS` is actually a 1300-byte message chunk size, not a ms interval — rename. [orig: NapiNPConnection_QueueMessage @ 0x628640]
- **D-NET-25** [LOW, TRACKED] add Reject1009→NWEC14; unknown-reject default → NWEC13 (currently NWEC02). [orig: CNapiGameSession_ConnectOrHost @ 0x4d4f10]

`novacrypto/epask.cpp` (B2, edge-case only):
- **D-NET-26** [LOW, FIXED] `epask_from_string` uses `_atoi64` semantics (return 0, no throw). [orig: parse_colon_delimited_string @ 0x666710]
- **D-NET-27** [LOW, FIXED] `epask_encrypt` truncates plaintext at first NUL (strlen). [orig: sub_6669A0 @ 0x6669a0]

`napi` tlv/envelope (B6/B7):
- **D-NET-28** [LOW, TRACKED] enforce name length [1,63] and data length [0,4095] at the novaworld builder layer (not the TLV codec). [orig: NapiStatementParam_Create @ 0x632b30]
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
- **D-NET-55** [HIGH, TRACKED] No `build_tag_20_pool3_sync` builder exists; `game_session.cpp` dispatch (around lines 1023-1075) has no inbound `handle_tag_20_*` either — every S2C 0x20 falls through to `handle_unknown_or_passive_tag`. Pool-3 markers / waypoints / nav-nodes are therefore not registered into the client's pool 3, which blocks AI navigation, target markers, and any spawn-select markers that resolve via pool 3. §5.12 has the full record map; the builder needs a `[u16 start_idx][u16 count]` header + per-entity flag-driven serializer matching the witnessed 29-payload / 792-entity loopback shape. [orig: NapiNPClientMsg_0x020 @ 0x425C00]
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
- **D-NET-65** [HIGH, DOC] High-bit protocol-message packets are a separate NAPI high-table control namespace, not low-table gameplay tags and not generic "unknown settings." Retail registers only `H:0x00..H:0x03` in `g_np_msginfo_highbit @ 0x849E80`: `H:0x00` is CS config update, `H:0x01` is connection name/tag update, `H:0x02` is data-transfer control, and `H:0x03` is description packet. `H:0x00` is the sparse runtime update form of the opcode-`0x82` `CS` TLVs: payload `[direction:u8][mask:u32le][u32 per set field]`, with the same 15 `NapiCSConfig` field indexes documented under §6.5. Observed masks match IDA callers: `0x2000` -> field 13 `max_packet_bytes=1300` from `NapiNPServer_HandleNewConnection`, and `0x0008` -> field 3 `send_holdoff_ticks=12` from `NapiNPServer_UpdateHoldoffTicks`. Also corrected the terminology trap: `NA=jop:cus2` is connection/game/gate tag state in the NOVAWORLDUDP path, not the player display name (`NWHANDLE`/`CHAR`). No source change in this commit; this records the finding and implementation implication. [orig: NapiNPProtocol_InitMsgInfoIndex @ 0x61E400 / NapiNPProtocol_FindMsgInfo @ 0x61E380 / NapiNPConnection_DispatchMessage @ 0x622570 / NapiNPConnection_HandleCSConfigUpdate @ 0x621940 / NapiNPConnection_SendSessionInit @ 0x620EF0 / NapiNP_HandleServerJoinResponse @ 0x629840 / NapiNPConnection_SendConfigUpdate @ 0x6286E0 / NapiNPServer_HandleNewConnection @ 0x4C8040 / NapiNPServer_UpdateHoldoffTicks @ 0x4C5F40 / NapiNPServer_GetSendHoldoffTicks @ 0x4C4AB0]

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
pool-3 0x20 runtime wiring (D-NET-55, the sibling of the now-FIXED D-NET-50 — its
`encode_pool3_sync_batch` is written but unwired into the tick loop); and giving
`make_client_host_request` the same `ClientVarList` wrapping `make_client_play_request`
now has (D-NET-38). Each carries its `[orig]` anchor and corrected behavior above.
