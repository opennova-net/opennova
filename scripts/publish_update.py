#!/usr/bin/env python3
"""Upload a packed update through the existing admin API and optionally publish it.

Example:
  python scripts/publish_update.py --url https://opennova.net --token-file admin.token \
      --package runtime.sfs --name "OpenNova runtime v0.1.0" --publish

The data worker validates the upload before publication. Without --publish the validated
release remains a draft for review in Game data. No AWS credentials or direct store access
are needed; the portal streams multipart uploads to the configured object store.
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


class PublishError(RuntimeError):
    pass


class AdminApi:
    def __init__(self, url: str, token: str, timeout: float = 120):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.query or parsed.fragment:
            raise PublishError("--url must be the portal's HTTP or HTTPS base URL")
        self.base = url.rstrip("/") + "/api/admin/releases"
        self.token = token
        self.timeout = timeout

    def request(self, method: str, path: str = "", body=None):
        # Cloudflare in front of the portal refuses urllib's default agent (error 1010).
        headers = {"Authorization": "Bearer " + self.token, "User-Agent": "opennova-publish-update/1"}
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


def upload(api: AdminApi, package: Path, name: str, publish: bool, wait_timeout: float = 600,
           poll_interval: float = 2):
    size = package.stat().st_size
    if size == 0:
        raise PublishError("the package is empty")
    # Keep one handle throughout so every uploaded part comes from the same file.
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
            # Abandon only an unfinished upload. Once completion is attempted its reply
            # can be lost after validation starts, so retain that release for inspection.
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
            raise PublishError(f"release {release_id} is still validating; inspect it in Game data")
        time.sleep(min(poll_interval, max(0, deadline - time.monotonic())))
    if publish:
        release = api.request("POST", path + "/publish")["release"]
    return release


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True, help="portal base URL")
    auth = parser.add_mutually_exclusive_group(required=True)
    auth.add_argument("--token-file", type=Path, help="file containing the existing admin bearer token")
    auth.add_argument("--admin-token", help="existing admin bearer token (prefer --token-file in CI)")
    parser.add_argument("--package", required=True, type=Path, help="packed .sfs update")
    parser.add_argument("--name", required=True, help="release label, at most 64 characters")
    parser.add_argument("--publish", action="store_true", help="publish after validation; otherwise leave a draft")
    parser.add_argument("--wait-timeout", type=float, default=600, help="validation deadline in seconds")
    args = parser.parse_args(argv)
    try:
        if len(args.name) > 64 or any(ord(char) < 32 or 127 <= ord(char) <= 159 for char in args.name):
            raise PublishError("--name must be at most 64 characters with no control characters")
        if args.wait_timeout <= 0:
            raise PublishError("--wait-timeout must be positive")
        token = args.token_file.read_text(encoding="utf-8").strip() if args.token_file else args.admin_token
        if not token or any(char in token for char in "\r\n"):
            raise PublishError("the admin token is empty or contains a newline")
        release = upload(AdminApi(args.url, token), args.package, args.name, args.publish, args.wait_timeout)
        print(json.dumps(release))
        return 0
    except (OSError, PublishError, KeyError, TypeError, ValueError) as error:
        print(f"publish_update: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
