"""CLI interface for the standalone importer.

Usage:
    python -m apps.importer scan --dir /path/to/game
    python -m apps.importer import --dir /path/to/game --item M16A2 --type weapon --output /out
    python -m apps.importer import-loose --file /path/to/model.3di --output /out
    python -m apps.importer export-all --dir /path/to/game --output /out [--type weapon|item]
"""
import argparse
import logging
import sys
import os

logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
log = logging.getLogger(__name__)


def cmd_scan(args) -> int:
    from apps.importer.import_runner import scan_directory
    items = scan_directory(args.dir)
    if not items:
        print("No items found.")
        return 1
    for item in sorted(items, key=lambda x: x["name"]):
        print(f"  [{item['type']:6}] {item['name']}")
    print(f"\n{len(items)} item(s) found.")
    return 0


def cmd_import(args) -> int:
    from apps.importer import bpy_session
    from apps.importer.import_runner import run_import

    bpy_session.init_headless()
    ok = run_import(
        base_dir=args.dir,
        item_name=args.item,
        item_type=args.type,
        output_dir=args.output,
    )
    return 0 if ok else 1


def cmd_import_loose(args) -> int:
    from apps.importer import bpy_session
    from apps.importer.import_runner import run_loose_import

    bpy_session.init_headless()
    ok = run_loose_import(
        threedi_path=args.file,
        output_dir=args.output,
        output_stem=args.name,
    )
    return 0 if ok else 1


def cmd_export_all(args) -> int:
    from apps.importer import bpy_session
    from apps.importer.import_runner import scan_directory, run_import

    items = scan_directory(args.dir)
    if not items:
        print("No items found.")
        return 1

    # Optional type filter
    if args.type:
        items = [i for i in items if i["type"] == args.type]

    bpy_session.init_headless()

    success = 0
    failed = 0
    for item in items:
        out_dir = os.path.join(args.output, item["name"])
        log.info("Importing %s (%s) → %s", item["name"], item["type"], out_dir)
        try:
            from apps.importer import bpy_session as bs
            bs.new_scene()
            ok = run_import(
                base_dir=args.dir,
                item_name=item["name"],
                item_type=item["type"],
                output_dir=out_dir,
            )
        except Exception as exc:
            log.error("  FAILED: %s", exc)
            ok = False

        if ok:
            success += 1
        else:
            failed += 1

    print(f"\nDone: {success} succeeded, {failed} failed.")
    return 0 if failed == 0 else 1


def run_cli(args: list[str]) -> None:
    parser = argparse.ArgumentParser(prog="apps.importer")
    sub = parser.add_subparsers(dest="command", required=True)

    # scan
    p_scan = sub.add_parser("scan", help="List available items in a game directory")
    p_scan.add_argument("--dir", required=True, help="Game asset directory")

    # import
    p_import = sub.add_parser("import", help="Import a single item")
    p_import.add_argument("--dir",    required=True, help="Game asset directory")
    p_import.add_argument("--item",   required=True, help="Item name")
    p_import.add_argument("--type",   required=True, choices=["weapon", "item"])
    p_import.add_argument("--output", required=True, help="Output directory")

    # import-loose
    p_loose = sub.add_parser("import-loose", help="Import a standalone .3di file")
    p_loose.add_argument("--file",   required=True, help="Path to .3di file")
    p_loose.add_argument("--output", required=True, help="Output directory")
    p_loose.add_argument("--name",   default=None,  help="Output file stem (default: derived from input filename)")

    # export-all
    p_all = sub.add_parser("export-all", help="Import all items in a game directory")
    p_all.add_argument("--dir",    required=True, help="Game asset directory")
    p_all.add_argument("--output", required=True, help="Output root directory")
    p_all.add_argument("--type",   choices=["weapon", "item"], default=None,
                       help="Filter by type (default: all)")

    parsed = parser.parse_args(args)

    cmds = {
        "scan":         cmd_scan,
        "import":       cmd_import,
        "import-loose": cmd_import_loose,
        "export-all":   cmd_export_all,
    }
    sys.exit(cmds[parsed.command](parsed))
