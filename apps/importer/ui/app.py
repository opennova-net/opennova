"""Standalone importer GUI — tkinter."""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Optional
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext


@dataclass
class ImportJob:
    base_dir:   str
    item_name:  str
    item_type:  str
    output_dir: str
    flags:      dict
    status:     str = "pending"  # pending | running | done | error
    error:      str = ""


class ImporterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("OpenNova Importer")
        self.geometry("1100x720")
        self.minsize(800, 500)

        self._jobs: list[ImportJob] = []
        self._jobs_lock = threading.Lock()
        self._worker: Optional[threading.Thread] = None
        self._scanning = False

        self._all_items: list[dict] = []
        self._active_tab: str = "all"

        self._build_ui()
        self._refresh_queue()

    # ------------------------------------------------------------------
    # Layout
    # ------------------------------------------------------------------

    def _build_ui(self):
        # Game directory row
        dir_frame = ttk.Frame(self, padding=(8, 6))
        dir_frame.pack(fill=tk.X)
        ttk.Label(dir_frame, text="Game directory:").pack(side=tk.LEFT)
        self._dir_var = tk.StringVar()
        ttk.Entry(dir_frame, textvariable=self._dir_var).pack(
            side=tk.LEFT, padx=6, fill=tk.X, expand=True)
        ttk.Button(dir_frame, text="Browse…", command=self._browse_dir).pack(side=tk.LEFT)
        self._scan_btn = ttk.Button(dir_frame, text="Scan", command=self._scan)
        self._scan_btn.pack(side=tk.LEFT, padx=(6, 0))
        self._scan_status = ttk.Label(dir_frame, text="")
        self._scan_status.pack(side=tk.LEFT, padx=6)

        # Main split: left = item list, right = options + queue
        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

        # --- Left: type tabs, search bar, item list, action buttons ---
        left = ttk.Frame(paned)
        paned.add(left, weight=3)

        # Type tabs
        tab_frame = ttk.Frame(left)
        tab_frame.pack(fill=tk.X, pady=(0, 2))
        self._tab_var = tk.StringVar(value="all")
        for label, value in [("All", "all"), ("Weapons", "weapon"), ("Items", "item")]:
            ttk.Radiobutton(
                tab_frame, text=label, variable=self._tab_var, value=value,
                command=self._on_tab_change,
            ).pack(side=tk.LEFT, padx=2)

        # Search bar
        search_frame = ttk.Frame(left)
        search_frame.pack(fill=tk.X, pady=(0, 2))
        ttk.Label(search_frame, text="Filter:").pack(side=tk.LEFT)
        self._search_var = tk.StringVar()
        self._search_var.trace_add("write", lambda *_: self._apply_filter())
        ttk.Entry(search_frame, textvariable=self._search_var).pack(
            side=tk.LEFT, padx=4, fill=tk.X, expand=True)
        ttk.Button(search_frame, text="×", width=2,
                   command=lambda: self._search_var.set("")).pack(side=tk.LEFT)

        # Item treeview
        tree_frame = ttk.Frame(left)
        tree_frame.pack(fill=tk.BOTH, expand=True)
        self._item_tree = ttk.Treeview(
            tree_frame, columns=("type", "name"), show="headings", selectmode="browse")
        self._item_tree.heading("type", text="Type")
        self._item_tree.heading("name", text="Name")
        self._item_tree.column("type", width=70, stretch=False)
        self._item_tree.column("name", stretch=True)
        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self._item_tree.yview)
        self._item_tree.configure(yscrollcommand=vsb.set)
        self._item_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.LEFT, fill=tk.Y)

        # Action buttons below item list
        btn_frame = ttk.Frame(left)
        btn_frame.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(btn_frame, text="Import Selected",
                   command=self._import_selected).pack(side=tk.LEFT, fill=tk.X, expand=True)
        self._export_all_btn = ttk.Button(
            btn_frame, text="Export All Visible (0)",
            command=self._export_all_visible)
        self._export_all_btn.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))

        # --- Right: options + queue ---
        right = ttk.Frame(paned)
        paned.add(right, weight=2)

        # Output directory
        out_lf = ttk.LabelFrame(right, text="Output directory", padding=6)
        out_lf.pack(fill=tk.X, pady=(0, 6))
        self._out_var = tk.StringVar()
        ttk.Entry(out_lf, textvariable=self._out_var).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        ttk.Button(out_lf, text="Browse…", command=self._browse_output).pack(side=tk.LEFT)

        # Import options
        flags_lf = ttk.LabelFrame(right, text="Import options", padding=6)
        flags_lf.pack(fill=tk.X, pady=(0, 6))
        self._flag_animations = tk.BooleanVar(value=True)
        self._flag_collisions  = tk.BooleanVar(value=True)
        self._flag_occlusion   = tk.BooleanVar(value=True)
        self._flag_lights      = tk.BooleanVar(value=True)
        self._flag_arms        = tk.BooleanVar(value=True)
        row1 = ttk.Frame(flags_lf)
        row1.pack(fill=tk.X)
        row2 = ttk.Frame(flags_lf)
        row2.pack(fill=tk.X)
        ttk.Checkbutton(row1, text="Animations", variable=self._flag_animations).pack(
            side=tk.LEFT, padx=(0, 8))
        ttk.Checkbutton(row1, text="Collisions", variable=self._flag_collisions).pack(
            side=tk.LEFT)
        ttk.Checkbutton(row2, text="Occlusion", variable=self._flag_occlusion).pack(
            side=tk.LEFT, padx=(0, 8))
        ttk.Checkbutton(row2, text="Lights", variable=self._flag_lights).pack(
            side=tk.LEFT)
        ttk.Checkbutton(flags_lf, text="Arms model", variable=self._flag_arms).pack(
            anchor=tk.W)

        # Output formats
        fmt_lf = ttk.LabelFrame(right, text="Output formats", padding=6)
        fmt_lf.pack(fill=tk.X, pady=(0, 6))
        self._flag_write_3dp = tk.BooleanVar(value=True)
        self._flag_write_ase = tk.BooleanVar(value=True)
        ttk.Checkbutton(fmt_lf, text="Project files (.3dp / .3da)",
                        variable=self._flag_write_3dp).pack(anchor=tk.W)
        ttk.Checkbutton(fmt_lf, text="ASE (.ase)",
                        variable=self._flag_write_ase).pack(anchor=tk.W)

        # Queue
        queue_lf = ttk.LabelFrame(right, text="Queue", padding=6)
        queue_lf.pack(fill=tk.BOTH, expand=True)
        self._queue_tree = ttk.Treeview(
            queue_lf,
            columns=("type", "name", "status", "info"),
            show="headings",
            height=8,
        )
        for col, w, stretch in [
            ("type", 60, False), ("name", 120, True),
            ("status", 70, False), ("info", 140, True),
        ]:
            self._queue_tree.heading(col, text=col.title())
            self._queue_tree.column(col, width=w, stretch=stretch)
        self._queue_tree.pack(fill=tk.BOTH, expand=True)
        queue_btn_frame = ttk.Frame(queue_lf)
        queue_btn_frame.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(queue_btn_frame, text="Clear Completed",
                   command=self._clear_completed).pack(side=tk.LEFT)
        ttk.Button(queue_btn_frame, text="Remove Selected",
                   command=self._remove_selected_pending).pack(side=tk.LEFT, padx=(4, 0))

        # Log
        log_lf = ttk.LabelFrame(self, text="Log", padding=4)
        log_lf.pack(fill=tk.BOTH, expand=False, padx=8, pady=(0, 8))
        btn_row = ttk.Frame(log_lf)
        btn_row.pack(fill=tk.X)
        ttk.Button(btn_row, text="Clear", command=self._clear_log).pack(side=tk.RIGHT)
        self._log = scrolledtext.ScrolledText(
            log_lf, height=8, state=tk.DISABLED, wrap=tk.WORD)
        self._log.pack(fill=tk.BOTH, expand=True)

    # ------------------------------------------------------------------
    # Browse (native OS dialogs — always work on Windows)
    # ------------------------------------------------------------------

    def _browse_dir(self):
        path = filedialog.askdirectory(
            title="Select Game Directory",
            initialdir=self._dir_var.get() or None)
        if path:
            self._dir_var.set(path)

    def _browse_output(self):
        path = filedialog.askdirectory(
            title="Select Output Directory",
            initialdir=self._out_var.get() or None)
        if path:
            self._out_var.set(path)

    # ------------------------------------------------------------------
    # Scan
    # ------------------------------------------------------------------

    def _scan(self):
        d = self._dir_var.get().strip()
        if not d or self._scanning:
            return
        self._scanning = True
        self._scan_btn.config(state=tk.DISABLED)
        self._scan_status.config(text="Scanning…")
        threading.Thread(target=self._do_scan, args=(d,), daemon=True).start()

    def _do_scan(self, d: str):
        try:
            from apps.importer.import_runner import scan_directory
            items = scan_directory(d)
            self.after(0, self._populate_items, items)
        except Exception as exc:
            self.after(0, self._log_append, f"Scan ERROR: {exc}")
        finally:
            self.after(0, self._scan_done)

    def _scan_done(self):
        self._scanning = False
        self._scan_btn.config(state=tk.NORMAL)
        self._scan_status.config(text="")

    def _populate_items(self, items: list[dict]):
        self._all_items = items
        self._apply_filter()
        self._log_append(f"Scan complete: {len(items)} item(s) found.")

    def _on_tab_change(self):
        self._active_tab = self._tab_var.get()
        self._apply_filter()

    def _apply_filter(self):
        search = self._search_var.get().lower()
        visible = [
            i for i in self._all_items
            if (self._active_tab == "all" or i["type"] == self._active_tab)
            and (not search or search in i["name"].lower())
        ]
        for row in self._item_tree.get_children():
            self._item_tree.delete(row)
        seen: set[str] = set()
        for item in sorted(visible, key=lambda x: (x["type"], x["name"])):
            iid = f"{item['type']}:{item['name']}"
            if iid in seen:
                continue
            seen.add(iid)
            self._item_tree.insert("", tk.END, iid=iid,
                                   values=(item["type"], item["name"]))
        self._export_all_btn.config(text=f"Export All Visible ({len(visible)})")

    # ------------------------------------------------------------------
    # Import queue
    # ------------------------------------------------------------------

    def _build_flags(self) -> dict:
        return {
            "import_animations": self._flag_animations.get(),
            "import_collisions":  self._flag_collisions.get(),
            "import_occlusion":   self._flag_occlusion.get(),
            "import_lights":      self._flag_lights.get(),
            "import_arms":        self._flag_arms.get(),
            "write_3dp":          self._flag_write_3dp.get(),
            "write_ase":          self._flag_write_ase.get(),
        }

    def _import_selected(self):
        sel = self._item_tree.selection()
        if not sel:
            return
        item_type, item_name = sel[0].split(":", 1)
        out_dir = self._out_var.get().strip()
        if not out_dir:
            self._log_append("ERROR: No output directory set.")
            return
        job = ImportJob(
            base_dir=self._dir_var.get().strip(),
            item_name=item_name,
            item_type=item_type,
            output_dir=out_dir,
            flags=self._build_flags(),
        )
        with self._jobs_lock:
            self._jobs.append(job)
        self._log_append(f"Queued: {item_type} {item_name} → {out_dir}")
        self._ensure_worker()

    def _export_all_visible(self):
        out_dir = self._out_var.get().strip()
        if not out_dir:
            self._log_append("ERROR: No output directory set.")
            return
        iids = self._item_tree.get_children()
        if not iids:
            return
        flags = self._build_flags()
        base_dir = self._dir_var.get().strip()
        queued = 0
        with self._jobs_lock:
            for iid in iids:
                item_type, item_name = iid.split(":", 1)
                self._jobs.append(ImportJob(
                    base_dir=base_dir,
                    item_name=item_name,
                    item_type=item_type,
                    output_dir=out_dir,
                    flags=flags,
                ))
                queued += 1
        self._log_append(f"Queued {queued} items for export")
        self._ensure_worker()

    def _ensure_worker(self):
        if self._worker is None or not self._worker.is_alive():
            self._worker = threading.Thread(target=self._worker_loop, daemon=True)
            self._worker.start()

    def _worker_loop(self):
        import logging
        _wlog = logging.getLogger(__name__)

        def _ckpt(msg):
            _wlog.debug("[CKPT] %s", msg)
            for h in logging.root.handlers:
                h.flush()

        try:
            from apps.importer import bpy_session
            from apps.importer.import_runner import run_import
            _ckpt("init_headless start")
            try:
                bpy_session.init_headless()
            except Exception:
                pass
            _ckpt("init_headless done")
            while True:
                job = self._next_pending()
                if job is None:
                    break
                with self._jobs_lock:
                    job.status = "running"
                try:
                    _ckpt(f"new_scene start ({job.item_name})")
                    bpy_session.new_scene()
                    _ckpt(f"new_scene done ({job.item_name})")
                    ok = run_import(
                        base_dir=job.base_dir,
                        item_name=job.item_name,
                        item_type=job.item_type,
                        output_dir=job.output_dir,
                        **job.flags,
                    )
                    _ckpt(f"run_import done ({job.item_name}) ok={ok}")
                    with self._jobs_lock:
                        job.status = "done" if ok else "error"
                        if not ok:
                            job.error = "import returned False"
                except Exception as exc:
                    with self._jobs_lock:
                        job.status = "error"
                        job.error = str(exc)
                label = "Done" if job.status == "done" else "FAILED"
                self.after(0, self._log_append, f"{label}: {job.item_name}")
        except Exception as exc:
            _wlog.error("Worker thread crashed: %s", exc, exc_info=True)
            self.after(0, self._log_append, f"Worker thread crashed: {exc}")

    def _next_pending(self) -> Optional[ImportJob]:
        with self._jobs_lock:
            for j in self._jobs:
                if j.status == "pending":
                    return j
        return None

    def _refresh_queue(self):
        with self._jobs_lock:
            jobs = list(self._jobs)
        existing = set(self._queue_tree.get_children())
        for i, job in enumerate(jobs):
            iid = f"job_{i}"
            if job.error:
                info = job.error[:50] + ("…" if len(job.error) > 50 else "")
            else:
                info = job.output_dir
            vals = (job.item_type, job.item_name, job.status, info)
            if iid in existing:
                self._queue_tree.item(iid, values=vals, tags=(job.status,))
            else:
                self._queue_tree.insert("", tk.END, iid=iid, values=vals,
                                        tags=(job.status,))
        self._queue_tree.tag_configure("pending", foreground="gray")
        self._queue_tree.tag_configure("running", foreground="orange")
        self._queue_tree.tag_configure("done",    foreground="green")
        self._queue_tree.tag_configure("error",   foreground="red")
        self.after(500, self._refresh_queue)

    def _clear_completed(self):
        with self._jobs_lock:
            self._jobs = [j for j in self._jobs if j.status in ("pending", "running")]

    def _remove_selected_pending(self):
        sel = self._queue_tree.selection()
        if not sel:
            return
        indices_to_remove = set()
        for iid in sel:
            if iid.startswith("job_"):
                indices_to_remove.add(int(iid[4:]))
        with self._jobs_lock:
            self._jobs = [
                job for i, job in enumerate(self._jobs)
                if not (i in indices_to_remove and job.status == "pending")
            ]

    # ------------------------------------------------------------------
    # Log
    # ------------------------------------------------------------------

    def _log_append(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        self._log.config(state=tk.NORMAL)
        self._log.insert(tk.END, f"[{ts}] {msg}\n")
        self._log.config(state=tk.DISABLED)
        self._log.see(tk.END)

    def _clear_log(self):
        self._log.config(state=tk.NORMAL)
        self._log.delete("1.0", tk.END)
        self._log.config(state=tk.DISABLED)


def run_gui() -> None:
    app = ImporterApp()
    app.mainloop()
