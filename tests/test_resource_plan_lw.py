from __future__ import annotations

from pathlib import Path

from pyopennova.asset_resolver import AssetResolver
from pyopennova.resource_plan import resolve_definition_import


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "lw" / "dflw" / "badguy"


def test_badguy_item_id_resolves_lw_model_and_animation_context() -> None:
    with AssetResolver(str(FIXTURE)) as resolver:
        plan = resolve_definition_import(
            str(FIXTURE),
            "Change me please!!!",
            "item",
            resolver,
            item_id=105130,
        )

    assert plan is not None
    assert plan.item_name == "Change me please!!!"
    assert plan.item_id == 105130
    assert plan.models[0].source_name == "badguy.3di"
    assert plan.animation_context is not None
    assert plan.animation_context.kind == "lw"
    assert plan.animation_context.anim_name == "enemy00"
    assert plan.animation_context.chr_name == "player01"
    assert plan.animation_context.ksa.slots[18].frame_count == 140
