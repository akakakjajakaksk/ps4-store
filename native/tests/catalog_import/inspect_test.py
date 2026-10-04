#!/usr/bin/env python3
"""Regression checks for local PKG metadata inspection; no network access."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "inspect-pkg.py"
SPEC = importlib.util.spec_from_file_location("peppy_inspect_pkg", SCRIPT)
INSPECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECTOR)


def fixture(content_id: str = "EP9000-CUSA00001_00-ABCDEFGHIJKLMNOP", size: int = 4096,
            content_type: int = 0x1A, content_flags: int = 0x0A000000,
            declared_size: int | None = None) -> bytes:
    data = bytearray(size)
    data[:4] = b"\x7fCNT"
    encoded = content_id.encode("ascii")
    data[0x40:0x40 + len(encoded)] = encoded
    data[0x74:0x78] = content_type.to_bytes(4, "big")
    data[0x78:0x7C] = content_flags.to_bytes(4, "big")
    data[0x430:0x438] = (size if declared_size is None else declared_size).to_bytes(8, "big")
    return bytes(data)


class InspectTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="peppy-catalog-inspection-")
        self.root = Path(self.directory.name)
        self.pkg = self.root / "Meu jogo (edição boa).pkg"
        self.data = fixture()
        self.pkg.write_bytes(self.data)

    def tearDown(self):
        self.directory.cleanup()

    def test_valid_header_exact_size_and_streamed_digest(self):
        report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertTrue(report["importable"])
        self.assertEqual(report["size_bytes"], len(self.data))
        self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(report["title_id"], "CUSA00001")
        self.assertEqual(report["platform"], "PS4")
        self.assertEqual(report["original_filename"], self.pkg.name)
        self.assertRegex(report["filename"], r"^[A-Za-z0-9_.-]+\.pkg$")
        self.assertLess(len(report["filename"]), 96)
        self.assertEqual(report["header_package_size"], len(self.data))
        self.assertEqual(report["content_type"], "0x0000001A")
        self.assertEqual(report["content_flags"], "0x0A000000")
        self.assertEqual(report["package_kind"], "base")
        self.assertFalse(report["full_package_validation"])
        self.assertFalse(report["network_verified"])
        self.assertNotIn("firmware", report)

    def test_agony_like_base_header_and_supported_non_game_flags(self):
        # Preserve the observed Agony identity/type/flags without allocating its
        # 10 GB payload. Fixture declared size must match the local fixture.
        for flags in (0x0A000000, 0x0E000000):
            with self.subTest(flags=hex(flags)):
                self.data = fixture("UP2047-CUSA10216_00-AGONY666AMERICAS", content_flags=flags)
                self.pkg.write_bytes(self.data)
                report = INSPECTOR.inspect_pkg(self.pkg)
                self.assertTrue(report["importable"])
                self.assertEqual(report["package_kind"], "base")
                self.assertEqual(report["title_id"], "CUSA10216")
                self.assertEqual(report["header_package_size"], len(self.data))
                self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())
                self.assertFalse(report["full_package_validation"])

    def test_yet_another_zombie_patch_flags_override_base_like_filename(self):
        self.data = fixture("UP2387-CUSA18354_00-YAZDHD0000000000", content_flags=0x62300000)
        self.pkg = self.root / "Yet.Another.Zombie.Defense.HD.v1.00bp.pkg"
        self.pkg.write_bytes(self.data)
        report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertFalse(report["importable"])
        self.assertEqual(report["platform"], "PS4")
        self.assertEqual(report["package_kind"], "patch")
        self.assertEqual(report["content_flags"], "0x62300000")
        self.assertIn("only known base", report["reason"])
        self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertIn("filename", report)
        self.assertFalse(report["full_package_validation"])

    def test_known_patch_flags_delta_type_and_addons_are_refused_with_checked_hash(self):
        cases = [(0x1A, 0x0A000000 | flag, "patch")
                 for flag in (0x00100000, 0x00200000, 0x40000000, 0x41000000, 0x60000000)]
        cases += [(0x1E, 0, "patch"), (0x1B, 0x0A000000, "addon"), (0x1C, 0, "addon")]
        for content_type, flags, kind in cases:
            with self.subTest(content_type=hex(content_type), flags=hex(flags)):
                self.data = fixture(content_type=content_type, content_flags=flags)
                self.pkg.write_bytes(self.data)
                report = INSPECTOR.inspect_pkg(self.pkg)
                self.assertFalse(report["importable"])
                self.assertEqual(report["package_kind"], kind)
                self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())
                self.assertIn("filename", report)
                self.assertIn("complete_ps4_base_package", report["catalog_review_required"])

    def test_unknown_types_and_flag_patterns_do_not_become_base(self):
        cases = [(0, 0x0A000000), (0x30, 0x0A000000), (0xFFFFFFFF, 0x0A000000)]
        cases += [(0x1A, flags) for flags in (0, 0x02000000, 0x08000000, 0x0A000001,
                                            0x0A400000, 0x0A800000, 0x8A000000)]
        for content_type, flags in cases:
            with self.subTest(content_type=hex(content_type), flags=hex(flags)):
                self.data = fixture(content_type=content_type, content_flags=flags)
                self.pkg.write_bytes(self.data)
                report = INSPECTOR.inspect_pkg(self.pkg)
                self.assertFalse(report["importable"])
                self.assertEqual(report["package_kind"], "unknown")
                self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())
                self.assertIn("filename", report)

    def test_invalid_declared_size_fails_before_hash_and_discards_proposed_filename(self):
        for declared in (0, INSPECTOR.HEADER_BYTES - 1, len(self.data) - 1,
                         len(self.data) + 1, 10518134784, (1 << 64) - 1):
            with self.subTest(declared_size=declared):
                self.pkg.write_bytes(fixture(declared_size=declared))
                with patch.object(INSPECTOR.hashlib, "sha256") as digest:
                    report = INSPECTOR.inspect_pkg(self.pkg)
                digest.assert_not_called()
                self.assertFalse(report["importable"])
                self.assertEqual(report["header_package_size"], declared)
                self.assertIn("package size", report["reason"])
                self.assertNotIn("sha256", report)
                self.assertNotIn("filename", report)

    def test_truncated_header_does_not_read_declared_size(self):
        self.pkg.write_bytes(self.data[:INSPECTOR.HEADER_BYTES - 1])
        report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertFalse(report["importable"])
        self.assertIn("truncated", report["reason"])
        self.assertNotIn("header_package_size", report)
        self.assertNotIn("sha256", report)

    def test_user_url_is_syntax_only_not_network_verified(self):
        url = "https://downloads.example.org/game.pkg?token=a%2Fb"
        report = INSPECTOR.inspect_pkg(self.pkg, url)
        self.assertTrue(report["importable"])
        self.assertEqual(report["url"], url)
        self.assertEqual(report["url_source"], "user_supplied")
        self.assertFalse(report["network_verified"])
        self.assertIn("verified_download_url", report["catalog_review_required"])
        self.assertNotIn("compatibility", report)

    def test_ps5_is_identified_but_refused(self):
        self.pkg.write_bytes(fixture("EP9000-PPSA00001_00-ABCDEFGHIJKLMNOP"))
        report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertFalse(report["importable"])
        self.assertEqual(report["platform"], "PS5")
        self.assertEqual(report["title_id"], "PPSA00001")
        self.assertIn("PS5", report["reason"])
        self.assertEqual(report["package_kind"], "unknown")
        self.assertNotIn("header_package_size", report)
        self.assertEqual(report["sha256"], hashlib.sha256(self.pkg.read_bytes()).hexdigest())
        self.assertIn("filename", report)

    def test_unknown_prefixes_do_not_claim_ps4(self):
        for title_id in ("ZZZZ12345", "BREW00001", "APOL00004", "SLES52325", "SLUS20909"):
            with self.subTest(title_id=title_id):
                self.pkg.write_bytes(fixture(f"IV0000-{title_id}_00-ABCDEFGHIJKLMNOP"))
                report = INSPECTOR.inspect_pkg(self.pkg)
                self.assertFalse(report["importable"])
                self.assertEqual(report["platform"], "unknown")
                self.assertEqual(report["package_kind"], "unknown")
                self.assertNotIn("header_package_size", report)
                self.assertEqual(report["title_id"], title_id)
                self.assertEqual(report["platform_basis"], "unrecognized_content_id_title_prefix")
                self.assertIn("platform_evidence", report["catalog_review_required"])
                self.assertIn("evidence", report["reason"])
                self.assertEqual(report["sha256"], hashlib.sha256(self.pkg.read_bytes()).hexdigest())
                self.assertIn("filename", report)

    def test_truncated_wrong_magic_and_bad_identifiers(self):
        for data in (b"\x7fCNT", b"PK\x03\x04" + bytes(4092), fixture("IV0000-APOL00004_00-APOLLO0000000PS!"),
                     fixture("IV0000-APOL00004_00-APOLLO0000000PS4").replace(b"APOL00004", b"APOL1234_")):
            with self.subTest(data=data[:16]):
                self.pkg.write_bytes(data)
                report = INSPECTOR.inspect_pkg(self.pkg)
                self.assertFalse(report["importable"])
                self.assertIn("reason", report)
                self.assertNotIn("sha256", report)

    def test_symlink_directory_and_fifo_never_read(self):
        link = self.root / "link.pkg"
        link.symlink_to(self.pkg)
        fifo = self.root / "pipe.pkg"
        os.mkfifo(fifo)
        for path in (link, self.root, fifo):
            with self.subTest(path=path):
                report = INSPECTOR.inspect_pkg(path)
                self.assertFalse(report["importable"])
                self.assertNotIn("sha256", report)

    def test_read_buffer_is_bounded(self):
        self.data = fixture(size=2 * INSPECTOR.READ_BYTES + 71)
        self.pkg.write_bytes(self.data)
        real_read = os.read
        requested = []

        def recording_read(descriptor, amount):
            requested.append(amount)
            return real_read(descriptor, amount)

        with patch.object(INSPECTOR.os, "read", side_effect=recording_read):
            report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertTrue(report["importable"])
        self.assertTrue(all(amount <= INSPECTOR.READ_BYTES for amount in requested))
        self.assertEqual(report["sha256"], hashlib.sha256(self.data).hexdigest())

    def test_file_mutated_during_hash_discards_digest(self):
        real_read = os.read
        changed = False

        def mutating_read(descriptor, amount):
            nonlocal changed
            part = real_read(descriptor, amount)
            if not changed and amount == INSPECTOR.READ_BYTES:
                changed = True
                with self.pkg.open("r+b") as stream:
                    stream.seek(0x500)
                    stream.write(b"changed")
                    stream.flush()
                info = os.stat(self.pkg)
                os.utime(self.pkg, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000))
            return part

        with patch.object(INSPECTOR.os, "read", side_effect=mutating_read):
            report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertFalse(report["importable"])
        self.assertIn("changed", report["reason"])
        self.assertNotIn("sha256", report)

    def test_path_replaced_during_hash_discards_digest(self):
        real_read = os.read
        changed = False

        def replacing_read(descriptor, amount):
            nonlocal changed
            part = real_read(descriptor, amount)
            if not changed and amount == INSPECTOR.READ_BYTES:
                changed = True
                replacement = self.root / "replacement.pkg"
                replacement.write_bytes(self.data)
                os.replace(replacement, self.pkg)
            return part

        with patch.object(INSPECTOR.os, "read", side_effect=replacing_read):
            report = INSPECTOR.inspect_pkg(self.pkg)
        self.assertFalse(report["importable"])
        self.assertNotIn("sha256", report)

    def test_invalid_urls_refused_without_reading_input(self):
        for url in ("http://example.org/a.pkg", "https://u:p@example.org/a.pkg", "https://example.org:8080/a.pkg",
                    "https://example.org/a.pkg#fragment", "https://example.org/a.pkg#", "https://example.org/", "https://example.org/a b.pkg",
                    "https://example.org/a\\b.pkg", "https://example.org/a%ZZ.pkg", "https://bad..org/a.pkg"):
            with self.subTest(url=url):
                report = INSPECTOR.inspect_pkg(self.pkg, url)
                self.assertFalse(report["importable"])
                self.assertNotIn("size_bytes", report)

    def test_cli_json_and_input_cannot_be_overwritten(self):
        run = subprocess.run([sys.executable, str(SCRIPT), str(self.pkg)], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(json.loads(run.stdout)["importable"])
        output = self.root / "report.json"
        run = subprocess.run([sys.executable, str(SCRIPT), str(self.pkg), "--output", str(output)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(json.loads(output.read_text())["importable"])
        run = subprocess.run([sys.executable, str(SCRIPT), str(self.pkg), "--output", str(self.pkg)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 2)
        self.assertEqual(self.pkg.read_bytes(), self.data)

    def test_cli_output_cannot_overwrite_input_aliases(self):
        hardlink = self.root / "alias.json"
        os.link(self.pkg, hardlink)
        symlink = self.root / "symlink.json"
        symlink.symlink_to(self.pkg)
        for output in (hardlink, symlink):
            with self.subTest(output=output):
                run = subprocess.run([sys.executable, str(SCRIPT), str(self.pkg), "--output", str(output)],
                                     capture_output=True, text=True, check=False)
                self.assertEqual(run.returncode, 2)
                self.assertEqual(self.pkg.read_bytes(), self.data)


if __name__ == "__main__":
    unittest.main()
