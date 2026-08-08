# ADR 0030: the formats placement criterion — what earns an engine/formats/ lib

- **Status**: accepted (2026-08-08; the format-placement inventory round,
  maintainer-approved)
- **Owners**: engine layout
- **Supersedes/updates**: nothing becomes false. Completes ADR 0024 decision 1,
  which affirms one-lib-per-format but never defines which parsers count as
  "a format"; ADR 0024's header gains a cross-reference here.

## Context

Nine NovaLogic on-disk formats were found parsed or written outside
`engine/formats/`: `.adm` in `runtime/anim` (despite carrying a full flat C
ABI, FFI mirrors, fixtures, and a writer — a formats lib in everything but
path), `.ptl` in `runtime/particle` (ONED-authored, roundtrip-tested, fully
RE-documented), the TPM1 tile mesh (`.tml`/`.tms`) in `runtime/terrain` (the
one member of the terrain format family left behind), the `.bms`/`.mis`
mission format under `runtime/mission` (in the formats *target* since
ADR 0029, but not the formats *directory*), ~60 lines of CBIN control-code
display semantics stranded in the Godot adapter's `.kda` loader, and four
smaller cases (`.aip`, `.wac` source, `.dep`, `.sph`) whose fusion had
arguable engineering rationales — all rejected on review (decision 3): file
knowledge belongs in formats/, period. The placement looked arbitrary because
the criterion was never written; each case below is now a decision, not an
accident.

The same inventory established two negatives worth recording: the Python/DCC
pipeline duplicates no byte-level format knowledge (its `*_ffi.py` files are
sanctioned ctypes mirrors over the C ABI), and no GDScript parses NovaLogic
bytes.

## Decision

1. **The boundary.** File knowledge — the parsed model and its read (and,
   where writer policy exists, write) code — lives in `engine/formats/<lib>`,
   depending only on `base/io` and other formats libs, never on runtime
   headers. Runtime semantics layered on the parsed model — simulation, world
   coupling, VMs, bake pipelines — stay in `runtime/` (or `net/` for wire
   interpretation). Placement follows dependency shape, not extension and not
   consumer count: a single-consumer format still gets its lib. The norm for
   a formats lib's correctness instrument is a roundtrip/parity test
   independent of any running sim (partial-port and intermediate-artifact
   libs state their weaker instrument in the lib header).

2. **The include-prefix rule.** A formats lib keeps its format's historical
   include prefix. When a domain splits across formats/ and runtime/, the two
   libs may expose ONE prefix with **disjoint header sets** — the
   terrain/terrain_query precedent, now also `particle/` (formats/ptl vs
   runtime/particle) and `mission/` (formats/mission vs runtime/mission).
   The directory is named for the format: extension-shaped where one
   extension dominates (`adm`, `ptl`), magic-shaped where extensions are
   plural (`tpm` for `.tml`/`.tms`; precedent `bfc1`).

3. **No format parser stays runtime-fused.** The maintainer's call
   (2026-08-08): every on-disk format's parse/write code lives in
   `engine/formats/`, including the four cases a first draft of this ADR
   proposed leaving fused. What stays in `runtime/` (or `net/`) is execution
   and simulation over the parsed model, never the file knowledge itself:

   | Format | Formats home (the extraction) | What stays behind, and where |
   |---|---|---|
   | `.aip` | `formats/aip` — the profile text parser, moved with its honest partial-coverage note: only `patrol_speed`/`combat_speed` are witnessed (`[orig: AIProfile_ParseProperty @ 0x45E6DF..0x45E717]`); the remainder is ledgered under the D-AI-11 residuals and lands HERE when witnessed | mission boot keeps profile resolution/consumption (`runtime_boot.cpp`) |
   | `.wac` source | `formats/wac` — the language front-end: lexer, parser, AST, compiler, bytecode/program model (source → compiled program is file knowledge; the mission-split shape). Recorded gap rides along: `lexer.cpp` (0 `[orig]`/141) and `parser.cpp` (0/358) are uncited despite the faithfulness claim — a named citation-pass follow-up | the VM and `wac_system` (execution against the World) stay `runtime/wac` |
   | `.dep` | `formats/dep` — the raw 1024×1024 u16 depth-buffer read/write, documented as the bake intermediate it is | the bake pipeline that produces/consumes it stays `runtime/terrain` |
   | `.sph` | `formats/sph` — the FOURCC chunk container walker (tags + payload spans over `io/`, no net types) | the wire-payload interpretation of chunk contents stays `net/npwire` (ADR 0019 Model-B; the legal edge is net → formats) |

4. **Extraction discipline.** Moving a parser into formats/ is a pre-1.0
   single-change move: every consumer updates in the same change, `[orig]`
   citations travel verbatim with the code, the format's roundtrip/parity
   suites stay green with zero fixture drift, and the include-seam lint
   (`scripts/lint/include_graph_check.py`) gains any new forbidden prefix in
   the same commit. The step-by-step recipe lives in the `new-format-lib`
   skill.

## Consequences

- `engine/formats/` gains `adm`, `ptl`, `tpm`, `mission` (the ADR 0029 fold's
  directory finally matching its target home), `aip`, `dep`, `sph`, and the
  `wac` front-end; the CBIN display-view semantics move from the Godot
  adapter into `formats/cbin`. Each lands as its own PR under decision 4's
  discipline.
- Anything still parsed outside `formats/` after this program is a gap, full
  stop — decision 3 removed the third state.
- The ABI surface is unaffected throughout: moved exports (`adm_*`,
  `opennova_mission_*`) stay whole-archived in the same DLL, and
  `abi_export_identity` stays byte-identical against an untouched baseline.
- The ratchet instruments are move-invariant (`engine/*/*/src` shape
  preserved); no baseline moves.
- Partial ports are documented in-lib, never hidden by placement: a formats
  lib whose coverage is partial (aip) says so in its header with the ledger
  pointer, so placement uniformity never misrepresents witness coverage.

## What this does not change

- ADR 0024's one-directory-per-format layout and Model A/B consumption
  models; ADR 0029's five group targets and the mission acyclicity fold
  (only the fold's *directory* language changes when the mission move
  lands).
- The faithful-port rules: extractions are moves, not rewrites; witnessed
  behavior and citations are untouched.
- Industry formats (`.tga`, `.dds`, `.wav`, pcap) keep using platform or
  standard loaders per the engine-wide CRT/OS exclusion; `.pcx` has a lib
  only because no platform loader exists.
