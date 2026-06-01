"""Thin wrapper over Blender scene construction modules for headless use.

Uses a stub `blender` package so we can import individual submodules without
executing blender/__init__.py (which registers Blender operators and would fail
outside a running Blender instance).
"""
import os
import sys
import types
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


def _export_ase(output_dir: str, name: str) -> None:
    """Export the current bpy scene to an ASE file in output_dir."""
    import bpy
    from blender.ase_exporter import AseExporter  # type: ignore[import]
    ase_path = os.path.join(output_dir, name + ".ase")
    AseExporter().export_scene(bpy.context.scene, ase_path)
    log.info("Wrote ASE: %s", ase_path)


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
    bpy.ops.export_scene.fbx(
        filepath=fbx_path,
        bake_anim=has_animations,
        bake_anim_use_nla_strips=has_animations,
        bake_anim_use_all_actions=False,
    )
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
    from pathlib import Path
    from pyopennova.definitions import (
        process_def_files, ensure_extension, build_animation_context,
    )
    from pyopennova.threedi_ffi import read_model_ir, free_model_ir
    from pyopennova.bad_ffi import parse_bad, free_bad
    from pyopennova.asset_resolver import AssetResolver
    from .scene_builder import BlenderSceneBuilder

    with AssetResolver(base_dir) as resolver:
        weapons, items = process_def_files(resolver=resolver)

        target_context = None
        if item_type == "weapon":
            target_context = next((w for w in weapons if w.name == item_name), None)
        else:
            target_context = next((i for i in items if i.name == item_name), None)

        if not target_context:
            log.error("Could not find %s '%s' in definitions", item_type, item_name)
            return False

        models_to_import = []
        if item_type == "weapon":
            main_file = target_context.graphic1.main
        else:
            main_file = target_context.graphic_us

        if main_file:
            main_file = ensure_extension(main_file, ".3di")
            models_to_import.append(("main", main_file))

        if item_type == "weapon" and import_arms and target_context.graphic1.arms:
            arms_file = ensure_extension(target_context.graphic1.arms, ".3di")
            models_to_import.append(("arms", arms_file))

        if not models_to_import:
            log.error("No model files specified for %s", item_name)
            return False

        graphic_name = Path(main_file).stem if main_file else item_name
        export_name = output_name if output_name else graphic_name

        bad_file = None
        anim_ctx = None
        anim_field = target_context.anim_adm if item_type == "weapon" else target_context.anim_def
        if anim_field:
            try:
                anim_ctx = build_animation_context(anim_field, resolver=resolver)
                if anim_ctx and anim_ctx.reset_animation:
                    bad_file = parse_bad(anim_ctx.reset_animation.bad_filepath)
            except Exception as e:
                log.warning("Could not load BAD file: %s", e)

        success_count = 0
        main_builder = None

        for model_type, model_file in models_to_import:
            model_path = resolver.resolve(model_file)
            if model_path is None:
                raise FileNotFoundError(f"Model file not found: {Path(base_dir) / model_file}")

            ir = None
            try:
                ir = read_model_ir(str(model_path))

                if model_type == "main":
                    ctx = anim_ctx if import_animations else None
                    main_builder = BlenderSceneBuilder(ir, bad_file=bad_file,
                                                       anim_context=ctx, resolver=resolver,
                                                       import_collisions=import_collisions,
                                                       import_occlusion=import_occlusion,
                                                       import_lights=import_lights)
                    log.debug("[CKPT] build_basic_scene start (%s)", item_name)
                    for _h in log.root.handlers: _h.flush()
                    result = main_builder.build_basic_scene(item_name)
                    log.debug("[CKPT] build_basic_scene done (%s)", item_name)
                    for _h in log.root.handlers: _h.flush()
                    if result:
                        success_count += 1
                        if output_dir:
                            project_dir = os.path.join(output_dir, export_name)
                            os.makedirs(project_dir, exist_ok=True)
                            if write_3dp:
                                _write_3dp_from_ir(
                                    ir,
                                    os.path.join(project_dir, export_name + ".3dp"),
                                )
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
                if ir is not None:
                    free_model_ir(ir)

        if bad_file is not None:
            free_bad(bad_file)

        if success_count > 0 and output_dir:
            project_dir = os.path.join(output_dir, export_name)
            if write_ase:
                _export_ase(project_dir, export_name)
            if write_glb:
                _export_glb(project_dir, export_name)
            if write_fbx:
                _export_fbx(project_dir, export_name)
            if write_blend:
                _save_blend_scene(project_dir, export_name, checkpoint=True)

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


def _write_3dp_from_ir(ir, tdp_path: str) -> None:
    """Write a .3dp object workspace from the C IR using the native tdp library."""
    from pyopennova.tdp_ffi import tdp_from_ir, write_tdp, free_tdp
    proj = tdp_from_ir(ir)
    try:
        write_tdp(tdp_path, proj)
    finally:
        free_tdp(proj)
    log.info("Wrote 3DP: %s", tdp_path)


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
            if result and write_3dp:
                _write_3dp_from_ir(
                    ir,
                    os.path.join(output_dir, name + ".3dp"),
                )
    except Exception as exc:
        log.error("run_loose_import failed: %s", exc, exc_info=True)
        raise
    finally:
        free_model_ir(ir)

    if result:
        if write_ase:
            _export_ase(output_dir, name)
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

    from pathlib import Path
    from pyopennova.definitions import (
        ensure_extension,
        process_def_files,
    )
    from pyopennova.asset_resolver import AssetResolver

    with AssetResolver(base_dir) as resolver:
        weapons, item_defs = process_def_files(resolver)
        if item_type == "weapon":
            target = next((weapon for weapon in weapons if weapon.name == item_name), None)
            if target and target.graphic1.main:
                return Path(ensure_extension(target.graphic1.main, ".3di")).stem
        elif item_type == "item":
            target = next((item for item in item_defs if item.name == item_name), None)
            if target and target.graphic_us:
                return Path(ensure_extension(target.graphic_us, ".3di")).stem
    return ""


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

    from pyopennova import asset_resolver as ar
    from pyopennova import definitions as defs

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
