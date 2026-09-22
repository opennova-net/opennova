# Current state and work routes

OpenNova is pre-1.0. The [maturity program](maturity-program.md) closed in
July 2026; current fidelity work is tracked by the
[divergence ledger](divergence-ledger.md), with non-parity work in
[TODO.md](../TODO.md). This page routes readers to the owners of current
information. The ledger and owning records win if a summary here ages.

## Architecture

[ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md) is the current
architecture contract. [ADR 0044](adr/0044-shared-native-assets.md) amends
its asset ownership; [ADR 0045](adr/0045-cli-game-data-runtime-only.md)
defines the runtime-only distribution and required game-data argument.
The [runtime map](runtime-architecture.md) explains a mission frame.
Earlier architecture rounds remain in the ADR archive as history.

## Where work lives

| Area | Current owner |
| --- | --- |
| Parity status and research gaps | [Divergence ledger](divergence-ledger.md); the `NEEDS-RE` rows name research questions. |
| World, AI, mission, and vehicles | [World/WAC/AI](world/world-wac-ai-re.md), [vehicle movers](world/vehicle-client-movers-re.md), [NPC mission completion](world/npc-mission-completion.md), and [mission records](mission/). |
| Matchmaking and in-match networking | [Network RE record](net/novaworld-net-re.md), especially its divergence catalog. The [in-match roadmap](../engine/runtime/inmatch/ROADMAP.md) records the completed build. |
| Interface, audio, and player data | [HUD](interface/hud-re.md), [menus](mnu/menu-re.md), [audio](audio/lwf-dbf-sound-re.md), and [avatars](playerinfo/avatars-re.md). |
| Render, terrain, and environment | [Render index](render/README.md), [terrain](terrain/terrain-re.md), [foliage](foliage/foliage-re.md), and [environment](env/env-tod-re.md). |
| Formats and other domains | The matching record in the [documentation index](README.md). |
| Project health and product work | [TODO.md](../TODO.md); these are outside the parity ledger. |

The [2026-09-13 audit](jo-c-parity-audit-2026-09-13.md) and
[2026-09-18 validation](jo-c-validation-2026-09-18.md) are snapshots of
their baselines. Their findings remain useful, but the ledger records
subsequent status changes.

## Working loop

For a fidelity slice, establish the retail witness, make a structural port
with inline `[orig: Name @ 0xADDR]` citations, update the owning RE record
and ledger together, then run the relevant tests and lint gates. The
[engine primer](engine-primer.md) explains the witness workflow and the
[re-doc skill](../.claude/skills/re-doc/SKILL.md) defines record updates.
The [asset-gated test guide](asset-gated-tests.md) explains when a green
run did not exercise retail data.
