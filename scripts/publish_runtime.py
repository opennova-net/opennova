#!/usr/bin/env python3
"""Publish's runtime release: a build's runtime package published on a deployment's update service.

.github/workflows/publish.yml runs it once per Publish: the one opennova-runtime-windows-v*.sfs in
--from (a tagged release's asset, or a pull request's CI artifact) uploaded as a release of the
OpenNova runtime (the game with no project, whose releases arrive from CI) through the deployment's
admin API, waited through the data program's validation, and published, which is what the updater
serves from then on:

  python scripts/publish_runtime.py --url https://opennova.net --token-file "$token_file" \\
      --from dist --name "OpenNova runtime v0.0.21"

--url is the deployment's site (the environment's NOVAWORLD_URL variable) and --token-file holds its
admin API token (the environment's ADMIN_API_TOKEN secret, in a file so it never shows in a process
list). A package the service finds invalid stays an invalid release for an admin to inspect, never
published, and the run fails.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

PACKAGE_GLOB = "opennova-runtime-windows-v*.sfs"
NAME_MAX = 64


class PublishError(RuntimeError):
    pass


class AdminApi:
    def __init__(self, url: str, token: str, timeout: float = 120):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.query or parsed.fragment:
            raise PublishError("--url must be the deployment's HTTP or HTTPS base URL")
        self.base = url.rstrip("/") + "/api/admin/releases"
        self.token = token
        self.timeout = timeout

    def request(self, method: str, path: str = "", body=None):
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
            try:
                detail = json.loads(error.read()).get("message", error.reason)
            except (ValueError, AttributeError):
                detail = error.reason
            raise PublishError(f"{method} {path or '/'}: HTTP {error.code}: {detail}") from error
        except (urllib.error.URLError, TimeoutError) as error:
            raise PublishError(f"{method} {path or '/'}: {error}") from error
        if not raw:
            return {}
        try:
            return json.loads(raw)
        except ValueError as error:
            raise PublishError("the admin API returned invalid JSON") from error


def runtime_package(source_dir: Path) -> Path:
    """The one runtime package among a release's assets or a run's artifacts."""
    packages = sorted(path for path in source_dir.glob(PACKAGE_GLOB) if path.is_file())
    if len(packages) != 1:
        raise PublishError(f"{source_dir} holds {len(packages)} {PACKAGE_GLOB}, not one")
    return packages[0]


def publish(api: AdminApi, package: Path, name: str, wait_timeout: float = 600, poll_interval: float = 2):
    """The package uploaded as a release, waited through validation, then published."""
    size = package.stat().st_size
    if size == 0:
        raise PublishError("the package is empty")
    # One handle throughout, so every part comes from the same file.
    with package.open("rb") as source:
        reply = api.request("POST", body={"name": name, "size": size})
        release_id = reply["release"]["id"]
        path = f"/{release_id}"
        upload_id = reply["upload"]["upload_id"]
        part_size = int(reply["upload"]["part_size"])
        if part_size <= 0 or part_size > 32 * 1024 * 1024:
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
                print(f"Release {release_id}: uploaded {sent}/{size} bytes", file=sys.stderr)
            if sent != size:
                raise PublishError("the package size changed during upload")
        except Exception:
            # Only an unfinished upload is abandoned. A completion's reply can be lost once
            # validation has begun, so that release stays for an admin to inspect.
            try:
                api.request("DELETE", path)
            except PublishError:
                pass
            raise
        api.request("POST", path + "/complete", {"upload_id": upload_id, "parts": parts})

    print(f"Release {release_id}: waiting for validation", file=sys.stderr)
    deadline = time.monotonic() + wait_timeout
    while True:
        releases = api.request("GET")["releases"]
        release = next((item for item in releases if item["id"] == release_id), None)
        if release is None:
            raise PublishError(f"release {release_id} disappeared during validation")
        if release["status"] == "draft":
            break
        if release["status"] == "invalid":
            raise PublishError(f"release {release_id} is invalid: {release.get('error', '')}")
        if release["status"] not in ("uploading", "validating"):
            raise PublishError(f"release {release_id} unexpectedly became {release['status']}")
        if time.monotonic() >= deadline:
            raise PublishError(f"release {release_id} is still validating; inspect it in the web admin")
        time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
    return api.request("POST", path + "/publish")["release"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True, help="the deployment's site (its NOVAWORLD_URL)")
    parser.add_argument("--token-file", required=True, type=Path, help="the file holding its admin API token")
    parser.add_argument("--from", dest="source_dir", required=True, type=Path,
                        help=f"the folder holding the build's {PACKAGE_GLOB}")
    parser.add_argument("--name", required=True, help=f"the release's name, at most {NAME_MAX} characters")
    parser.add_argument("--wait-timeout", type=float, default=600, help="validation deadline in seconds")
    args = parser.parse_args(argv)
    try:
        if len(args.name) > NAME_MAX or any(ord(char) < 32 or 127 <= ord(char) <= 159 for char in args.name):
            raise PublishError(f"--name must be at most {NAME_MAX} characters with no control characters")
        if args.wait_timeout <= 0:
            raise PublishError("--wait-timeout must be positive")
        token = args.token_file.read_text(encoding="utf-8").strip()
        if not token or any(char in token for char in "\r\n"):
            raise PublishError("the admin token is empty or contains a newline")
        release = publish(AdminApi(args.url, token), runtime_package(args.source_dir), args.name,
                          args.wait_timeout)
        print(json.dumps(release))
        return 0
    except (OSError, PublishError, KeyError, TypeError, ValueError) as error:
        print(f"publish_runtime: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
