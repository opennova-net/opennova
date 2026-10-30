#!/usr/bin/env python3
"""Publish's apps: a build's four apps released on a deployment's update service (ADR 0072).

.github/workflows/publish.yml runs `publish` once per Publish over the run's artifacts in --from
(a staging-branch CI run, or a tag's release.yml run): each app's plain build, the artifact's zip
as it is, uploaded through the deployment's admin API as a release of its product (looked up by
its key), each waited until the service has made it a draft (an updatable app's packed with the
OPENNOVA.WIZ it writes and validated; a download-only app's stored as uploaded), and only then
all of them published, so a refusal of any one publishes none:

  python scripts/publish_apps.py publish --url https://opennova.net --token-file "$token_file" \\
      --from dist --version 0.0.22 --label v0.0.22 --out published.json

A download-only app whose zip is byte for byte the one its published release holds (a rebuild
of an unchanged app) is left as it is, "unchanged". An updatable app always uploads: its
build differs every time, and its published release holds the package the service packed.

For prod, `attach` then waits until the downloads host's manifest.json lists every app the run
published that offers a download at its new release (the data program renders the host once a
release is published; the runtime offers none, the game zips carry it), and downloads each
published file the host lists, its size and SHA-256 checked, as <name>-<tag>.zip for the GitHub
release:

  python scripts/publish_apps.py attach --manifest https://downloads.opennova.net/manifest.json \\
      --published published.json --tag v0.0.22 --to assets

--url is the deployment's site (the environment's NOVAWORLD_URL variable) and --token-file holds
its admin API token (the environment's ADMIN_API_TOKEN secret, in a file so it never shows in a
process list). A build the service refuses stays an invalid release for an admin to inspect, and
the run fails before anything is published.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

NAME_MAX = 64
VERSION_PATTERN = re.compile(r"^\d+(\.\d+){0,3}$")


@dataclasses.dataclass(frozen=True)
class App:
    key: str  # its product's key
    pattern: str  # its build's file among the artifacts
    title: str  # its releases' names: "OpenNova <title> <label>"
    updatable: bool  # the service packs it as an update package; else it is offered as uploaded


APPS = (
    App("ONRUNTIME", "opennova-runtime-windows-v*.zip", "Runtime", True),
    App("ONEDITOR", "opennova-editor-windows-v*.zip", "Editor", True),
    App("ONTOOLS", "opennova-tools-windows.zip", "Tools", False),
    App("ONBLENDER", "opennova-blender-addon-windows.zip", "Blender add-on", False),
)


class PublishError(RuntimeError):
    pass


class AdminApi:
    def __init__(self, url: str, token: str, timeout: float = 120):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.query or parsed.fragment:
            raise PublishError("--url must be the deployment's HTTP or HTTPS base URL")
        self.base = url.rstrip("/") + "/api/admin"
        self.token = token
        self.timeout = timeout

    def request(self, method: str, path: str, body=None):
        # Cloudflare in front of the portal refuses urllib's default agent (error 1010).
        headers = {"Authorization": "Bearer " + self.token, "User-Agent": "opennova-publish/1"}
        if isinstance(body, bytes):
            data = body
            headers["Content-Type"] = "application/octet-stream"
        elif body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        else:
            data = None
        request = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
        except urllib.error.HTTPError as error:
            with error:
                try:
                    detail = json.loads(error.read()).get("message", error.reason)
                except (ValueError, AttributeError):
                    detail = error.reason
            raise PublishError(f"{method} {path}: HTTP {error.code}: {detail}") from error
        except (urllib.error.URLError, TimeoutError) as error:
            raise PublishError(f"{method} {path}: {error}") from error
        if not raw:
            return {}
        try:
            return json.loads(raw)
        except ValueError as error:
            raise PublishError("the admin API returned invalid JSON") from error


def app_builds(source_dir: Path) -> list[tuple[App, Path]]:
    """Each app's one build among a run's artifacts."""
    builds = []
    for app in APPS:
        found = sorted(path for path in source_dir.glob(app.pattern) if path.is_file())
        if len(found) != 1:
            raise PublishError(f"{source_dir} holds {len(found)} {app.pattern}, not one")
        builds.append((app, found[0]))
    return builds


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def products_by_key(api: AdminApi) -> dict:
    """The deployment's products by key: each app's id and its published release."""
    return {product.get("key"): product for product in api.request("GET", "/products")["products"]}


def upload(api: AdminApi, product_id: int, version: str, name: str, build: Path) -> int:
    """`build` uploaded as a release of the product, its parts sent and the upload completed: the
    release's id, which the service builds from there."""
    size = build.stat().st_size
    if size == 0:
        raise PublishError(f"{build.name} is empty")
    # One handle throughout, so every part comes from the same file.
    with build.open("rb") as source:
        reply = api.request("POST", "/releases",
                            {"product_id": product_id, "version": version, "size": size, "name": name})
        release_id = reply["release"]["id"]
        path = f"/releases/{release_id}"
        upload_id = reply["upload"]["upload_id"]
        part_size = int(reply["upload"]["part_size"])
        if part_size <= 0 or part_size > 64 * 1024 * 1024:
            raise PublishError("the admin API returned an invalid part size")
        parts = []
        sent = 0
        try:
            while chunk := source.read(part_size):
                number = len(parts) + 1
                part = api.request("PUT", path + f"/parts/{number}?" +
                                   urllib.parse.urlencode({"upload_id": upload_id}), chunk)
                parts.append({"part_number": number, "etag": part["etag"]})
                sent += len(chunk)
                print(f"Release {release_id} ({build.name}): uploaded {sent}/{size} bytes", file=sys.stderr)
            if sent != size:
                raise PublishError(f"{build.name} changed size during upload")
        except Exception:
            # Only an unfinished upload is abandoned. A completion's reply can be lost once the
            # build has begun, so that release stays for an admin to inspect.
            try:
                api.request("DELETE", path)
            except PublishError:
                pass
            raise
        api.request("POST", path + "/complete", {"upload_id": upload_id, "parts": parts})
    return release_id


