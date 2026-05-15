"""Process pool dispatcher for ``ImportRequest`` -> ``ImportResult``.

Each worker handles one request and exits, guaranteeing that ``bpy`` state
cannot accumulate across imports. The cap on concurrent workers defaults to
``min(4, cpu_count())`` so a parallel batch doesn't run the machine out of
memory; each Blender process is roughly 500MB-1GB. Override via the
``ONIMPORT_MAX_WORKERS`` env var or the ``max_workers`` constructor arg.

Worker logs are forwarded to the parent's logging configuration through a
``multiprocessing.Queue`` + ``QueueListener`` pair.
"""
from __future__ import annotations

import concurrent.futures
import logging
import logging.handlers
import multiprocessing as mp
import os
from concurrent.futures.process import BrokenProcessPool
from typing import Iterable, Iterator

from opennova_jobs import ImportRequest, ImportResult
from .worker import init_worker, run_one


_log = logging.getLogger(__name__)


def _default_max_workers() -> int:
    raw = os.environ.get("ONIMPORT_MAX_WORKERS")
    if raw:
        try:
            n = int(raw)
            if n > 0:
                return n
        except ValueError:
            pass
        _log.warning(
            "ONIMPORT_MAX_WORKERS=%r is not a positive integer; using default", raw,
        )
    return min(4, os.cpu_count() or 1)


class ImportDispatcher:
    """Submit ``ImportRequest``s to a process pool and collect results.

    Use as a context manager (``with ImportDispatcher() as d``) so the pool
    and its log listener are torn down cleanly. ``close()`` does the same
    work for callers that hold the dispatcher long-term.
    """

    def __init__(self, max_workers: int | None = None) -> None:
        self._max_workers = max_workers if max_workers is not None else _default_max_workers()
        self._mp_context = mp.get_context("spawn")
        self._log_queue: "mp.Queue" = self._mp_context.Queue()
        self._listener = logging.handlers.QueueListener(
            self._log_queue,
            *logging.getLogger().handlers,
            respect_handler_level=True,
        )
        self._listener.start()
        # max_tasks_per_child=1 is the entire point of this design: each
        # worker handles exactly one import, then the pool replaces it with
        # a fresh process. Any value >1 reintroduces cross-import bpy state
        # degradation.
        self._pool = concurrent.futures.ProcessPoolExecutor(
            max_workers=self._max_workers,
            initializer=init_worker,
            initargs=(self._log_queue,),
            mp_context=self._mp_context,
            max_tasks_per_child=1,
        )
        self._closed = False

    @property
    def max_workers(self) -> int:
        return self._max_workers

    def submit(
        self, request: ImportRequest,
    ) -> "concurrent.futures.Future[ImportResult]":
        return self._pool.submit(run_one, request)

    def submit_batch(
        self, requests: Iterable[ImportRequest],
    ) -> Iterator[ImportResult]:
        """Submit all requests, yielding ``ImportResult``s as they complete.

        Crashes in a worker (segfault, OOM) surface as
        ``ImportResult.failure`` rather than poisoning the iterator.
        """
        future_to_request = {
            self._pool.submit(run_one, request): request for request in requests
        }
        for future in concurrent.futures.as_completed(future_to_request):
            request = future_to_request[future]
            try:
                yield future.result()
            except BrokenProcessPool as exc:
                _log.error("worker pool broken on %s: %s", request.label, exc)
                yield ImportResult.failure(
                    request, error=f"worker process crashed: {exc}",
                )
            except Exception as exc:  # noqa: BLE001 - surface any failure
                _log.error(
                    "worker raised on %s: %s", request.label, exc, exc_info=True,
                )
                yield ImportResult.failure(request, error=str(exc))

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._pool.shutdown(wait=True, cancel_futures=True)
        self._listener.stop()

    def __enter__(self) -> "ImportDispatcher":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()
