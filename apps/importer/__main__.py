"""Entry point for `python -m apps.importer`."""
import logging
import multiprocessing
import os
import sys
from pathlib import Path

multiprocessing.freeze_support()

# Set up file logging before any other imports so all downstream loggers
# (import_runner, scene_builder, bpy_session, etc.) inherit this handler.
def _log_dir() -> Path:
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "OpenNova"
    return Path.home() / ".opennova"


_log_dir().mkdir(parents=True, exist_ok=True)
_log_path = str(_log_dir() / "onimport.log")
_file_handler = logging.FileHandler(_log_path, mode="w", encoding="utf-8")
_file_handler.setLevel(logging.DEBUG)
_console_handler = logging.StreamHandler(sys.stdout)
_console_handler.setLevel(logging.INFO)
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-8s [%(name)s] %(message)s",
    handlers=[_file_handler, _console_handler],
)

log = logging.getLogger(__name__)

def main():
    args = sys.argv[1:]
    if not args or args[0] == "gui":
        log.info("Loading OpenNova Importer... (log: %s)", _log_path)
        from apps.importer.ui.qt_app import run_gui
        run_gui()
    else:
        log.debug("Starting OpenNova Importer CLI (log: %s)", _log_path)
        from apps.importer.cli import run_cli
        run_cli(args)

if __name__ == "__main__":
    main()
