"""Pyopennova-only CLI for the OpenNova Importer.

Writes OpenNova-native outputs (ASE, 3DP/3DA, textures) directly from the
3DI3. No Blender, no subprocess pool, sequential.
"""
from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path

from opennova_jobs import ImportOptions, ImportRequest


logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
log = logging.getLogger(__name__)


_OPTION_FIELDS = (
    "import_collisions",
    "import_occlusion",
    "import_lights",
    "write_3dp",
    "write_ase",
    "copy_textures",
)


def options_from_args(args: argparse.Namespace) -> ImportOptions:
    defaults = ImportOptions(
        import_animations=False,
        import_arms=False,
        write_blend=False,
        write_max=False,
    )
    values = {}
    for field in _OPTION_FIELDS:
        value = getattr(args, field, None)
        values[field] = getattr(defaults, field) if value is None else value
    return ImportOptions(
        import_animations=False,
        import_arms=False,
        write_blend=False,
        write_max=False,
        import_collisions=values["import_collisions"],
        import_occlusion=values["import_occlusion"],
        import_lights=values["import_lights"],
        write_3dp=values["write_3dp"],
        write_ase=values["write_ase"],
        copy_textures=values["copy_textures"],
    )


def _add_options(parser: argparse.ArgumentParser) -> None:
    parser.set_defaults(
        import_collisions=None,
        import_occlusion=None,
        import_lights=None,
        write_3dp=None,
        write_ase=None,
        copy_textures=None,
    )
    parser.add_argument("--no-collisions", dest="import_collisions", action="store_false")
    parser.add_argument("--no-occlusion", dest="import_occlusion", action="store_false")
    parser.add_argument("--no-lights", dest="import_lights", action="store_false")
    parser.add_argument("--no-3dp", dest="write_3dp", action="store_false")
    parser.add_argument("--no-ase", dest="write_ase", action="store_false")
    parser.add_argument("--no-textures", dest="copy_textures", action="store_false")


def cmd_scan(args: argparse.Namespace) -> int:
    from pyopennova.scan import scan_definitions

    result = scan_definitions(args.dir)
    if not result.ok:
        print(f"Scan failed: {result.error}", file=sys.stderr)
        return 1
    if not result.items:
        print("No items found.")
        return 1
    for item in sorted(result.items, key=lambda x: (x.type, x.name.casefold())):
        print(f"  [{item.type:6}] {item.name}")
    print(f"\n{len(result.items)} item(s) found.")
    return 0


def cmd_import_loose(args: argparse.Namespace) -> int:
    return _execute_loose(
        threedi_path=args.file,
        output_root=args.output,
        output_stem=args.name or "",
        asset_dir=args.asset_dir or "",
        options=options_from_args(args),
    )


def cmd_import(args: argparse.Namespace) -> int:
    return _execute_definition(
        base_dir=args.dir,
        item_name=args.item,
        item_type=args.type,
        output_root=args.output,
        output_stem=args.name or "",
        options=options_from_args(args),
    )


def cmd_export_all(args: argparse.Namespace) -> int:
    from pyopennova.scan import scan_definitions

    scan = scan_definitions(args.dir)
    if not scan.ok:
        print(f"Scan failed: {scan.error}", file=sys.stderr)
        return 1
    items = scan.items
    if args.type:
        items = [item for item in items if item.type == args.type]
    if not items:
        print("No items found.")
        return 1

    options = options_from_args(args)
    success = 0
    failed = 0
    for item in sorted(items, key=lambda x: (x.type, x.name.casefold())):
        rc = _execute_definition(
            base_dir=args.dir,
            item_name=item.name,
            item_type=item.type,
            output_root=args.output,
            output_stem=item.output_stem,
            options=options,
        )
        if rc == 0:
            success += 1
        else:
            failed += 1
    print(f"\nDone: {success} succeeded, {failed} failed.")
    return 0 if failed == 0 else 1


def _execute_loose(
    *,
    threedi_path: str,
    output_root: str,
    output_stem: str,
    asset_dir: str,
    options: ImportOptions,
) -> int:
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.texture_outputs import copy_model_textures
    from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3

    name = output_stem or Path(threedi_path).stem
    output_dir = os.path.join(output_root, name)
    os.makedirs(output_dir, exist_ok=True)

    base_dir = asset_dir or str(Path(threedi_path).parent)
    ir = read_model_3di3(threedi_path)
    try:
        with AssetResolver(base_dir) as resolver:
            written = write_host_neutral_outputs(
                ir,
                output_dir,
                name,
                write_ase=options.write_ase,
                write_3dp=options.write_3dp,
                include_collisions=options.import_collisions,
                include_occlusion=options.import_occlusion,
                include_lights=options.import_lights,
                collision_lod_index=None,
            )
            if options.copy_textures:
                copy_model_textures(ir, output_dir, resolver=resolver)
        log.info("loose %s -> %s (%d files)", name, output_dir, len(written))
        return 0
    except Exception as exc:
        log.error("loose %s failed: %s", name, exc, exc_info=True)
        return 1
    finally:
        free_model_3di3(ir)


