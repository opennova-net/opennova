"""Thin wrapper over Blender-side importer modules for headless use.

Uses a stub `blender` package so we can import individual submodules without
executing blender/__init__.py (which registers Blender operators and would fail
outside a running Blender instance).
"""
import os
import sys
import types
import logging

log = logging.getLogger(__name__)


def _setup_blender_package() -> None:
    """Register the blender/ directory as a Python package without executing __init__.py."""
    # Repo root is two levels up from this file (apps/importer/ → apps/ → repo root)
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
    write_ase: bool = True,
    write_3dp: bool = True,
) -> bool:
    """Import a single weapon/item via the C IR pipeline and write .ase + .3dp."""
    import bpy
    from pathlib import Path
    from blender.opennova.definitions import (  # type: ignore[import]
        process_def_files, ensure_extension, build_animation_context,
    )
    from blender.opennova.threedi_ffi import read_model_ir, free_model_ir  # type: ignore[import]
    from blender.opennova.bad_ffi import parse_bad, free_bad  # type: ignore[import]
    from blender.opennova.asset_resolver import AssetResolver  # type: ignore[import]
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
                                    bullet_lod_index=main_builder.bullet_lod_index,
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
            blend_path = os.path.join(project_dir, export_name + ".blend")
            log.debug("[CKPT] save_as_mainfile start (%s)", blend_path)
            for _h in log.root.handlers: _h.flush()
            bpy.ops.wm.save_as_mainfile(filepath=blend_path)
            log.info("[CKPT] save_as_mainfile done → Wrote blend: %s", blend_path)
            for _h in log.root.handlers: _h.flush()

        return success_count > 0


def run_import(
    base_dir: str,
    item_name: str,
    item_type: str,
    output_dir: str,
    *,
    import_arms: bool = False,
    import_animations: bool = True,
    import_collisions: bool = True,
    import_occlusion: bool = False,
    import_lights: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
) -> bool:
    """Import a single weapon/item and produce .ase + .3dp files.

    Args:
        base_dir:    Root directory containing game assets (weapon.def / items.def).
        item_name:   Name of the weapon or item (e.g. "M16A2", "Armry01").
        item_type:   "weapon" or "item".
        output_dir:  Directory where .ase/.3dp will be written.
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
            write_ase=write_ase,
            write_3dp=write_3dp,
        )
    except Exception as exc:
        log.error("import failed: %s", exc, exc_info=True)
        return False


def _write_3dp_from_ir(ir, tdp_path: str, bullet_lod_index: int = -1) -> None:
    """Write .3dp and .3da project files from the C IR using the native tdp library."""
    from blender.opennova.tdp_ffi import tdp_from_ir, write_tdp, write_3da, free_tdp, TDP_MAX_LODS  # type: ignore[import]
    proj = tdp_from_ir(ir)
    try:
        if 0 <= bullet_lod_index < TDP_MAX_LODS:
            lod_slot = proj.lods[bullet_lod_index]
            name = ir.name.decode("utf-8", errors="replace").rstrip("\x00")
            lod_slot.scene_file = f"{name}_bullet.ase".encode("utf-8")[:63]
            lod_slot.attributes = proj.lods[0].attributes
            lod_slot.render_function = proj.lods[0].render_function
            proj.poly_collision_lod = bullet_lod_index
        write_tdp(tdp_path, proj)
        tda_path = tdp_path.replace(".3dp", ".3da")
        write_3da(tda_path, proj)
    finally:
        free_tdp(proj)
    log.info("Wrote 3DP: %s", tdp_path)
    log.info("Wrote 3DA: %s", tda_path)


def run_loose_import(
    threedi_path: str,
    output_dir: str,
    output_stem: str | None = None,
) -> bool:
    """Import a standalone .3di file (no DEF lookup required).

    Produces .ase + .3dp/.3da in output_dir.
    output_stem overrides the output filename stem (default: derived from threedi_path).
    """
    _setup_blender_package()

    import bpy
    from pathlib import Path
    from blender.opennova.threedi_ffi import read_model_ir, free_model_ir  # type: ignore[import]
    from blender.opennova.asset_resolver import AssetResolver  # type: ignore[import]
    from .scene_builder import BlenderSceneBuilder
    from blender.ase_exporter import AseExporter  # type: ignore[import]
    from . import bpy_session

    os.makedirs(output_dir, exist_ok=True)
    bpy_session.new_scene()

    base_dir = str(Path(threedi_path).parent)
    name = output_stem or Path(threedi_path).stem

    ir = read_model_ir(threedi_path)
    result = False
    try:
        with AssetResolver(base_dir) as resolver:
            builder = BlenderSceneBuilder(ir, resolver=resolver,
                                          import_collisions=True,
                                          import_occlusion=True,
                                          import_lights=True)
            result = builder.build_basic_scene(name)
            if result:
                _write_3dp_from_ir(
                    ir,
                    os.path.join(output_dir, name + ".3dp"),
                    bullet_lod_index=builder.bullet_lod_index,
                )
    except Exception as exc:
        log.error("run_loose_import failed: %s", exc, exc_info=True)
    finally:
        free_model_ir(ir)

    if result:
        ase_path = os.path.join(output_dir, name + ".ase")
        AseExporter().export_scene(bpy.context.scene, ase_path)
        log.info("Wrote ASE: %s", ase_path)

    return bool(result)


def scan_directory(base_dir: str) -> list[dict]:
    """Scan a game directory and return available weapons and items.

    Returns a list of dicts: {"name": str, "type": "weapon"|"item"}
    """
    _setup_blender_package()

    from blender.opennova import definitions as defs  # type: ignore[import]
    from blender.opennova import asset_resolver as ar  # type: ignore[import]

    items = []
    try:
        resolver = ar.AssetResolver(base_dir)
        weapons, item_defs = defs.process_def_files(resolver)
        for w in weapons:
            items.append({"name": w.name, "type": "weapon"})
        for it in item_defs:
            items.append({"name": it.name, "type": "item"})
    except Exception as exc:
        log.error("scan_directory failed: %s", exc, exc_info=True)

    return items
