# NPC and mission scripting coverage inventory

Routing snapshot: 2026-09-09, refreshed 2026-09-23 for the rows the WAC/BMS/AI parity re-grill changed. This records every declared WAC command, BMS trigger/action and brain-table row. An explicit branch can still contain approximations or depend on an unfinished consumer. It is not a parity or mission-playthrough verdict. The [completion record](npc-mission-completion.md) owns acceptance gates; [world-wac-ai-re.md section 33](world-wac-ai-re.md) records the current witnesses and tests.

## WAC command registry

165 registry entries; all 165 have explicit VM branches and none reach the unsupported-command diagnostic (pinned by the `wac_dispatch_sweep` ctest, which drives every registry row through the VM). Parameter types below are the declared contracts, not a claim that every asset resolver is complete.

| Index | Command | Original | Parameters | Dispatch |
| --- | --- | --- | --- | --- |
| 0 | elapse | 0x4ECEF0 | Seconds | Explicit branch; consumer witnessed in section 33.1 (wac_behavior) |
| 1 | never | 0x4ED2E0 | none | Explicit branch; verify consumer |
| 2 | previous | 0x4ECF90 | none | Explicit branch; consumer witnessed in section 33.1 (wac_behavior) |
| 3 | chain | 0x4ECF20 | Seconds | Explicit branch; consumer witnessed in section 33.1 (wac_behavior) |
| 4 | past | 0x4ED010 | Seconds | Explicit branch; consumer witnessed in section 33.1 (wac_behavior) |
| 5 | before | 0x4ED030 | Seconds | Explicit branch; consumer witnessed in section 33.1 (wac_behavior) |
| 6 | ontick | 0x4ECFF0 | Seconds | Explicit branch; verify consumer |
| 7 | groupdead | 0x4ED1A0 | Number | Ported 2026-09-23: reads the group's live count `g_TriggerGroupLiveCount[g]` (the periodic `EntityPool_RecountLiveByGroup @0x40E8D0` rescan, group 0 forced 0), `setle` @0x4ED1AC..0x4ED1B2; a kill between rescans does not flip it; script_command_parity |
| 8 | groupalive | 0x4ED1C0 | Number | Ported 2026-09-23: the same live count, `setnle` @0x4ED1CC..0x4ED1D2; script_command_parity |
| 9 | dooropen | 0x4F70A0 | Number | Explicit branch; consumer witnessed in section 33.14 (doors) |
| 10 | SSNcritical | 0x4F1BF0 | Ssn | Explicit branch; consumer witnessed in section 33.7 (wac_behavior) |
| 11 | SSNexists | 0x4F1A70 | Ssn | Explicit branch; the ItemTypeIndex (+0x1C) gate @0x4F1AB9; script_command_parity |
| 12 | SSNdead | 0x4F1AC0 | Ssn | Ported 2026-09-23 as `EntityCommands::wac_ssn_dead`: the ItemTypeIndex gate @0x4F1B07, then Flags bit 1 (`and eax,2` @0x4F1B0D..0x4F1B11), not health; script_command_parity |
| 13 | SSNalive | 0x4F1B20 | Ssn | Ported 2026-09-23 as `EntityCommands::wac_ssn_alive`: the ItemTypeIndex gate @0x4F1B67, then Flags bit 1 clear (@0x4F1B6D..0x4F1B75), not health; script_command_parity |
| 14 | SSNwounded | 0x4F1B80 | Ssn | Explicit branch; consumer witnessed in section 32.1 (wac_behavior): a signed word compare, `sar cx,1` @0x4F1BD9 of def+0x17C, `cmp [eax+11Eh],cx` @0x4F1BDC, `setle` @0x4F1BE3 (a negative health word reads wounded) |
| 15 | SSNride | 0x4F7000 | Ssn | Explicit branch; consumer witnessed in section 33.7 (wac_behavior) |
| 16 | SSNonSSN | 0x4F19A0 | Ssn, Ssn | Explicit branch; verify consumer |
| 17 | SSNnearSSN | 0x4F14C0 | Ssn, Ssn, Distance | Explicit branch; consumer witnessed in section 32.1 (wac_behavior) |
| 18 | SSNlosSSN | 0x4F15E0 | Ssn, Ssn, Distance | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); the <= 20 u entity-aware / > 20 u terrain-sector walker split on the authored range (`script_los_clear`, bms-event-runtime-re section 3b; ai_los) |
| 19 | SSNseesSSN | 0x4F17C0 | Ssn, Ssn, Distance | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); the same walker split as SSNlosSSN |
| 20 | SSNarea | 0x4F1020 | Ssn, Area | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 21 | SSNarea3D | 0x4F0F60 | Ssn, Area | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 22 | SSNloc | 0x4F0E90 | Ssn, Number | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 23 | SSNLeadSSN2SSN | 0x4F12E0 | Ssn, Ssn, Ssn, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 24 | reset | 0x4ED300 | IfName | Explicit branch; consumer witnessed in section 33.4 (wac_behavior) |
| 25 | Gkill | 0x4F1F40 | Group | Explicit branch; consumer witnessed in section 33.10 (wac_behavior); runs the killSSN body per handle (the call @0x4F1F5E), each with its own ItemTypeIndex gate; returns 0 (@0x4F1F72) |
| 26 | Gremove | 0x4F1F80 | Group | Explicit branch; consumer witnessed in section 33.10 (wac_behavior); retail removes each member without a gate through `Server_RemoveEntityAndNotify` (S2C 0x12, the call @0x4F1FF2); the port calls the bare destroy `remove_ssn` (no 0x12): follow-up |
| 27 | Gsetaccuracy | 0x4F7BE0 | Number, Number, Number | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); returns 1 (@0x4F7C43) |
| 28 | GtoWP | 0x4ED3D0 | Number, WpList | Explicit branch; consumer `Entity_SetWaypointByTeam @0x43CD20` (call `@0x4ED3DC` from `WacCmd_GroupToWaypoint`, node -1), ported as `EntityCommands::group_to_waypoint` (2026-09-22, world-wac-ai-re section 23.4); returns 1 (@0x4ED3E4) |
| 29 | kill | 0x4EDC90 | Number | Ported 2026-09-23: `Entity_KillAllByNetId @0x43C8E0` (group 0 exit @0x43C8F2; pools 2, 0, 1 @0x43C8F8/@0x43C946/@0x43C996; every row of the group, dead rows included: Health 0 and the class event (e,1,0) @0x43C917..0x43C93F, the attacker kept), shared with BMS KillGroup; returns 0 (@0x4EDC9D); script_command_parity |
| 30 | remove | 0x4EDCA0 | Number | Explicit branch; consumer witnessed in section 33.11 (wac_behavior); shares BMS VaporizeGroup's walk (`Entity_TeleportAllByNetId @0x43D5D0`; the handler's call @0x4EDCA5): a notifying removal (S2C 0x12) per row on the authority, group 0 a no-op (bms-event-runtime-re section 11.4) |
| 31 | teleport | 0x4EE170 | Number, Target | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 32 | GroupMin | 0x4F7C50 | Number, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 33 | GroupMax | 0x4F7CA0 | Number, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 34 | GroupAtt | 0x4F7CF0 | Number, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 35 | GroupSpawn | 0x4F7AE0 | Number, Number | Explicit branch; consumer witnessed in section 33.12 (infantry_spawn) |
| 36 | GroupHP | 0x4F7B30 | Number, Number | Ported 2026-09-23 (`WacCmd_GroupHp`): pools 0, 1, 2 (@0x4F7B30/@0x4F7B6D/@0x4F7B9D), every row whose signed commandGroup word (+0x11C) matches (@0x4F7B57), the health word only (@0x4F7B64), return 1 (@0x4F7BD0); script_command_parity |
| 37 | opendoors | 0x4F7D40 | Number | Explicit branch; consumer witnessed in section 33.14 (doors) |
| 38 | closedoors | 0x4F7DA0 | Number | Explicit branch; consumer witnessed in section 33.14 (doors) |
| 39 | text | 0x4EDB50 | Text | Explicit branch; the line posts into the HUD CHAT ring (`Chat_AddSystemMessage @0x4EDB50` -> `Chat_AddMessageChannel1 @0x4985D0`, color -1, 930 ticks), returns 1 (@0x4EDB64); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 40 | wave | 0x4ED610 | Filename | Explicit branch; verify consumer; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 41 | hideSSN | 0x4F7750 | Ssn | Ported 2026-09-23: Flags bit 0 set on both Flags views (the ItemTypeIndex gate @0x4F7797, `or Flags,1` @0x4F779D); read by the area/location tests (`WacCmd_SsnArea` @0x4F1081), the AI target scan, zone capture and the traces; script_command_parity; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 42 | unhideSSN | 0x4F77B0 | Ssn | Ported 2026-09-23: Flags bit 0 cleared (`and Flags,~1` @0x4F77FD); script_command_parity; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 43 | disableSSN | 0x4F7690 | Ssn | Ported 2026-09-23: Flags bit 28 (0x10000000, `kEntityFlagScriptDisabled`) set (the gate @0x4F76D7, `or Flags,10000000h` @0x4F76DD), read with the dead bit as `Flags & 0x10000002` by the vehicle motors ahead of the driver (`Entity_UpdateVehiclePhysics` @0x48B980, `Entity_UpdateAircraftPhysics` @0x490F1E); script_command_parity; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 44 | enableSSN | 0x4F76F0 | Ssn | Ported 2026-09-23: Flags bit 28 cleared (`and Flags,0EFFFFFFFh` @0x4F773D); script_command_parity; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 45 | holdSSN | 0x4F7810 | Ssn | Ported 2026-09-23: bit 0x2000 of the entity+0x2C dword (`Entity::cause_flags`, `kCauseFlagScriptHold`) behind the ItemTypeIndex gate (@0x4F7857, `or [eax+2Ch],2000h` @0x4F785D); the reader is the org1 think's hold, move mode 12 and distance 0 (`Entity_UpdateInfantryAI` @0x4BD235..0x4BD240, ported); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 46 | unholdSSN | 0x4F7870 | Ssn | Ported 2026-09-23: the bit cleared behind the gate (@0x4F78B7, `and ...,0FFFFDFFFh` @0x4F78BD); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 47 | setaccuracy | 0x4F2070 | Ssn, Number, Number | Explicit branch; consumer witnessed in section 32.1 (wac_behavior) |
| 48 | SSNtoWP | 0x4F1CE0 | Ssn, WpList | Explicit branch; the handler is itself the route writer (nearest node, no detach or resets, brain copy and budget), ported as `EntityCommands::set_ssn_waypoint` (2026-09-22, world-wac-ai-re section 23.4) |
| 49 | killSSN | 0x4F1E40 | Ssn | Ported 2026-09-23: its own handler (`WacCmd_KillSsn`), `EntityCommands::wac_kill_ssn`: the ItemTypeIndex gate @0x4F1E89, the global hit record zeroed @0x4F1E8F..0x4F1E99, Health 0 @0x4F1EA4, lastAttacker 0 @0x4F1EAD, a person's staged clip +0x2C0 cleared @0x4F1EB7..0x4F1EBD, the class event (e,1,0) @0x4F1EC7..0x4F1ED2 (bms-event-runtime-re section 11.3); script_command_parity |
| 50 | removeSSN | 0x4F1EE0 | Ssn | Explicit branch; retail (`WacCmd_RemoveSsn`) removes through `Server_RemoveEntityAndNotify` (S2C 0x12, the call @0x4F1F28) and returns 1 (@0x4F1F30); the port calls the bare destroy `remove_ssn` (no 0x12): follow-up |
| 51 | teleSSN | 0x4F7E00 | Ssn, Target | Explicit branch; consumer witnessed in section 33.13 (wac_behavior); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 52 | SSNwave | 0x4F78D0 | Ssn, Filename, Distance | Explicit branch; verify consumer; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 53 | SSNradio | 0x4F79B0 | Ssn, Filename | Explicit branch; verify consumer; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 54 | SS2SSN | 0x4F1DD0 | SoundSet, Ssn | Verified binding, admission, positional playback and retry; device policy D-SND-8; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 55 | SSNanim | 0x4F7630 | Ssn, Anim | Explicit branch; consumer witnessed in section 33.19 (wac_actors) |
| 56 | SSNMin | 0x4F2010 | Ssn, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 57 | SSNMax | 0x4F2210 | Ssn, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 58 | SSNAtt | 0x4F2270 | Ssn, Distance | Explicit branch; consumer witnessed in section 33.13 (wac_behavior) |
| 59 | SSNSpawn | 0x4F7A80 | Ssn, Number | Explicit branch; consumer witnessed in section 33.12 (infantry_spawn) |
| 60 | SSNHP | 0x4F2100 | Ssn, Number | Ported 2026-09-23: no gate past the resolve; the 16-bit health store (`mov [eax+11Eh],dx` @0x4F214C), lastAttacker cleared @0x4F2153, return 1 @0x4F215D; the alive latch is not written (a corpse stays a corpse); script_command_parity |
| 61 | SSNADDHP | 0x4F2170 | Ssn, Number | Ported 2026-09-23: the ItemDef pointer (+0x20) gates it (@0x4F21B8..0x4F21BD), not the ItemTypeIndex; a 16-bit `add [eax+11Eh],cx` @0x4F21C4; a negative sum floors to 0 and returns 1 (@0x4F21D7..0x4F21E5); a sum above the signed def healthMax (+0x17C) word stores that word and returns 1 (@0x4F21E6..0x4F21FE); otherwise lastAttacker = 0 (@0x4F21FF) and return 0 (@0x4F2209); script_command_parity |
| 62 | ssn2ssn | 0x4F7330 | Ssn, Ssn | Explicit branch; consumer witnessed in section 33.7 (wac_behavior); the detach goes through `Entity_DetachFromVehicleIfServer` (the call @0x4F73DC) |
| 63 | ssnrelease | 0x4F7420 | Ssn | Explicit branch; the ItemTypeIndex gate @0x4F7465; the detach is `Entity_DetachFromVehicleIfServer` (the call @0x4F7475), authority only |
| 64 | ssnface | 0x4F1C60 | Ssn, Face | Timed GRM expression; witnessed inactive JO texture sink, section 33.30 |
| 65 | ssnturn | 0x4F72B0 | Ssn, Heading | Explicit branch; consumer witnessed in section 33.24 (wac_actors) |
| 66 | ssnguard | 0x4F71C0 | Ssn, Number | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); the ItemTypeIndex gate @0x4F7207 |
| 67 | ssnname | 0x4F7230 | Ssn, TextToken | Explicit branch; consumer witnessed in section 33.7 (wac_behavior) |
| 68 | ssnpspd | 0x4F7570 | Ssn, Number | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); the event-11 twin of ssncspd |
| 69 | ssncspd | 0x4F74B0 | Ssn, Number | Explicit branch; consumer witnessed in section 32.1 (wac_behavior); the ItemTypeIndex gate (@0x4F74FD) decides the return 1 (@0x4F755A), and only the AI event queue (10 / 11) is gated on the SM brain pointer +0x64 (@0x4F7503..0x4F7508); the port queues through the shared ChangeAI brain arms, which an organic skips |
| 70 | ssnuse | 0x4F70F0 | Ssn | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 71 | set | 0x4ED520 | Variable, Value | Explicit branch; verify consumer |
| 72 | add | 0x4ED530 | Variable, Value | Explicit branch; verify consumer |
| 73 | sub | 0x4ED540 | Variable, Value | Explicit branch; verify consumer |
| 74 | inc | 0x4ED550 | Variable | Explicit branch; verify consumer |
| 75 | dec | 0x4ED560 | Variable | Explicit branch; verify consumer |
| 76 | store | 0x4ED580 | Variable | Explicit branch; verify consumer |
| 77 | load | 0x4ED570 | Value | Explicit branch; verify consumer |
| 78 | TOD | 0x4EDC70 | Hour | Explicit branch; `WacCmd_Tod` stores `arg * 0x44444` raw into `Env_CurTimeFixed24` (@0x4EDC74/@0x4EDC7A) and returns 1 (@0x4EDC7F); the day wrap happens in the weather tick on the SIGNED word (`Environment_ComputeTimeOfDayColors` @0x57DE51..0x57DE78, stored @0x57DE84): TOD(-60) is 23:00 |
| 79 | targetfx | 0x4EE190 | Target | Typed particle consumer; native command/lifetime tests; D-PTL-26 tracks shared entity-slot integration; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 80 | ammo2tgt | 0x4F8100 | Ammo, Target | Explicit branch; projectile consumer verified |
| 81 | fx2tgt | 0x4F7FD0 | Fx, Target | Typed particle consumer; native command/lifetime tests; D-PTL-26 tracks shared entity-slot integration; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 82 | ammoarea | 0x4EE240 | Ammo, Area | Explicit branch; projectile consumer verified |
| 83 | sound2tgt | 0x4F7F60 | SoundSet, Target | Verified first target, return values and positional audio consumer; D-SND-8; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 84 | flash | 0x4ED500 | none | Explicit branch; returns 1 (@0x4ED50A); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 85 | farflash | 0x4ED510 | none | Explicit branch; returns 1 (@0x4ED51A); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 86 | quake | 0x4ED4C0 | Number | Explicit branch; returns 1 (@0x4ED4CE); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 87 | win | 0x4ED4A0 | Team | Explicit branch; `Server_ProcessRoundEnd` with no chat line; returns 1 (@0x4ED4AD) |
| 88 | lose | 0x4ED3F0 | Team | Explicit branch; the Misc chat line posts into the CHAT ring and relays its key to the joiners with team 0 on both branches (S2C 0x3F kind 1, `GameMsg_AddChatLineAndRelay` @0x4ED411 / @0x4ED477) before `Server_ProcessRoundEnd`; relay ported 2026-09-23 (`World::relay_mission_text_chat`; wac_behavior `test_lose_relays_its_chat_key`) |
| 89 | music | 0x4ED910 | Number | Witnessed closed-stream success; independent opener has no retail callers; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 90 | skyspeed | 0x4EDEB0 | Number | Explicit branch; verify consumer |
| 91 | skyheight | 0x4EDEC0 | Number | Explicit branch; verify consumer |
| 92 | fogtype | 0x4EDED0 | Number | Explicit branch; verify consumer |
| 93 | fogdist | 0x4EE100 | Distance | Explicit branch; consumer witnessed in section 33.13 (wac_environment_wire) |
| 94 | movefog | 0x4EE0A0 | Distance, Seconds | Explicit branch; consumer witnessed in section 33.13 (wac_environment_wire) |
| 95 | rain | 0x4EDF60 | Number, Seconds | Explicit branch; verify consumer |
| 96 | snow | 0x4EDFD0 | Number, Seconds | Explicit branch; verify consumer |
| 97 | overcast | 0x4EE040 | Number, Seconds | Explicit branch; verify consumer |
| 98 | Help | 0x4F6DE0 | none | Retail text/XML export and debug messages; section 33.29 |
| 99 | text# | 0x4EDB70 | Text, Number | Explicit branch; the handler's "%s %i" line (`Chat_AddFormattedIntMessage`, the sprintf call @0x4EDB9E) posts into the HUD CHAT ring; returns 1 (@0x4EDBC0); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 100 | consol | 0x4EDBE0 | Text | Explicit branch; `debug_text` into the SYSTEM ring (`Chat_AddMessageChannel2 @0x4987F0`); returns 1 (@0x4EDBF4); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 101 | consol# | 0x4EDC00 | Text, Number | Explicit branch; the "%s %i" line (`WacCmd_ConsolNumber`, the sprintf call @0x4EDC2E) into the SYSTEM ring; returns 1 (@0x4EDC50); replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 102 | sound | 0x4ED590 | SoundSet, Distance, Heading | Verified explicit distance/bearing and direct audio consumer; D-SND-8; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 103 | forceanim | 0x4F2610 | Anim | Explicit branch; consumer witnessed in section 33.19 (wac_actors); its console line is `debug_text` in the SYSTEM ring |
| 104 | tele | 0x4F22D0 | Ssn | Explicit branch; consumer witnessed in section 33.24 (wac_actors) |
| 105 | fall | 0x4ED4E0 | none | Explicit branch; consumer witnessed in section 33.19 (wac_actors) |
| 106 | fov | 0x4EDEA0 | Number | Explicit branch; shared weather/camera consumer verified |
| 107 | squadevent | 0x4ED070 | Number | Verified four-slot selection, named exports, TTL and retry; publisher has no retail callers |
| 108 | random | 0x4ED280 | Number | Explicit branch; consumer witnessed in section 33.26 (wac_state) |
| 109 | outside | 0x4ED050 | none | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 110 | location | 0x4ED190 | Number | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 111 | area | 0x4ED0C0 | Area | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 112 | area3D | 0x4ED120 | Area | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 113 | waveready | 0x4ED380 | none | Explicit branch; consumer witnessed in section 33.18 (wac_voice) |
| 114 | weaponfired | 0x4ED360 | Number | Explicit branch; consumer witnessed in section 33.17 (wac_weapon_input) |
| 115 | event | 0x4ED1E0 | Number | Reads the BMS active flag, including activation delay; verified |
| 116 | meride | 0x4F1260 | Ssn | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 117 | meattached | 0x4F10D0 | Ssn | Explicit branch; consumer witnessed in section 33.11 (wac_behavior) |
| 118 | medrive | 0x4F1150 | Ssn | Explicit branch; consumer witnessed in section 32.1 (wac_behavior) |
| 119 | meongun | 0x4F11E0 | Ssn | Explicit branch; consumer witnessed in section 32.1 (wac_behavior) |
| 120 | ammorain | 0x4EE1A0 | Ammo | Explicit branch; projectile consumer verified |
| 121 | fxrain | 0x4EE3E0 | Fx | Typed particle consumer; native command/lifetime tests; D-PTL-26 tracks shared entity-slot integration |
| 122 | lightning | 0x4EDE20 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 123 | face | 0x4ED5D0 | Face | Local timed GRM expression; section 33.30; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 124 | anim | 0x4ED5B0 | Anim | Explicit branch; consumer witnessed in section 33.19 (wac_actors) |
| 125 | sunfade | 0x4EDF10 | Number, Seconds | Explicit branch; verify consumer |
| 126 | gain | 0x4EDD30 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 127 | squadclear | 0x4ED390 | none | Verified selected-row and named-export clearing; retains selected index |
| 128 | blockfire | 0x4EE140 | Number, Number | Explicit branch; consumer witnessed in section 33.17 (wac_weapon_input) |
| 129 | colorfade | 0x4EDCB0 | Number | Explicit branch; verify consumer |
| 130 | sun | 0x4EDCD0 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 131 | sky | 0x4EDD00 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 132 | ground | 0x4EDD60 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 133 | floor | 0x4EDDF0 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 134 | ceiling | 0x4EDDC0 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 135 | cloud | 0x4EDD90 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 136 | fogcolor | 0x4EDE40 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 137 | fog | 0x4EDE40 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 138 | skyfogcolor | 0x4EDE70 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 139 | skyfog | 0x4EDE70 | Red, Green, Blue | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 140 | crash | 0x4EDE70 | Red, Green, Blue, Green | Explicit branch; consumer witnessed in section 33.21 (wac_environment_wire) |
| 141 | eq | 0x4ED220 | Number, Number | Explicit branch; verify consumer |
| 142 | ne | 0x4ED230 | Number, Number | Explicit branch; verify consumer |
| 143 | lt | 0x4ED240 | Number, Number | Explicit branch; verify consumer |
| 144 | gt | 0x4ED250 | Number, Number | Explicit branch; verify consumer |
| 145 | le | 0x4ED260 | Number, Number | Explicit branch; verify consumer |
| 146 | ge | 0x4ED270 | Number, Number | Explicit branch; verify consumer |
| 147 | true | 0x4ED200 | Number | Explicit branch; verify consumer |
| 148 | false | 0x4ED210 | Number | Explicit branch; verify consumer |
| 149 | onptick | 0x4F0E10 | Seconds | Ported 2026-09-23: the selected player's slot dword +0x184 in whole seconds (the `Entity_ValidatePtr` call @0x4F0E58, `mov ecx,[eax+184h]` @0x4F0E65, /62 @0x4F0E6B..0x4F0E7C) == arg; the dword is `Server_TickUpdate`'s unsaturated per-tick count for a state-6 slot with a present, unhidden entity (`add [esi+184h],1` @0x51D977), `MatchPlayer::play_ticks`; wac_players |
| 150 | ptext | 0x4EDB50 | Text | Replicated S2C 0x23 targeted (flags 0x12) under wire index 39/40/100: a registered non-local selection is sent to that player only and the VM returns 1 without the local call; a local, unregistered or empty selection runs the local text/wave/consol handler (section 33.39), which returns 1 (@0x4EDB64) and posts into the CHAT ring |
| 151 | pwave | 0x4ED610 | Filename | Replicated S2C 0x23 targeted (flags 0x12) under wire index 39/40/100: a registered non-local selection is sent to that player only and the VM returns 1 without the local call; a local, unregistered or empty selection runs the local text/wave/consol handler (section 33.39) |
| 152 | pconsol | 0x4EDBE0 | Text | Replicated S2C 0x23 targeted (flags 0x12) under wire index 39/40/100: a registered non-local selection is sent to that player only and the VM returns 1 without the local call; a local, unregistered or empty selection runs the local text/wave/consol handler (section 33.39), which returns 1 (@0x4EDBF4) and posts `debug_text` into the SYSTEM ring |
| 153 | pisgold | 0x4F0AF0 | none | Witnessed clear Gold flag; section 33.28 |
| 154 | AddExp | 0x4F2690 | Ssn, Number | Match raw points and recursive sharing; section 33.28 |
| 155 | IsPSPallteam | 0x4EE4B0 | Number | Explicit branch; consumer witnessed in section 33.19 (wac_actors) |
| 156 | dropflare | 0x4F2710 | Ssn | Explicit branch; consumer witnessed in section 33.19 (wac_actors) |
| 157 | ammo2ssn | 0x4F24E0 | Ammo, Ssn, Ssn | Alternating posed model userpoints; source-fire ownership and retry verified |
| 158 | fx2ssn | 0x4F23A0 | Fx, Ssn | Typed particle consumer; native command/lifetime tests; D-PTL-26 tracks shared entity-slot integration; replicated S2C 0x23 broadcast (flags 0x0a/0x0c) AND run locally through the shared handler wac::run_remote_command (section 33.39) |
| 159 | piskills | 0x4F0B60 | Number | Registered player's signed enemy-kill counter; section 33.28 |
| 160 | ppunt | 0x4F0DA0 | none | Connection description code 33; section 33.28 |
| 161 | pkillpunt | 0x4F0D30 | none | Connection description code 49; section 33.28 |
| 162 | pisvar | 0x4F0BD0 | Number | Player-slot byte bank; section 33.28 / D-WAC-2 |
| 163 | psetvar | 0x4F0CB0 | Number | Player-slot byte bank; section 33.28 / D-WAC-2 |
| 164 | pisteam | 0x4F0C50 | Team | Explicit branch; verify consumer |

