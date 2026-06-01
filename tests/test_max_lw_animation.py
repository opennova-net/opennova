from __future__ import annotations

from pyopennova.animation_build import SampledBoneFrame, SampledFrame, quat_to_matrix
from opennova_max.animation import _frame_assignments


def test_lw_frame_assignments_use_sampled_world_rotation_rows() -> None:
    node = object()
    world_rotation = (0.7071067811865476, 0.0, 0.0, 0.7071067811865476)
    bone = SampledBoneFrame(
        bone_index=0,
        name="BN01",
        parent_index=-1,
        world_rotation=world_rotation,
        world_position=(1.0, 2.0, 3.0),
        local_rotation=world_rotation,
        local_position=(1.0, 2.0, 3.0),
        source_rotation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    frame = SampledFrame(frame_index=0, frame=1, bones=(bone,))

    assignments = _frame_assignments(frame, (node,), source_format="lw")

    assert assignments == [(node, quat_to_matrix(world_rotation), (1.0, 2.0, 3.0))]
