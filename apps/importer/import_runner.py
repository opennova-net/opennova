"""Thin wrapper over Blender-side importer modules for headless use.

Uses a stub `blender` package so we can import individual submodules without
executing blender/__init__.py (which registers Blender operators and would fail
outside a running Blender instance).
"""
from __future__ import annotations

import os
import sys
import types
import logging

from .jobs import (
    IMPORT_MODE_DEF,
    IMPORT_MODE_LOOSE,
    ImportRequest,
    ImportResult,
    ScanResult,
    ScanItem,
    validate_import_request,
)

log = logging.getLogger(__name__)


def _setup_blender_package() -> None:
    """Register the blender/ directory as a Python package without executing __init__.py."""
    # Repo root is two levels up from this file (apps/importer/ -> apps/ -> repo root)
    apps_dir = os.path.dirname(os.path.dirname(__file__))
    repo_root = os.path.dirname(apps_dir)

    if repo_root not in sys.path:
        sys.path.insert(0, repo_root)

    if "blender" not in sys.modules:
        blender_dir = os.path.join(repo_root, "blender")
        pkg = types.ModuleType("blender")
        pkg.__path__ = [blender_dir]
        pkg.__package__ = "blender"
        pkg.__spec__ = None
        sys.modules["blender"] = pkg
        log.debug("Registered stub blender package at %s", blender_dir)


def _export_glb(output_dir: str, name: str) -> None:
    """Export the current bpy scene to a glTF 2.0 binary (.glb) in output_dir."""
    import bpy
    glb_path = os.path.join(output_dir, name + ".glb")
    has_animations = len(bpy.data.actions) > 0
    bpy.ops.export_scene.gltf(
        filepath=glb_path,
        export_format="GLB",
        export_animations=has_animations,
        export_force_sampling=False,
    )
    log.info("Wrote GLB: %s", glb_path)


def _export_fbx(output_dir: str, name: str) -> None:
    """Export the current bpy scene to FBX in output_dir."""
    import bpy
    fbx_path = os.path.join(output_dir, name + ".fbx")
    has_animations = len(bpy.data.actions) > 0
    bpy.ops.export_scene.fbx(filepath=fbx_path, bake_anim=has_animations)
    log.info("Wrote FBX: %s", fbx_path)