## BMS trigger and action schema

Every named schema value is listed below. Explicit cases are counted against the evaluator/dispatcher, including cases whose side effects require more work. Unknown numeric values retain the existing default behavior. AI action subtypes enter the shared Entity_ApplyCommand dispatcher and need its field-level witness, not just this enum inventory. Every arm of that dispatcher is carried (2026-09-23): the slot arms gate on the AI slot (+0x68), while the alert, skill, state, speed and fire events, TARGETSSN, AIUSEWPZ/AICLEARWPZ and PLAYPARTANIM gate on the vehicle brain (+0x64), so an organic takes only the slot arms; subs 1, 3, 4, 7, 9..14, 18..20, 24 and 25 have no editor token and dispatch by number (world-wac-ai-re section 32.2; ai_brain_rows).

### TriggerMainType

| Code | Name | Dispatch |
| --- | --- | --- |
| 1 | Group | Explicit case; verify consumer |
| 2 | Single | Explicit case; verify consumer |
| 3 | Event | Explicit case; verify consumer |
| 4 | MissionVariable | Explicit case; verify consumer |
| 5 | SecondTimeThrough | Explicit case; verify consumer |
| 6 | Teammate | Explicit case; verify consumer |
| 7 | Player | Explicit case; verify consumer |

### GroupTriggerType

