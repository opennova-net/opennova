"""publish_runtime.py against the admin API's multipart and validation contract over loopback."""

import contextlib
import http.server
import io
import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import publish_runtime
from publish_runtime import AdminApi, PublishError, publish, runtime_package


class PublishRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.requests = []
        self.parts = []
        self.fail_part = False
        self.fail_complete = False
        self.statuses = ["validating", "draft"]
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                owner.requests.append((self.command, self.path, body))
                owner.assertEqual(self.headers.get("Authorization"), "Bearer test-token")
                owner.assertEqual(self.headers.get("User-Agent"), "opennova-publish/1")
                path = urlsplit(self.path).path
                status = 200
                reply = {}
                if self.command == "POST" and path == "/api/admin/releases":
                    owner.assertEqual(json.loads(body), {"name": "OpenNova runtime v1", "size": 8})
                    reply = {"release": {"id": 7}, "upload": {"upload_id": "u+/=", "part_size": 3}}
                    status = 201
                elif self.command == "PUT":
                    owner.assertEqual(parse_qs(urlsplit(self.path).query)["upload_id"], ["u+/="])
                    if owner.fail_part:
                        status, reply = 502, {"message": "store unavailable"}
                    else:
                        owner.parts.append(body)
                        reply = {"etag": f'"part-{len(owner.parts)}"'}
                elif path.endswith("/complete"):
                    owner.assertEqual(json.loads(body), {"upload_id": "u+/=", "parts": [
                        {"part_number": index, "etag": f'"part-{index}"'} for index in (1, 2, 3)]})
                    status = 202
                    if owner.fail_complete:
                        status, reply = 502, {"message": "completion reply lost"}
                elif self.command == "GET":
                    state = owner.statuses.pop(0) if len(owner.statuses) > 1 else owner.statuses[0]
                    reply = {"releases": [{"id": 7, "status": state, "error": "bad package"}]}
                elif path.endswith("/publish"):
                    status, reply = 202, {"release": {"id": 7, "status": "published"}}
                elif self.command == "DELETE":
                    status = 204
                else:
                    owner.fail(f"unexpected request {self.command} {self.path}")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                if status != 204:
                    self.wfile.write(json.dumps(reply).encode())

            do_GET = do_POST = do_PUT = do_DELETE = handle_request

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.directory = tempfile.TemporaryDirectory()
        self.dist = Path(self.directory.name) / "dist"
        self.dist.mkdir()
        self.package = self.dist / "opennova-runtime-windows-v1.sfs"
        self.package.write_bytes(b"package!")
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.api = AdminApi(self.url, "test-token")

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def run_publish(self, timeout=2):
        with contextlib.redirect_stderr(io.StringIO()):
            return publish(self.api, self.package, "OpenNova runtime v1", timeout, 0.001)

    def test_multipart_then_validate_then_publish(self):
        self.assertEqual(self.run_publish(), {"id": 7, "status": "published"})
        self.assertEqual(b"".join(self.parts), b"package!")
        self.assertEqual(self.requests[-1][:2], ("POST", "/api/admin/releases/7/publish"))

    def test_failed_part_abandons_upload(self):
        self.fail_part = True
        with self.assertRaisesRegex(PublishError, "store unavailable"):
            self.run_publish()
        self.assertEqual(self.requests[-1][:2], ("DELETE", "/api/admin/releases/7"))

    def test_invalid_package_is_retained_and_not_published(self):
        self.statuses = ["invalid"]
        with self.assertRaisesRegex(PublishError, "bad package"):
            self.run_publish()
        self.assertFalse(any(method == "DELETE" or path.endswith("/publish")
                             for method, path, _ in self.requests))

    def test_completion_failure_retains_release(self):
        self.fail_complete = True
        with self.assertRaisesRegex(PublishError, "completion reply lost"):
            self.run_publish()
        self.assertFalse(any(method == "DELETE" or path.endswith("/publish")
                             for method, path, _ in self.requests))

    def test_validation_deadline_does_not_publish(self):
        self.statuses = ["validating"]
        with self.assertRaisesRegex(PublishError, "still validating"):
            self.run_publish(timeout=0.005)
        self.assertFalse(any(path.endswith("/publish") for _, path, _ in self.requests))

    def test_the_one_runtime_package(self):
        (self.dist / "opennova-game-windows-v1.zip").write_bytes(b"zip")
        self.assertEqual(runtime_package(self.dist), self.package)
        (self.dist / "opennova-runtime-windows-v2.sfs").write_bytes(b"other")
        with self.assertRaisesRegex(PublishError, "holds 2"):
            runtime_package(self.dist)
        with self.assertRaisesRegex(PublishError, "holds 0"):
            runtime_package(Path(self.directory.name))

    def test_main_publishes_the_folder_package(self):
        token = Path(self.directory.name) / "token"
        token.write_text("test-token\n", encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = publish_runtime.main(["--url", self.url, "--token-file", str(token), "--from", str(self.dist),
                                         "--name", "OpenNova runtime v1"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue()), {"id": 7, "status": "published"})

    def test_main_refuses_a_bad_name_or_token_before_any_request(self):
        token = Path(self.directory.name) / "token"
        token.write_text("", encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(publish_runtime.main(["--url", self.url, "--token-file", str(token),
                                                   "--from", str(self.dist), "--name", "OpenNova runtime v1"]), 1)
            token.write_text("test-token", encoding="utf-8")
            self.assertEqual(publish_runtime.main(["--url", self.url, "--token-file", str(token),
                                                   "--from", str(self.dist), "--name", "x" * 65]), 1)
        self.assertEqual(self.requests, [])


if __name__ == "__main__":
    unittest.main()
