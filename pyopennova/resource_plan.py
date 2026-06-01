"""Shared resource resolution for DCC import runners."""
from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

log = logging.getLogger(__name__)


@dataclass(frozen=True)
class PlannedModel:
    """One model file participating in a definition import."""

    role: str
    source_name: str
    path: str


@dataclass(frozen=True)
class DefinitionImportPlan:
    """Resolved DEF import inputs independent of Blender or 3ds Max."""

    item_name: str
    item_type: str
    scene_name: str
    export_name: str
    models: Tuple[PlannedModel, ...]
    animation_context: object = None
    reset_bad_path: Optional[str] = None


def resolve_definition_import(
    base_dir: str,
    item_name: str,
    item_type: str,
    resolver,
    *,
    import_arms: bool = False,
    import_animations: bool = True,
    output_name: str = "",
) -> Optional[DefinitionImportPlan]:
    """Resolve a weapon/item definition into concrete model and animation paths.

    Returns ``None`` when the requested definition cannot produce a model.
    Missing model files still raise ``FileNotFoundError`` because the caller
    asked for a concrete resource and should see the bad reference.
    """
    from pyopennova.definitions import (
        build_animation_context,
        ensure_extension,
        process_def_files,
        resolve_reset_bad_path,
    )

    del base_dir  # kept for call-site symmetry with older runner code

    weapons, items = process_def_files(resolver=resolver)

    if item_type == "weapon":
        target_context = next((w for w in weapons if w.name == item_name), None)
    else:
        target_context = next((i for i in items if i.name == item_name), None)

    if target_context is None:
        return None

    model_specs: list[tuple[str, str]] = []
    if item_type == "weapon":
        main_file = target_context.graphic1.main
    else:
        main_file = target_context.graphic_us

    if main_file:
        main_file = ensure_extension(main_file, ".3di")
        model_specs.append(("main", main_file))

    if (
        item_type == "weapon"
        and import_arms
        and getattr(target_context.graphic1, "arms", None)
    ):
        arms_file = ensure_extension(target_context.graphic1.arms, ".3di")
        model_specs.append(("arms", arms_file))

    if not model_specs:
        return None

    resolved_models: list[PlannedModel] = []
    for role, model_file in model_specs:
        model_path = resolver.resolve(model_file)
        if model_path is None:
            raise FileNotFoundError(f"Model file not found: {model_file}")
        resolved_models.append(
            PlannedModel(role=role, source_name=model_file, path=str(model_path))
        )

    graphic_name = Path(main_file).stem if main_file else item_name
    export_name = output_name if output_name else graphic_name

    anim_ctx = None
    reset_bad_path = None
    anim_field = (
        target_context.anim_adm
        if item_type == "weapon"
        else target_context.anim_def
    )
    if anim_field:
        if import_animations:
            try:
                anim_ctx = build_animation_context(anim_field, resolver=resolver)
                if anim_ctx and anim_ctx.reset_animation:
                    reset_bad_path = str(anim_ctx.reset_animation.bad_filepath)
            except Exception as exc:
                log.warning("Could not load BAD file: %s", exc)
        else:
            try:
                reset_bad_path = resolve_reset_bad_path(anim_field, resolver=resolver)
            except Exception as exc:
                log.warning("Could not resolve reset BAD file: %s", exc)

    return DefinitionImportPlan(
        item_name=item_name,
        item_type=item_type,
        scene_name=item_name,
        export_name=export_name,
        models=tuple(resolved_models),
        animation_context=anim_ctx,
        reset_bad_path=reset_bad_path,
    )


def resolve_definition_output_stem(base_dir: str, item_name: str, item_type: str) -> str:
    """Return the output directory/file stem for a definition import."""
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.definitions import ensure_extension, process_def_files

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
