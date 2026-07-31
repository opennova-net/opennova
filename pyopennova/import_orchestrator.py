"""DCC-neutral scaffolding for loose / definition imports.

Both ``opennova_max/import_runner.py`` and ``opennova_blender/import_runner.py``
had parallel copies of the same orchestration: resolve the import plan, walk
the model list, attach a scene builder, write outputs, and clean up the
loaded 3DI / BAD handles. This module owns that scaffolding once; each DCC
supplies a small `builder_factory` plus `output_writer` callable and delegates.

Lifetime contract:
  - Loaded 3DI IRs are freed via the ``loaded_3di_model`` context manager.
  - Parsed BAD files are freed via the ``parsed_bad_file`` context manager.
  - Callers don't need to wrap their builder/writer in try/finally — the
    orchestrator handles failure cleanup.

Design note on callbacks: the alternative was a heavyweight ``SceneBuilder``
protocol with a long abstract method set. We chose plain callables instead
because the per-DCC builder classes (``MaxSceneBuilder``,
``BlenderSceneBuilder``) already share the same constructor + method names
by convention; constraining them via a protocol would constrain refactoring
without preventing any real bug.
"""
from __future__ import annotations

import os
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterator


@contextmanager
def loaded_3di_model(threedi_path: str | Path) -> Iterator[object]:
    """Read a 3DI into an IR pointer; ``free_model_3di3`` on exit."""
    from pyopennova.threedi_ffi import free_model_3di3, read_model

    ir = read_model(str(threedi_path))
    try:
        yield ir
    finally:
        free_model_3di3(ir)


@contextmanager
def parsed_bad_file(bad_path: str | Path | None) -> Iterator[object | None]:
    """Parse a BAD file if a path is given; ``free_bad`` on exit. Yields ``None``
    when ``bad_path`` is falsy or parsing fails (matches both DCCs' soft-fail).
    """
    from pyopennova.bad_ffi import free_bad, parse_bad

    if not bad_path:
        yield None
        return

    bad = None
    try:
        bad = parse_bad(str(bad_path))
    except Exception:  # noqa: BLE001 — both DCCs soft-fail BAD parse errors
        bad = None
    try:
        yield bad
    finally:
        if bad is not None:
            free_bad(bad)


def execute_loose_import(
    *,
    threedi_path: str,
    output_dir: str,
    output_stem: str | None,
    asset_base_dir: str | None,
    builder_factory: Callable,
    output_writer: Callable,
    reset_scene_fn: Callable[[], None] | None,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    game: str = "jo",
) -> tuple[bool, list[str]]:
    """Loose import: read one 3DI, build a scene, write outputs.

    ``builder_factory(ir, *, resolver, import_collisions, import_occlusion,
    import_lights)`` -> scene builder instance.

    ``output_writer(ir, output_dir, name, builder, resolver)`` -> list of
    written paths. ``resolver`` is the active ``AssetResolver`` (some DCCs
    need it for texture copying; Max's writer ignores it).

    Returns ``(built_ok, written)``. ``built_ok`` reflects whether the
    scene builder succeeded; ``written`` is the writer's output list (may be
    empty even when ``built_ok`` is True, e.g., when all output flags are
    off but the builder still produced a scene).
    """
    from pyopennova.asset_resolver import AssetResolver

    if reset_scene_fn is not None:
        reset_scene_fn()
    os.makedirs(output_dir, exist_ok=True)

    base_dir = asset_base_dir or str(Path(threedi_path).parent)
    name = output_stem or Path(threedi_path).stem

    with loaded_3di_model(threedi_path) as ir, AssetResolver(base_dir, game=game) as resolver:
        builder = builder_factory(
            ir,
            resolver=resolver,
            import_collisions=import_collisions,
            import_occlusion=import_occlusion,
            import_lights=import_lights,
        )
        if not builder.build_basic_scene(name):
            return False, []
        return True, list(output_writer(ir, output_dir, name, builder, resolver))


def execute_definition_import(
    *,
    base_dir: str,
    item_name: str,
    item_type: str,
    output_dir: str,
    builder_factory: Callable,
    output_writer: Callable,
    post_main_build: Callable | None,
    secondary_texture_writer: Callable | None,
    reset_scene_fn: Callable[[], None] | None,
    import_arms: bool,
    import_animations: bool,
    import_collisions: bool,
    import_occlusion: bool,
    import_lights: bool,
    output_stem: str,
    game: str = "jo",
) -> tuple[bool, list[str], str]:
    """Definition import: resolve a plan, walk its main/secondary models, write.

    ``builder_factory(ir, *, bad_file, anim_context, resolver,
    import_collisions, import_occlusion, import_lights)`` -> scene builder.

    ``output_writer(main_ir, project_dir, name, main_builder, resolver)``
    -> list of written paths. ``resolver`` is the active ``AssetResolver``
    (Blender's writer uses it for texture copy; Max's ignores it).

    ``post_main_build(main_builder)`` -> None (called between main build and
    write; Max uses this for ``apply_animations``; Blender passes None
    because animation is wired during build).

    ``secondary_texture_writer(ir, project_dir, resolver)`` -> None (Blender
    copies textures per-secondary; Max bundles it into ``output_writer``).

    Returns ``(built_ok, written_paths, project_dir)``. ``built_ok`` tracks
    whether the main scene builder succeeded. ``project_dir`` is the empty
    string when the plan could not be resolved.
    """
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.resource_plan import resolve_definition_import
    from pyopennova.threedi_ffi import free_model_3di3, read_model

    if reset_scene_fn is not None:
        reset_scene_fn()

    with AssetResolver(base_dir, game=game) as resolver:
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
            return False, [], ""

        project_dir = os.path.join(output_dir, plan.export_name)
        os.makedirs(project_dir, exist_ok=True)

        written: list[str] = []
        with parsed_bad_file(plan.reset_bad_path) as bad_file:
            main_builder = None
            main_ir = None
            main_built = False
            try:
                for model in plan.models:
                    ir = read_model(model.path)
                    try:
                        if model.role == "main":
                            main_ir = ir
                            ctx = plan.animation_context if import_animations else None
                            main_builder = builder_factory(
                                ir,
                                bad_file=bad_file,
                                anim_context=ctx,
                                resolver=resolver,
                                import_collisions=import_collisions,
                                import_occlusion=import_occlusion,
                                import_lights=import_lights,
                            )
                            main_built = bool(main_builder.build_basic_scene(plan.scene_name))
                        elif main_builder is not None and main_built:
                            secondary = builder_factory(
                                ir,
                                bad_file=bad_file,
                                anim_context=None,
                                resolver=resolver,
                                import_collisions=import_collisions,
                                import_occlusion=import_occlusion,
                                import_lights=import_lights,
                            )
                            if secondary.merge_with_existing_scene(main_builder):
                                if secondary_texture_writer is not None:
                                    secondary_texture_writer(ir, project_dir, resolver)
                    finally:
                        if model.role != "main":
                            free_model_3di3(ir)

                if main_built and main_builder is not None and main_ir is not None:
                    if post_main_build is not None:
                        post_main_build(main_builder)
                    written = list(output_writer(main_ir, project_dir, plan.export_name, main_builder, resolver))
            finally:
                if main_ir is not None:
                    free_model_3di3(main_ir)

        return main_built, written, project_dir
