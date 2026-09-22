# Retail message dispatch audit (2026-09-09)

> **Dated snapshot.** The [network RE record](novaworld-net-re.md) and
> [divergence ledger](../divergence-ledger.md) own subsequent D-NET-218 work.

The retail dispatch tables contain **193 handlers: 122 S2C and 71 C2S**.
The previous wire coverage catalog contained 113. This audit adds the missing
80 entries and fixes the receive/host behaviors below. It does not establish
complete host/client fidelity; the remaining runtime work is D-NET-218.

Witness: Joint Operations executable, image base `0x400000`, client table
`g_np_msginfo_client @0x82AE28` and server table
`g_np_msginfo_server @0x82B5D8`. Entries are 16 bytes; each table ends in one
sentinel. Handler addresses below are resolved function starts. No executable
bytes or decompiler output are included in this record.

## Regressions repaired

| Path | Prior behavior and resulting fix | Retail witness | Regression |
|---|---|---|---|
| Received S2C messages | Connection maintained a second, incomplete allowlist. Real chat and text ceasefire messages were dropped before the reducer. Every admitted low game message now reaches it in packet order. | Client dispatch table `@0x82AE28`; text handler `@0x429E70`; chat `@0x42F240` | `npruntime_client_message_dispatch`, encrypted receive path |
| Session configuration ordering | The final game type learned by the connection was applied before every message in that packet. Earlier world-state/frame bodies could be decoded with a later layout. Apply 0x08/0x7B at their original positions instead. | Client dispatch table `@0x82AE28`; world-state handler `@0x42E200` | Real encrypted 0x0F followed by 0x08: preserves the earlier ceasefire update without a malformed-body count |
| S2C 0x18 entity repair | Codec existed without a consumer. Rebuild the row and native pool-1/2/3 lifetime, including ownership/class and relationships. The generation survives intervening deletion; old deferred death/reload notifications cannot act on the replacement. | `NapiNPClientMsg_FullEntitySpawn @0x433780` | Same-type repair, empty-slot replacement within one receive pump, native ownership/class, death-before/after-repair |
| S2C 0x66 weapon restrictions | Received bans never reached the joiner's loadout table. Each message resets all 255 entries to allowed, then applies only index != 255 and values 0/2. A new world reapplies the retained policy. | `NapiNPClientMsg_HandleWeaponRestrictions @0x42D4C0` | `inmatch_joiner_role`, actual loadout consumer |
| S2C 0x34 sound | Received sounds never reached audio. Flag 0 now requests interface audio; flag 1 requests a positional sound with signed world coordinates converted to Q16. Other flags do nothing. Both remote and listen clients consume once; single-player is gated out. | `NapiNPClientMsg_PlaySoundByName @0x4283A0` | `inmatch_joiner_role`, `npruntime_client_message_dispatch`, Godot extension build |
| C2S 0x06 fire admission | Host accepted stale/duplicate/premature/unseeded fire. Validate the retail signed floor and reject ceasefire/spectator fire before effects. Only accepted shots advance the floor. | `PlayerSlot_IsActive @0x4FC760`; `NapiNPServerMsg_0x006_ClientFiredRound @0x513310`, floor store `@0x513740` | `npruntime_client_fire`, ammo/event/floor assertions |
| Spawn/death fire clock | Seed/disarm messages had no corresponding host gate state. Reroll arms the floor; disarm retains it and accepts delayed shots only before host tick + three send periods. Primary cooldown is FIRE.delay_end + RECOIL.delay_start + RECOIL.delay_end; alt fire adds zero. | `Server_SendRandomSeedToPlayer @0x5101A0`; `AdmDefs_PostParseRecompute @0x53FEA0`; send-period initialization `@0x4C9CE3` | Unseeded, seed equality, strict cooldown boundary, disarm boundary, reroll, invalid ADM, alt fire |

The first framed-message tests failed on dropped ceasefire and absent full-slot
repair. The role test failed first on loadout policy, then on positional audio.
Additional tests failed on missing repair ownership/class, same-frame slot
reuse, and an earlier world-state body decoded with a later game type. The fire test failed on accepting an unseeded shot. These failures were
observed before their corresponding fixes.

## Remaining fidelity work

- **2026-09-20 (net-parity slice):** the C2S 0x0D / 0x3D / 0x42 / 0x4C / 0x51 host
  handlers, the outer 0x45/0x85 ping, the S2C 0x0F pose/waypoint application and
  the periodic S2C 0x46 quality resend are ported (net-re §8 D-NET-218 lists
  them with their witnesses); C2S 0x40 has its gates but spawns through an
  embedder seam.
