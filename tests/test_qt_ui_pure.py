from __future__ import annotations

from pathlib import Path

from opennova_jobs import ImportRequest, ScanItem


def _items() -> list[ScanItem]:
    return [
        ScanItem(name="M16A2", type="weapon", source_model="m16a2.3di", output_stem="m16a2"),
        ScanItem(name="AK74", type="weapon", source_model="ak74.3di", output_stem="ak74"),
        ScanItem(name="Armry01", type="item", source_model="Armry01.3di", output_stem="Armry01"),
    ]


def test_filtered_items_searches_type_name_source_and_stem() -> None:
    from opennova_qt_ui.filtering import filtered_items

    assert [item.name for item in filtered_items(_items(), "weapon", "")] == ["AK74", "M16A2"]
    assert [item.name for item in filtered_items(_items(), "all", "Arm01")] == ["Armry01"]
    assert [item.name for item in filtered_items(_items(), "all", "ak74.3di")] == ["AK74"]


def test_preferences_round_trip(tmp_path: Path) -> None:
    from opennova_qt_ui.preferences import load_from_path, save_to_path

    target = tmp_path / "prefs.json"
    prefs = {
        "window_geometry": "1000x700",
        "last_game_dir": " /game ",
        "last_output_dir": "/out",
        "last_loose_file_dir": "/loose",
    }
    assert save_to_path(target, prefs)
    assert load_from_path(target) == {
        "window_geometry": "1000x700",
        "last_game_dir": "/game",
        "last_output_dir": "/out",
        "last_loose_file_dir": "/loose",
    }


def test_collision_resolver_can_skip_existing_outputs(tmp_path: Path) -> None:
    from opennova_qt_ui.collisions import CollisionDecision, resolve_output_collisions

    existing = tmp_path / "m16_1st"
    existing.mkdir()
    requests = [
        ImportRequest.for_definition(
            base_dir=str(tmp_path),
            item_name="WPN_M16",
            item_type="weapon",
            output_root=str(tmp_path),
            output_stem="m16_1st",
        ),
        ImportRequest.for_definition(
            base_dir=str(tmp_path),
            item_name="WPN_AK47",
            item_type="weapon",
            output_root=str(tmp_path),
            output_stem="ak47_1st",
        ),
    ]

    choice = resolve_output_collisions(requests, CollisionDecision.SKIP)

    assert not choice.canceled
    assert choice.skipped_count == 1
    assert [request.output_stem for request in choice.requests] == ["ak47_1st"]
