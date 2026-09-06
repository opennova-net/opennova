# ADR 0045: Godot owns development workflows

- **Status**: accepted (2026-09-05)
- **Updates**: ADR 0015's two Godot products, ADR 0037's ONED run/packaging
  contract, ADR 0039's ONED ImGui surface, and ADR 0044's remaining migration.

The source Godot project owns world authoring, game launch, retail staging and
packing. OpenNova is the only exported Godot application. ONED's executable,
scene, settings, native UI classes, export feature and consumers are removed.
Historical ONED decisions remain records of the previous architecture.

The OpenNova editor dock uses the active GameWorld's WorldSource. It does not
create a second workspace, asset database or content format. Machine-local
installation and output paths live in Godot project metadata. Native documents
remain authoritative, and pending edits and disk conflicts are resolved through
the existing WorldEditSession save seam before run or pack.

GameRunSession and GamePacker live under godot/tools. The editor and a headless
source-project command share these workflow modules. Native format and packing
policy remain in their engine owners. OpenNova launches always use the current
Godot executable and source project; the session owns exactly one child and
waits for its exit before replacing or restaging it. Retail staging uses a
project-local cache. Retail lookup policy follows the selected source kind.

Packing accepts loose sources and rejects archive-backed input instead of
silently dropping its dependencies. Output directories cannot overlap sources;
unmarked nonempty directories are preserved. The existing retail staging and
release archive contracts do not change. The release script runs the source
command directly and keeps both established ZIP names, each with only the game
executable. The debug game keeps F3 tools; release packaging omits the ImGui addon.

Next: native mission placement authoring through Godot selection, transforms and
undo, with BMS save/reopen as the acceptance test. Broader node composition and
asset conversion follow as separate slices; none introduces another app or a
parallel saved world representation.