- **D-NET-174:** authority-local fire still needs the same admission predicate.
  Retail calls it from the authority arm at `@0x42BE3A`; pure joiner prediction
  intentionally has no equivalent gate. Remote C2S admission is covered here.
- **D-NET-139:** outgoing C2S 0x0C interest pairs are still defaulted. The earlier
  ledger assertion that the builder populated them was incorrect. Retail's
  `Server_BuildEntityPriorityListForPlayer @0x50DF20` needs a full port, including
  bounds, view/LOS, aim target and the four-entry selection order.
- **S2C 0x24:** SETFLASH1, SU, GOTO and SPECTATORTARGET remain beyond the current
  SETCEASEFIRE consumer (`@0x429E70`). **S2C 0x67** is teleport (`@0x42D570`),
  not a weapon policy; successful messages carry six i32 position/orientation
  values and must update the local motor as well as its entity.
- **S2C 0x4E:** retail processes every complete remaining u16 handle and replies
  C2S 0x28 with two timestamp dwords and the leading u16 token (`@0x431870`).
  A working wire decoder alone does not perform that lifecycle/reply.
- **S2C 0x3B:** replies with reliable C2S 0x01 and one zero byte (`@0x431340`).
  **S2C 0x7D:** replies with C2S 0x50 carrying four metrics dwords (`@0x432690`).
- **S2C 0x18 residuals (D-NET-133/137):** native pool-0 reconstruction and retail
  minimap identity allocation are not established by the pool-1/2/3 tests.
  Byte +340 is retained without a gameplay interpretation. Destruction effects
  on an old lifetime replaced before the deferred consumer are not replayed.
- **Integrity challenges:** S2C 0x62's retail address/length/seed challenge must
  use a deliberate compatibility model. Arbitrary incoming retail pointers are
  not valid addresses in this executable.
- Squad, administration, profiles and other newly inventoried handlers below
  need their runtime semantics and role gates audited. Verified empty retail
  stubs are distinct from unimplemented behavior.

## Newly cataloged table entries

These 80 rows were absent from the previous catalog. Presence in this table or
`MsgCoverage::Decoded` is not a claim that a gameplay consumer exists. The
catalog records decoded, printer-only and uncharacterized coverage explicitly.