def wait_for_drafts(api: AdminApi, release_ids: list[int], wait_timeout: float, poll_interval: float) -> None:
    """Each release waited through its build (and an updatable app's validation) until it is a
    draft; an invalid one, or one still building at the deadline, fails the run."""
    deadline = time.monotonic() + wait_timeout
    waiting = set(release_ids)
    while waiting:
        releases = {item["id"]: item for item in api.request("GET", "/releases")["releases"]}
        for release_id in sorted(waiting):
            release = releases.get(release_id)
            if release is None:
                raise PublishError(f"release {release_id} disappeared during its build")
            status = release["status"]
            if status == "draft":
                waiting.discard(release_id)
            elif status == "invalid":
                raise PublishError(f"release {release_id} is invalid: {release.get('error', '')}")
            elif status not in ("uploading", "building", "validating"):
                raise PublishError(f"release {release_id} unexpectedly became {status}")
        if not waiting:
            return
        if time.monotonic() >= deadline:
            raise PublishError(f"releases {sorted(waiting)} are still building; inspect them in the web admin")
        time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))


def publish(api: AdminApi, builds: list[tuple[App, Path]], version: str, label: str,
            wait_timeout: float = 1800, poll_interval: float = 5) -> list[dict]:
    """Every app's build released and waited until each is a draft, then all of them published.
    What each app came to: its key and its published release, or unchanged."""
    products = products_by_key(api)
    plan = []
    for app, build in builds:
        product = products.get(app.key)
        if product is None or product.get("kind") != "app":
            raise PublishError(f"the deployment has no app {app.key}")
        published = product.get("published_release") or {}
        if not app.updatable and published.get("sha256") == sha256_of(build):
            print(f"{app.key}: unchanged ({build.name} is release {published.get('id')}'s)", file=sys.stderr)
            plan.append((app, None))
            continue
        plan.append((app, (product["id"], build)))
    results = []
    uploaded = []
    for app, target in plan:
        if target is None:
            results.append({"key": app.key, "unchanged": True, "release": None})
            continue
        product_id, build = target
        name = f"OpenNova {app.title} {label}"
        release_id = upload(api, product_id, version, name, build)
        uploaded.append(release_id)
        results.append({"key": app.key, "unchanged": False, "release": release_id})
    print(f"Waiting for releases {uploaded} to be drafts", file=sys.stderr)
    wait_for_drafts(api, uploaded, wait_timeout, poll_interval)
    for result in results:
        if result["release"] is not None:
            result["release"] = api.request("POST", f"/releases/{result['release']}/publish")["release"]
            print(f"{result['key']}: published release {result['release']['id']}", file=sys.stderr)
    return results


def fetch(url: str, timeout: float = 120) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "opennova-publish/1", "Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()
    except (urllib.error.URLError, TimeoutError) as error:
        raise PublishError(f"GET {url}: {error}") from error


def lists_release(entry: dict, release: dict) -> bool:
    """Whether a manifest entry names the release (by its id or its name)."""
    named = entry.get("release")
    return named is not None and named in (release.get("id"), str(release.get("id")), release.get("name"))


def offers_download(entry: dict) -> bool:
    """Whether a manifest entry is a product with a download of its own. One without (the
    runtime) is listed with every field of its file null, its release among them, so the host
    never names its release: the game zips carry it, rendered in the same pass as the editor's."""
    return entry.get("file") is not None


