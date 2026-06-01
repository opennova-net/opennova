# Land Warrior 3DI/SAF Integration Status

This tracks the Delta Force: Land Warrior character path used by the fixture
entry:

```
begin "Change me please!!!"
  id 105130
  type person
  graphic badguy
  chr_file player01
  anim_def enemy00
end
```

## Implemented

- DFLW SCR v1 key is `0x01234567`. `AssetResolver` auto-selects it for DFLW-like
  roots and still keeps the JO/DFX2 key for other profiles.
- `items.def` parsing now preserves `chr_file`, so `badguy.3di` can resolve
  `player01` character animation data and `enemy00` move definitions by item id
  `105130`.
- LW `.3di` v10 is parsed natively and dispatched through `threedi_ir_read`.
  `BADGUY.3DI` parses as 4 LODs, 7 materials, 15 LOD0 subobjects, and 492 LOD0
  faces. The IR converter emits visible part/material primitives and material
  texture names for importer use.
- LW animation authoring/runtime metadata is parsed in Python:
  `.ANM` move name -> slot/velocity/override, `.ACA` slot -> `.SAF`, `.SAF1`
  frame/bone records, and packed `.KSA` slot tables.
- Definition import plans now attach an LW animation context for
  `anim_def + chr_file` items. Blender and Max build a rigid part armature and
  bind LW meshes to part bones.
- LW `.KSA` slots are sampled in memory into the shared animation IR. Blender
  now emits keyed actions/NLA tracks for every `.ANM` move and Max applies the
  same sampled clips without writing intermediary `.BAD` files.
- DFLW `sub_44A0B0` confirms the loose `.SAF1` loader converts 112-byte disk
  frames into 88-byte runtime frames: 15 four-byte part records followed by a
  28-byte tail. The first part byte is biased by `+0x80`, bytes 1-2 are copied,
  and byte 3 is zeroed. The tail is derived from float header fields using
  scales `85.333336` and `1365.3334`, with tail field 4 clamped to at most
  `-30`.
- DFLW `sub_4A0C00` confirms the LW runtime pose path loops 15 part records,
  treats bytes 0-2 as 8-bit turn angles, and computes part transforms from
  record rotations plus the model subobject parent hierarchy. Child positions
  are parent-rotation transformed from rest offsets; child rotations are stored
  as authored/world rotations, matching the later BAD sampling convention.
- Modern GPP skin binding now remaps render-only skin parts that are absent from
  a matching BAD to their nearest animated parent. This fixes the Aidid case
  where source bone 19 was previously bound to importer-created `root_motion`
  instead of the head-child parent chain.

## Known LW RE Gaps

- Runtime aim/weapon adjustments in `sub_4A0C00` still need validation. The
  importer currently samples raw KSA part rotations and intentionally skips the
  object-heading and arm-clamp paths that the game applies at runtime.
- `.KSA` file layout is characterized enough to address slots and frame counts.
  The slot table's third dword is loop target metadata used when a slot reaches
  the end; it still needs broader validation across characters.
- The LW skeleton/subobject relationship needs validation across more character
  models. `badguy.3di` has 15 render subobjects, while `player01.ksa` provides
  255 animation slots whose frames appear to target a 15-bone runtime pose.
- Move blending and velocity behavior from `.ANM` is not yet imported. Slot
  lookup, `override`, and velocity values are parsed, but Blender/Max currently
  expose direct clips rather than the runtime state-machine rules.
- `.ACA` plus loose `.SAF` does not cover every `enemy00.anm` slot for this
  fixture; `PLAYER01.KSA` does. KSA should remain primary for game-accurate
  imports until the source/packed relationship is fully reconciled.

## Next RE Targets

- Capture a small set of in-game/reference poses for `badguy + enemy00` slots
  0, 18, and 61, then assert Blender action samples against those poses.
- RE the object-heading and special arm adjustment paths around `sub_4A0D50`
  and `sub_4A0DC2` to decide whether they belong in authored clip import,
  runtime preview controls, or both.
- Verify whether LW v8 differs structurally or only by header/count layout
  before enabling it; v8 is currently detected and rejected.
- Extend LW fixture coverage beyond `BADGUY.3DI` to catch material selector,
  hidden surface, and subobject hierarchy variants.
