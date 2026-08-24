# `assets/src/` — modelling sources

`.blend` scene sources and the `.ase` exports they produce, for the models that
end up compiled to `.3di` at the `assets/` root.

Kept out of the flat game dir on purpose: under `/d` retail resolves bare
filenames at `assets/`, so every file there is one retail may attempt to load.
Nothing here is loadable by the game — it is the source we author from.

Pipeline: Blender (`.blend`) → `blender/ase_exporter.py` → `.ase` → ONED's
Object workspace (LODs + materials) → `.3di` written to `assets/`.

Texture names must stay ≤ 11 characters before the extension: the ASE exporter
caps the emitted filename at 15 including `.TGA`. And the compiled set must
contain **no `.dds`** — under `/d` a `.dds`-named texture cannot load loose and
renders as a checkerboard.