| Direction | Tag | Retail function | Address |
|---|---|---|---|
| S2C | `0x06` | `NapiNPClientMsg_HandleChatCommand` | `0x432BC0` |
| S2C | `0x3A` | `NapiNPClientMsg_0x03A` | `0x422680` |
| S2C | `0x3F` | `NapiNPClientMsg_0x03F` | `0x42BB20` |
| S2C | `0x27` | `NapiNPClientMsg_SpawnEffect` | `0x425AA0` |
| S2C | `0x28` | `NapiNPClientMsg_0x028` | `0x425B40` |
| S2C | `0x22` | `NetPacket_HandleEntityCreate` | `0x42EC90` |
| S2C | `0x17` | `NapiNPClientMsg_0x017` | `0x4226F0` |
| S2C | `0x07` | `NapiNPClientMsg_0x007` | `0x422730` |
| S2C | `0x33` | `NapiNPClientMsg_0x033` | `0x425FA0` |
| S2C | `0x1B` | `NapiNPClientMsg_HandleRandomSeed` | `0x426080` |
| S2C | `0x38` | `handle_weapon_switch_packet` | `0x4260B0` |
| S2C | `0x36` | `NapiNPClientMsg_0x036` | `0x426120` |
| S2C | `0x35` | `NapiNPClientMsg_0x035` | `0x4261A0` |
| S2C | `0x1F` | `NapiNPClientMsg_0x01F` | `0x427CB0` |
| S2C | `0x21` | `NapiNPClientMsg_HandleSpawnEffect` | `0x430B10` |
| S2C | `0x25` | `NapiNPClientMsg_GameReset` | `0x422800` |
| S2C | `0x2B` | `NapiNPClientMsg_0x02B` | `0x427DF0` |
| S2C | `0x2D` | `NapiNPClientMsg_0x02D` | `0x427E90` |
| S2C | `0x2E` | `NapiNPClientMsg_0x02E` | `0x427F80` |
| S2C | `0x6D` | `NapiNPClientMsg_HandleEntityDeath` | `0x430C50` |
| S2C | `0x32` | `NapiNPClientMsg_0x032` | `0x428060` |
| S2C | `0x37` | `NapiNPClientMsg_HandleWeaponSlotAction` | `0x431250` |
| S2C | `0x3B` | `NapiNPClientMsg_0x03B` | `0x431340` |
| S2C | `0x3D` | `NapiNPClientMsg_0x03D` | `0x422870` |
| S2C | `0x48` | `NapiNPClientMsg_0x048` | `0x4284B0` |
| S2C | `0x4F` | `NapiNPClientMsg_0x04F` | `0x4286C0` |
| S2C | `0x5B` | `NapiNPClientMsg_HandleHealthUpdate` | `0x4322B0` |
| S2C | `0x5C` | `NapiNPClientMsg_0x05C` | `0x425200` |
| S2C | `0x5E` | `NapiNPClientMsg_0x05E` | `0x4297B0` |
| S2C | `0x5F` | `NapiNPClientMsg_0x05F` | `0x4228F0` |
| S2C | `0x62` | `NapiNPClientMsg_0x062` | `0x42D200` |
| S2C | `0x63` | `NetMsg_HandlePoofToggle` | `0x42D450` |
| S2C | `0x65` | `NapiNPClientMsg_0x065` | `0x429870` |
| S2C | `0x67` | `NapiNPClientMsg_0x067` | `0x42D570` |
| S2C | `0x6A` | `NapiNPClientMsg_HandlePlayerJoinLeave` | `0x432510` |
| S2C | `0x70` | `NapiNPClientMsg_HandleWeaponLoadoutList` | `0x429A30` |
| S2C | `0x71` | `NapiNPClientMsg_HandleSquadJoin` | `0x425600` |
| S2C | `0x72` | `NapiNPClientMsg_0x072` | `0x425710` |
| S2C | `0x73` | `NapiNPClientMsg_0x073` | `0x425770` |
| S2C | `0x74` | `NapiNPClientMsg_PlayerRecruited` | `0x4258B0` |
| S2C | `0x78` | `NapiNPClientMsg_0x078` | `0x425970` |
| S2C | `0x7C` | `NapiNPClientMsg_0x07C` | `0x426020` |
| S2C | `0x7D` | `NapiNPClientMsg_0x07D` | `0x432690` |
| S2C | `0x7F` | `NapiNPClientMsg_0x07F` | `0x429E60` |
| S2C | `0x24` | `NapiNPClientMsg_HandleTextCommand` | `0x429E70` |
| S2C | `0x80` | `NapiNPClientMsg_0x080` | `0x42A070` |
| S2C | `0x82` | `NapiNPClientMsg_0x082` | `0x42A0E0` |
| S2C | `0x83` | `NapiNPClientMsg_0x083` | `0x4326E0` |
| C2S | `0x04` | `NapiNPServerMsg_Chat` | `0x5199D0` |
| C2S | `0x07` | `NapiNPServerMsg_0x007` | `0x4FC970` |
| C2S | `0x18` | `NapiNPServerMsg_HandleWeaponSpawn` | `0x51A020` |
| C2S | `0x1B` | `NapiNPServerMsg_SetFarClip` | `0x501D90` |
| C2S | `0x19` | `NapiNPServerMsg_0x019` | `0x514250` |
| C2S | `0x14` | `NapiNPServerMsg_HandleObjectSound` | `0x501E00` |
| C2S | `0x13` | `NapiNPServerMsg_HandleSectorAction` | `0x514330` |
| C2S | `0x17` | `NapiNPServerMsg_HandleChatOrWhisper` | `0x514850` |
| C2S | `0x1A` | `NapiNPServerMsg_HandleVoteUpdate` | `0x514B20` |
| C2S | `0x24` | `NapiNPServerMsg_0x024` | `0x514DC0` |
| C2S | `0x30` | `NapiNPServerMsg_0x030` | `0x5029B0` |
| C2S | `0x31` | `NapiNPServerMsg_0x031` | `0x5024A0` |
| C2S | `0x35` | `NapiNPServerMsg_0x035` | `0x500DF0` |
| C2S | `0x36` | `NapiNPServerMsg_0x036` | `0x500E00` |
| C2S | `0x38` | `NapiNPServerMsg_0x038` | `0x502510` |
| C2S | `0x39` | `NapiNPServerMsg_0x039` | `0x500E20` |
| C2S | `0x3C` | `Server_HandleClientCRCValidation` | `0x519110` |
| C2S | `0x3E` | `NapiNPServerMsg_0x03E` | `0x500E10` |
| C2S | `0x3F` | `NapiNPServerMsg_VoteKick` | `0x518F10` |
| C2S | `0x40` | `NapiNPServerMsg_HandleVehicleSpawnRequest` | `0x51C4C0` |
| C2S | `0x41` | `NapiNPServerMsg_ResetPlayerAmmo` | `0x510540` |
| C2S | `0x42` | `NapiNPServerMsg_SendWeaponSlotStates` | `0x510930` |
| C2S | `0x43` | `Server_HandleEntitySync` | `0x510990` |
| C2S | `0x44` | `NapiNPServerMsg_HandleChatBroadcast` | `0x510AE0` |
| C2S | `0x45` | `NapiNPServerMsg_0x045_HandleTeamAssignment` | `0x510C00` |
| C2S | `0x46` | `NapiNPServerMsg_HandleVoteKick` | `0x510D20` |
| C2S | `0x49` | `NapiNPServerMsg_0x049_ParsePlayerStatus` | `0x510F40` |
| C2S | `0x4B` | `NapiNPServer_BroadcastPlayerProfileUpdate` | `0x510DC0` |
| C2S | `0x4D` | `NapiNPServerMsg_0x04D_ChangeTeam` | `0x518F70` |
| C2S | `0x4F` | `NapiNPServerMsg_0x04F` | `0x514A40` |
| C2S | `0x50` | `NapiNPServerMsg_0x050_UpdatePlayerState` | `0x5112B0` |
| C2S | `0x51` | `Server_ProcessClientRequestSpectatorRespawn` | `0x51C840` |

