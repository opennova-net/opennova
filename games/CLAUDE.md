# games/ — every game's own code

- One directory per title, named by its code (`jo/`, `dfx2/`, `acidgambit/`, the unwitnessed
  `jodemo/`, `dfx/`, `bhd/`), plus `novalogic_2009/`, the network-code generation Jointops.exe and
  dfx2.exe share (NVS, CO, PV2 "16", the 0x42's BN/VN/MBN/SOPD). STATIC `opennova_games`,
  PUBLIC-linking `opennova_runtime`; the repository root is its include root
  (`#include <games/jo/jo.h>`), a same-directory sibling `"jo.h"`.
- `engine/` holds the types, the registries and the lookups (`base/title/title.h` Title,
  `net/protocol/protocol.h` Protocol); a game's files hold its rows: `<code>.h` declares its
  accessors, `<code>_title.cpp` its Title, `<code>_protocol.cpp` the Protocol it speaks. A title
  that speaks another's protocol returns that object (`acidgambit_protocol()` is `jo_protocol()`),
  never a copy of its literals. An unwitnessed title is one TU holding its Title and its stub
  Protocol with exactly the values a binary witnessed, so its consumers refuse it by name.
- `games.cpp`'s `games::install()` fills the registries; each program calls it at its start and
  the tests through `opennova_test_games` (`tests/common/install_games.cpp`). Nothing under
  `engine/` includes `games/`, `<games/games.h>` is included only at those registration points
  (`include_graph_check.py`), and no engine target links `opennova_games`
  (`link_graph_check.py`).
- Every retail value keeps its `[orig: Name @0xADDR]` cite where it is written (cite_census scopes
  `games/`). A protocol's version (PV2) bumps in its `<code>_protocol.cpp` with any change to its
  wire bytes or its joiner-side rules; JO's and DFX2's never bump (retail-pinned).
- The fold-back exception: engine/CLAUDE.md folds a TU under ~100 lines into its one owner; a
  game's files stay one per responsibility (title, protocol) and one directory per title, however
  short, so each game is read in its own place.
