"""The workflows' triggers cover the sources their builds are made from.

The NovaWorld images (novaworld-images.yml): master's push builds and publishes every
image `app deploy --tag sha-<head>` pins, so its path filter must hold every source the
images' binaries are built from (apps/novaworld_common/Dockerfile); a merge touching only
one it missed built no images at all.
"""
import fnmatch
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"


def trigger_paths(workflow: Path, event: str) -> list:
    """The `paths:` list of `on: <event>:` in `workflow`, read line by line (stdlib, no YAML)."""
    out = []
    in_on = in_event = in_paths = False
    for line in workflow.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            in_on = stripped == "on:"
            in_event = in_paths = False
            continue
        if in_on and indent == 2:
            in_event = stripped == event + ":"
            in_paths = False
            continue
        if in_event and stripped == "paths:":
            in_paths = True
            continue
        if in_paths:
            match = re.match(r"-\s*'([^']+)'", stripped)
            if match:
                out.append(match.group(1))
            else:
                in_paths = False
    return out


def dockerfile_build_targets(dockerfile: Path) -> list:
    """The opennova_* targets the Dockerfile's first `cmake --build build --target` compiles."""
    text = dockerfile.read_text(encoding="utf-8")
    start = text.index("cmake --build build --target")
    command = []
    for line in text[start:].splitlines():
        command.append(line.rstrip("\\").strip())
        if not line.rstrip().endswith("\\"):
            break
    return [token for token in " ".join(command).split() if token.startswith("opennova_")]


def target_directory(target: str) -> str:
    """The repository directory whose CMakeLists.txt adds `target` as an executable."""
    for cmake in sorted((ROOT / "apps").glob("*/CMakeLists.txt")):
        if re.search(r"add_executable\(\s*" + re.escape(target) + r"\b", cmake.read_text(encoding="utf-8")):
            return cmake.parent.relative_to(ROOT).as_posix()
    raise AssertionError(f"no apps/*/CMakeLists.txt adds {target}")


def covered(path: str, patterns: list) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


class NovaWorldImagesTrigger(unittest.TestCase):
    def setUp(self):
        self.workflow = WORKFLOWS / "novaworld-images.yml"
        self.push = trigger_paths(self.workflow, "push")
        self.pull_request = trigger_paths(self.workflow, "pull_request")

    def test_the_lists_read(self):
        self.assertIn(".github/workflows/novaworld-images.yml", self.push)
        self.assertIn(".github/workflows/novaworld-images.yml", self.pull_request)

    def test_master_builds_on_every_source_of_the_images_binaries(self):
        targets = dockerfile_build_targets(ROOT / "apps" / "novaworld_common" / "Dockerfile")
        self.assertIn("opennova_project", targets)
        self.assertIn("opennova_update_tool", targets)
        for target in targets:
            source = target_directory(target) + "/main.cpp"
            self.assertTrue(covered(source, self.push), f"{target}: {source} is in no push path")
        # Every program links the engine's groups and the libraries they link.
        for source in ("engine/base/io/le.h", "engine/formats/pff/pff.cpp", "engine/runtime/world/world.cpp",
                       "engine/editor/session/session_core.cpp", "engine/net/update/update_server.cpp",
                       "engine/net/title/title_identity.h", "apps/common/net_sockets.h", "third_party/miniz/miniz.c",
                       "CMakeLists.txt", ".dockerignore"):
            self.assertTrue(covered(source, self.push), f"{source} is in no push path")

    def test_a_pull_request_builds_on_what_the_images_alone_build(self):
        # The rest of the engine reaches Linux through ci.yml's test-linux.
        for source in ("apps/project/main.cpp", "apps/update_tool/main.cpp", "engine/net/update/update_wire.cpp",
                       "engine/net/title/title_identity.h", "apps/common/net_sockets.h", "apps/novaworld_data/main.cpp"):
            self.assertTrue(covered(source, self.pull_request), f"{source} is in no pull_request path")


if __name__ == "__main__":
    unittest.main()
