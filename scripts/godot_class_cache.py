#!/usr/bin/env python3
"""Whether the Godot project's global class cache is stale (stdlib only).

A headless run (GUT, a probe) resolves a `class_name` only through the class cache the editor's
import writes (godot/.godot/global_script_class_cache.cfg). After a base change brings a script with
a new `class_name`, or moves one, every script naming that class fails to parse until the project is
imported again, and GUT drops those scripts. This compares the cache with the `class_name` lines of
the project's scripts: exit 0 when it lists every class where its script declares it and names no
script that is gone, exit 1 (printing why) when it does not or there is none.

    python scripts/godot_class_cache.py godot      # 0 fresh, 1 stale
"""

from __future__ import annotations

import pathlib
import re
import sys

CLASS_NAME = re.compile(r"^class_name\s+([A-Za-z_][A-Za-z0-9_]*)", re.MULTILINE)
ENTRY = re.compile(r"\{(.*?)\}", re.DOTALL)
FIELD_CLASS = re.compile(r'"class":\s*&"([^"]+)"')
FIELD_PATH = re.compile(r'"path":\s*"([^"]+)"')


def declared_classes(project: pathlib.Path) -> dict[str, str]:
    """Each class_name a script of the project declares, by its res:// path."""
    classes: dict[str, str] = {}
    for script in project.rglob("*.gd"):
        relative = script.relative_to(project).as_posix()
        if relative.startswith(".godot/"):
            continue
        match = CLASS_NAME.search(script.read_text(encoding="utf-8", errors="replace"))
        if match:
            classes[match.group(1)] = "res://" + relative
    return classes


def cached_classes(cache: pathlib.Path) -> dict[str, str]:
    """Each class the cache lists, by the path it names."""
    classes: dict[str, str] = {}
    for entry in ENTRY.findall(cache.read_text(encoding="utf-8", errors="replace")):
        name = FIELD_CLASS.search(entry)
        path = FIELD_PATH.search(entry)
        if name and path:
            classes[name.group(1)] = path.group(1)
    return classes


def stale_reasons(project: pathlib.Path) -> list[str]:
    cache = project / ".godot" / "global_script_class_cache.cfg"
    if not cache.is_file():
        return [f"no class cache at {cache}"]
    declared = declared_classes(project)
    cached = cached_classes(cache)
    reasons = []
    for name, path in sorted(declared.items()):
        if name not in cached:
            reasons.append(f"{name} ({path}) is not in the cache")
        elif cached[name] != path:
            reasons.append(f"{name} is at {path}, the cache says {cached[name]}")
    for name, path in sorted(cached.items()):
        if name not in declared and path.startswith("res://") and not (project / path[len("res://"):]).is_file():
            reasons.append(f"{name}'s script {path} is gone")
    return reasons


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    reasons = stale_reasons(pathlib.Path(argv[1]))
    for reason in reasons:
        print(f"class cache stale: {reason}")
    return 1 if reasons else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
