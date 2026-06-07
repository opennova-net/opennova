"""Single and grouped import runners for 3ds Max."""
from __future__ import annotations

import os
import time
from typing import Iterable

from opennova_jobs import IMPORT_MODE_LOOSE, ImportRequest, ImportResult
from pyopennova.import_orchestrator import execute_definition_import, execute_loose_import


SUPPORTS_SCENE_IMPORT = True


def run_batch(requests):
    # type: (Iterable[ImportRequest]) -> list[ImportResult]
    """Run a grouped batch sequentially inside the current 3ds Max process."""
    return [execute_import_request(request) for request in requests]


def execute_import_request(request):
    # type: (ImportRequest) -> ImportResult
    start = time.monotonic()
    try:
        written, output_path = _execute_to_files(request)
        return ImportResult.success(
            request,
            output_path=output_path,
            written_files=written,
            elapsed_seconds=time.monotonic() - start,
        )
    except Exception as exc:  # noqa: BLE001 - surface any Max/import failure
        return ImportResult.failure(
            request,
            error=str(exc),
            output_path=request.likely_output_dir,
            elapsed_seconds=time.monotonic() - start,
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
    write_max: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
    reset_scene: bool = True,
) -> bool:
    """Import a standalone .3di and write selected Max-owned outputs."""
    written, _output_path = _run_loose_import_impl(
        threedi_path=threedi_path,
        output_dir=output_dir,
        output_stem=output_stem,
        asset_base_dir=asset_base_dir,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        write_max=write_max,
        write_ase=write_ase,
        write_3dp=write_3dp,
        reset_scene=reset_scene,
    )
    return bool(written)


def run_import(
    base_dir: str,
    item_name: str,
    item_type: str,
    output_root: str,
    *,
    output_stem: str = "",
    import_arms: bool = True,
    import_animations: bool = True,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    write_max: bool = True,
    write_ase: bool = True,
    write_3dp: bool = True,
) -> bool:
    """Import one DEF resource and write selected Max-owned outputs."""
    written, _output_path = _run_definition_import_impl(
        base_dir=base_dir,
        item_name=item_name,
        item_type=item_type,
        output_root=output_root,
        output_stem=output_stem,
        import_arms=import_arms,
        import_animations=import_animations,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        write_max=write_max,
        write_ase=write_ase,
        write_3dp=write_3dp,
    )
    return bool(written)


def _execute_to_files(request: ImportRequest) -> tuple[list[str], str]:
    options = request.options
    if request.mode == IMPORT_MODE_LOOSE:
        raw = _run_loose_import_impl(
            threedi_path=request.threedi_path,
            output_dir=request.loose_output_dir,
            output_stem=request.output_stem or request.display_name,
            asset_base_dir=request.base_dir or None,
            import_collisions=options.import_collisions,
            import_occlusion=options.import_occlusion,
            import_lights=options.import_lights,
            write_max=options.write_max,
            write_ase=options.write_ase,
            write_3dp=options.write_3dp,
            reset_scene=True,
            game=request.game,
        )
        return _coerce_written_output(raw, request.loose_output_dir)

    raw = _run_definition_import_impl(
        base_dir=request.base_dir,
        item_name=request.item_name,
        item_type=request.item_type,
        output_root=request.output_root,
        output_stem=request.output_stem,
        import_arms=options.import_arms,
        import_animations=options.import_animations,
        import_collisions=options.import_collisions,
        import_occlusion=options.import_occlusion,
        import_lights=options.import_lights,
        write_max=options.write_max,
        write_ase=options.write_ase,
        write_3dp=options.write_3dp,
        game=request.game,
    )
    return _coerce_written_output(raw, request.likely_output_dir)


def _run_loose_import_impl(
    *,
    threedi_path: str,
    output_dir: str,
    output_stem: str | None,
    asset_base_dir: str | None,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    write_max: bool,
    write_ase: bool,
    write_3dp: bool,
    reset_scene: bool,
    game: str = "jo",
) -> tuple[list[str], str]:
    _require_output(write_max=write_max, write_ase=write_ase, write_3dp=write_3dp)
    built, written = execute_loose_import(
        threedi_path=threedi_path,
        output_dir=output_dir,
        output_stem=output_stem,
        asset_base_dir=asset_base_dir,
        builder_factory=_build_max_scene_builder,
        output_writer=_make_output_writer(
            write_max=write_max,
            write_ase=write_ase,
            write_3dp=write_3dp,
        ),
        reset_scene_fn=_reset_max_scene if reset_scene else None,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        game=game,
    )
    if not built:
        raise RuntimeError("3ds Max scene builder did not create any meshes.")
    return written, output_dir


def _run_definition_import_impl(
    *,
    base_dir: str,
    item_name: str,
    item_type: str,
    output_root: str,
    output_stem: str,
    import_arms: bool,
    import_animations: bool,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    write_max: bool,
    write_ase: bool,
    write_3dp: bool,
    game: str = "jo",
) -> tuple[list[str], str]:
    _require_output(write_max=write_max, write_ase=write_ase, write_3dp=write_3dp)
    built, written, project_dir = execute_definition_import(
        base_dir=base_dir,
        item_name=item_name,
        item_type=item_type,
        output_dir=output_root,
        builder_factory=_build_max_scene_builder,
        output_writer=_make_output_writer(
            write_max=write_max,
            write_ase=write_ase,
            write_3dp=write_3dp,
        ),
        post_main_build=(lambda builder: builder.apply_animations()) if import_animations else None,
        secondary_texture_writer=None,
        reset_scene_fn=_reset_max_scene,
        import_arms=import_arms,
        import_animations=import_animations,
        import_collisions=import_collisions,
        import_occlusion=import_occlusion,
        import_lights=import_lights,
        output_stem=output_stem,
        game=game,
    )
    if not built:
        raise RuntimeError("3ds Max scene builder did not create any meshes.")
    return written, project_dir


def _make_output_writer(*, write_max: bool, write_ase: bool, write_3dp: bool):
    from .output_writers import write_outputs

    def writer(model, output_dir, name, builder, resolver):
        return write_outputs(
            model,
            output_dir,
            name,
            builder,
            resolver,
            write_max=write_max,
            write_ase=write_ase,
            write_3dp=write_3dp,
            copy_textures=True,
        )

    return writer


def _build_max_scene_builder(model, **kwargs):
    from .scene_builder import MaxSceneBuilder

    return MaxSceneBuilder(model, **kwargs)


def _reset_max_scene() -> None:
    from .output_writers import reset_scene

    reset_scene()


def _require_output(*, write_max: bool, write_ase: bool, write_3dp: bool) -> None:
    if not (write_max or write_ase or write_3dp):
        raise ValueError("Select .max, .ase, or .3dp output for 3ds Max imports.")


def _coerce_written_output(raw, default_output_path: str) -> tuple[list[str], str]:
    if isinstance(raw, tuple) and len(raw) == 2:
        written, output_path = raw
    else:
        written, output_path = raw, default_output_path
    if isinstance(written, (str, os.PathLike)):
        written_files = [os.fspath(written)]
    else:
        written_files = [os.fspath(path) for path in written]
    return written_files, os.fspath(output_path or default_output_path)
