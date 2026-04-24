"""Standalone importer GUI built with tkinter."""
from __future__ import annotations

import json
import logging
import os
import subprocess
import sys
import threading
import time
import tkinter as tk
from dataclasses import asdict
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk

from apps.importer.jobs import (
    ACTIVE_JOB_STATUSES,
    JOB_DONE,
    JOB_ERROR,
    JOB_PENDING,
    JOB_RUNNING,
    ImportJob,
    ImportOptions,
    ImportRequest,
    ScanResult,
    validate_import_request,
)


log = logging.getLogger(__name__)

MAX_RECENT_PATHS = 10
LOG_FILTER_ALL = "All"
LOG_FILTER_CURRENT = "Current Job"
LOG_FILTER_ERRORS = "Errors"
LOG_FILTERS = (LOG_FILTER_ALL, LOG_FILTER_CURRENT, LOG_FILTER_ERRORS)
CUSTOM_PRESET_LABEL = "Custom"

OPTION_PRESETS: dict[str, ImportOptions] = {
    "Round-trip": ImportOptions(
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
    "Blender only": ImportOptions(
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
    "Godot/runtime export": ImportOptions(
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
    "Debug all geometry": ImportOptions(
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
}


def preset_name_for_options(options: ImportOptions) -> str:
    for name, preset in OPTION_PRESETS.items():
        if options == preset:
            return name
    return CUSTOM_PRESET_LABEL


class ImporterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self._prefs = self._load_preferences()
        self.title("OpenNova Importer")
        self.geometry(str(self._prefs.get("window_geometry") or "1180x800"))
        self.minsize(940, 620)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self._jobs: list[ImportJob] = []
        self._jobs_lock = threading.Lock()
        self._worker: threading.Thread | None = None
        self._scanning = False
        self._closing = False
        self._applying_preset = False

        self._all_items: list[dict[str, str]] = []
        self._visible_items: list[dict[str, str]] = []
        self._last_scanned_dir = ""
        self._recent_game_dirs = self._clean_recent_paths(self._prefs.get("recent_game_dirs"))
        self._recent_output_dirs = self._clean_recent_paths(self._prefs.get("recent_output_dirs"))
        self._item_sort_column = str(self._prefs.get("item_sort_column") or "name")
        self._item_sort_reverse = bool(self._prefs.get("item_sort_reverse", False))
        self._queue_sort_column = str(self._prefs.get("queue_sort_column") or "")
        self._queue_sort_reverse = bool(self._prefs.get("queue_sort_reverse", False))
        self._log_entries: list[dict[str, str]] = []

        self._build_ui()
        self._refresh_queue()
        self._update_action_states()
        self._post_ui(0, self._restore_layout)
        log_path = self._current_log_path()
        if log_path:
            self._log_append(f"Log file: {log_path}")

    # ------------------------------------------------------------------
    # Preferences
    # ------------------------------------------------------------------

    def _settings_path(self) -> Path:
        appdata = os.environ.get("APPDATA")
        if appdata:
            return Path(appdata) / "OpenNova" / "onimport_settings.json"
        return Path.home() / ".opennova" / "onimport_settings.json"

    def _load_preferences(self) -> dict:
        path = self._settings_path()
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        return data if isinstance(data, dict) else {}

    def _save_preferences(self) -> None:
        prefs = {
            "window_geometry": self.winfo_geometry(),
            "paned_sash": self._current_sash_position(),
            "recent_game_dirs": self._recent_game_dirs,
            "recent_output_dirs": self._recent_output_dirs,
            "last_game_dir": self._dir_var.get().strip(),
            "last_output_dir": self._out_var.get().strip(),
            "last_options": asdict(self._build_options()),
            "item_sort_column": self._item_sort_column,
            "item_sort_reverse": self._item_sort_reverse,
            "queue_sort_column": self._queue_sort_column,
            "queue_sort_reverse": self._queue_sort_reverse,
            "log_filter": self._log_filter_var.get(),
        }
        path = self._settings_path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(prefs, indent=2), encoding="utf-8")
        except OSError as exc:
            log.debug("Could not save importer preferences: %s", exc)

    def _restore_layout(self) -> None:
        sash_pos = self._prefs.get("paned_sash")
        if isinstance(sash_pos, int) and sash_pos > 0:
            try:
                self._paned.sashpos(0, sash_pos)
            except tk.TclError:
                pass

    def _current_sash_position(self) -> int:
        try:
            return int(self._paned.sashpos(0))
        except (tk.TclError, AttributeError):
            return 0

    def _clean_recent_paths(self, value: object) -> list[str]:
        if not isinstance(value, list):
            return []
        cleaned: list[str] = []
        for item in value:
            if not isinstance(item, str):
                continue
            path = item.strip()
            if path and path not in cleaned:
                cleaned.append(path)
        return cleaned[:MAX_RECENT_PATHS]

    def _options_from_preferences(self) -> ImportOptions:
        defaults = ImportOptions()
        raw = self._prefs.get("last_options")
        if not isinstance(raw, dict):
            return defaults
        values = {}
        for key, default in asdict(defaults).items():
            value = raw.get(key, default)
            values[key] = bool(value)
        return ImportOptions(**values)

    def _remember_recent_path(self, kind: str, path: str) -> None:
        clean = path.strip()
        if not clean:
            return
        target = self._recent_game_dirs if kind == "game" else self._recent_output_dirs
        if clean in target:
            target.remove(clean)
        target.insert(0, clean)
        del target[MAX_RECENT_PATHS:]
        if kind == "game":
            self._dir_combo.configure(values=self._recent_game_dirs)
        else:
            self._out_combo.configure(values=self._recent_output_dirs)

    def _on_close(self) -> None:
        self._closing = True
        self._save_preferences()
        self.destroy()

    def _post_ui(self, delay_ms: int, callback, *args) -> None:
        if self._closing:
            return
        try:
            self.after(delay_ms, callback, *args)
        except tk.TclError:
            pass

    # ------------------------------------------------------------------
    # Layout
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        saved_options = self._options_from_preferences()
        self._dir_var = tk.StringVar(value=str(self._prefs.get("last_game_dir") or ""))
        self._out_var = tk.StringVar(value=str(self._prefs.get("last_output_dir") or ""))
        self._search_var = tk.StringVar()
        self._tab_var = tk.StringVar(value="all")
        self._preset_var = tk.StringVar(value=preset_name_for_options(saved_options))
        self._log_filter_var = tk.StringVar(
            value=str(self._prefs.get("log_filter") or LOG_FILTER_ALL)
            if self._prefs.get("log_filter") in LOG_FILTERS
            else LOG_FILTER_ALL
        )
        self._scan_status_var = tk.StringVar(value="Choose a game directory and scan.")
        self._filter_status_var = tk.StringVar(value="No scan results.")
        self._asset_empty_var = tk.StringVar(value="No scanned items. Pick a game directory and scan.")
        self._queue_empty_var = tk.StringVar(value="Queue is empty.")
        self._queue_summary_var = tk.StringVar(value="Queue: 0 pending, 0 running, 0 done, 0 failed")

        self._flag_animations = tk.BooleanVar(value=saved_options.import_animations)
        self._flag_collisions = tk.BooleanVar(value=saved_options.import_collisions)
        self._flag_occlusion = tk.BooleanVar(value=saved_options.import_occlusion)
        self._flag_lights = tk.BooleanVar(value=saved_options.import_lights)
        self._flag_arms = tk.BooleanVar(value=saved_options.import_arms)
        self._flag_write_blend = tk.BooleanVar(value=saved_options.write_blend)
        self._flag_write_3dp = tk.BooleanVar(value=saved_options.write_3dp)
        self._flag_write_ase = tk.BooleanVar(value=saved_options.write_ase)
        self._flag_write_glb = tk.BooleanVar(value=saved_options.write_glb)
        self._flag_write_fbx = tk.BooleanVar(value=saved_options.write_fbx)

        top = ttk.Frame(self, padding=(8, 6))
        top.pack(fill=tk.X)
        ttk.Label(top, text="Game directory:").pack(side=tk.LEFT)
        self._dir_combo = ttk.Combobox(top, textvariable=self._dir_var, values=self._recent_game_dirs)
        self._dir_combo.pack(side=tk.LEFT, padx=6, fill=tk.X, expand=True)
        ttk.Button(top, text="Browse...", command=self._browse_dir).pack(side=tk.LEFT)
        self._scan_btn = ttk.Button(top, text="Scan", command=self._scan)
        self._scan_btn.pack(side=tk.LEFT, padx=(6, 0))

        status_row = ttk.Frame(self, padding=(8, 0))
        status_row.pack(fill=tk.X)
        ttk.Label(status_row, textvariable=self._scan_status_var).pack(side=tk.LEFT)
        ttk.Label(status_row, textvariable=self._filter_status_var).pack(side=tk.RIGHT)

        self._paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        self._paned.pack(fill=tk.BOTH, expand=True, padx=8, pady=6)

        left = ttk.Frame(self._paned)
        self._paned.add(left, weight=3)

        tab_frame = ttk.Frame(left)
        tab_frame.pack(fill=tk.X, pady=(0, 4))
        for label, value in (("All", "all"), ("Weapons", "weapon"), ("Items", "item")):
            ttk.Radiobutton(
                tab_frame,
                text=label,
                variable=self._tab_var,
                value=value,
                command=self._on_filter_changed,
            ).pack(side=tk.LEFT, padx=(0, 8))

        search_frame = ttk.Frame(left)
        search_frame.pack(fill=tk.X, pady=(0, 4))
        ttk.Label(search_frame, text="Filter:").pack(side=tk.LEFT)
        ttk.Entry(search_frame, textvariable=self._search_var).pack(
            side=tk.LEFT, padx=4, fill=tk.X, expand=True
        )
        ttk.Button(search_frame, text="Clear", command=lambda: self._search_var.set("")).pack(side=tk.LEFT)
        ttk.Label(left, textvariable=self._asset_empty_var).pack(anchor=tk.W, pady=(0, 4))

        item_frame = ttk.Frame(left)
        item_frame.pack(fill=tk.BOTH, expand=True)
        self._item_tree = ttk.Treeview(
            item_frame,
            columns=("type", "name"),
            show="headings",
            selectmode="extended",
        )
        self._item_tree.column("type", width=80, stretch=False)
        self._item_tree.column("name", stretch=True)
        item_scroll = ttk.Scrollbar(item_frame, orient=tk.VERTICAL, command=self._item_tree.yview)
        self._item_tree.configure(yscrollcommand=item_scroll.set)
        self._item_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        item_scroll.pack(side=tk.LEFT, fill=tk.Y)

        item_buttons = ttk.Frame(left)
        item_buttons.pack(fill=tk.X, pady=(6, 0))
        self._import_selected_btn = ttk.Button(
            item_buttons,
            text="Queue Selected",
            command=self._import_selected,
        )
        self._import_selected_btn.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._export_all_btn = ttk.Button(
            item_buttons,
            text="Queue Visible (0)",
            command=self._export_all_visible,
        )
        self._export_all_btn.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))
        self._loose_btn = ttk.Button(
            item_buttons,
            text="Queue Loose .3di...",
            command=self._import_loose,
        )
        self._loose_btn.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))

        right = ttk.Frame(self._paned)
        self._paned.add(right, weight=2)

        output_lf = ttk.LabelFrame(right, text="Output directory", padding=6)
        output_lf.pack(fill=tk.X, pady=(0, 6))
        self._out_combo = ttk.Combobox(output_lf, textvariable=self._out_var, values=self._recent_output_dirs)
        self._out_combo.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        ttk.Button(output_lf, text="Browse...", command=self._browse_output).pack(side=tk.LEFT)

        import_lf = ttk.LabelFrame(right, text="Import options", padding=6)
        import_lf.pack(fill=tk.X, pady=(0, 6))
        preset_frame = ttk.Frame(import_lf)
        preset_frame.pack(fill=tk.X, pady=(0, 4))
        ttk.Label(preset_frame, text="Preset:").pack(side=tk.LEFT)
        self._preset_combo = ttk.Combobox(
            preset_frame,
            textvariable=self._preset_var,
            values=tuple(OPTION_PRESETS.keys()) + (CUSTOM_PRESET_LABEL,),
            state="readonly",
            width=22,
        )
        self._preset_combo.pack(side=tk.LEFT, padx=(4, 0))
        self._preset_combo.bind("<<ComboboxSelected>>", lambda _event: self._apply_selected_preset())

        option_grid = ttk.Frame(import_lf)
        option_grid.pack(fill=tk.X)
        ttk.Checkbutton(option_grid, text="Animations", variable=self._flag_animations).grid(
            row=0, column=0, sticky=tk.W, padx=(0, 12)
        )
        ttk.Checkbutton(option_grid, text="Collisions", variable=self._flag_collisions).grid(
            row=0, column=1, sticky=tk.W
        )
        ttk.Checkbutton(option_grid, text="Occlusion", variable=self._flag_occlusion).grid(
            row=1, column=0, sticky=tk.W, padx=(0, 12)
        )
        ttk.Checkbutton(option_grid, text="Lights", variable=self._flag_lights).grid(
            row=1, column=1, sticky=tk.W
        )
        ttk.Checkbutton(import_lf, text="Arms model", variable=self._flag_arms).pack(anchor=tk.W)

        format_lf = ttk.LabelFrame(right, text="Files to write", padding=6)
        format_lf.pack(fill=tk.X, pady=(0, 6))
        ttk.Checkbutton(
            format_lf,
            text="Blender scene (.blend)",
            variable=self._flag_write_blend,
        ).pack(anchor=tk.W)
        ttk.Checkbutton(
            format_lf,
            text="Project files (.3dp / .3da)",
            variable=self._flag_write_3dp,
        ).pack(anchor=tk.W)
        ttk.Checkbutton(format_lf, text="ASE (.ase)", variable=self._flag_write_ase).pack(anchor=tk.W)
        ttk.Checkbutton(
            format_lf,
            text="glTF 2.0 binary (.glb)",
            variable=self._flag_write_glb,
        ).pack(anchor=tk.W)
        ttk.Checkbutton(format_lf, text="FBX (.fbx)", variable=self._flag_write_fbx).pack(anchor=tk.W)

        queue_lf = ttk.LabelFrame(right, text="Queue", padding=6)
        queue_lf.pack(fill=tk.BOTH, expand=True)
        ttk.Label(queue_lf, textvariable=self._queue_summary_var).pack(anchor=tk.W)
        self._progress = ttk.Progressbar(queue_lf, mode="determinate", maximum=1, value=0)
        self._progress.pack(fill=tk.X, pady=(2, 6))
        ttk.Label(queue_lf, textvariable=self._queue_empty_var).pack(anchor=tk.W, pady=(0, 4))
        self._queue_tree = ttk.Treeview(
            queue_lf,
            columns=("type", "name", "status", "info"),
            show="headings",
            height=7,
        )
        for col, width, stretch in (
            ("type", 70, False),
            ("name", 120, True),
            ("status", 80, False),
            ("info", 180, True),
        ):
            self._queue_tree.column(col, width=width, stretch=stretch)
        self._queue_tree.pack(fill=tk.BOTH, expand=True)
        self._queue_tree.tag_configure(JOB_PENDING, foreground="gray")
        self._queue_tree.tag_configure(JOB_RUNNING, foreground="#b36b00")
        self._queue_tree.tag_configure(JOB_DONE, foreground="green")
        self._queue_tree.tag_configure(JOB_ERROR, foreground="red")

        queue_buttons = ttk.Frame(queue_lf)
        queue_buttons.pack(fill=tk.X, pady=(6, 0))
        self._remove_pending_btn = ttk.Button(
            queue_buttons,
            text="Remove Pending",
            command=self._remove_selected_pending,
        )
        self._remove_pending_btn.pack(side=tk.LEFT)
        self._retry_failed_btn = ttk.Button(
            queue_buttons,
            text="Retry Failed",
            command=self._retry_failed,
        )
        self._retry_failed_btn.pack(side=tk.LEFT, padx=(4, 0))
        self._clear_finished_btn = ttk.Button(
            queue_buttons,
            text="Clear Finished",
            command=self._clear_finished,
        )
        self._clear_finished_btn.pack(side=tk.LEFT, padx=(4, 0))

        details_lf = ttk.LabelFrame(queue_lf, text="Job details", padding=4)
        details_lf.pack(fill=tk.BOTH, expand=False, pady=(6, 0))
        self._job_detail = scrolledtext.ScrolledText(details_lf, height=6, state=tk.DISABLED, wrap=tk.WORD)
        self._job_detail.pack(fill=tk.BOTH, expand=True)

        log_lf = ttk.LabelFrame(self, text="Log", padding=4)
        log_lf.pack(fill=tk.BOTH, expand=False, padx=8, pady=(0, 8))
        log_buttons = ttk.Frame(log_lf)
        log_buttons.pack(fill=tk.X)
        ttk.Label(log_buttons, text="Show:").pack(side=tk.LEFT)
        self._log_filter_combo = ttk.Combobox(
            log_buttons,
            textvariable=self._log_filter_var,
            values=LOG_FILTERS,
            state="readonly",
            width=14,
        )
        self._log_filter_combo.pack(side=tk.LEFT, padx=(4, 0))
        self._log_filter_combo.bind("<<ComboboxSelected>>", lambda _event: self._render_log())
        ttk.Button(log_buttons, text="Clear", command=self._clear_log).pack(side=tk.RIGHT)
        self._log = scrolledtext.ScrolledText(log_lf, height=8, state=tk.DISABLED, wrap=tk.WORD)
        self._log.pack(fill=tk.BOTH, expand=True)

        self._build_queue_context_menu()
        self._wire_state_traces()
        self._set_item_headings()
        self._set_queue_headings()
        self._item_tree.bind("<<TreeviewSelect>>", lambda _event: self._update_action_states())
        self._item_tree.bind("<Double-1>", lambda _event: self._import_selected())
        self._queue_tree.bind("<<TreeviewSelect>>", lambda _event: self._on_queue_selection_changed())
        self._queue_tree.bind("<Button-3>", self._show_queue_context_menu)

    def _wire_state_traces(self) -> None:
        self._dir_var.trace_add("write", lambda *_: self._on_game_dir_changed())
        self._search_var.trace_add("write", lambda *_: self._on_filter_changed())
        self._out_var.trace_add("write", lambda *_: self._update_action_states())
        option_vars = (
            self._flag_animations,
            self._flag_collisions,
            self._flag_occlusion,
            self._flag_lights,
            self._flag_arms,
            self._flag_write_blend,
            self._flag_write_3dp,
            self._flag_write_ase,
            self._flag_write_glb,
            self._flag_write_fbx,
        )
        for var in option_vars:
            var.trace_add("write", lambda *_: self._on_options_changed())

    def _build_queue_context_menu(self) -> None:
        self._queue_menu = tk.Menu(self, tearoff=False)
        self._queue_menu.add_command(label="Retry Failed", command=self._retry_failed)
        self._queue_menu.add_command(label="Remove Pending", command=self._remove_selected_pending)
        self._queue_menu.add_separator()
        self._queue_menu.add_command(label="Copy Output Path", command=self._copy_selected_output_path)
        self._queue_menu.add_command(label="Copy Error", command=self._copy_selected_error)
        self._queue_menu.add_command(label="Open Output Folder", command=self._open_selected_output_folder)

    # ------------------------------------------------------------------
    # Browsing and scanning
    # ------------------------------------------------------------------

    def _browse_dir(self) -> None:
        path = filedialog.askdirectory(
            title="Select Game Directory",
            initialdir=self._dir_var.get() or None,
        )
        if path:
            self._dir_var.set(path)
            self._remember_recent_path("game", path)

    def _browse_output(self) -> None:
        path = filedialog.askdirectory(
            title="Select Output Directory",
            initialdir=self._out_var.get() or None,
        )
        if path:
            self._out_var.set(path)
            self._remember_recent_path("output", path)

    def _scan(self) -> None:
        game_dir = self._dir_var.get().strip()
        if self._scanning:
            return
        if not game_dir:
            self._show_errors(["Game directory is required."])
            return
        if not Path(game_dir).is_dir():
            self._show_errors(["Game directory does not exist."])
            return

        self._scanning = True
        self._scan_btn.config(state=tk.DISABLED)
        self._scan_status_var.set("Scanning...")
        self._log_append(f"Scanning: {game_dir}")
        threading.Thread(target=self._do_scan, args=(game_dir,), daemon=True).start()

    def _do_scan(self, game_dir: str) -> None:
        from apps.importer.import_runner import scan_directory_result

        result = scan_directory_result(game_dir)
        self._post_ui(0, self._scan_finished, game_dir, result)

    def _scan_finished(self, game_dir: str, result: ScanResult) -> None:
        if self._closing:
            return
        self._scanning = False
        if result.ok:
            self._last_scanned_dir = game_dir
            self._remember_recent_path("game", game_dir)
            self._all_items = result.items
            self._apply_filter()
            if result.items:
                self._scan_status_var.set(f"Scan complete: {len(result.items)} item(s).")
            else:
                self._scan_status_var.set("Scan complete: no items found.")
            self._log_append(f"Scan complete: {len(result.items)} item(s) found.")
        else:
            self._scan_status_var.set("Scan failed.")
            self._log_append(f"Scan failed: {result.error}", level="error")
            self._show_errors([result.error or "Scan failed."])
        self._update_action_states()

    # ------------------------------------------------------------------
    # Asset filtering and sorting
    # ------------------------------------------------------------------

    def _on_game_dir_changed(self) -> None:
        game_dir = self._dir_var.get().strip()
        if self._all_items and game_dir != self._last_scanned_dir:
            self._all_items = []
            self._visible_items = []
            for row in self._item_tree.get_children():
                self._item_tree.delete(row)
            self._filter_status_var.set("No scan results.")
            self._asset_empty_var.set("Game directory changed. Scan again.")
            self._scan_status_var.set("Game directory changed. Scan again.")
            self._export_all_btn.config(text="Queue Visible (0)")
        self._update_action_states()

    def _on_filter_changed(self) -> None:
        self._apply_filter()
        self._update_action_states()

    def _set_item_headings(self) -> None:
        for col, title in (("type", "Type"), ("name", "Name")):
            marker = self._sort_marker(col, self._item_sort_column, self._item_sort_reverse)
            self._item_tree.heading(col, text=f"{title}{marker}", command=lambda c=col: self._sort_items(c))

    def _set_queue_headings(self) -> None:
        for col, title in (("type", "Type"), ("name", "Name"), ("status", "Status"), ("info", "Info")):
            marker = self._sort_marker(col, self._queue_sort_column, self._queue_sort_reverse)
            self._queue_tree.heading(col, text=f"{title}{marker}", command=lambda c=col: self._sort_queue(c))

    def _sort_marker(self, col: str, active_col: str, reverse: bool) -> str:
        if col != active_col:
            return ""
        return " v" if reverse else " ^"

    def _sort_items(self, column: str) -> None:
        if self._item_sort_column == column:
            self._item_sort_reverse = not self._item_sort_reverse
        else:
            self._item_sort_column = column
            self._item_sort_reverse = False
        self._set_item_headings()
        self._apply_filter()

    def _sort_queue(self, column: str) -> None:
        if self._queue_sort_column == column:
            self._queue_sort_reverse = not self._queue_sort_reverse
        else:
            self._queue_sort_column = column
            self._queue_sort_reverse = False
        self._set_queue_headings()
        self._refresh_queue_once()

    def _apply_filter(self) -> None:
        search = self._search_var.get().strip().casefold()
        active_tab = self._tab_var.get()
        visible = [
            item
            for item in self._all_items
            if (active_tab == "all" or item["type"] == active_tab)
            and (not search or search in item["name"].casefold())
        ]

        for row in self._item_tree.get_children():
            self._item_tree.delete(row)

        deduped: list[dict[str, str]] = []
        seen: set[str] = set()
        for item in self._sorted_items(visible):
            iid = f"{item['type']}:{item['name']}"
            if iid in seen:
                continue
            seen.add(iid)
            deduped.append(item)
            self._item_tree.insert("", tk.END, iid=iid, values=(item["type"], item["name"]))

        self._visible_items = deduped
        self._filter_status_var.set(f"Showing {len(deduped)} of {len(self._all_items)} item(s).")
        self._export_all_btn.config(text=f"Queue Visible ({len(deduped)})")
        if not self._all_items:
            self._asset_empty_var.set("No scanned items. Pick a game directory and scan.")
        elif not deduped:
            self._asset_empty_var.set("No assets match the current filter.")
        else:
            self._asset_empty_var.set("")

    def _sorted_items(self, items: list[dict[str, str]]) -> list[dict[str, str]]:
        if self._item_sort_column == "type":
            key = lambda item: (item["type"].casefold(), item["name"].casefold())
        else:
            key = lambda item: (item["name"].casefold(), item["type"].casefold())
        return sorted(items, key=key, reverse=self._item_sort_reverse)

    def _selected_items(self) -> list[tuple[str, str]]:
        selected: list[tuple[str, str]] = []
        for iid in self._item_tree.selection():
            values = self._item_tree.item(iid, "values")
            if len(values) >= 2:
                selected.append((str(values[0]), str(values[1])))
        return selected

    # ------------------------------------------------------------------
    # Queueing
    # ------------------------------------------------------------------

    def _build_options(self) -> ImportOptions:
        return ImportOptions(
            import_animations=self._flag_animations.get(),
            import_collisions=self._flag_collisions.get(),
            import_occlusion=self._flag_occlusion.get(),
            import_lights=self._flag_lights.get(),
            import_arms=self._flag_arms.get(),
            write_blend=self._flag_write_blend.get(),
            write_3dp=self._flag_write_3dp.get(),
            write_ase=self._flag_write_ase.get(),
            write_glb=self._flag_write_glb.get(),
            write_fbx=self._flag_write_fbx.get(),
        )

    def _apply_selected_preset(self) -> None:
        preset_name = self._preset_var.get()
        preset = OPTION_PRESETS.get(preset_name)
        if preset is None:
            self._sync_preset_label()
            return
        self._apply_options(preset)
        self._preset_var.set(preset_name)
        self._log_append(f"Applied preset: {preset_name}")

    def _apply_options(self, options: ImportOptions) -> None:
        self._applying_preset = True
        try:
            self._flag_animations.set(options.import_animations)
            self._flag_collisions.set(options.import_collisions)
            self._flag_occlusion.set(options.import_occlusion)
            self._flag_lights.set(options.import_lights)
            self._flag_arms.set(options.import_arms)
            self._flag_write_blend.set(options.write_blend)
            self._flag_write_3dp.set(options.write_3dp)
            self._flag_write_ase.set(options.write_ase)
            self._flag_write_glb.set(options.write_glb)
            self._flag_write_fbx.set(options.write_fbx)
        finally:
            self._applying_preset = False
        self._sync_preset_label(options)
        self._update_action_states()

    def _on_options_changed(self) -> None:
        if self._applying_preset:
            return
        self._sync_preset_label()
        self._update_action_states()

    def _sync_preset_label(self, options: ImportOptions | None = None) -> None:
        self._preset_var.set(preset_name_for_options(options or self._build_options()))

    def _import_selected(self) -> None:
        selected = self._selected_items()
        if not selected:
            self._show_errors(["Select at least one item to queue."])
            return
        requests = [
            ImportRequest.for_definition(
                base_dir=self._dir_var.get().strip(),
                item_name=item_name,
                item_type=item_type,
                output_root=self._out_var.get().strip(),
                options=self._build_options(),
            )
            for item_type, item_name in selected
        ]
        self._enqueue_requests(requests)

    def _export_all_visible(self) -> None:
        if not self._visible_items:
            self._show_errors(["There are no visible items to queue."])
            return
        requests = [
            ImportRequest.for_definition(
                base_dir=self._dir_var.get().strip(),
                item_name=item["name"],
                item_type=item["type"],
                output_root=self._out_var.get().strip(),
                options=self._build_options(),
            )
            for item in self._visible_items
        ]
        self._enqueue_requests(requests)

    def _import_loose(self) -> None:
        paths = filedialog.askopenfilenames(
            title="Select .3di file(s) to queue",
            filetypes=(("3DI models", "*.3di"), ("All files", "*.*")),
        )
        if not paths:
            return
        requests = [
            ImportRequest.for_loose(
                threedi_path=path,
                output_root=self._out_var.get().strip(),
                base_dir=self._dir_var.get().strip(),
                options=self._build_options(),
            )
            for path in paths
        ]
        self._enqueue_requests(requests)

    def _enqueue_requests(self, requests: list[ImportRequest]) -> None:
        errors: list[str] = []
        valid_requests: list[ImportRequest] = []
        for request in requests:
            request_errors = validate_import_request(request)
            if request_errors:
                errors.append(f"{request.label}: {' '.join(request_errors)}")
            else:
                valid_requests.append(request)

        if errors:
            self._show_errors(errors)
        if not valid_requests:
            return
        if not self._confirm_output_collisions(valid_requests):
            self._log_append("Queue canceled because output folders already exist.")
            return

        queued = 0
        skipped = 0
        with self._jobs_lock:
            active_keys = {
                job.request.dedupe_key()
                for job in self._jobs
                if job.status in ACTIVE_JOB_STATUSES
            }
            for request in valid_requests:
                key = request.dedupe_key()
                if key in active_keys:
                    skipped += 1
                    continue
                self._jobs.append(ImportJob(request=request))
                active_keys.add(key)
                queued += 1

        if queued:
            self._remember_recent_path("output", self._out_var.get().strip())
            if self._dir_var.get().strip():
                self._remember_recent_path("game", self._dir_var.get().strip())
            self._log_append(f"Queued {queued} job(s).")
            self._ensure_worker()
        if skipped:
            self._log_append(f"Skipped {skipped} duplicate pending/running job(s).")
        self._refresh_queue_once()
        self._update_action_states()

    def _confirm_output_collisions(self, requests: list[ImportRequest]) -> bool:
        collisions = [
            request.likely_output_dir
            for request in requests
            if Path(request.likely_output_dir).exists()
        ]
        if not collisions:
            return True
        shown = "\n".join(collisions[:8])
        if len(collisions) > 8:
            shown += f"\n...and {len(collisions) - 8} more."
        message = (
            "Some output folders already exist. Existing files may be overwritten by the importer.\n\n"
            f"{shown}\n\nQueue these jobs anyway?"
        )
        try:
            return bool(messagebox.askyesno("Existing Output", message))
        except tk.TclError:
            return True

    def _ensure_worker(self) -> None:
        if self._worker is None or not self._worker.is_alive():
            self._worker = threading.Thread(target=self._worker_loop, daemon=True)
            self._worker.start()

    def _worker_loop(self) -> None:
        from apps.importer.import_runner import execute_import_request

        try:
            while True:
                job = self._take_next_pending()
                if job is None:
                    break
                self._post_ui(0, self._log_append, f"Running: {job.label}", "info", job.id)
                result = execute_import_request(job.request)
                with self._jobs_lock:
                    job.finish(result)
                if result.ok:
                    self._post_ui(0, self._log_append, f"Done: {job.label}", "info", job.id)
                else:
                    self._post_ui(0, self._log_append, f"FAILED: {job.label}: {result.error}", "error", job.id)
        except Exception as exc:
            log.error("Worker thread crashed: %s", exc, exc_info=True)
            self._post_ui(0, self._log_append, f"Worker thread crashed: {exc}", "error", "")
        finally:
            self._post_ui(0, self._update_action_states)

    def _take_next_pending(self) -> ImportJob | None:
        with self._jobs_lock:
            for job in self._jobs:
                if job.status == JOB_PENDING:
                    job.mark_running()
                    return job
        return None

    # ------------------------------------------------------------------
    # Queue controls
    # ------------------------------------------------------------------

    def _refresh_queue(self) -> None:
        if self._closing:
            return
        self._refresh_queue_once()
        self._post_ui(500, self._refresh_queue)

    def _refresh_queue_once(self) -> None:
        with self._jobs_lock:
            jobs = list(self._jobs)

        display_jobs = self._sorted_jobs(jobs)
        existing = set(self._queue_tree.get_children())
        wanted = {job.id for job in jobs}
        for iid in existing - wanted:
            self._queue_tree.delete(iid)

        for index, job in enumerate(display_jobs):
            info = self._job_info(job)
            values = (job.request.item_type, job.request.display_name, job.status, info)
            if job.id in existing:
                self._queue_tree.item(job.id, values=values, tags=(job.status,))
            else:
                self._queue_tree.insert("", tk.END, iid=job.id, values=values, tags=(job.status,))
            self._queue_tree.move(job.id, "", index)

        counts = self._job_counts(jobs)
        self._queue_summary_var.set(
            "Queue: {pending} pending, {running} running, {done} done, {error} failed".format(
                **counts
            )
        )
        total = len(jobs)
        completed = counts[JOB_DONE] + counts[JOB_ERROR]
        self._progress.configure(maximum=max(total, 1), value=completed)
        self._queue_empty_var.set("Queue is empty." if not jobs else "")
        self._update_job_details()

    def _sorted_jobs(self, jobs: list[ImportJob]) -> list[ImportJob]:
        if not self._queue_sort_column:
            return jobs
        if self._queue_sort_column == "type":
            key = lambda job: (job.request.item_type.casefold(), job.request.display_name.casefold())
        elif self._queue_sort_column == "name":
            key = lambda job: (job.request.display_name.casefold(), job.request.item_type.casefold())
        elif self._queue_sort_column == "status":
            key = lambda job: (job.status.casefold(), job.request.display_name.casefold())
        else:
            key = lambda job: (self._job_info(job).casefold(), job.request.display_name.casefold())
        return sorted(jobs, key=key, reverse=self._queue_sort_reverse)

    def _job_info(self, job: ImportJob) -> str:
        if job.result:
            return job.result.output_path if job.result.ok else job.result.error[:80]
        if job.status == JOB_RUNNING:
            return "Running"
        return job.request.likely_output_dir

    def _job_counts(self, jobs: list[ImportJob]) -> dict[str, int]:
        return {
            JOB_PENDING: sum(1 for job in jobs if job.status == JOB_PENDING),
            JOB_RUNNING: sum(1 for job in jobs if job.status == JOB_RUNNING),
            JOB_DONE: sum(1 for job in jobs if job.status == JOB_DONE),
            JOB_ERROR: sum(1 for job in jobs if job.status == JOB_ERROR),
        }

    def _selected_job_ids(self) -> set[str]:
        return set(self._queue_tree.selection())

    def _selected_jobs(self) -> list[ImportJob]:
        selected = self._selected_job_ids()
        with self._jobs_lock:
            jobs = [job for job in self._jobs if job.id in selected]
        return jobs

    def _primary_selected_job(self) -> ImportJob | None:
        selected = self._selected_jobs()
        return selected[0] if selected else None

    def _on_queue_selection_changed(self) -> None:
        self._update_action_states()
        self._update_job_details()
        if self._log_filter_var.get() == LOG_FILTER_CURRENT:
            self._render_log()

    def _show_queue_context_menu(self, event: tk.Event) -> None:
        row = self._queue_tree.identify_row(event.y)
        if row and row not in self._queue_tree.selection():
            self._queue_tree.selection_set(row)
            self._on_queue_selection_changed()
        selected = self._selected_jobs()
        can_remove = any(job.status == JOB_PENDING for job in selected)
        can_retry = any(job.status == JOB_ERROR for job in selected)
        has_output = any(self._job_output_path(job) for job in selected)
        has_error = any(self._job_error(job) for job in selected)
        self._queue_menu.entryconfig("Retry Failed", state=tk.NORMAL if can_retry else tk.DISABLED)
        self._queue_menu.entryconfig("Remove Pending", state=tk.NORMAL if can_remove else tk.DISABLED)
        self._queue_menu.entryconfig("Copy Output Path", state=tk.NORMAL if has_output else tk.DISABLED)
        self._queue_menu.entryconfig("Copy Error", state=tk.NORMAL if has_error else tk.DISABLED)
        self._queue_menu.entryconfig("Open Output Folder", state=tk.NORMAL if has_output else tk.DISABLED)
        try:
            self._queue_menu.tk_popup(event.x_root, event.y_root)
        finally:
            self._queue_menu.grab_release()

    def _remove_selected_pending(self) -> None:
        selected = self._selected_job_ids()
        if not selected:
            return
        with self._jobs_lock:
            before = len(self._jobs)
            self._jobs = [
                job
                for job in self._jobs
                if not (job.id in selected and job.status == JOB_PENDING)
            ]
            removed = before - len(self._jobs)
        if removed:
            self._log_append(f"Removed {removed} pending job(s).")
        self._refresh_queue_once()
        self._update_action_states()

    def _retry_failed(self) -> None:
        selected = self._selected_job_ids()
        if not selected:
            return
        retried = 0
        skipped = 0
        with self._jobs_lock:
            active_keys = {
                job.request.dedupe_key()
                for job in self._jobs
                if job.status in ACTIVE_JOB_STATUSES
            }
            for job in self._jobs:
                if job.id not in selected or job.status != JOB_ERROR:
                    continue
                key = job.request.dedupe_key()
                if key in active_keys:
                    skipped += 1
                    continue
                job.retry()
                active_keys.add(key)
                retried += 1
        if retried:
            self._log_append(f"Retried {retried} failed job(s).")
            self._ensure_worker()
        if skipped:
            self._log_append(f"Skipped {skipped} retry duplicate(s).")
        self._refresh_queue_once()
        self._update_action_states()

    def _clear_finished(self) -> None:
        with self._jobs_lock:
            before = len(self._jobs)
            self._jobs = [
                job
                for job in self._jobs
                if job.status not in (JOB_DONE, JOB_ERROR)
            ]
            cleared = before - len(self._jobs)
        if cleared:
            self._log_append(f"Cleared {cleared} finished job(s).")
        self._refresh_queue_once()
        self._update_action_states()

    def _copy_selected_output_path(self) -> None:
        job = self._primary_selected_job()
        if job is None:
            return
        path = self._job_output_path(job)
        if not path:
            return
        self.clipboard_clear()
        self.clipboard_append(path)
        self._log_append(f"Copied output path: {path}", job_id=job.id)

    def _copy_selected_error(self) -> None:
        job = self._primary_selected_job()
        if job is None:
            return
        error = self._job_error(job)
        if not error:
            return
        self.clipboard_clear()
        self.clipboard_append(error)
        self._log_append(f"Copied error for {job.label}", job_id=job.id)

    def _open_selected_output_folder(self) -> None:
        job = self._primary_selected_job()
        if job is None:
            return
        raw_path = self._job_output_path(job)
        if not raw_path:
            return
        path = Path(raw_path)
        if not path.exists():
            path = path.parent
        if not path.exists():
            self._show_errors([f"Output folder does not exist: {raw_path}"])
            return
        try:
            if hasattr(os, "startfile"):
                os.startfile(str(path))  # type: ignore[attr-defined]
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(path)])
            else:
                subprocess.Popen(["xdg-open", str(path)])
        except OSError as exc:
            self._show_errors([f"Could not open output folder: {exc}"])

    def _job_output_path(self, job: ImportJob) -> str:
        if job.result and job.result.output_path:
            return job.result.output_path
        return job.request.likely_output_dir

    def _job_error(self, job: ImportJob) -> str:
        if job.result and job.result.error:
            return job.result.error
        return job.error

    def _update_job_details(self) -> None:
        selected = self._selected_jobs()
        if not selected:
            text = "Select a queued job to see its request, options, output path, and error details."
        elif len(selected) > 1:
            counts = self._job_counts(selected)
            text = (
                f"{len(selected)} jobs selected.\n"
                "Pending: {pending}, Running: {running}, Done: {done}, Failed: {error}".format(**counts)
            )
        else:
            text = self._format_job_details(selected[0])
        self._job_detail.config(state=tk.NORMAL)
        self._job_detail.delete("1.0", tk.END)
        self._job_detail.insert(tk.END, text)
        self._job_detail.config(state=tk.DISABLED)

    def _format_job_details(self, job: ImportJob) -> str:
        request = job.request
        lines = [
            f"Name: {request.display_name}",
            f"Type: {request.item_type}",
            f"Mode: {request.mode}",
            f"Status: {job.status}",
            f"Game/asset dir: {request.base_dir or '(none)'}",
            f"Source file: {request.threedi_path or '(definition lookup)'}",
            f"Output: {self._job_output_path(job)}",
            f"Options: {self._format_options(request.options)}",
        ]
        error = self._job_error(job)
        if error:
            lines.append("")
            lines.append("Error:")
            lines.append(error)
        return "\n".join(lines)

    def _format_options(self, options: ImportOptions) -> str:
        imports = [
            label
            for label, value in (
                ("animations", options.import_animations),
                ("collisions", options.import_collisions),
                ("occlusion", options.import_occlusion),
                ("lights", options.import_lights),
                ("arms", options.import_arms),
            )
            if value
        ]
        outputs = [
            label
            for label, value in (
                ("blend", options.write_blend),
                ("3dp", options.write_3dp),
                ("ase", options.write_ase),
                ("glb", options.write_glb),
                ("fbx", options.write_fbx),
            )
            if value
        ]
        import_text = ", ".join(imports) if imports else "geometry only"
        output_text = ", ".join(outputs) if outputs else "none"
        return f"import: {import_text}; files: {output_text}"

    # ------------------------------------------------------------------
    # State and log helpers
    # ------------------------------------------------------------------

    def _update_action_states(self) -> None:
        selected_items = self._selected_items()
        has_game_dir = bool(self._dir_var.get().strip())
        has_output = bool(self._out_var.get().strip())
        has_output_file = self._build_options().writes_any_output_file()
        has_visible_items = bool(self._visible_items)

        self._scan_btn.config(state=tk.DISABLED if self._scanning or not has_game_dir else tk.NORMAL)
        selected_count = len(selected_items)
        self._import_selected_btn.config(
            text=f"Queue Selected ({selected_count})" if selected_count else "Queue Selected",
            state=tk.NORMAL if has_game_dir and has_output and has_output_file and selected_count else tk.DISABLED,
        )
        self._export_all_btn.config(
            state=tk.NORMAL if has_game_dir and has_output and has_output_file and has_visible_items else tk.DISABLED
        )
        self._loose_btn.config(state=tk.NORMAL if has_output and has_output_file else tk.DISABLED)

        selected_jobs = self._selected_jobs()
        can_remove = any(job.status == JOB_PENDING for job in selected_jobs)
        can_retry = any(job.status == JOB_ERROR for job in selected_jobs)
        with self._jobs_lock:
            can_clear = any(job.status in (JOB_DONE, JOB_ERROR) for job in self._jobs)
        self._remove_pending_btn.config(state=tk.NORMAL if can_remove else tk.DISABLED)
        self._retry_failed_btn.config(state=tk.NORMAL if can_retry else tk.DISABLED)
        self._clear_finished_btn.config(state=tk.NORMAL if can_clear else tk.DISABLED)

    def _show_errors(self, errors: list[str]) -> None:
        clean = [error for error in errors if error]
        if not clean:
            return
        message = "\n".join(clean[:8])
        if len(clean) > 8:
            message += f"\n...and {len(clean) - 8} more."
        self._log_append("ERROR: " + " | ".join(clean[:8]), level="error")
        try:
            messagebox.showerror("OpenNova Importer", message)
        except tk.TclError:
            pass

    def _log_append(self, msg: str, level: str = "info", job_id: str = "") -> None:
        entry = {
            "time": time.strftime("%H:%M:%S"),
            "message": msg,
            "level": level,
            "job_id": job_id,
        }
        self._log_entries.append(entry)
        if len(self._log_entries) > 2000:
            self._log_entries = self._log_entries[-2000:]
        self._render_log()

    def _render_log(self) -> None:
        if not hasattr(self, "_log"):
            return
        selected_job_ids = self._selected_job_ids()
        filter_value = self._log_filter_var.get()
        lines = []
        for entry in self._log_entries:
            if filter_value == LOG_FILTER_ERRORS and entry["level"] != "error":
                continue
            if filter_value == LOG_FILTER_CURRENT and entry["job_id"] not in selected_job_ids:
                continue
            lines.append(f"[{entry['time']}] {entry['message']}")
        self._log.config(state=tk.NORMAL)
        self._log.delete("1.0", tk.END)
        if lines:
            self._log.insert(tk.END, "\n".join(lines) + "\n")
        self._log.config(state=tk.DISABLED)
        self._log.see(tk.END)

    def _clear_log(self) -> None:
        self._log_entries.clear()
        self._render_log()

    def _current_log_path(self) -> str:
        for handler in logging.getLogger().handlers:
            if isinstance(handler, logging.FileHandler):
                return handler.baseFilename
        return ""


def run_gui() -> None:
    app = ImporterApp()
    app.mainloop()
