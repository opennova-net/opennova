# Music Editor Workspace

One screen, two levels:

- **The map** — every state of the music program as a graph: transitions,
  dispatch fan-outs, the start badge, idle loops. The States sidebar lists the
  same states; the breadcrumb trail tracks where you are (back/forward,
  Alt+Left / Alt+Right).
- **A state's program** — drill in and the state reads top to bottom as
  sentence-style steps: *Play [track]*, *Go to [state]*, *Set [variable] =
  [value]*. If/else and choose-by-value are blocks with indented lanes and
  case rows. Steps edit in place: dropdowns commit the moment you pick;
  conditions and values are chips that open a floating expression editor with
  room (build it structurally or type it). Drag rows to reorder, drop a track
  from the Tracks dock right where it should play, and resize a repeated run
  from its ×N badge. ＋ Add step inserts a working default for every kind —
  no creation dialogs.

Engine plumbing is explained, not rendered: a state that takes values from
its caller gets an *Inputs from caller* card (name the inputs; the names live
in the `.music_profile.json` sidecar beside variable names, the script keeps
its tokens), and the leaked main-loop tail the game dispatches into sits
behind an *⚡ Engine events* divider instead of posing as unreachable steps.

Open paired `.sbf` + `.bin` files from the configured resource root; press
Start to hear changes live — the running step glows and the view follows the
playback (clicking a state pins it; the Follow live toggle re-arms).