## Validation

- Full Release native build succeeded. Final aggregate: **455 native tests
  passed**. The round-simulation fixture now seeds its connection and advances
  shot ticks. The previously asset-skipped `motorcycle_gravity_06tr` passes with
  the supplied revx02 installation. The focused coverage, message-dispatch,
  client-fire and joiner-role suites also pass.
- Rebuilt the final Godot extension from this worktree and imported with
  isolated user settings. The network/audio/deployment selection passes
  **92 tests, 1,810 assertions**, with no pending tests, parse errors or dropped
  scripts. The previously pending wire-header test now performs the actual
  initial streamed-zone pick and release; deployment also covers death selections.
- Repository size/maturity, module/include graph, header ownership, fixture,
  environment, conventions, citation census and ledger gates pass.
- `wait_parity_wire_ready.ps1 -SelfTest` passes. Live captures exposed a schema
  drift: `nw_pp` emits a participant column on `PARITY_EVENT`, but the parser
  rejected it. The parser now retains that field; its synthetic packet owners
  and positive/malformed-event regressions use the current format.

### Live revx02 checks

The supplied installation unblocked live testing. All cells used `01TR.bms`,
game type `65568`, port `32787`, 1920x1080, session name `Untitled ` (including
its trailing space), and callsigns `ParityHost`/`ParityJoin`. The mission header
is bound to the exact BMS hash, with matching extracted item definitions.
Runtime code was commit `9d3dea0e04a16a7ff6cfa1b027726c3f5216379a`; the parser
fix above was applied for these runs.

| Host / client | Run suffix | Result | Decoded uplinks / local frames / matched RTT replies at readiness |
|---|---|---|---|
| Retail / retail | `rr-revx02-20260909-h` | PASS | 60 / 60 / 59 |
| Retail / OpenNova | `ro-revx02-20260909-a` | PASS | 58 / 53 / 57 |
| OpenNova / retail | `or-revx02-20260909-a` | BLOCKED | Host loaded and answered LAN discovery; installed MCP has no `onhook_join_lan` tool |
| OpenNova / OpenNova | `oo-revx02-20260909-a` | PASS | 111 / 43 / 107 |

Run IDs have the prefix `netcode-`. Passing cells prove authenticated in-match
readiness, at least ten seconds of decoded gameplay traffic, and a 15-second
steady window containing completed input exercises. The OpenNova movement probes
completed while remaining in match. Captures have verified completion; retail
capture counters report no drops/truncation/write errors, and OpenNova roles
shut down cleanly. Full native decoding of the canonical RR/RO/OO captures
covers 324/330/287 gameplay state records respectively, with zero state decode
failures. These checks do not establish all-message fidelity or replace the
remaining work listed above.

The initial retail startup crashes were test provisioning errors: disabling
`PatchesEnabled` also disabled the expanded item table and memory allocation.
The supplied definitions contain 2,455 item blocks, above the unpatched
2,048-item capacity. Keeping the supplied patched Bink DLL and enabling that
group eliminated the repeated pre-connection access violation. The runbook now
records the required DLL companions, startup patches and corpus arguments.
Exact binary hashes, effective hook configs and full captures are recorded
locally. The supplied hook's source commit was not verified; these results are
specific to that hash-bound instrumented build. The newest available MCP was
also checked and lacks the retail joiner tool, so no OR verdict is claimed.

Raw captures, local install paths and tool transcripts stay in ignored scratch.
