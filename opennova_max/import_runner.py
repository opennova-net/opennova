"""Single and batch import runners for 3ds Max."""
from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Iterable, Optional



def run_loose_import(
    threedi_path: str,
    output_dir: str,
    output_stem: Optional[str] = None,
    *,
    asset_base_dir: Optional[str] = None,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = True,
    copy_textures: bool = True,
    reset_scene: bool = True,
) -> bool:
    """Import a standalone ``.3di`` and write selected outputs."""
    written = _run_loose_import_impl(
        threedi_path=threedi_path,
        output_dir=output_dir,
        output_stem=output_stem,
        asset_base_dir=asset_base_dir,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        write_ase=write_ase,
        write_3dp=write_3dp,
        write_max=write_max,
        copy_textures=copy_textures,
        reset_scene=reset_scene,
    )
    return bool(written)


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
    output_stem: str = "",
    write_ase: bool = True,
    write_3dp: bool = True,
    write_max: bool = True,
    copy_textures: bool = True,
    reset_scene: bool = True,
) -> bool:
    """Import one DEF resource and write outputs under ``output_dir/<stem>``."""
    written, _project_dir = _run_definition_import_impl(
        base_dir=base_dir,
        item_name=item_name,
        item_type=item_type,
        output_dir=output_dir,
        import_arms=import_arms,
        import_animations=import_animations,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        output_stem=output_stem,
        write_ase=write_ase,
        write_3dp=write_3dp,
        write_max=write_max,
        copy_textures=copy_textures,
        reset_scene=reset_scene,
    )
    return bool(written)


def execute_import_request(request) -> object:
    """Run one shared ImportRequest sequentially in Max."""
    start = time.time()
    try:
        written, output_path = _execute_to_files(request)
        return _success_result(
            request,
            output_path=output_path,
            written_files=written,
            elapsed_seconds=time.time() - start,
        )
    except Exception as exc:
        return _failure_result(
            request,
            error=str(exc),
            elapsed_seconds=time.time() - start,
        )


def run_batch(requests: Iterable[object]) -> list[object]:
    """Run a batch sequentially inside the current Max process."""
    return [execute_import_request(request) for request in requests]


def _run_loose_import_impl(
    *,
    threedi_path: str,
    output_dir: str,
    output_stem: Optional[str],
    asset_base_dir: Optional[str],
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = True,
    reset_scene: bool = True,
) -> list[str]:
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.threedi_ffi import free_model_3di3, read_model
    from .output_writers import reset_scene as _reset_scene, write_outputs
    from .scene_builder import MaxSceneBuilder

    if not write_ase and not write_3dp and not write_max and not copy_textures:
        raise ValueError("Max imports require ASE, 3DP/3DA, MAX, and/or textures output.")
    if reset_scene:
        _reset_scene()
    os.makedirs(output_dir, exist_ok=True)

    base_dir = asset_base_dir or str(Path(threedi_path).parent)
    name = output_stem or Path(threedi_path).stem
    ir = read_model(threedi_path)
    try:
        with AssetResolver(base_dir) as resolver:
            builder = MaxSceneBuilder(
                ir,
                resolver=resolver,
                import_collisions=import_collisions,
                import_occlusion=import_occlusion,
                import_lights=import_lights,
            )
            if not builder.build_basic_scene(name):
                return []
            return write_outputs(
                ir,
                output_dir,
                name,
                builder,
                write_ase=write_ase,
                write_3dp=write_3dp,
                write_max=write_max,
                copy_textures=copy_textures,
            )
    finally:
        free_model_3di3(ir)


def _run_definition_import_impl(
    *,
    base_dir: str,
    item_name: str,
    item_type: str,
    output_dir: str,
    import_arms: bool,
    import_animations: bool,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    output_stem: str,
    write_ase: bool,
    write_3dp: bool,
    write_max: bool,
    copy_textures: bool = True,
    reset_scene: bool = True,
) -> tuple[list[str], str]:
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.bad_ffi import free_bad, parse_bad
    from pyopennova.threedi_ffi import free_model_3di3, read_model
    from pyopennova.resource_plan import resolve_definition_import
    from .output_writers import reset_scene as _reset_scene, write_outputs
    from .scene_builder import MaxSceneBuilder

    if not write_ase and not write_3dp and not write_max and not copy_textures:
        raise ValueError("Max imports require ASE, 3DP/3DA, MAX, and/or textures output.")
    if reset_scene:
        _reset_scene()

    with AssetResolver(base_dir) as resolver:
        plan = resolve_definition_import(
            base_dir=base_dir,
            item_name=item_name,
            item_type=item_type,
            resolver=resolver,
            import_arms=import_arms,
            import_animations=import_animations,
            output_name=output_stem,
        )
        if plan is None:
            raise ValueError(f"Could not find {item_type} '{item_name}' in definitions")

        project_dir = os.path.join(output_dir, plan.export_name)
        os.makedirs(project_dir, exist_ok=True)

        bad_file = None
        if plan.reset_bad_path:
            try:
                bad_file = parse_bad(plan.reset_bad_path)
            except Exception:
                bad_file = None

        written: list[str] = []
        main_builder = None
        main_ir = None
        main_built = False
        try:
            for model in plan.models:
                ir = read_model(model.path)
                try:
                    if model.role == "main":
                        main_ir = ir
                        main_builder = MaxSceneBuilder(
                            ir,
                            bad_file=bad_file,
                            anim_context=plan.animation_context if import_animations else None,
                            resolver=resolver,
                            import_collisions=import_collisions,
                            import_occlusion=import_occlusion,
                            import_lights=import_lights,
                        )
                        main_built = bool(main_builder.build_basic_scene(plan.scene_name))
                    elif main_builder is not None and main_built:
                        secondary = MaxSceneBuilder(
                            ir,
                            bad_file=bad_file,
                            resolver=resolver,
                            import_collisions=import_collisions,
                            import_occlusion=import_occlusion,
                            import_lights=import_lights,
                        )
                        if secondary.merge_with_existing_scene(main_builder) and copy_textures:
                            _copy_model_textures(ir, project_dir, resolver)
                finally:
                    if model.role != "main":
                        free_model_3di3(ir)

            if main_built and main_builder is not None and main_ir is not None:
                if import_animations:
                    main_builder.apply_animations()
                written.extend(write_outputs(
                    main_ir,
                    project_dir,
                    plan.export_name,
                    main_builder,
                    write_ase=write_ase,
                    write_3dp=write_3dp,
                    write_max=write_max,
                    copy_textures=copy_textures,
                ))
        finally:
            if main_ir is not None:
                free_model_3di3(main_ir)
            if bad_file is not None:
                free_bad(bad_file)

    return written, project_dir


