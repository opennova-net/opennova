"""The ASE exporter emits ONLY BN## bones as helper nodes.

The rig IS the BN## rows: node_id = ##-1 is the .bad channel row and the 3DI subobject row.
Non-BN bones (IK targets, controls, authoring helpers) must stay invisible to the export --
the same rule anim_exporter._sorted_export_pose_bones applies to the .bad -- so a scene can
keep its rig controls without leaking them into the .ase as inert helper nodes.

bpy is not importable under pytest, so this pins the rule at source level in the repo's
existing contract-test style (see test_blender_export_menu_contract.py).
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_only_bn_bones_export_as_helper_nodes() -> None:
    src = _read("blender/ase_exporter.py")
    assert 'if not (bone.name.startswith("BN") and len(bone.name) >= 4 and bone.name[2:4].isdigit()):' in src
    assert "bone_list.append((o, bone))" in src


def test_anim_exporter_applies_the_same_bn_rule() -> None:
    src = _read("blender/anim_exporter.py")
    assert 'pose_bones = [pb for pb in armature_obj.pose.bones if pb.name.upper().startswith("BN")]' in src
