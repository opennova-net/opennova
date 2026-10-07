"""`stop` asks only the process it was given to quit (editor_mcp.py and game_mcp.py, docs/mcp.md).

    python -B -m unittest discover -s scripts/mcp -p "test_*.py"

Over a stand-in endpoint on an ephemeral port that names its process in its initialize result
(`_meta.pid`, as godot/game/mcp/mcp_server.gd does): a launch's pid file records its port, and a
plain-integer one still reads; `stop --pid-file` reaches the recorded port with no --port; an
endpoint that is another process's, or names none, is refused with nothing sent; a stop given no
process and no --port sends nothing (each client's default port is the stand-in's here, so a
mistake shows as a call recorded, never as a person's editor quit); --port alone stops as before;
a launch that reaches another process's endpoint fails; the console wrapper is never launched.
"""
import contextlib
import io
import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import editor_mcp  # noqa: E402
import game_mcp  # noqa: E402
from game_mcp import GameMcpError  # noqa: E402

UNSET = object()


class Endpoint:
    """A stand-in MCP endpoint: initialize names `pid` (none when None), every tools/call recorded;
    asked to quit, it closes."""

    def __init__(self, pid):
        self.pid = pid
        self.calls = []
        endpoint = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802 (http.server's name)
                message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                method = message.get("method")
                if "id" not in message:  # a notification
                    self.send_response(202)
                    self.end_headers()
                    return
                if method == "initialize":
                    result = {"protocolVersion": game_mcp.PROTOCOL_VERSION, "capabilities": {},
                              "serverInfo": {"name": "stand-in", "version": "0"}}
                    if endpoint.pid is not None:
                        result["_meta"] = {"pid": endpoint.pid}
                else:
                    endpoint.calls.append((message["params"]["name"], message["params"].get("arguments", {})))
                    result = {"content": [], "structuredContent": {}}
                body = json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": result}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Mcp-Session-Id", "s1")
                self.end_headers()
                self.wfile.write(body)
                if endpoint.quit_asked():  # the process leaves, and its endpoint with it
                    threading.Thread(target=endpoint.close, daemon=True).start()

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def quit_asked(self) -> bool:
        return any(name in ("editor_request", "game_control") and "quit" in arguments.values()
                   for name, arguments in self.calls)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class StopCase(unittest.TestCase):
    """Two stand-in endpoints, A and B (pids 1111 and 2222); the client's default port is A's, and
    a process counts as alive until its endpoint is asked to quit."""

    client = editor_mcp

    def setUp(self):
        self.a, self.b = Endpoint(1111), Endpoint(2222)
        self.addCleanup(self.a.close)
        self.addCleanup(self.b.close)
        folder = tempfile.TemporaryDirectory(prefix="opennova-mcp-stop-")
        self.addCleanup(folder.cleanup)
        self.dir = Path(folder.name)
        by_pid = {self.a.pid: self.a, self.b.pid: self.b}
        alive = mock.patch.object(self.client, "pid_alive",
                                  side_effect=lambda pid: pid in by_pid and not by_pid[pid].quit_asked())
        alive.start()
        self.addCleanup(alive.stop)
        default = mock.patch.object(self.client, "DEFAULT_PORT", self.a.port)
        default.start()
        self.addCleanup(default.stop)

    def pid_file(self, endpoint, content=UNSET) -> str:
        path = self.dir / f"{endpoint.pid}.pid"
        if content is UNSET:
            game_mcp.write_pid_file(path, endpoint.pid, endpoint.port)
        else:
            path.write_text(content, encoding="utf-8")
        return str(path)

    def stop(self, *argv) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            code = self.client.main(["stop", "--timeout", "2", *argv])
        return code, err.getvalue()


