# `art/onjo1/`: OpenNova's own game art

The Blender sources of OpenNova's own game (the base game in `assets/`, its first
expansion `onjo1` in `expansions/onjo1/`). Each model is made from scratch in the
look of Joint Operations: the same scale, silhouettes and muted palette, upgraded
as far as the original engine draws it, so the original `Jointops.exe` and
OpenNova show it alike. The game's sounds and their CC0 sources are in `sound/`,
under their own rules (`sound/README.md`).

## Rules

- **The `.blend` is the source of truth.** Models are authored in their `.blend`
  and exported with the OpenNova 3DI add-on (`tools/blender/opennova_3di/`) into
  `assets/` (or `expansions/onjo1/`). No generator script is committed: anything
  used to drive Blender is throwaway.
- **Made from scratch.** The original game's models and textures are reference
  only (looked at in a scratch file outside the repo, never imported into a
  `.blend` here, never committed).
- **Third-party material is CC0 only** (Poly Haven, ambientCG), recorded per model
  in its `SOURCES.md`: asset, author, URL, licence, and what it was used for.
- **Retail conventions are kept where gameplay or the engine depends on them:**
  size and origin, part names and order, user points and seats, the collision
  LOD and its volumes, LOD thresholds of the same order. A person keeps the
  original 19-bone person rig and bind so the game's own clips drive it.

## Budgets (the original engine's limits, for now)

| What | Budget | Why |
|---|---|---|
| Diffuse | 2048 for hero models (people, vehicles, first-person weapons), 1024 for props and buildings (2048 only where the player looks close, as `oncrate1`) | Retail draws any size; its 32-bit process is the real limit |
| Diffuse file | DXT5 DDS with a full mip chain beside the `.tga` name the model holds | The model loader takes the `.dds` of the name first; a TGA loads uncompressed with the engine's own mips, 4 bytes a texel (a 2048 TGA is about 21 MiB in retail's process, its DXT5 DDS about 5.3 MiB); every texture the original ships for its models is DXT5 with a full chain |
| Normal map | 512 a side, a `.mdt` on a normal row (type 4 or 5) | Retail halves a normal map until it is at most 512; a `.tga` on a normal row is a height map the engine converts, and a normal map on a type 0 row loads as a plain colour image with no cap |
| Specular | in the diffuse alpha | `VS_PHONGT` and the other Phong shaders read it there |
| Detail | a tiling texture on the second UV map, mid grey (128) neutral | The `FF_MT` and `*2` shaders multiply it in at 2x; the original's buildings (`Dhut02`) pair a 512 base on the first UV map with small detail textures tiled about 20 times across the second, which is how a building stays sharp up close |
| Brightness | a lit surface's texel at about half its seen value | Retail lights in gamma space as texture x (hemisphere + sun) x 2, so mid grey under full light reads as white |
| Sides | powers of two; TGAs stored bottom row first | Every model texture the original ships is a power of two (a height map must be); retail's TGA reader ignores the origin bit, so a top-left TGA draws upside down |
| Triangles | LOD 0 about 3 to 5 times the original's; keep the LOD chain | Retail lights fixed-function shaders per vertex; more vertices light better |
| Vertices per LOD | at most 2 MiB of vertex data: 52,428 static, 32,768 static with tangents (the bump shaders), 37,449 skinned, 26,214 skinned with tangents | Retail uploads a LOD into one 2 MiB pool buffer and drops a larger one (`allocate_lod_gpu_buffers @ 0x5B2610`); strides 40, 64, 56, 80 bytes |
| Indices per LOD | at most 262,144 (512 KiB) | The index pool's smallest buffer |
| LODs | at most 8 | The loader's eight-slot tables |
| Strips and materials | near the original's count (materials have no hard limit) | Each is a draw call |
| Names | model names at most 8 characters, starting `on` (`oncrate1`), so the add-on's texture names `<model>_<i>.tga` and `<model>_<i>n.mdt` fit the 15 characters a game archive holds | Longer names are cut to a shared stem (`on_crate1` gave `on_crate_0.tga`) |

## Shaders

Pick from the engine's own table (`opennova-3di catalog`), the family the original
uses for that kind of model, upgraded to the normal-mapped member where one exists:
static props `VS_PHONGT` (as the original's `Barl01`), buildings `FF_MT_OP` with a
detail texture or `VS_DOT3DIFF2` (bump plus detail), skinned people
`VS_SKBUMPPHONGT`, first-person arms `VS_SKBUMPPHONGT` (the extended skinned layout
the original's arms need), first-person weapons `VS_PHONGT` (as the original's `M4_1st`
and `M16_1st`; a fixed-function `FF_ST_OP` gun has no highlight and reads flat).

## Workflow, per model

1. Read the original's counterpart (`opennova-3di info --verbose` on a file
   extracted to a local reference folder): size, parts, LODs and thresholds, user
   points, collision.
2. Model in `art/onjo1/models/<name>/<name>.blend`: a high-detail bake source and
   the game LODs (Add Model, Add LOD, parts `PN##`, collision `CB-colonly`).
3. Bake diffuse (with ambient occlusion), normal and the specular mask onto the
   game LODs; keep UV islands unmirrored on normal-mapped meshes.
4. Export Model into `assets/`; check it in the editor (Problems, the model
   preview), then in a mission in OpenNova and in the original game.
