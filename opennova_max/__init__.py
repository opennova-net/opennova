"""3ds Max addon for importing OpenNova .3di assets.

Runs inside 3ds Max 2021 or newer (Python 3 + PyMXS). Install the
release MZP by dragging it into Max (see opennova_max/README.md), then
open the importer from OpenNova > Importer... or use the Max listener:

    python.execute "from opennova_max import import_loose; import_loose(r'C:/path/to/Shed.3di')"
    python.execute "from opennova_max import show_importer; show_importer()"

The scene builder mirrors the Blender importer, while batch outputs use
shared host-neutral writers for ASE and 3DP/3DA.

``from __future__ import annotations`` is required: Max 2022 ships
Python 3.7, where the ``X | None`` union syntax fails at runtime
without it.
"""
from __future__ import annotations

import os


def import_loose(threedi_path: str, asset_base_dir: str | None = None) -> bool:
    """Import a single .3di into the running Max scene.

    Parameters
    ----------
    threedi_path:
        Absolute path to a .3di file.
    asset_base_dir:
        Optional override for texture/material search root. Defaults to
        the directory containing ``threedi_path``.

    Returns ``True`` on success, ``False`` if the file produced no
    geometry. Raises on parse error.
    """
    from pyopennova.threedi_ffi import read_model_ir, free_model_ir
    from pyopennova.asset_resolver import AssetResolver
    from .scene_builder import MaxSceneBuilder

    base_dir = asset_base_dir or os.path.dirname(threedi_path)
    name = os.path.splitext(os.path.basename(threedi_path))[0]

    ir = read_model_ir(threedi_path)
    try:
        with AssetResolver(base_dir) as resolver:
            builder = MaxSceneBuilder(ir, resolver=resolver)
            return builder.build_basic_scene(name)
    finally:
        free_model_ir(ir)


from .import_runner import (  # noqa: E402
    MaxImportRequest,
    MaxImportResult,
    execute_import_request,
    run_batch,
    run_import,
    run_loose_import,
)
from .ui import close_importer, register_menu, show_importer  # noqa: E402
from .version import __version__, get_version  # noqa: E402


__all__ = [
    "MaxImportRequest",
    "MaxImportResult",
    "__version__",
    "close_importer",
    "execute_import_request",
    "get_version",
    "import_loose",
    "register_menu",
    "run_batch",
    "run_import",
    "run_loose_import",
    "show_importer",
]
