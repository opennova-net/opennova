"""Thin wrapper over Blender-side importer modules for headless use."""
from __future__ import annotations

import os
import logging

from opennova_jobs import (
    IMPORT_MODE_DEF,
    IMPORT_MODE_LOOSE,
    ImportRequest,
    ImportResult,
    ScanResult,
    ScanItem,
    validate_import_request,
)

log = logging.getLogger(__name__)


def _import_basic_model(
    base_dir: str,
    item_name: str,
    item_type: str,
    output_dir: str,
    *,
    import_arms: bool = False,
    import_animations: bool = True,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    output_name: str = "",
    write_blend: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = False,
    copy_textures: bool = True,
) -> bool:
    """Import a single weapon/item via the C 3DI3 pipeline and write selected outputs."""
    from pyopennova.threedi_ffi import read_model, free_model_3di3
    from pyopennova.bad_ffi import parse_bad, free_bad
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.resource_plan import resolve_definition_import
    from .scene_builder import BlenderSceneBuilder
    from .exports import save_blend_scene

    with AssetResolver(base_dir) as resolver:
        plan = resolve_definition_import(
            base_dir=base_dir,
            item_name=item_name,
            item_type=item_type,
            resolver=resolver,
            import_arms=import_arms,
            import_animations=import_animations,
            output_name=output_name,
        )
        if plan is None:
            log.error("Could not find %s '%s' in definitions", item_type, item_name)
            return False

        bad_file = None
        if plan.reset_bad_path:
            try:
                bad_file = parse_bad(plan.reset_bad_path)
            except Exception as e:
                log.warning("Could not load BAD file: %s", e)

        success_count = 0
        main_builder = None
        main_ir = None
        project_dir = os.path.join(output_dir, plan.export_name) if output_dir else ""

        try:
            for model in plan.models:
                ir = read_model(model.path)
                try:
                    if model.role == "main":
                        main_ir = ir
                        ctx = plan.animation_context if import_animations else None
                        main_builder = BlenderSceneBuilder(ir, bad_file=bad_file,
                                                           anim_context=ctx, resolver=resolver,
                                                           import_collisions=import_collisions,
                                                           import_occlusion=import_occlusion,
                                                           import_lights=import_lights)
                        log.debug("[CKPT] build_basic_scene start (%s)", plan.scene_name)
                        for _h in log.root.handlers: _h.flush()
                        result = main_builder.build_basic_scene(plan.scene_name)
                        log.debug("[CKPT] build_basic_scene done (%s)", plan.scene_name)
                        for _h in log.root.handlers: _h.flush()
                        if result:
                            success_count += 1
                    else:
                        if main_builder is None:
                            continue
                        secondary = BlenderSceneBuilder(ir, bad_file=bad_file,
                                                        resolver=resolver,
                                                        import_collisions=import_collisions,
                                                        import_occlusion=import_occlusion,
                                                        import_lights=import_lights)
                        if secondary.merge_with_existing_scene(main_builder):
                            success_count += 1
                            if copy_textures and project_dir:
                                _copy_model_textures(ir, project_dir, resolver)
                finally:
                    if model.role != "main":
                        free_model_3di3(ir)

            if success_count > 0 and output_dir and main_ir is not None and main_builder is not None:
                _write_host_neutral_outputs(
                    main_ir,
                    project_dir,
                    plan.export_name,
                    write_ase=write_ase,
                    write_3dp=write_3dp,
                    import_collisions=import_collisions,
                    import_occlusion=import_occlusion,
                    import_lights=import_lights,
                    bad_file=bad_file,
                    collision_lod_index=None,
                )
                if copy_textures:
                    _copy_model_textures(main_ir, project_dir, resolver)
                if write_blend:
                    save_blend_scene(project_dir, plan.export_name, checkpoint=True)
        finally:
            if main_ir is not None:
                free_model_3di3(main_ir)
            if bad_file is not None:
                free_bad(bad_file)

        return success_count > 0


def run_import(
    base_dir: str,
    item_name: str,
    item_type: str,
    output_dir: str,
    *,
    import_arms: bool = True,
    import_animations: bool = True,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    write_blend: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = False,
    copy_textures: bool = True,
    output_stem: str = "",
) -> bool:
    """Import a single weapon/item and produce selected output files.

    Args:
        base_dir:    Root directory containing game assets (weapon.def / items.def).
        item_name:   Name of the weapon or item (e.g. "M16A2", "Armry01").
        item_type:   "weapon" or "item".
        output_dir:  Root directory where selected files will be written.
    """
    os.makedirs(output_dir, exist_ok=True)

    try:
        return _import_basic_model(
            base_dir=base_dir,
            item_name=item_name,
            item_type=item_type,
            output_dir=output_dir,
            import_arms=import_arms,
            import_animations=import_animations,
            import_collisions=import_collisions,
            import_occlusion=import_occlusion,
            import_lights=import_lights,
            write_blend=write_blend,
            write_ase=write_ase,
            write_3dp=write_3dp,
            write_max=write_max,
            copy_textures=copy_textures,
            output_name=output_stem,
        )
    except Exception as exc:
        log.error("import failed: %s", exc, exc_info=True)
        raise