| Code | Name | Dispatch |
| --- | --- | --- |
| 0 | Null | No retail arm either: the evaluator's default returns false, and a zone trigger the load-time resolver neutered (main and sub type zeroed) lands here and reads false (bms-event-runtime-re section 7.3) |
| 1 | GroupSeesGroup | Explicit case; verify consumer |
| 2 | GroupHasTargetedGroup | Explicit case; verify consumer |
| 3 | GroupAtRedAlert | Explicit case; verify consumer |
| 4 | GroupDestroyed | Explicit case; verify consumer |
| 5 | GroupAlive | Explicit case; verify consumer |
| 6 | GroupHasLostMoreUnits | Explicit case; verify consumer |
| 7 | GroupAtWaypoint | Explicit case; verify consumer |
| 9 | GroupIntact | Explicit case; verify consumer |
| 10 | GroupIsWithinArea | Explicit case; verify consumer |
| 11 | GroupHoldingGroup | Explicit case; verify consumer |
| 12 | GroupHasMoreUnits | Explicit case; verify consumer |
| 13 | GroupHasShotGroup | Explicit case; verify consumer |
| 14 | GroupAtYellowAlert | Explicit case; verify consumer |
| 15 | GroupHasTargetedSingle | Explicit case; verify consumer |
| 16 | GroupSeesSingle | Explicit case; verify consumer |
| 17 | GroupHasShotSingle | Explicit case; verify consumer |

