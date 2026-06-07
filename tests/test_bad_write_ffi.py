"""FFI round-trip: parse BINOC.bad -> neutral -> write_bad -> parse, structural equality.

Validates the C bad_write serializer + the ctypes marshalling in bad_build.
"""
from __future__ import annotations

import os

import pytest

from pyopennova import bad_build, bad_ffi

try:
    bad_ffi._bind()
except Exception as exc:  # pragma: no cover - skip when the native lib is absent
    pytest.skip(f"native opennova library unavailable: {exc}", allow_module_level=True)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINOC = os.path.join(REPO, "fixtures", "bad", "BINOC.bad")


def _approx(a, b, eps=1e-6):
    return all(abs(float(x) - float(y)) <= eps for x, y in zip(a, b))


def test_binoc_neutral_roundtrip(tmp_path):
    orig = bad_ffi.parse_bad(BINOC)
    try:
        clip = bad_build.neutral_from_badfile(orig)
        out = str(tmp_path / "binoc_neutral.bad")
        bad_build.write_bad(out, clip)
        rt = bad_ffi.parse_bad(out)
        try:
            assert rt.version == orig.version
            assert rt.fps == orig.fps
            assert rt.frame_count == orig.frame_count
            assert rt.flags == orig.flags
            assert rt.num_bones == orig.num_bones
            assert rt.num_channels == orig.num_channels
            assert rt.num_events == orig.num_events
            assert rt.num_translations == orig.num_translations

            for i in range(int(orig.num_bones)):
                assert rt.bones[i].name == orig.bones[i].name
                assert rt.bones[i].parent_index == orig.bones[i].parent_index
                assert _approx([rt.bones[i].length], [orig.bones[i].length])
                assert _approx(list(rt.bones[i].position), list(orig.bones[i].position))
                assert _approx(list(rt.bones[i].rotation), list(orig.bones[i].rotation))

            for i in range(int(orig.num_channels)):
                assert rt.channels[i].frame_count == orig.channels[i].frame_count
                for j in range(int(orig.channels[i].frame_count)):
                    assert rt.channels[i].frame_lengths[j] == orig.channels[i].frame_lengths[j]
                    a = rt.channels[i].rotations[j]
                    b = orig.channels[i].rotations[j]
                    assert _approx((a.x, a.y, a.z, a.w), (b.x, b.y, b.z, b.w))

            for i in range(int(orig.num_events)):
                a = rt.events[i]
                b = orig.events[i]
                assert _approx(list(a.velocity), list(b.velocity))
                assert _approx([a.bottom, a.top], [b.bottom, b.top])
                assert a.trigger == b.trigger
        finally:
            bad_ffi.free_bad(rt)
    finally:
        bad_ffi.free_bad(orig)
