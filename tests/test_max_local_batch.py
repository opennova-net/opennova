"""3ds Max batch listener validation."""
from __future__ import annotations


REQUIRED_VALIDATION_MILESTONES = (
    "OPENNOVA_MAX_LOCAL_START",
    "OPENNOVA_MAX_QT_UI_MODULE_OK",
    "OPENNOVA_MAX_LOCAL_RESULT True",
    "OPENNOVA_MAX_MATERIALS_OK",
    "OPENNOVA_MAX_VEHICLE_RESULT True",
    "OPENNOVA_MAX_VEHICLE_PLACEMENT_OK",
    "OPENNOVA_MAX_SKIN_RESULT True",
    "OPENNOVA_MAX_SKIN_OK",
)
KNOWN_STARTUP_FAILURES = (
    "OpenNova Max editable startup failed: No module named 'opennova_max'",
)


def _missing_validation_milestones(listener: str) -> list[str]:
    return [
        milestone
        for milestone in REQUIRED_VALIDATION_MILESTONES
        if milestone not in listener
    ]


def _max_batch_completed_validation(returncode: int, listener: str) -> bool:
    if _missing_validation_milestones(listener):
        return False
    if returncode == 0:
        return True
    return any(failure in listener for failure in KNOWN_STARTUP_FAILURES)


def test_max_batch_validation_accepts_known_startup_failure_after_completion() -> None:
    listener = "\n".join(
        [
            "OpenNova Max editable startup failed: No module named 'opennova_max'",
            *REQUIRED_VALIDATION_MILESTONES,
        ]
    )
    assert _max_batch_completed_validation(4294967166, listener)


def test_max_batch_validation_rejects_missing_milestone() -> None:
    listener = "\n".join(REQUIRED_VALIDATION_MILESTONES[:-1])
    assert not _max_batch_completed_validation(0, listener)


def test_max_batch_validation_rejects_unknown_nonzero_exit() -> None:
    listener = "\n".join(REQUIRED_VALIDATION_MILESTONES)
    assert not _max_batch_completed_validation(1, listener)
