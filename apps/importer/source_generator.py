"""Host-neutral source generation for stock 3DI -> ASE/3DP roundtrips."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class GeneratedSource:
    directory: Path
    project_path: Path
    ase_paths: tuple[Path, ...]

    @property
    def written_files(self) -> tuple[Path, ...]:
        files: list[Path] = []
        if self.project_path.is_file():
            files.append(self.project_path)
            tda_path = self.project_path.with_suffix(".3da")
            if tda_path.is_file():
                files.append(tda_path)
        files.extend(path for path in self.ase_paths if path.is_file())
        return tuple(files)


def generate_source_from_3di(
    threedi_path: str | Path,
    output_dir: str | Path,
    *,
    output_stem: str | None = None,
    import_collisions: bool = True,
    import_occlusion: bool = True,
    import_lights: bool = True,
    write_3dp: bool = True,
    write_ase: bool = True,
    collision_lod_index: int | None = None,
    bad_file=None,
) -> GeneratedSource:
    """Generate OED source files from a .3di without importing Blender."""
    from .import_runner import _setup_blender_package

    _setup_blender_package()

    from blender.opennova.project_writer import write_3dp_from_3di3  # type: ignore[import]
    from blender.opennova.threedi_ffi import free_model_3di3, read_model_auto  # type: ignore[import]

    source_path = Path(threedi_path)
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = output_stem or source_path.stem
    project_path = out_dir / f"{stem}.3dp"
    ase_path = out_dir / f"{stem}.ase"

    model = read_model_auto(str(source_path))
    try:
        if write_3dp:
            write_3dp_from_3di3(
                model,
                str(project_path),
                collision_lod_index=(
                    collision_lod_index if import_collisions else -1
                ),
            )

        ase_paths: tuple[Path, ...] = ()
        if write_ase:
            from blender.opennova.ase_writer_ffi import write_ase_files_from_3di3  # type: ignore[import]

            ase_paths = tuple(
                Path(path)
                for path in write_ase_files_from_3di3(
                    model,
                    ase_path,
                    bad_file=bad_file,
                    include_collisions=import_collisions,
                    include_occlusion=import_occlusion,
                    include_lights=import_lights,
                )
            )
    finally:
        free_model_3di3(model)

    return GeneratedSource(
        directory=out_dir,
        project_path=project_path,
        ase_paths=ase_paths,
    )