### SingleTriggerType

| Code | Name | Dispatch |
| --- | --- | --- |
| 0 | Null | No retail arm either: the evaluator's default returns false, and a zone trigger the load-time resolver neutered (main and sub type zeroed) lands here and reads false (bms-event-runtime-re section 7.3) |
| 1 | SingleSeesGroup | Explicit case; verify consumer |
| 2 | SingleHasTargetedGroup | Explicit case; verify consumer |
| 3 | SingleAtRedAlert | Explicit case; verify consumer |
| 4 | SingleDestroyed | Explicit case; the `Entity_IsAliveByBmsRef` row walk negated (`EntityCommands::bms_ref_alive`): an SSN no pool 0/1/2 row carries reads destroyed (bms-event-runtime-re section 11.2); bms_event_parity, mission_event_vectors |
| 5 | SingleAlive | Explicit case; the `Entity_IsAliveByBmsRef` row walk (`bms_ref_alive`); bms_event_parity, mission_event_vectors |
| 6 | SingleHasLostMoreUnits | Explicit case; verify consumer |
| 7 | SingleAtWaypoint | Explicit case; verify consumer |
| 9 | SingleIntact | Explicit case; verify consumer |
| 10 | SingleIsWithinArea | Explicit case; verify consumer |
| 11 | SingleHoldingGroup | Explicit case; verify consumer |
| 12 | SingleHasMoreUnits | Explicit case; verify consumer |
| 13 | SingleHasShotGroup | Explicit case; verify consumer |
| 14 | SingleAtYellowAlert | Explicit case; verify consumer |
| 15 | SingleHasTargetedSingle | Explicit case; verify consumer |
| 16 | SingleSeesSingle | Explicit case; verify consumer |
| 17 | SingleHasShotSingle | Explicit case; verify consumer |
| 42 | SingleOnTopOf | Explicit case; verify consumer |
| 43 | SingleFartherThan | Explicit case; verify consumer |
| 44 | SingleHasNoLOS | Explicit case; the raw in-range-and-clear read with retail's <= 20 u / > 20 u walker split (`script_los_clear`, bms-event-runtime-re section 3b); ai_los |
| 45 | SingleDoesNotSeeOrFarther | Explicit case; sub 44 over the offset points plus the 30 degree facing cone, the same walker split; the cone's yaw is the D-EVT-3 residual |

