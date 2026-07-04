---
name: blender-object
description: Drives a running Blender (via the blender MCP server) to create a custom NovaLogic object model — built procedurally or from a generator — names every object and material to ONED's convention so the round trip is lossless, then exports it to ASE with the bundled opennova add-on. Use when asked to create, author, or model a custom object / 3di in Blender, generate a game model, or export Blender geometry to .ase.
---

# Author a NovaLogic object in Blender and export to ASE

The export half already exists: the add-on at `blender/` writes NovaLogic `.ase` via the
native `opennova.dll`, and an object's 3DI role is encoded entirely in its **scene-object
name**. Your job is to create geometry in a live Blender, name everything to the
convention so `blender/ase_exporter.py` classifies it correctly, then export. The full
name table is in [NAMING.md](NAMING.md) — read it before conforming.

All Blender actions go through `mcp__blender__*` tools. Code blocks below are pasted into
`mcp__blender__execute_blender_code` unless noted. `.ase` verification is done with the
host's `Read`/`Grep` on the written file (`.ase` is ASCII).

## 0. Connect & preflight

1. `mcp__blender__get_scene_info` — confirms the bridge is up. If it errors, tell the user
   to launch Blender and enable the **BlenderMCP** add-on (its socket server), then retry.
   Do not try to start Blender yourself.
2. Confirm the native lib exists at `blender/lib/windows-x64/opennova.dll` (Linux:
   `blender/lib/linux-x64/libopennova.so`). If missing, the export raises "Native library
   not found" — fix with `bash scripts/package_addon.sh`, or copy
   `build/Release/opennova.dll` into `blender/lib/windows-x64/`.
3. Note the **absolute repo root** — the directory that contains `blender/` for the
   project you're working in (e.g. the current worktree). §5's export snippet injects it
   into Blender's `sys.path`. The Blender process does not know your repo path; you supply
   it as a literal string.

## 1. Plan the object

Before touching Blender, list:

- **Parts** — one `PN##` per logical subobject (a static prop is usually one part, `PN01`).
- **Render mesh per part** — `## Mesh<n>` (the two leading digits = part index).
- **Hierarchy + attach points** — which subobject parents which. Every non-root part needs
  a `~PPx attach` (PP = parent subobject index) so the 3DI tree is rebuilt.
- **Centers** — one `_NN center` per part (the subobject pivot/center).
- **User points** — named, oriented points other systems look up by label (`Ground` for
  placement, a particle/launch point, etc.). `UP<c>NN <label>` — never `USR` (dropped on
  export). See [NAMING.md](NAMING.md).
- **Collision** — at least one `<code>NN-colonly` volume (`CB01-colonly` is a box). Pick a
  code from [NAMING.md](NAMING.md); its semantic meaning is *inferred*, not witnessed.
  Occlusion is the `<prefix>NN-occonly` variant.
- **Materials + textures** — one shader tag per material from [NAMING.md](NAMING.md)'s
  table. `FF_ST_*` shaders are *textured* — give each a diffuse image and UVs, or the
  surface is untextured. Default opaque single-texture diffuse is `FF_ST_OP`; alpha-blended
  is `FF_ST_AB`; glass is `FFP_GLASS`. Materials are named `Material_<index>_<shader>` and
  may be shared by parts.

A *complete* object has all of these — a render mesh alone is not a finished model. Build
the parts/meshes/materials (§3), then the centers, attach points, user points, collision,
and textures (§3.5). Every helper carries a **real position** — never leave them at the
world origin.

## 2. Create the base geometry — pick a method

**Procedural** (full control of names/topology — preferred for precise props):
build with `bmesh` / Blender ops. `blender/mesh_primitives.py` has reference
`create_cube_mesh` / `create_direction_arrow_mesh` shapes. Example: two boxes for a
two-part object —

```python
import bpy, bmesh
def add_box(name, size, loc):
    me = bpy.data.meshes.new(name + "_mesh")
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=size); bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    bpy.context.scene.collection.objects.link(ob)
    return ob
add_box("body", 2.0, (0, 0, 1))
add_box("lid",  2.0, (0, 0, 2.1))
print("created", [o.name for o in bpy.context.scene.objects if o.type == 'MESH'])
```

**Generator** (text/image-to-3D or asset download): use
`mcp__blender__generate_hyper3d_model_via_text` / `..._via_images` or
`generate_hunyuan3d_model`, poll `get_hyper3d_status` / `get_hunyuan3d_status` (or
`poll_rodin_job_status` / `poll_hunyuan_job_status`), then
`import_generated_asset` / `import_generated_asset_hunyuan`. Or search/download from a
library: `search_sketchfab_models` + `download_sketchfab_model`, or
`search_polyhaven_assets` + `download_polyhaven_asset`. After import, decimate/clean and
treat the result as the render mesh for §3. (Generators need their backend configured; if
unavailable, build procedurally.)

