"""Pure filtering/search helpers for the Qt importer dialog."""
from __future__ import annotations

from pathlib import Path
from typing import Iterable


def filtered_items(items: Iterable, type_filter: str, search_text: str = "") -> list:
    normalized = _normalize_type_filter(type_filter)
    terms = _search_terms(search_text)
    filtered = [
        item
        for item in items
        if normalized == "all" or getattr(item, "type", "") == normalized
    ]
    if terms:
        filtered = [item for item in filtered if _matches_search(item, terms)]
    return sorted(
        filtered,
        key=lambda item: (
            getattr(item, "type", "").casefold(),
            getattr(item, "name", "").casefold(),
        ),
    )


def _matches_search(item, terms: list[str]) -> bool:
    fields = [str(field).casefold() for field in _search_fields(item)]
    normalized_fields = [_normalize_search_text(field) for field in fields]
    return all(
        any(
            term in field or _is_ordered_subsequence(term, normalized_field)
            for field, normalized_field in zip(fields, normalized_fields)
        )
        for term in terms
    )


def _search_fields(item) -> list[str]:
    source = getattr(item, "source_model", "")
    stem = getattr(item, "output_stem", "") or Path(source).stem
    return [
        getattr(item, "type", ""),
        getattr(item, "name", ""),
        source,
        stem,
        f"[{getattr(item, 'type', '')}] {getattr(item, 'name', '')} -> {stem}",
    ]


def _search_terms(search_text: str) -> list[str]:
    return [
        normalized
        for term in search_text.strip().split()
        for normalized in [_normalize_search_text(term)]
        if normalized
    ]


def _normalize_search_text(value: object) -> str:
    return "".join(ch for ch in str(value).casefold() if ch.isalnum())


def _is_ordered_subsequence(needle: str, haystack: str) -> bool:
    if not needle:
        return True
    if len(needle) > len(haystack):
        return False
    if len(needle) == 1:
        return needle in haystack
    max_span = max(len(needle) + 3, int(len(needle) * 1.6))
    for start, ch in enumerate(haystack):
        if ch != needle[0]:
            continue
        pos = 1
        for end in range(start + 1, len(haystack)):
            if haystack[end] == needle[pos]:
                pos += 1
                if pos == len(needle):
                    if end - start + 1 <= max_span:
                        return True
                    break
    return False


def _normalize_type_filter(type_filter: str) -> str:
    value = (type_filter or "all").strip().casefold()
    return value if value in {"all", "weapon", "item"} else "all"
