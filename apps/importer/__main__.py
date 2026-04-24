"""Entry point for `python -m apps.importer`."""
import logging
import os
import sys

# Set up file logging before any other imports so all downstream loggers
# (import_runner, scene_builder, bpy_session, etc.) inherit this handler.
_log_path = os.path.join(os.path.dirname(os.path.abspath(sys.executable
    if getattr(sys, 'frozen', False) else __file__)), "onimport.log")
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
        from apps.importer.ui.app import run_gui
        run_gui()
    else:
        log.debug("Starting OpenNova Importer CLI (log: %s)", _log_path)
        from apps.importer.cli import run_cli
        run_cli(args)

if __name__ == "__main__":
    main()