## 3. Conform names to the ONED convention (the load-bearing step)

Read [NAMING.md](NAMING.md), then for each part: create a `PN##` empty, rename the render
mesh `## Mesh0` and parent it under that empty, and assign a `Material_<i>_<shader>`
material. The leading two-digit index ties `01 Mesh0` to `PN01` — get it right.

```python
import bpy

# index 0-based -> "Material_<index>_<shader>"
MATERIALS = ["FF_ST_OP"]
# one entry per part, in order; "mesh" is the existing object name to adopt
PARTS = [
    {"mesh": "body", "material": 0},
    {"mesh": "lid",  "material": 0},
]

def ensure_material(index, shader):
    name = f"Material_{index}_{shader}"
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat["opennova_shader"] = shader          # opennova_* key per project convention
    return mat

from mathutils import Vector
mats = [ensure_material(i, s) for i, s in enumerate(MATERIALS)]
scene = bpy.context.scene
for i, part in enumerate(PARTS):
    nn = i + 1
    mesh = bpy.data.objects[part["mesh"]]
    mesh.name = f"{nn:02d} Mesh0"
    pn_name = f"PN{nn:02d}"
    pn = bpy.data.objects.get(pn_name) or bpy.data.objects.new(pn_name, None)  # Empty = part node
    if pn.name not in scene.collection.objects:
        scene.collection.objects.link(pn)
    # The part node sits at the part's PIVOT (here its centroid), NOT the world origin —
    # the center marker rides this node, so leaving every PN at origin is wrong.
    bb = [mesh.matrix_world @ Vector(c) for c in mesh.bound_box]
    pn.location = sum(bb, Vector()) / 8.0
    bpy.context.view_layer.update()
    mesh.parent = pn
    mesh.matrix_parent_inverse = pn.matrix_world.inverted()   # keep geometry in place
    mat = mats[part["material"]]
    mesh.data.materials.clear(); mesh.data.materials.append(mat)
    for poly in mesh.data.polygons:
        poly.material_index = 0
print("conformed", [o.name for o in scene.objects])
```

Notes: single-LOD objects need **no** `_lod_index` — omit it and the whole scene exports
as one model. Add `_lod_index` empties only for multi-LOD (see NAMING.md). Material slot
index is global across the object; parts may share a material. Put each `PN##` at that
part's real pivot — for an articulated part that's the hinge/rotation point, not just the
centroid.

## 3.5 Complete the object — centers, attach points, user points, collision, textures

A render mesh alone is not a finished model. Centers, attach points, user points, and
collision volumes are small **helper meshes** (they export via the helper path) — and they
must carry **real positions, not the origin**. Run this after §3 so the `PN##` pivots exist.