### MissionVariableTriggerType

| Code | Name | Dispatch |
| --- | --- | --- |
| 1 | MissionVariableIsEqual | Explicit case; verify consumer |
| 2 | MissionVariableIsLessThan | Explicit case; verify consumer |
| 3 | MissionVariableIsGreaterThan | Explicit case; verify consumer |
| 4 | MissionVariableIsLessThanOrEqual | Explicit case; verify consumer |
| 5 | MissionVariableIsGreaterThanOrEqual | Explicit case; verify consumer |

### TeammateTriggerType

| Code | Name | Dispatch |
| --- | --- | --- |
| 1 | TeammateIsEnabled | Explicit case; verify consumer |
| 2 | TeammateMedicAssisting | Explicit case; verify consumer |
| 3 | TeammateEvacuating | Explicit case; verify consumer |

### PlayerTriggerType

| Code | Name | Dispatch |
| --- | --- | --- |
| 18 | PlayerBerserk | Explicit case; the local player's AiSlot behavior word & 0x200 returned raw, folded bitwise by the chain (bms-event-runtime-re sections 1.3 and 1.4) |
| 19 | PlayerFirstPerson | Explicit case; input-bit mirror consume; set by view1st (400) through `Simulation::apply_local_player_view_action` (bms-event-runtime-re section 1.4; GUT simulation_test.gd) |
| 20 | PlayerThirdPerson | Explicit case; input-bit mirror consume; set by viewchase (402) |
| 21 | PlayerCockpitView | Explicit case; input-bit mirror consume; set by viewwithgun (401) |
| 22 | PlayerInputBit10 | Explicit case; the mask has no setter in the image (false in retail too) |
| 23 | PlayerInputBit11 | Explicit case; no setter in the image |
| 24 | PlayerInputBit12 | Explicit case; no setter in the image |
| 25 | PlayerInputBit13 | Explicit case; no setter in the image |
| 26 | PlayerLookByteBit0Clear | Explicit case, constant true: the byte's only store writes 0 (`HUD_BuildEntityInfo @0x4B8440` (the store @0x4B84D9), bms-event-runtime-re section 1.4) |
| 27 | PlayerLookByteBit0Set | Explicit case, constant false (the same witness) |
| 28 | PlayerInputBit29 | Explicit case; no setter in the image |
| 29 | PlayerInputBit14 | Explicit case; no setter in the image |
| 30 | PlayerInputBit15 | Explicit case; no setter in the image |
| 32 | PlayerInputBitIndex | Explicit case; `1 << p1` through the mirror consume |
| 33 | PlayerInputBitIndexPlus15 | Explicit case; `1 << (low byte of p1 + 15)` through the mirror consume |
| 34 | PlayerDialogDone | Explicit case over `ScriptDialogRegistry`; the dialog playback producer is a shell hook |
| 35 | PlayerDialogFinished | Explicit case over `ScriptDialogRegistry`; producer a shell hook |
| 36 | PlayerAwol | Explicit case; verify consumer |
| 37 | PlayerSatchel | Explicit case over the placed satchels and the area bounds (event_runtime ctest) |
| 38 | PlayerAttachedToSsn | Explicit case; verify consumer |
| 39 | PlayerOnSsn | Explicit case; verify consumer |
| 40 | PlayerDrivingSsn | Explicit case; verify consumer |
| 41 | PlayerOnGun | Explicit case; verify consumer |

