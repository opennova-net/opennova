"""Tests for the filtering helpers used by the Qt importer dialog."""
from __future__ import annotations

from opennova_jobs import ScanItem


def _items() -> list[ScanItem]:
    return [
        ScanItem(name="M16A2", type="weapon", source_model="m16a2.3di", output_stem="m16a2"),
        ScanItem(name="AK74", type="weapon", source_model="ak74.3di", output_stem="ak74"),
        ScanItem(name="Armry01", type="item", source_model="Armry01.3di", output_stem="Armry01"),
    ]


def test_filtered_items_no_filter_returns_sorted_all() -> None:
    from opennova_qt_ui.filtering import filtered_items

    out = filtered_items(_items(), "all", "")
    # sorted by (type, name) casefold — item before weapon, then alpha
    keys = [(getattr(i, "type", "").casefold(), getattr(i, "name", "").casefold()) for i in out]
    assert keys == sorted(keys)


def test_filtered_items_type_filter() -> None:
    from opennova_qt_ui.filtering import filtered_items

    out = filtered_items(_items(), "weapon", "")
    assert {item.type for item in out} == {"weapon"}


def test_filtered_items_search_substring() -> None:
    from opennova_qt_ui.filtering import filtered_items

    out = filtered_items(_items(), "all", "m16")
    assert [item.name for item in out] == ["M16A2"]


def test_filtered_items_search_subsequence() -> None:
    from opennova_qt_ui.filtering import filtered_items

    out = filtered_items(_items(), "all", "Arm01")
    assert any(item.name == "Armry01" for item in out)
