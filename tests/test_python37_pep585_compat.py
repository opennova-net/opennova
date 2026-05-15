"""Catches PEP 585 generics (`list[X]`, `dict[X, Y]`, ...) used at runtime
in files that ship to 3ds Max's bundled Python 3.7.

Any .py file in a host-shipped package that contains a PEP 585 generic
must also have `from __future__ import annotations` so the annotation is
stringified at class/function-definition time. Without that guard,
Python 3.7 raises `TypeError: 'type' object is not subscriptable` on
module import.

Originally added after `pyopennova/threedi_ffi.py:46` shipped without the
guard in the 0.1.47 MZP and bricked the Max 2022 importer.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]

# Packages that ship into Max 2022 (Python 3.7) via the MZP bundle.
# Anything imported from these (directly or transitively) must be
# 3.7-compatible.
SHIPPED_PACKAGES = (
    "pyopennova",
    "opennova_max",
    "opennova_jobs",
    "opennova_qt_ui",
)

# Matches PEP 585 subscript syntax on builtin generic types. Excludes
# typing.Tuple / typing.List / typing.Dict (capitalized: those are
# subscriptable in 3.7).
PEP585_PATTERN = re.compile(r"\b(list|dict|tuple|set|type|frozenset)\[")


def _python_files(package_dir: Path):
    for path in sorted(package_dir.rglob("*.py")):
        if "__pycache__" in path.parts:
            continue
        yield path


def _shipped_files():
    for pkg in SHIPPED_PACKAGES:
        pkg_dir = REPO_ROOT / pkg
        if not pkg_dir.is_dir():
            continue
        yield from _python_files(pkg_dir)


@pytest.mark.parametrize(
    "path",
    list(_shipped_files()),
    ids=lambda p: str(p.relative_to(REPO_ROOT)).replace("\\", "/"),
)
def test_pep585_subscripts_have_future_annotations_guard(path: Path) -> None:
    """If a shipped file uses PEP 585 generic subscript, it must have
    `from __future__ import annotations` for Python 3.7 compatibility.
    """
    text = path.read_text(encoding="utf-8")
    if not PEP585_PATTERN.search(text):
        return
    assert "from __future__ import annotations" in text, (
        f"{path.relative_to(REPO_ROOT)} uses PEP 585 generic subscript "
        f"(list[X] / dict[X,Y] / tuple[X,...] / etc.) but does not declare "
        f"`from __future__ import annotations`. This will crash on Python 3.7 "
        f"(3ds Max 2022's bundled Python) with `TypeError: 'type' object is "
        f"not subscriptable`."
    )
