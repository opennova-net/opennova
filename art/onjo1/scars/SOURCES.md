# The impact scars' sources

The marks a round leaves where it strikes: our own art, made from scratch (procedural noise, no
third-party material), in the look of Joint Operations' marks. The original game's scar textures were
looked at for size, form and drawing mode only (in a scratch folder outside the repo); nothing of
theirs is here.

## How the game draws them

Every round whose ammo.def entry carries `scar_type 1` (as `AMMO_ON_556` does) leaves a scar on the
struck model: a square 0.25 m a side, turned at random about the face's normal, holding one of the
four `scorch` textures picked at random, whatever the surface; a face of the glass surface takes
`bhole1` at half the size instead ([orig: Impact_SpawnGlassEffectsOrScar @ 0x5CF1B0], [orig:
Scar_TextureForId @ 0x5CC360]). The game loads the textures by these fixed names as it starts
([orig: Scar_LoadTextures @ 0x5CC2E0]; `docs/world/world-wac-ai-re.md` §24.9); without them it
draws each scar as a plain white square. The art is made for the drawer's states:

- **`scorch1` .. `scorch4`** blend by their alpha over what they mark, colour `2 x texel x light`,
  no depth write: the RGB is near black (soot in the hole, a near-black brown powder round it), so
  the mark darkens a dark wall as well as a light one, and the mark is in the alpha, which peaks at 0.82 in the hole and falls off through the crushed ring, the
  streaks and the specks to nothing well inside the square.
- **`bhole1`** is alpha-tested at 128 (writes depth, both sides drawn): pale blue-grey crack lines
  and a crushed rim round the hole, opaque on the cracks and clear between them.

## The files

| Texture (in `assets/`) | Size | Master | What it is |
|---|---|---|---|
| `scorch1.tga` .. `scorch4.tga` | 64 x 64, 32-bit | `src/scorch1.png` .. `src/scorch4.png` (256 x 256) | Four bullet strikes: a dark hole, a ragged ring of crushed material, radial streaks and specks, each from its own seed. |
| `bhole1.tga` | 128 x 128, 32-bit | `src/bhole1.png` (512 x 512) | A round through glass: radial cracks with branches, three broken rings and the crushed rim. |

**Export**: each texture is its master area-averaged down to the texture's size, written as an
uncompressed 32-bit TGA, bottom row first, no image ID, the form of the original's own scar textures.
The editor's image importer does exactly that: import the master with `format tga` and
`size <W>x<H>` (`replace_texture` naming the texture), then copy the `.tga` from the project's import
cache into `assets/`. No generator script is kept.