def _write_host_neutral_outputs(
    ir,
    output_dir: str,
    name: str,
    *,
    write_ase: bool,
    write_3dp: bool,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    bad_file=None,
    collision_lod_index: int | None = None,
) -> list[str]:
    from pyopennova.host_outputs import write_host_neutral_outputs

    return write_host_neutral_outputs(
        ir,
        output_dir,
        name,
        write_ase=write_ase,
        write_3dp=write_3dp,
        include_collisions=import_collisions,
        include_occlusion=import_occlusion,
        include_lights=import_lights,
        bad_file=bad_file,
        collision_lod_index=collision_lod_index,
    )


def _copy_model_textures(ir, output_dir: str, resolver=None) -> list[str]:
    from pyopennova.texture_outputs import copy_model_textures

    return copy_model_textures(ir, output_dir, resolver=resolver)


def run_loose_import(
    threedi_path: str,
    output_dir: str,
    output_stem: str | None = None,
    *,
    asset_base_dir: str | None = None,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    write_blend: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = False,
    copy_textures: bool = True,
    reset_scene: bool = True,
) -> bool:
    """Import a standalone .3di file (no DEF lookup required).

    Produces the selected output files in output_dir.
    output_stem overrides the output filename stem (default: derived from threedi_path).
    asset_base_dir overrides the texture/material search root (default: parent of threedi_path).
    """
    from pathlib import Path
    from pyopennova.threedi_ffi import read_model, free_model_3di3
    from pyopennova.asset_resolver import AssetResolver
    from .scene_builder import BlenderSceneBuilder
    from .exports import save_blend_scene

    os.makedirs(output_dir, exist_ok=True)

    base_dir = asset_base_dir or str(Path(threedi_path).parent)
    name = output_stem or Path(threedi_path).stem

    ir = read_model(threedi_path)
    result = False
    try:
        with AssetResolver(base_dir) as resolver:
            builder = BlenderSceneBuilder(ir, resolver=resolver,
                                          import_collisions=import_collisions,
                                          import_occlusion=import_occlusion,
                                          import_lights=import_lights)
            result = builder.build_basic_scene(name)
            if result:
                _write_host_neutral_outputs(
                    ir,
                    output_dir,
                    name,
                    write_ase=write_ase,
                    write_3dp=write_3dp,
                    import_collisions=import_collisions,
                    import_occlusion=import_occlusion,
                    import_lights=import_lights,
                    collision_lod_index=None,
                )
                if copy_textures:
                    _copy_model_textures(ir, output_dir, resolver)
    except Exception as exc:
        log.error("run_loose_import failed: %s", exc, exc_info=True)
        raise
    finally:
        free_model_3di3(ir)

    if result:
        if write_blend:
            save_blend_scene(output_dir, name)

    return bool(result)


_default_dispatcher = None


def _get_default_dispatcher():
    """Lazy-init a 1-worker dispatcher for synchronous single-item callers."""
    global _default_dispatcher
    if _default_dispatcher is None:
        import atexit
        from .dispatcher import ImportDispatcher
        _default_dispatcher = ImportDispatcher(max_workers=1)
        atexit.register(_default_dispatcher.close)
    return _default_dispatcher


def execute_import_request(request: ImportRequest) -> ImportResult:
    """Run one import request and block until it finishes.

    Dispatches to a process pool so the import runs in a fresh subprocess.
    For batch work that benefits from concurrency, construct your own
    ``ImportDispatcher`` and use ``submit_batch`` directly.
    """
    return _get_default_dispatcher().submit(request).result()


def resolve_definition_output_stem(base_dir: str, item_name: str, item_type: str) -> str:
    """Return the output directory/file stem for a definition import."""
    from pyopennova.resource_plan import resolve_definition_output_stem as _resolve

    return _resolve(base_dir, item_name, item_type)


def scan_directory_result(base_dir: str) -> ScanResult:
    """Scan a game directory and return available weapons and items.

    Thin delegation to `pyopennova.scan.scan_definitions`. Kept here for
    backwards compatibility; callers should migrate to the canonical
    location during Phase 5.
    """
    from pyopennova.scan import scan_definitions
    return scan_definitions(base_dir)


def scan_directory(base_dir: str) -> list[dict[str, str]]:
    """Compatibility wrapper returning only scanned items."""
    return [item.to_dict() for item in scan_directory_result(base_dir).items]
