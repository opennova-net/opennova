"""publish_apps.py against the admin API's release contract and a downloads host, over loopback."""

import contextlib
import hashlib
import http.server
import io
import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import publish_apps
from publish_apps import APPS, AdminApi, PublishError, app_builds, attach, publish

BUILDS = {
    "ONRUNTIME": "opennova-runtime-windows-v0.0.22.zip",
    "ONEDITOR": "opennova-editor-windows-v0.0.22.zip",
    "ONTOOLS": "opennova-tools-windows.zip",
    "ONBLENDER": "opennova-blender-addon-windows.zip",
    "ONSWITCHER": "opennova-switcher-windows.zip",
}


class PublishAppsTest(unittest.TestCase):
    def setUp(self):
        self.requests = []
        self.parts = {}  # release id -> the bytes its parts carried
        self.fail_part = False
        self.invalid = None  # a release id the build refuses
        self.stuck = False  # every release still building
        self.polls_to_draft = 2  # the release list's reads a build takes
        self.next_id = 40
        self.releases = {}  # id -> {"product_id", "version", "name", "polls"}
        self.products = [{"id": index + 1, "key": app.key, "kind": "app", "published_release": None}
                         for index, app in enumerate(APPS)]
        self.products.append({"id": 9, "key": "JO", "kind": "game", "published_release": None})
        self.host = {}  # the downloads host's files by path
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, status, body=None, raw=None):
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                if raw is not None:
                    self.wfile.write(raw)
                elif status != 204:
                    self.wfile.write(json.dumps(body or {}).encode())

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                path = urlsplit(self.path).path
                if path.startswith("/downloads/"):
                    data = owner.host.get(path[len("/downloads/"):])
                    return self.reply(200, raw=data) if data is not None else self.reply(404, {"message": "no"})
                owner.requests.append((self.command, path, body))
                owner.assertEqual(self.headers.get("Authorization"), "Bearer test-token")
                owner.assertEqual(self.headers.get("User-Agent"), "opennova-publish/1")
                if self.command == "GET" and path == "/api/admin/products":
                    return self.reply(200, {"products": owner.products})
                if self.command == "POST" and path == "/api/admin/releases":
                    asked = json.loads(body)
                    owner.assertEqual(set(asked), {"product_id", "version", "size", "name"})
                    release_id = owner.next_id
                    owner.next_id += 1
                    owner.releases[release_id] = dict(asked, polls=0)
                    return self.reply(201, {"release": {"id": release_id, "status": "uploading"},
                                            "upload": {"key": f"updates/source-{release_id}.zip",
                                                       "upload_id": "u+/=", "part_size": 4}})
                release_id = int(path.split("/")[4]) if path.count("/") >= 4 else 0
                if self.command == "PUT":
                    owner.assertEqual(parse_qs(urlsplit(self.path).query)["upload_id"], ["u+/="])
                    owner.assertEqual(self.headers.get("Content-Type"), "application/octet-stream")
                    if owner.fail_part:
                        return self.reply(502, {"message": "store unavailable"})
                    owner.parts[release_id] = owner.parts.get(release_id, b"") + body
                    return self.reply(200, {"etag": f'"{release_id}-{path.rsplit("/", 1)[1]}"'})
                if path.endswith("/complete"):
                    count = len(json.loads(body)["parts"])
                    owner.assertEqual(json.loads(body)["parts"], [
                        {"part_number": n, "etag": f'"{release_id}-{n}"'} for n in range(1, count + 1)])
                    return self.reply(202, {"release": {"id": release_id, "status": "building"}})
                if self.command == "GET" and path == "/api/admin/releases":
                    listed = []
                    for rid, release in owner.releases.items():
                        release["polls"] += 1
                        status = "building" if owner.stuck or release["polls"] < owner.polls_to_draft else "draft"
                        if rid == owner.invalid and release["polls"] >= 2:
                            status = "invalid"
                        listed.append({"id": rid, "status": status, "error": "the zip holds no oneditor.exe"})
                    return self.reply(200, {"releases": listed[::-1]})
                if path.endswith("/publish"):
                    release = owner.releases[release_id]
                    return self.reply(202, {"release": {"id": release_id, "status": "published",
                                                        "name": release["name"]}})
                if self.command == "DELETE":
                    return self.reply(204)
                owner.fail(f"unexpected request {self.command} {self.path}")

            do_GET = do_POST = do_PUT = do_DELETE = handle_request

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.directory = tempfile.TemporaryDirectory()
        self.dist = Path(self.directory.name) / "dist"
        self.dist.mkdir()
        for key, name in BUILDS.items():
            (self.dist / name).write_bytes(f"{key} build".encode())
        (self.dist / "opennova-project-windows").mkdir()  # another artifact of the run
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.api = AdminApi(self.url, "test-token")

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def product(self, key):
        return next(product for product in self.products if product["key"] == key)

    def run_publish(self, timeout=2):
        with contextlib.redirect_stderr(io.StringIO()):
            return publish(self.api, app_builds(self.dist), "0.0.22.123", "PR #7 (abc1234)", timeout, 0.001)

    def calls(self, method, suffix):
        return [index for index, (verb, path, _) in enumerate(self.requests)
                if verb == method and path.endswith(suffix)]

    def test_every_app_released_then_all_published(self):
        results = self.run_publish()
        self.assertEqual([result["key"] for result in results], [app.key for app in APPS])
        self.assertTrue(all(result["release"]["status"] == "published" for result in results))
        asked = {release["product_id"]: release for release in self.releases.values()}
        for app in APPS:
            release = asked[self.product(app.key)["id"]]
            self.assertEqual(release["version"], "0.0.22.123")
            self.assertEqual(release["name"], f"OpenNova {app.title} PR #7 (abc1234)")
            self.assertEqual(release["size"], len(f"{app.key} build"))
        self.assertEqual(sorted(self.parts.values()), sorted(f"{key} build".encode() for key in BUILDS))
        # Nothing is published before every release is a draft.
        publishes = self.calls("POST", "/publish")
        self.assertEqual(len(publishes), 5)
        self.assertLess(max(self.calls("POST", "/complete") + self.calls("GET", "/releases")), min(publishes))

    def test_an_unchanged_download_only_app_is_left_as_it_is(self):
        switcher = hashlib.sha256(b"ONSWITCHER build").hexdigest()
        self.product("ONSWITCHER")["published_release"] = {"id": 3, "name": "old", "sha256": switcher}
        # An updatable app's published sha is its package's: its build is always uploaded.
        runtime = hashlib.sha256(b"ONRUNTIME build").hexdigest()
        self.product("ONRUNTIME")["published_release"] = {"id": 4, "name": "old", "sha256": runtime}
        results = {result["key"]: result for result in self.run_publish()}
        self.assertTrue(results["ONSWITCHER"]["unchanged"] and results["ONSWITCHER"]["release"] is None)
        self.assertEqual(results["ONRUNTIME"]["release"]["status"], "published")
        self.assertNotIn(self.product("ONSWITCHER")["id"], [r["product_id"] for r in self.releases.values()])
        self.assertEqual(len(self.calls("POST", "/publish")), 4)

    def test_a_refused_build_publishes_nothing(self):
        self.invalid = 42
        with self.assertRaisesRegex(PublishError, "release 42 is invalid: the zip holds no oneditor.exe"):
            self.run_publish()
        self.assertEqual(self.calls("POST", "/publish"), [])
        self.assertEqual(self.calls("DELETE", ""), [])

    def test_a_failed_part_abandons_its_upload_and_publishes_nothing(self):
        self.fail_part = True
        with self.assertRaisesRegex(PublishError, "store unavailable"):
            self.run_publish()
        self.assertEqual(self.requests[-1][:2], ("DELETE", "/api/admin/releases/40"))
        self.assertEqual(self.calls("POST", "/publish"), [])

    def test_the_build_deadline_publishes_nothing(self):
        self.stuck = True
        with self.assertRaisesRegex(PublishError, "still building"):
            self.run_publish(timeout=0.005)
        self.assertEqual(self.calls("POST", "/publish"), [])

    def test_a_deployment_without_an_app_uploads_nothing(self):
        self.products = [product for product in self.products if product["key"] != "ONEDITOR"]
        with self.assertRaisesRegex(PublishError, "no app ONEDITOR"):
            self.run_publish()
        self.assertEqual(self.requests, [("GET", "/api/admin/products", b"")])

    def test_each_app_has_one_build(self):
        (self.dist / "opennova-runtime-windows-v0.0.23.zip").write_bytes(b"other")
        with self.assertRaisesRegex(PublishError, "holds 2 opennova-runtime-windows-v"):
            app_builds(self.dist)
        (self.dist / "opennova-runtime-windows-v0.0.23.zip").unlink()
        (self.dist / "opennova-tools-windows.zip").unlink()
        with self.assertRaisesRegex(PublishError, "holds 0 opennova-tools-windows.zip"):
            app_builds(self.dist)

    def write_token(self, text="test-token\n"):
        token = Path(self.directory.name) / "token"
        token.write_text(text, encoding="utf-8")
        return str(token)

    def test_main_publishes_and_writes_the_result(self):
        self.polls_to_draft = 1  # main polls at its own pace
        out = Path(self.directory.name) / "published.json"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = publish_apps.main(["publish", "--url", self.url, "--token-file", self.write_token(),
                                      "--from", str(self.dist), "--version", "0.0.22", "--label", "v0.0.22",
                                      "--out", str(out), "--wait-timeout", "5"])
        self.assertEqual(code, 0)
        self.assertEqual(len(json.loads(out.read_text(encoding="utf-8"))), 5)

    def test_main_refuses_a_bad_version_label_or_token_before_any_request(self):
        def main(*extra, token="test-token"):
            with contextlib.redirect_stderr(io.StringIO()):
                return publish_apps.main(["publish", "--url", self.url, "--token-file", self.write_token(token),
                                          "--from", str(self.dist), *extra])
        self.assertEqual(main("--version", "0.0.22-pr", "--label", "v1"), 1)
        self.assertEqual(main("--version", "1.2.3.4.5", "--label", "v1"), 1)
        self.assertEqual(main("--version", "0.0.22", "--label", "x" * 50), 1)
        self.assertEqual(main("--version", "0.0.22", "--label", "v1", token=""), 1)
        self.assertEqual(self.requests, [])

    # The manifest as the data program writes it (products/downloads_job.cpp, release_build_test's
    # test_downloads): a product with no download of its own (the runtime, an expansion) has every
    # field of its file null, its release among them; one with a download but no published release
    # names its file and nothing more.
    @staticmethod
    def unoffered(key, kind="app", file=None):
        return {"key": key, "kind": kind, "file": file, "size": None, "sha256": None,
                "version": None, "release": None, "published": None}

    @staticmethod
    def offered(key, file, data, release, kind="app"):
        return {"key": key, "kind": kind, "file": file, "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(), "version": "0.0.22", "release": release,
                "published": "2026-10-27T12:00:00Z"}

    def test_attach_waits_for_the_new_releases_then_names_the_files_for_the_tag(self):
        published = [{"key": "ONRUNTIME", "unchanged": False, "release": {"id": 40, "name": "OpenNova Runtime v0.0.22"}},
                     {"key": "ONEDITOR", "unchanged": False, "release": {"id": 41, "name": "OpenNova Editor v0.0.22"}},
                     {"key": "ONSWITCHER", "unchanged": True, "release": None}]
        game = b"the JO game zip"
        editor = b"the editor"
        switcher = b"the switcher"
        self.host["manifest.json"] = json.dumps({"written": "x", "products": [
            self.unoffered("ONRUNTIME"),
            self.offered("ONEDITOR", "opennova-editor-windows.zip", b"the old editor", "OpenNova Editor v0.0.21")]}).encode()
        polls = []

        def newer(url, timeout=120):
            polls.append(url)
            if len(polls) == 3:  # the host renders the new set after a while
                self.host["manifest.json"] = json.dumps({"written": "y", "products": [
                    self.unoffered("ONRUNTIME"),
                    self.offered("ONEDITOR", "opennova-editor-windows.zip", editor, "OpenNova Editor v0.0.22"),
                    self.offered("JO", "jointops-game-windows.zip", game, "Joint Operations 1.7.5.7", "game"),
                    self.unoffered("DFX2", "game", "dfx2-game-windows.zip"),
                    self.offered("ONSWITCHER", "opennova-switcher-windows.zip", switcher, "OpenNova Switcher v0.0.21"),
                    self.unoffered("NIFTY", "expansion")]}).encode()
            return original(url, timeout)

        original = publish_apps.fetch
        self.host["jointops-game-windows.zip"] = game
        self.host["opennova-editor-windows.zip"] = editor
        self.host["opennova-switcher-windows.zip"] = switcher
        out = Path(self.directory.name) / "assets"
        manifest = f"{self.url}/downloads/manifest.json"
        with contextlib.redirect_stderr(io.StringIO()):
            publish_apps.fetch = newer
            try:
                files = attach(manifest, published, "v0.0.22", out, 5, 0.001)
            finally:
                publish_apps.fetch = original
        # It waited for the editor's new release, never for the runtime's (which the host never
        # names), and fetched no unpublished game's file (DFX2's: the host has none).
        self.assertEqual(sum(url.endswith("manifest.json") for url in polls), 3)
        self.assertEqual([path.name for path in files],
                         ["opennova-editor-windows-v0.0.22.zip", "jointops-game-windows-v0.0.22.zip",
                          "opennova-switcher-windows-v0.0.22.zip"])
        self.assertEqual(files[1].read_bytes(), game)
        # A file that is not what the manifest says is refused.
        self.host["jointops-game-windows.zip"] = b"tampered"
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(PublishError, "does not match"):
                attach(manifest, published, "v0.0.22", out, 5, 0.001)

    def test_attach_fails_when_the_host_never_lists_the_release(self):
        self.host["manifest.json"] = json.dumps({"written": "x", "products": [
            self.unoffered("ONRUNTIME"),
            self.offered("ONEDITOR", "opennova-editor-windows.zip", b"old", "OpenNova Editor v0.0.21")]}).encode()
        published = [{"key": "ONRUNTIME", "unchanged": False, "release": {"id": 40, "name": "n"}},
                     {"key": "ONEDITOR", "unchanged": False, "release": {"id": 41, "name": "OpenNova Editor v0.0.22"}}]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(PublishError, r"does not list the new releases of \['ONEDITOR'\]"):
                attach(f"{self.url}/downloads/manifest.json", published, "v0.0.22",
                       Path(self.directory.name) / "assets", 0.01, 0.001)

if __name__ == "__main__":
    unittest.main()
