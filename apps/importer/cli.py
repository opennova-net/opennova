"""CLI interface for the standalone importer."""
from __future__ import annotations

import argparse
import logging
import sys

from opennova_jobs import ImportOptions, ImportRequest

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
log = logging.getLogger(__name__)


_OPTION_FIELDS = (
    "import_animations",
    "import_collisions",
    "import_occlusion",
    "import_lights",
    "import_arms",
    "write_blend",
    "write_3dp",
    "write_ase",
    "write_glb",
    "write_fbx",
    "write_max",
)


def options_from_args(args: argparse.Namespace) -> ImportOptions:
    defaults = ImportOptions()
    values = {}
    for field in _OPTION_FIELDS:
        value = getattr(args, field, None)
        values[field] = getattr(defaults, field) if value is None else value
    return ImportOptions(**values)


def _add_import_options(parser: argparse.ArgumentParser, *, include_def_only: bool = True) -> None:
    parser.set_defaults(
        import_animations=None,
        import_collisions=None,
        import_occlusion=None,
        import_lights=None,
        import_arms=None,
        write_blend=None,
        write_3dp=None,
        write_ase=None,
        write_glb=None,
        write_fbx=None,
        write_max=None,
    )
    if include_def_only:
        parser.add_argument("--no-animations", dest="import_animations", action="store_false",
                            help="Skip animation import")
    parser.add_argument("--no-collisions", dest="import_collisions", action="store_false",
                        help="Skip collision geometry")
    parser.add_argument("--no-occlusion", dest="import_occlusion", action="store_false",
                        help="Skip occlusion geometry")
    parser.add_argument("--no-lights", dest="import_lights", action="store_false",
                        help="Skip scene lights")
    if include_def_only:
        parser.add_argument("--arms", dest="import_arms", action="store_true",
                            help="Import weapon arms model")
        parser.add_argument("--no-arms", dest="import_arms", action="store_false",
                            help="Skip weapon arms model")
    parser.add_argument("--no-blend", dest="write_blend", action="store_false",
                        help="Do not write .blend scene files")
    parser.add_argument("--no-3dp", dest="write_3dp", action="store_false",
                        help="Do not write .3dp object workspace files")
    parser.add_argument("--no-ase", dest="write_ase", action="store_false",
                        help="Do not write .ase files")
    parser.add_argument("--glb", dest="write_glb", action="store_true",
                        help="Write .glb files via Blender")
    parser.add_argument("--fbx", dest="write_fbx", action="store_true",
                        help="Write .fbx files via Blender")
    parser.add_argument("--max", dest="write_max", action="store_true",
                        help="Write .max scene files via 3ds Max")
    parser.add_argument("--no-max", dest="write_max", action="store_false",
                        help="Do not write .max scene files")


def cmd_scan(args: argparse.Namespace) -> int:
    from apps.importer.import_runner import scan_directory_result

    result = scan_directory_result(args.dir)
    if not result.ok:
        print(f"Scan failed: {result.error}", file=sys.stderr)
        return 1
    if not result.items:
        print("No items found.")
        return 1
    for item in sorted(result.items, key=lambda x: (x["type"], x["name"].casefold())):
        print(f"  [{item['type']:6}] {item['name']}")
    print(f"\n{len(result.items)} item(s) found.")
    return 0


def cmd_import(args: argparse.Namespace) -> int:
    from opennova_blender import StandaloneBackend

    request = ImportRequest.for_definition(
        base_dir=args.dir,
        item_name=args.item,
        item_type=args.type,
        output_root=args.output,
        options=options_from_args(args),
    )
    backend = StandaloneBackend()
    try:
        result = backend.execute(request)
    finally:
        backend.shutdown()
    if not result.ok:
        log.error("%s failed: %s", request.label, result.error)
        return 1
    log.info("%s", result.message)
    return 0


def cmd_import_loose(args: argparse.Namespace) -> int:
    from opennova_blender import StandaloneBackend

    request = ImportRequest.for_loose(
        threedi_path=args.file,
        output_root=args.output,
        output_stem=args.name or "",
        base_dir=args.asset_dir or "",
        options=options_from_args(args),
    )
    backend = StandaloneBackend()
    try:
        result = backend.execute(request)
    finally:
        backend.shutdown()
    if not result.ok:
        log.error("%s failed: %s", request.label, result.error)
        return 1
    log.info("%s", result.message)
    return 0


def cmd_export_all(args: argparse.Namespace) -> int:
    from apps.importer.import_runner import scan_directory_result
    from opennova_blender import StandaloneBackend

    scan = scan_directory_result(args.dir)
    if not scan.ok:
        print(f"Scan failed: {scan.error}", file=sys.stderr)
        return 1

    items = scan.items
    if args.type:
        items = [item for item in items if item["type"] == args.type]
    if not items:
        print("No items found.")
        return 1

    options = options_from_args(args)
    requests = [
        ImportRequest.for_definition(
            base_dir=args.dir,
            item_name=item["name"],
            item_type=item["type"],
            output_root=args.output,
            output_stem=item["output_stem"],
            options=options,
        )
        for item in sorted(items, key=lambda x: (x["type"], x["name"].casefold()))
    ]

    success = 0
    failed = 0
    backend = StandaloneBackend()
    try:
        log.info(
            "Dispatching %d jobs across %d Blender worker(s)...",
            len(requests),
            backend.max_workers,
        )
        for result in backend.execute_batch(requests):
            if result.ok:
                success += 1
                log.info("Done: %s", result.request.label)
            else:
                failed += 1
                log.error("FAILED: %s: %s", result.request.label, result.error)
    finally:
        backend.shutdown()

    print(f"\nDone: {success} succeeded, {failed} failed.")
    return 0 if failed == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="onimport")
    sub = parser.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="List available items in a game directory")
    p_scan.add_argument("--dir", required=True, help="Game asset directory")
    p_scan.set_defaults(func=cmd_scan)

    p_import = sub.add_parser("import", help="Import a single item")
    p_import.add_argument("--dir", required=True, help="Game asset directory")
    p_import.add_argument("--item", required=True, help="Item name")
    p_import.add_argument("--type", required=True, choices=["weapon", "item"])
    p_import.add_argument("--output", required=True, help="Output root directory")
    _add_import_options(p_import)
    p_import.set_defaults(func=cmd_import)

    p_loose = sub.add_parser("import-loose", help="Import a standalone .3di file")
    p_loose.add_argument("--file", required=True, help="Path to .3di file")
    p_loose.add_argument("--output", required=True, help="Output root directory")
    p_loose.add_argument("--name", default=None, help="Output file stem")
    p_loose.add_argument("--asset-dir", default=None,
                         help="Optional texture/material search directory")
    _add_import_options(p_loose, include_def_only=False)
    p_loose.set_defaults(func=cmd_import_loose)

    p_all = sub.add_parser("export-all", help="Import all items in a game directory")
    p_all.add_argument("--dir", required=True, help="Game asset directory")
    p_all.add_argument("--output", required=True, help="Output root directory")
    p_all.add_argument("--type", choices=["weapon", "item"], default=None,
                       help="Filter by type")
    _add_import_options(p_all)
    p_all.set_defaults(func=cmd_export_all)

    return parser


def run_cli(args: list[str]) -> None:
    parser = build_parser()
    parsed = parser.parse_args(args)
    sys.exit(parsed.func(parsed))
