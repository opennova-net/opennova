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
  bind LW meshes to part bones; generated actions are intentionally not faked
  until SAF/KSA pose math is fully validated.
- Modern GPP skin binding now remaps render-only skin parts that are absent from
  a matching BAD to their nearest animated parent. This fixes the Aidid case
  where source bone 19 was previously bound to importer-created `root_motion`
  instead of the head-child parent chain.

## Known LW RE Gaps

- `.SAF1` frame pose semantics are only partially understood. We know the disk
  frame is 112 bytes and the runtime frame is 88 bytes, with 15 four-byte bone
  records transformed by the DFLW loader. The exact per-byte rotation order,
  signedness, scale, and parent-space composition still need a full pose oracle.
- `.KSA` file layout is characterized enough to address slots and frame counts,
  but the slot table's third dword is not authoritative. The engine addresses
  KSA by table index; the stored value may be stale runtime metadata.
- The LW skeleton/subobject relationship needs validation across more character
  models. `badguy.3di` has 15 render subobjects, while `player01.ksa` provides
  255 animation slots whose frames appear to target a 15-bone runtime pose.
- Move blending and velocity behavior from `.ANM` is not yet imported. Slot
  lookup, `override`, and velocity values are parsed, but Blender/Max action
  generation still needs the runtime state-machine rules.
- `.ACA` plus loose `.SAF` does not cover every `enemy00.anm` slot for this
  fixture; `PLAYER01.KSA` does. KSA should remain primary for game-accurate
  imports until the source/packed relationship is fully reconciled.

## Next RE Targets

- Use DFLW `sub_44A0B0` and the downstream runtime pose evaluator to derive the
  exact SAF/KSA 88-byte frame-to-bone-transform math.
- Capture a small set of in-game/reference poses for `badguy + enemy00` slots
  0, 18, and 61, then assert Blender action samples against those poses.
- Verify whether LW v8 differs structurally or only by header/count layout
  before enabling it; v8 is currently detected and rejected.
- Extend LW fixture coverage beyond `BADGUY.3DI` to catch material selector,
  hidden surface, and subobject hierarchy variants.
