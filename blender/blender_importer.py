"""
Blender-specific import logic for Novalogic files.
Uses ctypes FFI bindings (threedi_ffi / bad_ffi) instead of pure-Python parsers.
"""

from __future__ import annotations

from pathlib import Path

import bpy


def scan_and_parse_directory(directory: str):
    """Scan directory and parse definition files for weapons and items."""
    from .opennova.definitions import process_def_files
    from .opennova.asset_resolver import AssetResolver
    with AssetResolver(directory) as resolver:
        return process_def_files(resolver=resolver)


def import_basic_model(base_dir: str, item_name: str, item_type: str,
                       import_arms: bool = False, import_animations: bool = True,
                       import_collisions: bool = True, import_occlusion: bool = True,
                       import_lights: bool = True) -> bool:
    """Import a basic rigid model using the C IR pipeline."""
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

        return success_count > 0


def import_loose_3di(filepath: str, import_collisions: bool = True,
                     import_occlusion: bool = True, import_lights: bool = True) -> bool:
    """Import a standalone .3di file with textures resolved from its directory."""
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
        finally:
            free_model_ir(ir)

    return bool(result)