def _save_blend_scene(output_dir: str, name: str, *, checkpoint: bool = False) -> None:
    """Save the current bpy scene to a .blend file in output_dir."""
    import bpy
    blend_path = os.path.join(output_dir, name + ".blend")
    if checkpoint:
        log.debug("[CKPT] save_as_mainfile start (%s)", blend_path)
        for handler in log.root.handlers:
            handler.flush()
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    if checkpoint:
        log.info("[CKPT] save_as_mainfile done -> Wrote blend: %s", blend_path)
        for handler in log.root.handlers:
            handler.flush()
    else:
        log.info("Wrote blend: %s", blend_path)



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
    write_glb: bool = False,
    write_fbx: bool = False,
) -> bool:
    """Import a single weapon/item via the C IR pipeline and write selected outputs."""
    from pyopennova.threedi_ffi import read_model_ir, free_model_ir
    from pyopennova.bad_ffi import parse_bad, free_bad
    from pyopennova.asset_resolver import AssetResolver
    from .resource_plan import resolve_definition_import
    from .scene_builder import BlenderSceneBuilder

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

        try:
            for model in plan.models:
                ir = read_model_ir(model.path)
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
                finally:
                    if model.role != "main":
                        free_model_ir(ir)

            if success_count > 0 and output_dir and main_ir is not None and main_builder is not None:
                project_dir = os.path.join(output_dir, plan.export_name)
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
                    bullet_lod_index=main_builder.bullet_lod_index,
                )
                if write_glb:
                    _export_glb(project_dir, plan.export_name)
                if write_fbx:
                    _export_fbx(project_dir, plan.export_name)
                if write_blend:
                    _save_blend_scene(project_dir, plan.export_name, checkpoint=True)
        finally:
            if main_ir is not None:
                free_model_ir(main_ir)
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
    write_glb: bool = False,
    write_fbx: bool = False,
) -> bool:
    """Import a single weapon/item and produce selected output files.

    Args:
        base_dir:    Root directory containing game assets (weapon.def / items.def).
        item_name:   Name of the weapon or item (e.g. "M16A2", "Armry01").
        item_type:   "weapon" or "item".
        output_dir:  Root directory where selected files will be written.
    """
    _setup_blender_package()

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
            write_glb=write_glb,
            write_fbx=write_fbx,
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
    bullet_lod_index: int | None = None,
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
        bullet_lod_index=bullet_lod_index,
    )


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
    write_glb: bool = False,
    write_fbx: bool = False,
    reset_scene: bool = True,
) -> bool:
    """Import a standalone .3di file (no DEF lookup required).

    Produces the selected output files in output_dir.
    output_stem overrides the output filename stem (default: derived from threedi_path).
    asset_base_dir overrides the texture/material search root (default: parent of threedi_path).
    """
    _setup_blender_package()

    from pathlib import Path
    from pyopennova.threedi_ffi import read_model_ir, free_model_ir
    from pyopennova.asset_resolver import AssetResolver
    from .scene_builder import BlenderSceneBuilder

    os.makedirs(output_dir, exist_ok=True)

    base_dir = asset_base_dir or str(Path(threedi_path).parent)
    name = output_stem or Path(threedi_path).stem

    ir = read_model_ir(threedi_path)
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
                    bullet_lod_index=builder.bullet_lod_index,
                )
    except Exception as exc:
        log.error("run_loose_import failed: %s", exc, exc_info=True)
        raise
    finally:
        free_model_ir(ir)

    if result:
        if write_glb:
            _export_glb(output_dir, name)
        if write_fbx:
            _export_fbx(output_dir, name)
        if write_blend:
            _save_blend_scene(output_dir, name)

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
    _setup_blender_package()

    from .resource_plan import resolve_definition_output_stem as _resolve

    return _resolve(base_dir, item_name, item_type)


def scan_directory_result(base_dir: str) -> ScanResult:
    """Scan a game directory and return available weapons and items.

    Returns ScanResult with typed items shaped like:
    {"name": str, "type": "weapon"|"item", "source_model": str, "output_stem": str}.
    """
    _setup_blender_package()

    from pathlib import Path

    if not base_dir:
        return ScanResult(ok=False, error="Game directory is required.")
    if not Path(base_dir).is_dir():
        return ScanResult(ok=False, error="Game directory does not exist.")

    from pyopennova import definitions as defs
    from pyopennova import asset_resolver as ar

    items: list[ScanItem] = []
    try:
        with ar.AssetResolver(base_dir) as resolver:
            weapons, item_defs = defs.process_def_files(resolver)
            for w in weapons:
                source_model = defs.ensure_extension(w.graphic1.main, ".3di")
                items.append(
                    ScanItem(
                        name=w.name,
                        type="weapon",
                        source_model=source_model,
                        output_stem=Path(source_model).stem,
                    )
                )
            for it in item_defs:
                source_model = defs.ensure_extension(it.graphic_us, ".3di")
                items.append(
                    ScanItem(
                        name=it.name,
                        type="item",
                        source_model=source_model,
                        output_stem=Path(source_model).stem,
                    )
                )
    except Exception as exc:
        log.error("scan_directory failed: %s", exc, exc_info=True)
        return ScanResult(ok=False, error=str(exc))

    return ScanResult(ok=True, items=items)


def scan_directory(base_dir: str) -> list[dict[str, str]]:
    """Compatibility wrapper returning only scanned items."""
    return [item.to_dict() for item in scan_directory_result(base_dir).items]