```python
import bpy, bmesh
from mathutils import Vector, Matrix
def _cube(name, size):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=size); bm.to_mesh(me); bm.free()
    return me
def _place(ob, world_pos, parent):           # exact world position under a parent
    bpy.context.scene.collection.objects.link(ob)
    ob.parent = parent
    ob.location = world_pos
    ob.matrix_parent_inverse = parent.matrix_world.inverted()

scene = bpy.context.scene
# child subobject -> (parent subobject, child->parent connection point in world space)
PART_PARENT = {1: (0, Vector((0, -0.35, 0.30))), 2: (0, Vector((0, 0.55, 0.21)))}

# Center "_NN center": rides its PN## pivot (parent at PN local origin -> lands at the pivot,
# which §3 placed at the part centroid -- so these are NOT at world origin).
for i in range(len(PARTS)):
    ob = bpy.data.objects.new(f"_{i+1:02d} center", _cube(f"_{i+1:02d} center", 0.03))
    scene.collection.objects.link(ob)
    ob.parent = bpy.data.objects[f"PN{i+1:02d}"]
    ob["opennova_zero_axis"] = True          # static part = no PANM matrix; omit if part-animated

# Attach "~PPx attach": PP = PARENT subobject in the name; Blender-parent = CHILD part;
# position = the child->parent connection point.
seen = {}
for child, (parent, conn) in PART_PARENT.items():
    k = seen.get(parent, 0); seen[parent] = k + 1
    nm = f"~{parent+1:02d}{chr(ord('a')+k)} attach"
    _place(bpy.data.objects.new(nm, _cube(nm, 0.024)), conn, bpy.data.objects[f"PN{child+1:02d}"])

# User points "UP<c>NN <label>": MUST start "UP" (a USR name is DROPPED on ASE->3DI export);
# label after the first space is the engine lookup key; NN = owning subobject; <c> = category.
# ORIENT them: the importer maps a point's local +Z to its facing direction, so an unrotated
# marker faces straight up. FWD aims +Z at the model forward (+Y here). Vehicle seats are
# classified by label (Entity_FindBestSeatSlot): drvr/ctrl = operator, site = passenger,
# UseGun = gunner -- and each seat should face forward so the occupant does.
import math
FWD = (math.radians(-90), 0.0, 0.0)   # local +Z -> world +Y (bow)
UP  = (0.0, 0.0, 0.0)                  # local +Z -> world +Z (up)
USER_POINTS = [   # (name, world pos, rotation, owning PN)
    ("UPd01 drvr0",  Vector((0.0,  0.05,  0.60)), FWD, "PN01"),  # driver, faces forward
    ("UPs01 site0",  Vector((0.0, -0.40,  0.60)), FWD, "PN01"),  # passenger 1
    ("UPs01 site1",  Vector((0.0, -0.78,  0.58)), FWD, "PN01"),  # passenger 2
    ("UPg01 Ground", Vector((0.0,  0.10, -0.36)), UP,  "PN01"),  # entity placement ground point
]
for nm, pos, rot, pn in USER_POINTS:
    ob = bpy.data.objects.new(nm, _cube(nm, 0.04))
    _place(ob, pos, bpy.data.objects[pn])
    ob.rotation_euler = rot

# Collision "<code>NN-colonly": pick a code from NAMING.md (CB = collidable type 1, the
# simplest box-like volume; the semantic meanings are INFERRED, not witnessed). Size it to
# enclose the model; "-occonly" instead routes a volume to occlusion.
pts = []
for mname in ("01 Mesh0", "02 Mesh0", "03 Mesh0"):
    o = bpy.data.objects[mname]; pts += [o.matrix_world @ Vector(c) for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
me = bpy.data.meshes.new("CB01-colonly"); bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
for v in bm.verts:
    v.co = Vector((v.co.x*(hi.x-lo.x), v.co.y*(hi.y-lo.y), v.co.z*(hi.z-lo.z)))
bm.to_mesh(me); bm.free()
_place(bpy.data.objects.new("CB01-colonly", me), (lo + hi) / 2.0, bpy.data.objects["PN01"])
```

Textures: the exporter finds the diffuse image from the material's Principled **Base Color**
link, forces the name to `.TGA`, caps it at 15 chars, and saves it next to the `.ase`
(TARGA_RAW) when `export_textures=True`. So wire an Image Texture node and give the mesh
UVs. `bpy.ops.uv.smart_project` can fail under the MCP exec context (operator poll) — the
operator-free box projection below is reliable:

```python
import bpy
TEX = [   # (render-mesh, material, image name <=11 chars, generated pattern)
    ("01 Mesh0", "Material_0_FF_ST_OP", "hull", 'COLOR_GRID'),
]
def _box_uv(obj, scale=0.3):
    me = obj.data
    if not me.uv_layers: me.uv_layers.new(name="UVMap")
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        ax = max(range(3), key=lambda k: abs(poly.normal[k]))
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            u, v = (co.y, co.z) if ax == 0 else (co.x, co.z) if ax == 1 else (co.x, co.y)
            uv[li].uv = (u * scale + 0.5, v * scale + 0.5)
for obj_name, mat_name, img_name, gtype in TEX:
    _box_uv(bpy.data.objects[obj_name])
    img = bpy.data.images.get(img_name) or bpy.data.images.new(img_name, 256, 256)
    img.generated_type = gtype               # or load a real skin: img = bpy.data.images.load(path)
    mat = bpy.data.materials[mat_name]; mat.use_nodes = True
    nt = mat.node_tree; bsdf = nt.nodes.get("Principled BSDF")
    tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
```

## 4. Verify in Blender (before exporting)

Frame the view before the screenshot — `mcp__blender__get_viewport_screenshot` captures the
3D viewport as-is, so an *unframed* view just shows empty grid. Select all and
`view_selected` through a temp override on the VIEW_3D area, then screenshot:

```python
import bpy
win = bpy.context.window
area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
region = next(r for r in area.regions if r.type == 'WINDOW')
area.spaces[0].shading.type = 'MATERIAL'        # show material colors
with bpy.context.temp_override(window=win, area=area, region=region):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.view3d.view_selected()
```

Then audit names against the classifier's regexes — the audit + `get_scene_info` are the
**authoritative** correctness check; fix here, not downstream:

```python
import bpy, re
PATS = {                                    # name pattern -> required Blender type (None = any)
    "part(PN)":    (r"^PN\d{2}$",            'EMPTY'),
    "render-mesh": (r"^\d{2} Mesh\d+$",      'MESH'),
    "center":      (r"^_\d{2} center$",      'MESH'),
    "attach":      (r"^~\d{2}[a-z] attach$", 'MESH'),
    "collision":   (r".+-colonly$",          'MESH'),
    "occlusion":   (r".+-occonly$",          'MESH'),
    "userpoint":   (r"^(USR|UP.)\d{2}( .+)?$", None),
}
issues = []
for o in bpy.context.scene.objects:
    n = re.sub(r"\.\d{3}$", "", o.name)
    kind = next((k for k, (p, t) in PATS.items()
                 if re.match(p, n) and (t is None or o.type == t)), None)
    if o.type == 'LIGHT':
        kind = kind or "light"
    if kind is None:
        issues.append(f"unclassified: {o.name} ({o.type})")
    # only render meshes need a source material; markers carry marker materials or none
    if kind == "render-mesh" and not any(
            m and re.match(r"^Material_\d+_", m.name) for m in o.data.materials):
        issues.append(f"render mesh {o.name} has no source material")
    print(f"  {kind or '?':12} {o.name}")
print("AUDIT:", "OK" if not issues else issues)
```

Every object must classify to a known kind. Only render meshes need a source material —
centers, attach points, and user points legitimately carry marker materials or none, and
the exporter ignores them.

## 5. Export to ASE

Preferred: the **stub-package** pattern (same as `apps/importer/import_runner.py` —
imports the exporter without running `blender/__init__.py`, so no add-on registration is
needed). Fill `REPO_ROOT` and `OUT` with absolute paths:

```python
import bpy, os, sys, types
REPO_ROOT = r"<ABSOLUTE PATH TO PROJECT ROOT containing blender/>"
OUT       = r"<ABSOLUTE OUTPUT .ase PATH>"
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)
if "blender" not in sys.modules:
    pkg = types.ModuleType("blender")
    pkg.__path__ = [os.path.join(REPO_ROOT, "blender")]
    pkg.__package__ = "blender"; pkg.__spec__ = None
    sys.modules["blender"] = pkg
from blender.ase_exporter import AseExporter
ex = AseExporter()
ex.set_options(export_textures=True, scale=1.0, precision=4)
ex.export_scene(bpy.context.scene, OUT)
print("WROTE", OUT, os.path.getsize(OUT))
```

Alternative (only if the opennova add-on is installed/enabled in this Blender):
`bpy.ops.export_scene.novalogic_ase(filepath=OUT, export_textures=True, scale=1.0, precision=4)`.

Textures save as `TARGA_RAW` beside the `.ase`; each extra LOD writes `<stem>_lod<N>.ase`.
Texture filenames are capped at 15 chars and `.dds`→`.TGA`; keep source image names short.

## 6. Verify the ASE & hand off

`.ase` is ASCII — on the host, `Read`/`Grep` (`\*NODE_NAME|\*NODE_PARENT|\.TGA|BITMAP`) the
written file and confirm:

- each render mesh as `*NODE_NAME "01 Mesh0"` (etc.) with a non-zero `*MESH_NUMFACES`, and
  `*MESH_NUMTVERTEX > 0` if it is textured (UVs exported);
- every material as a `*MATERIAL_NAME "Material_<i>_<shader>"` submaterial under a
  `Scene_Materials_<slot>` parent, textured ones with `*BITMAP "<name>.TGA"`;
- one part node per part (`*NODE_NAME "PN01"` — the exporter keeps the `PN` prefix here);
- a `*NODE_NAME "_NN center"` per part and a `*NODE_NAME "~PPx attach"` per non-root part,
  each with `*NODE_PARENT "<child subobject>"` (the attach links child→parent);
- each user point as `*NODE_NAME "UP<c>NN <label>"` (every one starts `UP`, not `USR`);
- each collision volume as `*NODE_NAME "<code>NN-colonly"` (occlusion `…-occonly`);
- the helpers' `*TM_POS` are at their **real positions**, not `0 0 0` (spot-check the
  centers and user points — all-origin TMs mean the placement step was skipped);
- the `<name>.TGA` files written next to the `.ase` (the §5 log prints `Saved texture …`).

A `WROTE … <bytes>` line with a non-trivial size from §5 is the first signal.

Hand-off (out of scope for this skill): the `.ase` is mesh source — compile it to a
runtime `.3di` by binding it in ONED's **Object** workspace LODs flow (`godot/modtools/object/`).

## Done

Geometry created (procedural or generator); the object is *complete* — parts, render
meshes, centers, attach-point hierarchy, user points, collision, and textured materials with
UVs (§3 + §3.5), every helper at a real position (not the origin); every object conforms to
[NAMING.md](NAMING.md) (regex-audited in §4); the `.ase` plus its `.TGA` textures are
written and the node/material/center/attach/userpoint/collision/bitmap entries verified §6;
and Blender is left in a described state.
