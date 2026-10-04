#!/usr/bin/env python3
"""Regression fixtures for the package directory hierarchy."""

import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "normalize-gp4.py"
SPEC = importlib.util.spec_from_file_location("normalize_gp4", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NormalizeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "pkg.gp4"

    def fixture(self, targets):
        project = ET.Element("psproject", {"fmt": "gp4", "version": "1000"})
        volume = ET.SubElement(project, "volume")
        ET.SubElement(volume, "volume_type").text = "pkg_ps4_app"
        ET.SubElement(volume, "volume_ts").text = "2026-10-04 17:00:00"
        ET.SubElement(volume, "package", {"content_id": "IV0000-BREW00001_00-ORBISSTORE000001", "passcode": "0" * 32})
        files = ET.SubElement(project, "files", {"img_no": "0"})
        for target in targets:
            ET.SubElement(files, "file", {"targ_path": target, "orig_path": "source/" + target, "custom_attribute": "preserve"})
        # Simulate a bundled generator omitting a nested music directory.
        rootdir = ET.SubElement(project, "rootdir")
        ET.SubElement(rootdir, "dir", {"targ_name": "assets"})
        ET.ElementTree(project).write(self.path, encoding="utf-8", xml_declaration=True)
        return project

    def directory_paths(self):
        found = []

        def visit(parent, prefix=""):
            for child in parent.findall("dir"):
                path = prefix + child.get("targ_name")
                found.append(path)
                visit(child, path + "/")

        visit(ET.parse(self.path).getroot().find("rootdir"))
        return found

    def test_nested_music_and_root_files(self):
        self.fixture(["eboot.bin", "catalog.json", "assets/music/fight.wav", "assets/music/acendaofarol.wav", "assets/music/extra/track.wav", "sce_sys/about/right.sprx"])
        self.assertEqual(MODULE.normalize(self.path), 5)
        self.assertEqual(self.directory_paths(), ["assets", "assets/music", "assets/music/extra", "sce_sys", "sce_sys/about"])

    def test_repeated_components_and_sibling_names(self):
        self.fixture(["a/a/a/one.bin", "a/b/a/two.bin", "a/a/c/three.bin", "other/a/four.bin"])
        MODULE.normalize(self.path)
        self.assertEqual(self.directory_paths(), ["a", "a/a", "a/a/a", "a/a/c", "a/b", "a/b/a", "other", "other/a"])

    def test_preserves_metadata_files_mode_and_is_idempotent(self):
        original = self.fixture(["eboot.bin", "assets/music/fight.wav", "sce_sys/param.sfo"])
        before = copy.deepcopy(original)
        before.remove(before.find("rootdir"))
        self.path.chmod(0o640)
        MODULE.normalize(self.path)
        after = ET.parse(self.path).getroot()
        after.remove(after.find("rootdir"))
        self.assertEqual(ET.tostring(before), ET.tostring(after))
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o640)
        first = self.path.read_bytes()
        MODULE.normalize(self.path)
        self.assertEqual(first, self.path.read_bytes())

    def test_root_files_have_empty_rootdir(self):
        self.fixture(["eboot.bin", "catalog.json"])
        self.assertEqual(MODULE.normalize(self.path), 0)
        self.assertEqual(self.directory_paths(), [])

    def test_rejects_unsafe_targets_without_modifying_input(self):
        for target in ["../music.wav", "assets/../music.wav", "/music.wav", "assets\\music.wav", "C:/music.wav", "C:music.wav", "assets//music.wav", "./music.wav", "assets/", "", "assets/line\nfeed.wav"]:
            with self.subTest(target=target):
                self.fixture([target])
                original = self.path.read_bytes()
                with self.assertRaises(ValueError):
                    MODULE.normalize(self.path)
                self.assertEqual(original, self.path.read_bytes())

    def test_rejects_file_directory_conflict_and_duplicate_targets(self):
        for targets in [["assets", "assets/music/fight.wav"], ["eboot.bin", "eboot.bin"]]:
            with self.subTest(targets=targets):
                self.fixture(targets)
                original = self.path.read_bytes()
                with self.assertRaises(ValueError):
                    MODULE.normalize(self.path)
                self.assertEqual(original, self.path.read_bytes())


if __name__ == "__main__":
    unittest.main()
