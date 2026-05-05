"""Small data models used by the standalone importer GUI."""
from __future__ import annotations

from dataclasses import dataclass

from apps.importer.jobs import ImportOptions, ImportRequest


CUSTOM_PRESET_LABEL = "Custom"


@dataclass(frozen=True)
class OptionPreset:
    label: str
    options: ImportOptions
    description: str


@dataclass(frozen=True)
class LogEntry:
    time: str
    message: str
    level: str = "info"
    job_id: str = ""


@dataclass(frozen=True)
class CollisionChoice:
    requests: list[ImportRequest]
    canceled: bool = False
    skipped_count: int = 0


OPTION_PRESET_DEFS: tuple[OptionPreset, ...] = (
    OptionPreset(
        label="Round-trip",
        description="Writes Blender, ASE, and 3DP/3DA files for editable round trips.",
        options=ImportOptions(
            import_animations=True,
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            import_arms=True,
            write_blend=True,
            write_3dp=True,
            write_ase=True,
            write_glb=False,
            write_fbx=False,
        ),
    ),
    OptionPreset(
        label="Blender only",
        description="Writes only a .blend scene for inspection or Blender editing.",
        options=ImportOptions(
            import_animations=True,
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            import_arms=True,
            write_blend=True,
            write_3dp=False,
            write_ase=False,
            write_glb=False,
            write_fbx=False,
        ),
    ),
    OptionPreset(
        label="Godot/runtime export",
        description="Writes runtime-friendly GLB plus Nova project metadata.",
        options=ImportOptions(
            import_animations=True,
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            import_arms=True,
            write_blend=False,
            write_3dp=True,
            write_ase=False,
            write_glb=True,
            write_fbx=False,
        ),
    ),
    OptionPreset(
        label="Debug all geometry",
        description="Writes every supported file format for diagnosis and comparison.",
        options=ImportOptions(
            import_animations=True,
            import_collisions=True,
            import_occlusion=True,
            import_lights=True,
            import_arms=True,
            write_blend=True,
            write_3dp=True,
            write_ase=True,
            write_glb=True,
            write_fbx=True,
        ),
    ),
)

OPTION_PRESETS: dict[str, ImportOptions] = {
    preset.label: preset.options for preset in OPTION_PRESET_DEFS
}
PRESET_DESCRIPTIONS: dict[str, str] = {
    preset.label: preset.description for preset in OPTION_PRESET_DEFS
}


def preset_name_for_options(options: ImportOptions) -> str:
    for name, preset in OPTION_PRESETS.items():
        if options == preset:
            return name
    return CUSTOM_PRESET_LABEL
