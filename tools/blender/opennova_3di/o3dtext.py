# The scene text the add-on speaks with opennova-3di, and the one way it runs
# the CLI. The .o3d model text (docs/threedi/o3d-scene-format.md) and the .o3a
# clip-set text (docs/anim/o3a-scene-format.md) share one grammar: a record per
# line, whitespace-separated tokens, a name field bare or "quoted", and `#` a
# comment at the start of a line or after whitespace, never inside quotes
# (retail shader tags such as VS_PHONGT#UV hold one). Both carry MISSION axes
# (x forward, y left, z up); the axis helpers here map them to Blender's and
# back, and ModelSpace reads a model's objects in its root's frame for the
# model and animation exports.

import math
import os
import subprocess
import tempfile

import bpy
from mathutils import Matrix, Vector


class ExportError(Exception):
    """What stops an export, reported to the author."""


class ImportFailed(Exception):
    """What stops an import, reported to the author."""


# Styles above 0x70 carry a CTRL register (the loader's structural rule,
# threedi_panm_parameter_is_ctrl_reference); the others carry a phase.
CTRL_REFERENCE_THRESHOLD = 0x70

# The CLI reads its tokens with C++ stream extraction, which splits on C's
# isspace set.
BLANKS = " \t\n\v\f\r"


# --- the text ---------------------------------------------------------------

def fmt(*values):
    """Numbers as tokens: an int as it is, a float as the shortest text that
    reads back as the very same double (a float32 Blender holds, exactly),
    -0 as 0, and `nan`, `inf`, `-inf`. Nine digits would give a float32 back,
    but the CLI reads a double and truncates the fixed-point words from it
    (CXLT rows, section offsets and user points to 16.16, collision vertices
    to 8.8): a retail value on that grid, printed to nine digits, can read a
    hair below it and truncate a whole step down."""
    out = []
    for v in values:
        if isinstance(v, float):
            s = repr(v)
            out.append("0" if s == "-0.0" else s)
        else:
            out.append(str(v))
    return " ".join(out)


def quoted(name):
    """A name field: bare when it is one plain token, else "quoted" (empty, or
    holding a blank or a `#`). The text has no escape, so a name holding a
    double quote cannot be written."""
    if '"' in name:
        raise ExportError(f"the name '{name}' holds a double quote, which the scene text cannot carry")
    if name and not any(c in BLANKS or c == "#" for c in name):
        return name
    return f'"{name}"'


def strip_comment(line):
    """The line without its comment: `#` at its start or after a space or tab
    (the CLI's rule), never inside quotes."""
    inside = False
    for i, c in enumerate(line):
        if c == '"':
            inside = not inside
        elif c == "#" and not inside and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def tokens(line):
    """A record's tokens, "quoted names" kept whole (quotes dropped)."""
    out, i = [], 0
    while i < len(line):
        if line[i] in BLANKS:
            i += 1
            continue
        if line[i] == '"':
            end = line.find('"', i + 1)
            end = len(line) if end < 0 else end
            out.append(line[i + 1:end])
            i = end + 1
            continue
        end = i
        while end < len(line) and line[end] not in BLANKS:
            end += 1
        out.append(line[i:end])
        i = end
    return out


def num(text):
    """A number the CLI printed. Retail models carry NaNs (J_bsh1's vertex
    normals, ChmLFP1's occlusion planes), which a C runtime spells `nan` or
    `-nan` and MSVC's `-nan(ind)`, `1.#QNAN` or `-1.#IND`; infinities `inf` or
    `1.#INF`."""
    try:
        return float(text)
    except ValueError:
        t = text.lower()
        if "nan" in t or "#ind" in t:
            return math.nan
        if "inf" in t:
            return -math.inf if t.startswith("-") else math.inf
        raise


# --- the axes ---------------------------------------------------------------

def axis_basis(forward):
    """Mission -> Blender as a column matrix B (b = B m); a proper rotation."""
    if forward == "-Y":
        return Matrix(((0, 1, 0), (-1, 0, 0), (0, 0, 1)))
    return Matrix.Identity(3)


def axis_map(forward):
    """Blender -> mission for a vector, B^T as a swizzle."""
    if forward == "-Y":
        return lambda v: (-v.y, v.x, v.z)
    return lambda v: (v.x, v.y, v.z)


def blender_axes(forward):
    """Mission -> Blender for a vector, B as a swizzle (a not-finite component
    stays in its own place)."""
    if forward == "-Y":
        return lambda m: Vector((m[1], -m[0], m[2]))
    return lambda m: Vector((m[0], m[1], m[2]))


class ModelSpace:
    """A model root's frame, the one every export reads the scene in: an
    object's matrix relative to the root, so a model placed, parented or
    mounted anywhere in the scene exports the same model (a root at the origin
    reads Blender's world matrices untouched), and Blender's axes as the
    mission axes the text carries. Made after the view layer is updated, so
    the root's matrix is current."""

    def __init__(self, model, forward):
        self.basis = axis_basis(forward)
        self.to_mission = axis_map(forward)
        root = model.matrix_world
        self.into_root = None if root == Matrix.Identity(4) else root.inverted_safe()

    def world(self, ob):
        return ob.matrix_world if self.into_root is None else self.into_root @ ob.matrix_world

    def mission(self, v):
        return self.to_mission(v)

    def mission_rotation(self, rotation):
        """A Blender rotation (3x3, in the model root's frame) in mission axes:
        the basis change the vector map is, applied to a rotation."""
        return self.basis.transposed() @ rotation @ self.basis


# --- the CLI ----------------------------------------------------------------

def bundled_cli_path():
    return os.path.join(os.path.dirname(__file__), "bin", "opennova-3di.exe")


def cli_path(context=None):
    scene = (context or bpy.context).scene
    custom = scene.o3d.cli_path if scene is not None else ""
    if custom:
        return bpy.path.abspath(custom)
    return bundled_cli_path()


def scratch():
    """A directory of its own for the text the CLI reads or writes; it goes
    away with the block, whatever happens, so nothing lands beside the
    author's files."""
    return tempfile.TemporaryDirectory(prefix="opennova3di_")


def run_cli(context, args, failure, timeout=None, hide=()):
    """Run `opennova-3di <args>` and return what it printed. Raises `failure`
    when the executable is missing, cannot start or fails, with what it said
    (its errors first) or its exit code; `hide` pairs a path in that message
    (a scratch file) with the words shown instead. The CLI prints paths, which
    may hold any character: its output reads as UTF-8 whatever this Python's
    locale is."""
    cli = cli_path(context)
    if not os.path.isfile(cli):
        raise failure(f"opennova-3di not found at {cli}")
    try:
        result = subprocess.run([cli, *args], capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=timeout)
    except (OSError, subprocess.SubprocessError) as e:
        raise failure(str(e)) from e
    if result.returncode != 0:
        said = result.stderr + result.stdout
        for path, shown in hide:
            said = said.replace(path, shown)
        raise failure(said.strip()[:2000] or f"opennova-3di {args[0]} failed (exit {result.returncode})")
    return result


def cli_notes(result, marker):
    """The CLI's notes after `marker` (`note: `, `scene drops `), one per
    line of its error stream."""
    return [line.split(marker, 1)[1] for line in result.stderr.splitlines() if marker in line]