def _execute_definition(
    *,
    base_dir: str,
    item_name: str,
    item_type: str,
    output_root: str,
    output_stem: str,
    options: ImportOptions,
) -> int:
    from pyopennova import asset_resolver as ar
    from pyopennova import bad_ffi
    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.texture_outputs import copy_model_textures
    from pyopennova.threedi_ffi import free_model_3di3, read_model_3di3
    from pyopennova.resource_plan import resolve_definition_import

    with ar.AssetResolver(base_dir) as resolver:
        plan = resolve_definition_import(
            base_dir=base_dir,
            item_name=item_name,
            item_type=item_type,
            resolver=resolver,
            import_arms=False,
            import_animations=False,
            output_name=output_stem,
        )
        if plan is None:
            log.error("Could not find %s '%s'", item_type, item_name)
            return 1

        project_dir = os.path.join(output_root, plan.export_name)
        os.makedirs(project_dir, exist_ok=True)

        bad_file = None
        if plan.reset_bad_path:
            try:
                bad_file = bad_ffi.parse_bad(plan.reset_bad_path)
            except Exception:
                bad_file = None

        # The CLI writes only the main model's outputs (no Blender to merge
        # secondary models into a unified scene). Secondary parts contribute
        # textures only.
        try:
            main_model = next((m for m in plan.models if m.role == "main"), None)
            if main_model is None:
                log.error("Plan for %s has no main model", item_name)
                return 1
            main_ir = read_model_3di3(main_model.path)
            try:
                written = write_host_neutral_outputs(
                    main_ir,
                    project_dir,
                    plan.export_name,
                    write_ase=options.write_ase,
                    write_3dp=options.write_3dp,
                    include_collisions=options.import_collisions,
                    include_occlusion=options.import_occlusion,
                    include_lights=options.import_lights,
                    bad_file=bad_file,
                    collision_lod_index=None,
                )
                if options.copy_textures:
                    copy_model_textures(main_ir, project_dir, resolver=resolver)
                    for model in plan.models:
                        if model.role == "main":
                            continue
                        secondary_ir = read_model_3di3(model.path)
                        try:
                            copy_model_textures(secondary_ir, project_dir, resolver=resolver)
                        finally:
                            free_model_3di3(secondary_ir)
            finally:
                free_model_3di3(main_ir)
            log.info("%s %s -> %s (%d files)", item_type, item_name, project_dir, len(written))
            return 0
        finally:
            if bad_file is not None:
                bad_ffi.free_bad(bad_file)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="onimport")
    sub = parser.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="List items in a game directory")
    p_scan.add_argument("--dir", required=True)
    p_scan.set_defaults(func=cmd_scan)

    p_import = sub.add_parser("import", help="Import a single item")
    p_import.add_argument("--dir", required=True)
    p_import.add_argument("--item", required=True)
    p_import.add_argument("--type", required=True, choices=["weapon", "item"])
    p_import.add_argument("--output", required=True)
    p_import.add_argument("--name", default=None)
    _add_options(p_import)
    p_import.set_defaults(func=cmd_import)

    p_loose = sub.add_parser("import-loose", help="Import a standalone .3di file")
    p_loose.add_argument("--file", required=True)
    p_loose.add_argument("--output", required=True)
    p_loose.add_argument("--name", default=None)
    p_loose.add_argument("--asset-dir", default=None)
    _add_options(p_loose)
    p_loose.set_defaults(func=cmd_import_loose)

    p_all = sub.add_parser("export-all", help="Import all items in a game directory")
    p_all.add_argument("--dir", required=True)
    p_all.add_argument("--output", required=True)
    p_all.add_argument("--type", choices=["weapon", "item"], default=None)
    _add_options(p_all)
    p_all.set_defaults(func=cmd_export_all)

    return parser


def run_cli(args: list[str]) -> None:
    parser = build_parser()
    parsed = parser.parse_args(args)
    sys.exit(parsed.func(parsed))