### ActionType

| Code | Name | Dispatch |
| --- | --- | --- |
| 0 | Null | Witnessed no-op |
| 1 | RedirectGroupTo | Explicit case; verify consumer |
| 2 | KillGroup | Explicit case; `Entity_KillAllByNetId` shared with WAC kill: pools 2, 0, 1, dead rows included, the hit record's damage and owner words cleared per row (bms-event-runtime-re section 11.3); script_command_parity, bms_event_parity |
| 3 | ChangeGroupAI | Explicit case; pools 2 (aiRuntime rows only), 0, 1; group 0 and sub 0 no-ops; then the group alert stamps (bms-event-runtime-re section 3b); bms_event_parity |
| 4 | VaporizeGroup | Explicit case; `server_remove_and_notify` per row of pools 2, 0, 1, 3 (S2C 0x12, then the shared destroy), authority only, no recount (bms-event-runtime-re sections 10 and 11.4); bms_event_parity |
| 5 | MisvarChange | Explicit case; verify consumer |
| 6 | OutputText | Explicit case; verify consumer |
| 7 | PlayWavList | Explicit case; plays where `is_mp_session_peer` is set, and after the round-over latch only when p2 == 1 (bms-event-runtime-re section 1.5); bms_event_parity |
| 8 | BlueWin | Explicit case; verify consumer |
| 9 | RedWin | Explicit case; verify consumer |
| 10 | GreenWin | Explicit case; verify consumer |
| 11 | GroupVelocity | Explicit case; stores the group-row speed word, which retail never reads (bms-event-runtime-re section 10) |
| 12 | AreaAiRed | Explicit case; team 2, pool 0, the load-inlined zone box with its corners paired across axes (bms-event-runtime-re section 7.3); bms_event_parity |
| 13 | AreaAiBlue | Explicit case; team 1, as AreaAiRed |
| 14 | SubGoalWon | Explicit case; the `STRWINMSG` chat line posts into the CHAT ring and its key relays to the joiners as S2C 0x3F kind 1 with team 1 (bms-event-runtime-re section 11.5); bms_hud_relay |
| 15 | SubGoalLost | Explicit case; the `STRLOSEMSG` line and relay with team 0 (as SubGoalWon) |
| 16 | ChangeGTeamAction | Explicit case; verify consumer |
| 17 | ChangeGroupAction | Explicit case; verify consumer |
| 18 | GroupTeleportAction | Explicit case; verify consumer |
| 19 | RedirectSingleTo | Explicit case; verify consumer |
| 20 | KillSingle | Explicit case; `Entity_KillByNetId` port (`kill_ssn`): pools 0..3, pool 0 also clears lastAttacker and the staged clip, the hit record's damage word cleared (its owner on pool 0 only), pool 3 phase 4 (bms-event-runtime-re section 11.3); bms_event_parity |
| 21 | ChangeSingleAI | Explicit case; sub 0 / SSN 0 no-ops; the first match in pools 0, 1, 2 (bms-event-runtime-re section 7.5) |
| 22 | VaporizeSingle | Explicit case; `server_remove_and_notify` (S2C 0x12, then the shared destroy), authority only (bms-event-runtime-re section 11.4); bms_event_parity |
| 23 | SingleVelocity | Explicit case; verify consumer |
| 24 | ChangeSteamAction | Explicit case; verify consumer |
| 25 | SingleChangeGroup | Explicit case; verify consumer |
| 26 | SingleTeleportAction | Explicit case; verify consumer |
| 27 | ParticleEffectAction | Typed marker-name/position consumer; D-PTL-26 tracks shared entity-slot integration |
| 28 | SpecialSubType | Explicit case: sub 37 the HUD item flash (the `hud_item_flash` effect; hud_item_flash), sub 38 clears the input-action word, sub 39 a retail dead store, every other sub a no-op (bms-event-runtime-re section 11.6) |
| 30 | GroupOpenDoorAction | Door pool owner (DoorSystem); world-wac-ai-re section 33.14 |
| 31 | GroupCloseDoorAction | Door pool owner (DoorSystem); world-wac-ai-re section 33.14 |
| 32 | GroupResetHasVisited | Explicit case; verify consumer |
| 33 | SingleResetHasVisited | Explicit case; verify consumer |
| 34 | ResetEvent | Explicit case; verify consumer |
| 35 | ShowWinSubgoal | Explicit case; `World::show_objective_notification`: the New Objective and directive lines in the CHAT ring, NEW_GOAL at the local player, S2C 0x3F kind 0 (bms-event-runtime-re section 11.5); bms_hud_relay, hud_game_text, nw_message_coverage |
| 36 | ShowLoseSubgoal | Explicit case; the lose notification without the sound (as ShowWinSubgoal) |
| 37 | AttachToEmplaced | Explicit case; `use_boarding_target` (the WAC ssnuse port, `WacScript_TryMountEntityToVehicle`); mission_mount |
| 38 | SetLightState | Explicit case; retail `sub_5A8C80 @0x5A8C80` sets a light-group channel whose consumer is unresolved; the `set_light` effect has no consumer (bms-event-runtime-re section 11.7) |
| 39 | Teammates | Eight-slot operation owner; world-wac-ai-re section 33.32 |
| 40 | ShowWaypoints | Explicit case; verify consumer |
| 41 | ExecuteWac | Witnessed no-op |
| 42 | SsnTargetSsnPri | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 43 | SsnTargetSsnExc | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 44 | SsnTargetGroupPri | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 45 | SsnTargetGroupExc | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 46 | GroupTargetSsnPri | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 47 | GroupTargetSsnExc | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 48 | GroupTargetGroupPri | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |
| 49 | GroupTargetGroupExc | Target-policy word writer; world-wac-ai-re section 33.2 (D-EVT-6 closed) |

