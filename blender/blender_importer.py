"""
Blender-specific import logic for Novalogic files.
Uses ctypes FFI bindings (threedi_ffi / bad_ffi) instead of pure-Python parsers.
"""

from __future__ import annotations

import os
from pathlib import Path

import bpy


def _clear_scene():
    """Remove all objects and purge all data blocks for a clean import."""
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for attr in ('meshes', 'armatures', 'materials', 'textures', 'images',
                 'actions', 'collections', 'cameras', 'lights', 'curves'):
        for block in list(getattr(bpy.data, attr)):
            getattr(bpy.data, attr).remove(block)


def write_3dp_from_ir(ir, tdp_path):
    """Write .3dp and .3da project files from the C IR using the native tdp library."""
    from .opennova.tdp_ffi import tdp_from_ir, write_tdp, write_3da, free_tdp
    proj = tdp_from_ir(ir)
    try:
        write_tdp(tdp_path, proj)
        tda_path = tdp_path.replace('.3dp', '.3da')
        write_3da(tda_path, proj)
    finally:
        free_tdp(proj)
    print(f"3DI Import: Wrote 3DP project file {tdp_path}")
    print(f"3DI Import: Wrote 3DA project file {tda_path}")


def _export_ase_and_save_blend(project_dir: str, name: str):
    """Export ASE file(s) and save the .blend into the project directory."""
    from .ase_exporter import AseExporter

    ase_path = os.path.join(project_dir, name + ".ase")
    AseExporter().export_scene(bpy.context.scene, ase_path)
    print(f"3DI Import: Wrote ASE file {ase_path}")

    blend_path = os.path.join(project_dir, name + ".blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    print(f"3DI Import: Saved blend file {blend_path}")


def scan_and_parse_directory(directory: str):
    """Scan directory and parse definition files for weapons and items."""
    from .opennova.definitions import process_def_files
    from .opennova.asset_resolver import AssetResolver
    with AssetResolver(directory) as resolver:
        return process_def_files(resolver=resolver)


def import_basic_model(base_dir: str, item_name: str, item_type: str,
                       import_arms: bool = False, import_animations: bool = True,
                       import_collisions: bool = True, import_occlusion: bool = True,
                       import_lights: bool = True, output_dir: str = "") -> bool:
    """Import a basic rigid model using the C IR pipeline."""
    _clear_scene()

    from .opennova.definitions import (
        process_def_files, ensure_extension, build_animation_context,
    )
    from .opennova.threedi_ffi import read_model_ir, free_model_ir
    from .opennova.bad_ffi import parse_bad, free_bad
    from .opennova.asset_resolver import AssetResolver

    with AssetResolver(base_dir) as resolver:
        weapons, items = process_def_files(resolver=resolver)

        target_context = None
        if item_type == "weapon":
            target_context = next((w for w in weapons if w.name == item_name), None)
        else:
            target_context = next((i for i in items if i.name == item_name), None)

        if not target_context:
            print(f"Could not find {item_type} '{item_name}' in definitions")
            return False

        # Collect model files to import
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
            print(f"No model files specified for {item_name}")
            return False

        graphic_name = Path(main_file).stem if main_file else item_name

        # Get BAD bone data and animation context if available
        bad_file = None
        anim_ctx = None
        anim_field = None
        if item_type == "weapon":
            anim_field = target_context.anim_adm
        elif item_type == "item":
            anim_field = target_context.anim_def

        if anim_field:
            try:
                anim_ctx = build_animation_context(anim_field, resolver=resolver)
                if anim_ctx and anim_ctx.reset_animation:
                    bad_file = parse_bad(anim_ctx.reset_animation.bad_filepath)
            except Exception as e:
                print(f"Warning: could not load BAD file: {e}")

        # Import each model
        from .scene_builder import BlenderSceneBuilder

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
                    result = main_builder.build_basic_scene(item_name)
                    if result:
                        success_count += 1
                        if output_dir:
                            project_dir = os.path.join(output_dir, graphic_name)
                            os.makedirs(project_dir, exist_ok=True)
                            write_3dp_from_ir(ir, os.path.join(project_dir, graphic_name + ".3dp"))
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

        # Free BAD file
        if bad_file is not None:
            free_bad(bad_file)

        if success_count > 0 and output_dir:
            project_dir = os.path.join(output_dir, graphic_name)
            _export_ase_and_save_blend(project_dir, graphic_name)

        return success_count > 0


def import_loose_3di(filepath: str, import_collisions: bool = True,
                     import_occlusion: bool = True, import_lights: bool = True,
                     output_dir: str = "") -> bool:
    """Import a standalone .3di file with textures resolved from its directory."""
    _clear_scene()

    from .opennova.threedi_ffi import read_model_ir, free_model_ir
    from .opennova.asset_resolver import AssetResolver
    from .scene_builder import BlenderSceneBuilder

    base_dir = str(Path(filepath).parent)
    name = Path(filepath).stem

    with AssetResolver(base_dir) as resolver:
        ir = read_model_ir(filepath)
        try:
            builder = BlenderSceneBuilder(ir, resolver=resolver,
                                          import_collisions=import_collisions,
                                          import_occlusion=import_occlusion,
                                          import_lights=import_lights)
            result = builder.build_basic_scene(name)

            if result and output_dir:
                project_dir = os.path.join(output_dir, name)
                os.makedirs(project_dir, exist_ok=True)
                write_3dp_from_ir(ir, os.path.join(project_dir, name + ".3dp"))
        finally:
            free_model_ir(ir)

    if result and output_dir:
        _export_ase_and_save_blend(project_dir, name)

    return bool(result)
