# games/ — every game's own code

- One directory per title, named by its code (`jo/`, `dfx2/`, `acidgambit/`, the unwitnessed
  `dfx/` and `bhd/`), plus `novalogic_2009/`, the network-code generation Jointops.exe and
  dfx2.exe share (NVS, CO, PV2 "16", the 0x42's BN/VN/MBN/SOPD). Two STATIC targets: `opennova_games`,
  every title and protocol (the `_title.cpp` and `_protocol.cpp` files, `games.cpp`), linking
  `opennova_net` alone; and `opennova_games_rules`, the rules (the `_rules.cpp` files,
  `games_rules.cpp`), linking `opennova_games` and `opennova_runtime`. So a program that runs no
  match (the updater, the gate, the data program) carries no in-match runtime. The repository root
  is the include root (`#include <games/jo/jo.h>`), a same-directory sibling `"jo.h"`; a
  `<code>.h` forward-declares `world::TitleRules`, and only the rules files include
  `runtime/world/title_rules.h`.
- `engine/` holds the types, the registries and the lookups (`base/title/title.h` Title,
  `net/protocol/protocol.h` Protocol, `runtime/world/title_rules.h` TitleRules); a game's files
  hold its rows: `<code>.h` declares its accessors, `<code>_title.cpp` its Title,
  `<code>_protocol.cpp` the Protocol it speaks, `<code>_rules.cpp` the TitleRules it runs (the
  in-match knobs where its executable parts from Jointops.exe, whose behaviour a default
  TitleRules is, and the tables it owns). A title that speaks another's protocol or runs another's
  rules returns that object (`acidgambit_protocol()` is `jo_protocol()`, `acidgambit_rules()`
  `jo_rules()`), never a copy of its literals. A protocol of its own is a sheet: it copies JO's row
  (`Protocol p = jo_protocol();`) and overrides its identity, then its in-match knobs by the message
  each shapes (`ProtocolInMatch`'s groups, `s2c_0a` .. `c2s_2f`, indexed by `kProtocolDialect`),
  then its NovaWorld values, each override cited (`dfx2_protocol.cpp`); JO's row is the default
  sheet. An unwitnessed title has the same files: its Title, its stub Protocol with exactly the
  values a binary witnessed, so its consumers refuse it by name, and Joint Operations' rules, said
  so.
- `games.cpp`'s `games::install()` fills the title and protocol registries, `games_rules.cpp`'s
  `games::install_rules()` the rules registry. Each program calls `install()` at its start; one
  that runs a match (a session, a World, the editor's previews) links `opennova_games_rules` and
  calls `install_rules()` too. The tests install both through `opennova_test_games`
  (`tests/common/install_games.cpp`). Nothing under `engine/` includes `games/`,
  `<games/games.h>` is included only at those registration points (`include_graph_check.py`), and
  no engine target links either target; `opennova_games` never reaches the runtime, and every
  program that calls `install_rules()` links `opennova_games_rules` (`link_graph_check.py`).
- Every retail value keeps its `[orig: Name @0xADDR]` cite where it is written (cite_census scopes
  `games/`). A protocol's version (PV2) bumps in its `<code>_protocol.cpp` with any change to its
  wire bytes or its joiner-side rules (the TitleRules knobs marked joiner-side); games_test pins one
  bump-rule sheet per protocol (its ProtocolInMatch by message, its CD key and those knobs) and
  checks that the titles speaking one Protocol object run the same joiner-side knobs, so a change to
  one shows in review. JO's and DFX2's never bump (retail-pinned).
- The fold-back exception: engine/CLAUDE.md folds a TU under ~100 lines into its one owner; a
  game's files stay one per responsibility (title, protocol, rules) and one directory per title, however
  short, so each game is read in its own place.