### AIActionSubType

| Code | Name | Dispatch |
| --- | --- | --- |
| 2 | GuardBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 5 | RedAlert | Shared Entity_ApplyCommand; review field/consumer parity |
| 6 | GreenAlert | Shared Entity_ApplyCommand; review field/consumer parity |
| 8 | Accuracy | Shared Entity_ApplyCommand; review field/consumer parity |
| 15 | BlindBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 16 | BerserkBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 17 | ClimberBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 21 | CowardBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 22 | YellowAlert | Shared Entity_ApplyCommand; review field/consumer parity |
| 26 | DriveSkill | Shared Entity_ApplyCommand brain arm; the stored brain+176 word has no gameplay reader |
| 27 | AimSkill | Shared Entity_ApplyCommand; review field/consumer parity |
| 28 | AiSetState | Shared Entity_ApplyCommand; review field/consumer parity |
| 29 | CombatSpeed | Shared Entity_ApplyCommand; review field/consumer parity |
| 30 | PatrolSpeed | Shared Entity_ApplyCommand; review field/consumer parity |
| 31 | FindAndUse | Shared Entity_ApplyCommand; consumer witnessed in section 33.8 |
| 32 | AiUseWpz | Shared Entity_ApplyCommand brain arm; the brain+432 latch is read by the HELO waypoint mover (`AI_UpdateMovementTarget` @0x460FC7) |
| 33 | AiClearWpz | Shared Entity_ApplyCommand brain arm; clears the same latch |
| 34 | PlayPartAnim | Shared Entity_ApplyCommand brain arm; stores the direction and the rate only: retail never integrates the part phases (the integrator is unreferenced, world-wac-ai-re section 8.4) |
| 37 | HudItem | Witnessed retail no-op; world-wac-ai-re section 33.3 |
| 39 | TmateStatus | Witnessed retail no-op; world-wac-ai-re section 33.3 |
| 40 | AiNodePathBit | Shared Entity_ApplyCommand; consumer witnessed in section 33.8 |
| 41 | AttackDistanceValue | Shared Entity_ApplyCommand; review field/consumer parity |
| 42 | EngageDistanceMin | Shared Entity_ApplyCommand; review field/consumer parity |
| 43 | IndestructableBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 44 | TargetSsn | Shared Entity_ApplyCommand; consumer witnessed in section 33.3 |
| 45 | StartFiringBit | Shared Entity_ApplyCommand; review field/consumer parity |
| 46 | FiringAngle | Shared Entity_ApplyCommand; review field/consumer parity |