def attach(manifest_url: str, published: list[dict], tag: str, out_dir: Path,
           wait_timeout: float = 1800, poll_interval: float = 15) -> list[Path]:
    """The host's files, once its manifest lists every app this run published that offers a
    download at its new release, each published file downloaded as <name>-<tag>.zip and checked
    against the manifest (a product with no published release yet names its file and nothing
    more, and is skipped)."""
    expected = {result["key"]: result["release"] for result in published if result.get("release")}
    deadline = time.monotonic() + wait_timeout
    while True:
        try:
            manifest = json.loads(fetch(manifest_url))
            entries = {entry.get("key"): entry for entry in manifest["products"]}
        except (PublishError, ValueError, KeyError, TypeError, AttributeError) as error:
            entries, problem = {}, str(error)
        else:
            problem = ""
        behind = sorted(key for key, release in expected.items()
                        if key not in entries
                        or (offers_download(entries[key]) and not lists_release(entries[key], release)))
        if entries and not behind:
            break
        if time.monotonic() >= deadline:
            raise PublishError(f"{manifest_url} does not list the new releases of {behind or 'its apps'}"
                               + (f": {problem}" if problem else ""))
        print(f"Waiting for {manifest_url} to list {behind}", file=sys.stderr)
        time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
    base = manifest_url.rsplit("/", 1)[0]
    out_dir.mkdir(parents=True, exist_ok=True)
    attached = []
    for entry in manifest["products"]:
        name = entry.get("file")
        if not name or entry.get("release") is None:
            continue
        if not re.fullmatch(r"[A-Za-z0-9._-]+\.zip", name):
            raise PublishError(f"the manifest lists an unexpected file {name!r}")
        data = fetch(f"{base}/{name}")
        if len(data) != entry.get("size") or hashlib.sha256(data).hexdigest() != entry.get("sha256"):
            raise PublishError(f"{name} does not match the manifest's size and SHA-256")
        path = out_dir / f"{name[:-len('.zip')]}-{tag}.zip"
        path.write_bytes(data)
        attached.append(path)
    return attached


def read_token(path: Path) -> str:
    token = path.read_text(encoding="utf-8").strip()
    if not token or any(char in token for char in "\r\n"):
        raise PublishError("the admin token is empty or contains a newline")
    return token


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    release = commands.add_parser("publish", help="release and publish the four apps of a run's builds")
    release.add_argument("--url", required=True, help="the deployment's site (its NOVAWORLD_URL)")
    release.add_argument("--token-file", required=True, type=Path, help="the file holding its admin API token")
    release.add_argument("--from", dest="source_dir", required=True, type=Path,
                         help="the folder holding the run's build zips")
    release.add_argument("--version", required=True, help="the releases' VERSION: one to four numbers between dots")
    release.add_argument("--label", required=True, help="what the releases' names end with: the tag, or 'PR #n (sha)'")
    release.add_argument("--out", type=Path, help="where the result's JSON is written too")
    release.add_argument("--wait-timeout", type=float, default=1800, help="the builds' deadline in seconds")
    host = commands.add_parser("attach", help="the downloads host's files, as a GitHub release's assets")
    host.add_argument("--manifest", required=True, help="the downloads host's manifest.json URL")
    host.add_argument("--published", required=True, type=Path, help="publish's --out")
    host.add_argument("--tag", required=True, help="the release tag the files are named for")
    host.add_argument("--to", dest="out_dir", required=True, type=Path, help="where the files are written")
    host.add_argument("--wait-timeout", type=float, default=1800, help="the host's deadline in seconds")
    args = parser.parse_args(argv)
    try:
        if args.wait_timeout <= 0:
            raise PublishError("--wait-timeout must be positive")
        if args.command == "attach":
            if not re.fullmatch(r"v\d+(\.\d+){0,3}", args.tag):
                raise PublishError("--tag must be a release tag (v0.0.22)")
            published = json.loads(args.published.read_text(encoding="utf-8"))
            for path in attach(args.manifest, published, args.tag, args.out_dir, args.wait_timeout):
                print(path)
            return 0
        if not VERSION_PATTERN.match(args.version):
            raise PublishError("--version must be one to four numbers between dots")
        longest = max(len(f"OpenNova {app.title} {args.label}") for app in APPS)
        if longest > NAME_MAX or any(ord(char) < 32 or 127 <= ord(char) <= 159 for char in args.label):
            raise PublishError(f"--label makes a release name past {NAME_MAX} characters, or holds a control character")
        api = AdminApi(args.url, read_token(args.token_file))
        results = publish(api, app_builds(args.source_dir), args.version, args.label, args.wait_timeout)
        text = json.dumps(results)
        if args.out:
            args.out.write_text(text, encoding="utf-8")
        print(text)
        return 0
    except (OSError, PublishError, KeyError, TypeError, ValueError) as error:
        print(f"publish_apps: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
