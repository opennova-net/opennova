"""A shared build cannot silently become another run's image or lose its files."""

import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest

import service_bundle as bundle


class ServiceBundle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        for name in bundle.BINARIES:
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'ELF fixture ' + name.encode())
        for name in bundle.SOURCE_DIRS:
            path = self.root / name / 'fixture.txt'
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name, encoding='utf-8')
        for name in bundle.SOURCE_FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name, encoding='utf-8')
        self.source = bundle.identity('a' * 40, '123', '2')
        self.one = bundle.one_stamp('0.0.23.123', 'ON', 'bbbbbbb', '1234567890')
        self.archive = self.root / 'service.tar.gz'
        self.destination = self.root / 'unpacked'
        bundle.pack(self.build, self.archive, self.source, root=self.root, service_tests=True, one=self.one)

    def rewrite(self, transform):
        with tarfile.open(self.archive, 'r:gz') as archive:
            rows = [(member, archive.extractfile(member).read()) for member in archive.getmembers()]
        rows = transform(rows)
        with tarfile.open(self.archive, 'w:gz') as archive:
            for member, data in rows:
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))

    def test_round_trip_preserves_complete_runtime_and_executable_modes(self):
        bundle.unpack(self.archive, self.destination, self.source, require_service_tests=True)
        with tarfile.open(self.archive, 'r:gz') as archive:
            for name in bundle.BINARIES:
                self.assertEqual(archive.getmember('src/build/' + name).mode, 0o755)
        for name in bundle.BINARIES:
            path = self.destination / 'src/build' / name
            self.assertEqual(path.read_bytes(), (self.build / name).read_bytes())
            # Windows has no POSIX executable mode; the tar metadata above
            # remains portable, and Linux consumers must restore it on disk.
            if os.name != 'nt':
                self.assertTrue(path.stat().st_mode & 0o111)
        manifest = json.loads((self.destination / 'manifest.json').read_text())
        self.assertTrue(manifest['service_tests'])
        self.assertEqual(manifest['run_attempt'], '2')
        self.assertEqual(manifest['one'], self.one)

    def test_wrong_source_run_attempt_or_platform_is_rejected_before_extracting(self):
        for key, value in [('sha', 'b' * 40), ('run_id', '124'), ('run_attempt', '3'),
                           ('platform', 'ubuntu-22.04-x86_64')]:
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'does not match'):
                bundle.unpack(self.archive, self.destination, {**self.source, key: value})
            self.assertFalse(self.destination.exists())

    def test_stack_only_bundle_cannot_publish_images(self):
        bundle.pack(self.build, self.archive, self.source, root=self.root, service_tests=False, one=self.one)
        with self.assertRaisesRegex(ValueError, 'service test gate'):
            bundle.unpack(self.archive, self.destination, self.source, require_service_tests=True)
        bundle.unpack(self.archive, self.destination, self.source)

    def test_changed_binary_is_rejected(self):
        self.rewrite(lambda rows: [(member, b'changed' if index == 0 else data)
                                  for index, (member, data) in enumerate(rows)])
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            bundle.unpack(self.archive, self.destination, self.source)
        self.assertFalse(self.destination.exists())

    def test_missing_or_duplicate_member_is_rejected(self):
        original = self.archive.read_bytes()
        for change in (lambda rows: rows[1:], lambda rows: rows + [rows[0]]):
            self.archive.write_bytes(original)
            self.rewrite(change)
            with self.assertRaises(ValueError):
                bundle.unpack(self.archive, self.destination, self.source)
            self.assertFalse(self.destination.exists())

    def test_links_are_rejected(self):
        def link(rows):
            rows[0][0].type = tarfile.SYMTYPE
            rows[0][0].linkname = '/tmp/other'
            return rows
        self.rewrite(link)
        with self.assertRaisesRegex(ValueError, 'regular files only'):
            bundle.unpack(self.archive, self.destination, self.source)

    def test_missing_build_output_fails_pack(self):
        (self.build / bundle.BINARIES[0]).unlink()
        with self.assertRaisesRegex(ValueError, 'missing or nonregular'):
            bundle.pack(self.build, self.archive, self.source, root=self.root, one=self.one)

    def test_invalid_or_missing_one_stamp_cannot_be_packaged(self):
        for stamp in (None, {**self.one, 'commit': 'not-a-sha'}, {**self.one, 'staging': ''}):
            with self.subTest(stamp=stamp), self.assertRaises(ValueError):
                bundle.pack(self.build, self.archive, self.source, root=self.root, one=stamp)


if __name__ == '__main__':
    unittest.main()
