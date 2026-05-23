from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import textwrap


ROOT = Path(__file__).resolve().parents[1]


def run_blender_python(script: str, *args: str | Path) -> None:
    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script), *(str(arg) for arg in args)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, (
        f"child Python failed with exit code {result.returncode}\n"
        f"STDOUT:\n{result.stdout}\n"
        f"STDERR:\n{result.stderr}"
    )