### MissionVariableActionSubType

| Code | Name | Dispatch |
| --- | --- | --- |
| 0 | Null | OPEN: no explicit case |
| 1 | Set | Explicit case; verify consumer |
| 2 | Add | Explicit case; verify consumer |
| 3 | Subtract | Explicit case; verify consumer |
| 4 | Increment | Explicit case; verify consumer |
| 5 | Decrement | Explicit case; verify consumer |

### TeammateActionSubType

| Code | Name | Dispatch |
| --- | --- | --- |
| 0 | Null | Witnessed no-op |
| 1 | MedicAssist | Pickup operation; original missing-helicopter fault guarded |
| 2 | EvacuateTt | Flyover operation and dynamic helicopter/medics; retail no-pilot outcome retained |
| 3 | EvacuateAt | Witnessed no-op |

## Brain handler table

The 24 rows below are the actual enter/tick/exit/event bindings. The default handler is a no-op; a default row alone is not proof that retail performs no work. Remaining behavior and unmodeled fields stay in the owning RE record.

| State | Enter | Tick | Exit | Event |
| --- | --- | --- | --- | --- |
| 0 | default | default | default | default |
| 1 | default | default | default | default |
| 2 | default | default | default | default |
| 3 | default | default | default | default |
| 4 | default | default | default | default |
| 5 | default | default | default | default |
| 6 | h_enter_aircraft_land | h_aircraft_land_tick | default | h_aircraft_event |
| 7 | h_set_state_idle | h_aircraft_followwp_tick | default | h_aircraft_event |
| 8 | h_enter_aircraft_combat | h_aircraft_combat_tick | h_clear_target_and_bone_flag | h_aircraft_event |
| 9 | default | default | default | default |
| 10 | h_enter_aircraft_evade | h_aircraft_evade_tick | h_clear_target_ref | h_aircraft_event |
| 11 | h_reset_to_idle | h_aircraft_followwp_tick | default | h_aircraft_event |
| 12 | default | default | default | default |
| 13 | h_enter_aircraft_dying | h_aircraft_dying_tick | default | h_handle_alert_event |
| 14 | h_reset_to_patrol | h_pretty_tick | default | h_pretty_event |
| 15 | h_enter_aircraft_dead | h_vehicle_dead_tick | default | h_vehicle_dead_event |
| 16 | h_set_state_idle | h_ground_followwp_tick | default | h_combat_event |
| 17 | h_enter_ground_combat | h_ground_combat_tick | h_clear_bone_flag | h_combat_event |
| 18 | h_enter_ground_evade | h_patrol_tick | default | h_combat_event |
| 19 | h_full_reset_to_idle | h_ground_followwp_tick | default | h_combat_event |
| 20 | default | default | default | default |
| 21 | h_enter_vehicle_dying | h_vehicle_dying_tick | default | h_vehicle_dying_event |
| 22 | h_full_reset_to_patrol | h_pretty_tick | default | h_pretty_event |
| 23 | h_enter_vehicle_dead | h_vehicle_dead_tick | default | h_vehicle_dead_event |

## Motor and presentation acceptance

| Runtime family | Existing owner | Remaining acceptance |
| --- | --- | --- |
| Organic NPC | infantry.cpp and infantry_* helpers | Remaining movement/weapon consumers (retreat is retail dead code; the obstacle detour, idle facing, drag and the death edge are ported, world-wac-ai-re sections 17.3, 33.16, 33.27, 33.31); death/respawn fixtures are not a mission playthrough |
| Local and remote player body | infantry.cpp, infantry_remote_anim.cpp, player_spawn.cpp | Normal failure/redeploy/retry; preserve the player lifecycle before the first remote uplink |
| Wheeled/tracked ground vehicle | VehicleSystem and ground vehicle traits/motor | Spawn-marker deck localization and the flare target handle (D-NET-161 (b)/(c)) and remaining collision behavior |
| Watercraft | VehicleSystem and watercraft motor | Mission routes and controller/seat transitions, including solo progression |
| Helicopter and fixed-wing | Aircraft brain handlers and VehicleSystem | Carrier spawn localization (D-NET-161 (b)) and remaining route/engagement/death behavior |
| Emplacements and passengers | vehicle_attach, infantry_board and weapon-slot pump | Remaining seat-picker parity and scripted entry/exit consumers |
| Attachments and suspended motors | infantry_board and `Entity::motor_suspended` (the AINODEPATH motor swap) | FIND_AND_USE/AINODEPATH behavior is covered by focused fixtures; normal mission choreography remains |
| Corpses and wrecks | infantry_spawn and destruction lifecycle | Player-reset consolidation and the incendiary +72 burn source (the extended spawn producer, drag and collision force are ported, world-wac-ai-re sections 33.31 and 33.34) |
| NPC sound and effects | SoundSlotEvent and destruction/fire output consumers | Script resource resolution and normal presentation (the target/rain effect commands have typed consumers, registry rows 79/81/121/158) |
| NPC body and held weapon | InfantryState channels and pose/presentation adapters | Held-weapon and remaining aim/fire/death appearance |
| Mission outcome and retry | MissionKernel, Session and match/subgoal runtime | Normal spawn to success/failure, feedback, clean retry and next-mission transition |

No normal SP mission playthrough is accepted by this routing inventory.
