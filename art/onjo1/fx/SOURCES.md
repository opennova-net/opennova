# The particle effects' sources

The textures of the base game's particle effects (bullet impacts, blood, muzzle flashes, spent casings):
our own art, made from scratch by procedure (noise, falloffs, shapes and a turned-brass render; no
third-party material), in the look of Joint Operations' own particle art. The original game's particle
textures were looked at for size, shape, alpha and drawing mode only (in a scratch folder outside the
repo); nothing of theirs is here. The effects that draw them are `assets/onhit.ptl` (the impacts),
`assets/onmuz.ptl` (the guns' flashes and casings), `assets/onblood.ptu` (the body hit, the gore set) and
the curves in `assets/ontable.ptl`, hand-written in the original's particle-file layout.

## How the game draws them

A particle def names each texture on a `graphicN = <file>, <mode>` line; the game loads it as a TGA
and packs it into a shared atlas page (1024 a side for these modes) with a 2.5-texel inset, so every
texture here is a 32-bit TGA of 16 to 128 a side (`docs/particles/ptl-format-re.md`, §4 and §5.2):

- **Alpha art** (`blend`, mode 0) draws `SRCALPHA / INVSRCALPHA`, its texel times the def's colour
  (`color1..4`, one picked per particle) times the effect world's tint: the shape is in the alpha and
  the colour mostly the def's. So these textures are near white or mid grey, shaded only for volume,
  and their RGB runs on into the clear texels (a flat or bled colour, never black), so filtering never
  darkens an edge. A def with an alpha-mode graphic is lit by the scene's ambient (`AMBIENTCOLOR`),
  which is how an impact dims at night.
- **Additive art** (`additive`, mode 1) draws `ONE / INVSRCALPHA` over an atlas whose alpha the game
  clears for this mode, so it adds its RGB and nothing else: these textures are black backgrounded,
  their colour and brightness in the RGB; their alpha (the brightest channel, written for the
  editor's previews) is never drawn, and their fades ride the defs' colour curves.
- **A flipbook** of N frames names `<stem>.tga` with `gN_flip_frames N`; the game opens
  `<stem>_01.tga` .. `<stem>_0N.tga` (lower case); a stem of at most 8 characters keeps each frame's
  name within the archives' 15.

## What is the source

- **`src/<name>.png`**: each texture's master, four times the texture's size, made by a throwaway
  procedure (no script is kept); the masters are the source.
- **`assets/art/<name>.png`**: a byte copy of the master, the import source in the project, with the
  editor's import record beside it (`<name>.png.import`: `format tga`, `size <W>x<H>`). The editor's
  image import makes `<name>.tga` from it as the project builds (into the project's import cache;
  no `.tga` is kept in `assets/`).

**Export**: a texture is its master area-averaged down to the texture's size, written as an
uncompressed 32-bit TGA, bottom row first, no image ID: the form of the original's own particle
textures. Change a master by replacing both copies (the editor's Replace on the import source), then
build.

## The files

| Texture | Size | Mode | Master | What it is | How it is made |
|---|---|---|---|---|---|
| `onfxpuf1.tga` | 64 x 64 | alpha | `src/onfxpuf1.png` (256) | A dense dust puff (every impact's dust, the metal and object smoke). | Soft gaussian blobs summed and saturated, eroded at the edge by fractal value noise; the alpha peaks at 0.56; grey RGB lit from above left by the density's slope. |
| `onfxpuf2.tga` | 64 x 64 | alpha | `src/onfxpuf2.png` (256) | A thin, broken wisp of the same (the dust's second graphic, the glass and leaf dust, the casing smoke). | As `onfxpuf1`, sparser and more eroded, peaking at 0.40. |
| `onfxspl1.tga` | 64 x 64 | alpha | `src/onfxspl1.png` (256) | Water spray: droplets and short streaks round a faint mist (the water column and spray). | Some 320 soft dots of falling size thrown outward from the centre, 46 radial strokes, a noisy gaussian mist; white. |
| `onfxbld1.tga` | 64 x 64 | alpha | `src/onfxbld1.png` (256) | A mist with fine droplets round it, white for the def to colour (the blood mist). | Gaussian blobs times fractal noise, ringed by 140 small droplets. |
| `onfxrng1.tga` | 128 x 128 | alpha | `src/onfxrng1.png` (512) | A water ring seen from above: a foamy crest, a fainter inner ring, droplets at the rim, a clear centre. | Two radial gaussian bands on a wobbled radius, broken by fractal foam, a faint fill between them, a scatter of droplets. |
| `onfxflsh.tga` | 32 x 32 | additive | `src/onfxflsh.png` (128) | An impact flash: a white-hot core, a warm glow and six uneven rays. | Radial falloffs and angular gaussians per ray, noise along the rays, warm-to-white by heat. |
| `onfxspk1.tga` | 32 x 32 | additive | `src/onfxspk1.png` (128) | A spark streak, vertical (drawn aligned to its motion, `TOPALIGN`): a white core in an orange glow. | Two gaussians across, tapered along. |
| `onfxchp1.tga`, `onfxchp2.tga` | 32 x 32 | alpha | `src/` (128) | Two broken chips of stone or dirt (the dirt, stone and sand debris; small and white, snow clumps). | A jagged polygon each, shaded in random facets with fractal grain and a shadowed rim, the colour bled outward. |
| `onfxwod1.tga`, `onfxwod2.tga` | 32 x 32 | alpha | `src/` (128) | A long wood splinter; two thin slivers (the wood debris, the foliage's twigs). | Thin polygons with jagged tips, a stretched-noise grain along their length. |
| `onfxlef1.tga`, `onfxlef2.tga` | 32 x 32 | alpha | `src/` (128) | A torn leaf piece with its midrib and veins; a grass blade's tip (the foliage and grass bits, coloured green by the defs). | Polygons with a torn edge, a darker midrib and vein lines, light noise. |
| `onfxgls1.tga` | 32 x 32 | alpha | `src/onfxgls1.png` (128) | Two glass shards, clear inside with bright edges and a glint. | Polygons at alpha 0.3..0.4 inside, 0.85 at the edges, one specular stripe; near-white blue. |
| `onfxcl1.tga` | 32 x 32 | alpha | `src/onfxcl1.png` (128) | A tuft of torn cloth fibres. | 26 curling strokes from a fuzzy core. |
| `onfxdrp1.tga` | 16 x 16 | alpha | `src/onfxdrp1.png` (64) | A drop with two satellites, white with a highlight and a darker rim for the def to colour (the blood drops). | A slightly pointed disc and two dots; radial shading, a highlight. |
| `onmzfl1.tga` | 32 x 32 | additive | `src/onmzfl1.png` (128) | The muzzle flash seen from the front: a five-pointed star round a hot core. | Five tapering angular petals, a core and a glow, noise along the petals. |
| `onmzfl2.tga` | 64 x 64 | additive | `src/onmzfl2.png` (256) | A flame tongue seen from the side, along the texture's height (drawn aligned to its motion: the forward flame and the flash's side petals). | A tapered flame envelope warped by stretched noise, eroded into licks, a white core along its axis. |
| `oncas_01.tga` .. `oncas_05.tga` | 32 x 32 | alpha | `src/` (128) | A spent 5.56 mm case, five frames of its tumble (the carbine's casing flipbook, `oncas.tga`). | A lathe profile of the 5.56 x 45 case (rim, extractor groove, body, shoulder, neck, open mouth, primer) sampled as points, turned to each frame's angle and depth-sorted, lit as brass (diffuse, a tight highlight, a darkened groove, a dark mouth), area-averaged from four times the master. |
| `onpcs_01.tga` .. `onpcs_04.tga` | 32 x 32 | alpha | `src/` (128) | A spent 9 mm case, four frames (the pistol's casing flipbook, `onpcs.tga`). | As the 5.56 frames, from the 9 x 19 case's straight, slightly tapered profile. |

## Third-party material

None: everything here is drawn from scratch by procedure.