class EditorStopTest(StopCase):
    def test_the_pid_file_names_the_port_and_stops_that_editor_alone(self):
        pid_file = self.pid_file(self.b)
        code, _ = self.stop("--pid-file", pid_file)
        self.assertEqual(code, 0)
        self.assertEqual([name for name, _ in self.b.calls], ["editor_play", "editor_request"])
        self.assertEqual(self.b.calls[1][1], {"kind": "quit"})
        self.assertEqual(self.a.calls, [])
        self.assertFalse(Path(pid_file).exists())

    def test_another_editors_port_is_refused_with_nothing_sent(self):
        code, err = self.stop("--pid-file", self.pid_file(self.b), "--port", str(self.a.port))
        self.assertEqual(code, editor_mcp.EXIT_NOT_READ)
        self.assertIn("pid 1111's, not pid 2222's", err)
        self.assertEqual(self.a.calls + self.b.calls, [])

    def test_a_pid_given_alone_is_checked_on_the_default_port(self):
        code, err = self.stop("--pid", str(self.b.pid))
        self.assertEqual(code, editor_mcp.EXIT_NOT_READ)
        self.assertIn("not pid 2222's", err)
        self.assertEqual(self.a.calls, [])

    def test_a_plain_integer_pid_file_still_reads(self):
        code, _ = self.stop("--pid-file", self.pid_file(self.b, "2222\n"), "--port", str(self.b.port))
        self.assertEqual(code, 0)
        self.assertTrue(self.b.quit_asked())
        code, _ = self.stop("--pid-file", self.pid_file(self.b, "2222"))  # its port: the default, A's
        self.assertEqual(code, editor_mcp.EXIT_NOT_READ)
        self.assertEqual(self.a.calls, [])

    def test_an_endpoint_that_names_no_process_is_refused(self):
        self.b.pid = None
        code, err = self.stop("--pid", "2222", "--port", str(self.b.port))
        self.assertEqual(code, editor_mcp.EXIT_NOT_READ)
        self.assertIn("does not name its process", err)
        self.assertEqual(self.b.calls, [])

    def test_no_process_and_no_port_sends_nothing(self):
        for argv in ((), ("--pid-file", str(self.dir / "gone.pid")), ("--pid-file", self.pid_file(self.b, ""))):
            code, err = self.stop(*argv)
            self.assertEqual(code, editor_mcp.EXIT_NOT_READ, argv)
            self.assertIn("nothing was sent", err)
        self.assertEqual(self.a.calls + self.b.calls, [])

    def test_a_port_alone_stops_as_before(self):
        code, _ = self.stop("--port", str(self.b.port))
        self.assertEqual(code, 0)
        self.assertTrue(self.b.quit_asked())
        self.assertEqual(self.a.calls, [])


class GameStopTest(StopCase):
    client = game_mcp

    def test_the_pid_file_names_the_port_and_stops_that_game_alone(self):
        code, _ = self.stop("--pid-file", self.pid_file(self.b))
        self.assertEqual(code, 0)
        self.assertEqual(self.b.calls[-1], ("game_control", {"action": "quit"}))
        self.assertEqual(self.a.calls, [])

    def test_another_games_port_is_refused_with_nothing_sent(self):
        code, err = self.stop("--pid-file", self.pid_file(self.b), "--port", str(self.a.port))
        self.assertEqual(code, game_mcp.EXIT_USAGE)
        self.assertIn("pid 1111's, not pid 2222's", err)
        self.assertEqual(self.a.calls + self.b.calls, [])

    def test_no_process_and_no_port_sends_nothing(self):
        code, err = self.stop()
        self.assertEqual(code, game_mcp.EXIT_USAGE)
        self.assertIn("nothing was sent", err)
        self.assertEqual(self.a.calls, [])


class PidFileTest(unittest.TestCase):
    def test_a_launch_records_its_port_and_a_plain_integer_reads(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "editor.pid"
            game_mcp.write_pid_file(path, 4242, 8990)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"pid": 4242, "port": 8990})
            self.assertEqual(game_mcp.read_pid_file(path), (4242, 8990))
            for text, read in (("4242", (4242, None)), ("4242\r\n", (4242, None)), ("", (0, None)),
                               ("not a pid", (0, None)), ('{"pid": 4242}', (4242, None)), ("true", (0, None))):
                path.write_text(text, encoding="utf-8")
                self.assertEqual(game_mcp.read_pid_file(path), read, text)
            self.assertEqual(game_mcp.read_pid_file(Path(folder) / "missing.pid"), (0, None))

    def test_the_reported_pid(self):
        self.assertEqual(game_mcp.reported_pid({"_meta": {"pid": 77}}), 77)
        self.assertEqual(game_mcp.reported_pid({"_meta": {"pid": 77.0}}), 77)  # a JSON number read as a float
        for result in ({}, {"_meta": {}}, {"_meta": {"pid": "77"}}, {"_meta": {"pid": True}}, None):
            self.assertIsNone(game_mcp.reported_pid(result))


class LaunchTest(unittest.TestCase):
    def test_a_launch_that_reached_another_process_fails(self):
        game_mcp.check_launched_endpoint(8990, 7, 7, "editor")
        game_mcp.check_launched_endpoint(8990, None, 7, "editor")  # a build that names none: taken as it is
        with self.assertRaises(GameMcpError) as raised:
            game_mcp.check_launched_endpoint(8990, 8, 7, "editor")
        self.assertEqual(raised.exception.code, game_mcp.EXIT_LAUNCH_FAILED)

    def test_the_console_wrapper_is_never_launched(self):
        with tempfile.TemporaryDirectory() as folder:
            for wrapper, real in (("Godot_v4.6.1-stable_win64_console.exe", "Godot_v4.6.1-stable_win64.exe"),
                                  ("opennova-editor.console.exe", "opennova-editor.exe")):
                (Path(folder) / real).write_bytes(b"")
                self.assertEqual(game_mcp.runtime_sibling(Path(folder) / wrapper).name, real)
            self.assertEqual(game_mcp.runtime_sibling(Path(folder) / "lone.console.exe").name, "lone.console.exe")


if __name__ == "__main__":
    unittest.main()