def _execute_to_files(request) -> tuple[list[str], str]:
    mode = getattr(request, "mode", "loose")
    if mode == "loose":
        threedi_path = getattr(request, "threedi_path", "") or getattr(request, "resource", "")
        output_root = getattr(request, "output_root", "") or getattr(request, "output_dir", "")
        output_stem = getattr(request, "output_stem", "")
        name = output_stem or Path(threedi_path).stem
        if hasattr(request, "loose_output_dir"):
            output_dir = request.loose_output_dir
        elif hasattr(request, "output_dir") and not hasattr(request, "output_root"):
            output_dir = output_root
        else:
            output_dir = os.path.join(output_root, name)
        options = getattr(request, "options", None)
        written = _run_loose_import_impl(
            threedi_path=threedi_path,
            output_dir=output_dir,
            output_stem=output_stem or None,
            asset_base_dir=getattr(request, "base_dir", "") or None,
            import_collisions=getattr(options, "import_collisions", getattr(request, "import_collisions", True)),
            import_occlusion=getattr(options, "import_occlusion", getattr(request, "import_occlusion", True)),
            import_lights=getattr(options, "import_lights", getattr(request, "import_lights", True)),
            write_ase=getattr(options, "write_ase", getattr(request, "write_ase", True)),
            write_3dp=getattr(options, "write_3dp", getattr(request, "write_3dp", True)),
            write_max=getattr(options, "write_max", getattr(request, "write_max", True)),
            copy_textures=getattr(options, "copy_textures", getattr(request, "copy_textures", True)),
            reset_scene=True,
        )
        return written, output_dir

    base_dir = getattr(request, "base_dir", "")
    item_name = getattr(request, "item_name", "") or getattr(request, "resource", "")
    item_type = getattr(request, "item_type", "")
    output_root = getattr(request, "output_root", "") or getattr(request, "output_dir", "")
    options = getattr(request, "options", None)
    return _run_definition_import_impl(
        base_dir=base_dir,
        item_name=item_name,
        item_type=item_type,
        output_dir=output_root,
        import_arms=getattr(options, "import_arms", getattr(request, "import_arms", True)),
        import_animations=getattr(options, "import_animations", getattr(request, "import_animations", True)),
        import_collisions=getattr(options, "import_collisions", getattr(request, "import_collisions", True)),
        import_occlusion=getattr(options, "import_occlusion", getattr(request, "import_occlusion", True)),
        import_lights=getattr(options, "import_lights", getattr(request, "import_lights", True)),
        output_stem=getattr(request, "output_stem", ""),
        write_ase=getattr(options, "write_ase", getattr(request, "write_ase", True)),
        write_3dp=getattr(options, "write_3dp", getattr(request, "write_3dp", True)),
        write_max=getattr(options, "write_max", getattr(request, "write_max", True)),
        copy_textures=getattr(options, "copy_textures", getattr(request, "copy_textures", True)),
        reset_scene=True,
    )


def _copy_model_textures(ir, output_dir: str, resolver=None) -> list[str]:
    from pyopennova.texture_outputs import copy_model_textures

    return copy_model_textures(ir, output_dir, resolver=resolver)


def _success_result(request, *, output_path: str, written_files: list[str], elapsed_seconds: float):
    from opennova_jobs import ImportResult

    return ImportResult.success(
        request,
        output_path=output_path,
        written_files=written_files,
        elapsed_seconds=elapsed_seconds,
    )


def _failure_result(request, *, error: str, elapsed_seconds: float):
    from opennova_jobs import ImportResult

    return ImportResult.failure(
        request,
        error=error,
        elapsed_seconds=elapsed_seconds,
    )
