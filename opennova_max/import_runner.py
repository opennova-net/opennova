"""3ds Max-side import runner entry points.

The external runner and JSON protocol are host-agnostic. The actual 3ds Max
scene construction belongs behind this module so the parent app does not need
to know Max internals.
"""
from __future__ import annotations

from typing import Iterable, List

from opennova_jobs import ImportRequest, ImportResult


SUPPORTS_SCENE_IMPORT = False


def run_batch(requests):
    # type: (Iterable[ImportRequest]) -> List[ImportResult]
    """Run a grouped batch inside the current 3ds Max process."""
    return [execute_import_request(request) for request in requests]


def execute_import_request(request):
    # type: (ImportRequest) -> ImportResult
    return ImportResult.failure(
        request,
        error=(
            "3ds Max scene construction is not implemented in this build yet. "
            "The external batch protocol is available, but the Max scene builder "
            "still needs to be added before .max output can be written."
        ),
        output_path=request.likely_output_dir,
    )
