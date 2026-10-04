#!/usr/bin/env python3
"""Catalog provenance and package metadata regression checks, without network."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import quote


NATIVE = Path(__file__).resolve().parents[2]
SCRIPT = NATIVE / "scripts" / "generate-catalog.py"
SPEC = importlib.util.spec_from_file_location("peppy_generate_catalog", SCRIPT)
CATALOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CATALOG)
CID = "UP2047-CUSA10216_00-AGONY666AMERICAS"
REMOTE_NAME = f"[DLPSGAME.COM]-{CID}-A0100-V0100.pkg"


def external() -> dict:
    return {
        "id": "CUSA10216",
        "name": "Agony",
        "caption": "Jogo base PS4",
        "description": "Pacote base PS4; metadados conferidos na fonte indicada.",
        "category": "Jogos",
        "source_kind": "external_mediafire",
        "source": "mtps4store380.blogspot.com",
        "source_label": "MTPS4 / MEDIAFIRE",
        "source_url": "https://mtps4store380.blogspot.com/?m=1",
        "platform": "PS4",
        "package_kind": "base",
        "content_id": CID,
        "header_verification": {
            "magic": "7f434e54",
            "content_type": 26,
            "content_flags": 167772160,
            "package_size": 10518134784,
            "content_id": CID,
        },
        "version": "v1.00",
        "developer": "Madmind Studio",
        "firmware": "Compatibilidade não confirmada no console",
        "url": f"https://www.mediafire.com/file/Abc123xyz456/{quote(REMOTE_NAME, safe='')}/file",
        "filename": f"{CID}-A0100.pkg",
        "size_bytes": 10518134784,
        "sha256": None,
        "requires_data": "",
        "art": 1,
    }


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="peppy-catalog-tests-")
        self.path = Path(self.directory.name) / "catalog.json"
        raw = json.loads((NATIVE / "catalog.json").read_text(encoding="utf-8"))
        self.official = [copy.deepcopy(entry) for entry in raw["entries"]
                         if entry.get("source_kind", "official") == "official"]

    def tearDown(self):
        self.directory.cleanup()

    def validate(self, entries):
        self.path.write_text(json.dumps({"schema_version": 1, "entries": entries}), encoding="utf-8")
        return CATALOG.read_catalog(self.path)

    def assert_refused(self, entry, text=None):
        with self.assertRaises(ValueError) as caught:
            self.validate([entry])
        if text is not None:
            self.assertIn(text, str(caught.exception))

    def test_existing_official_catalog_remains_strict(self):
        self.assertEqual(len(self.official), 15)
        validated = self.validate(self.official)
        self.assertEqual(len(validated), 15)
        self.assertTrue(all(entry["source_kind"] == "official" for entry in validated))
        self.assertTrue(all(entry["source_badge"] == "PKG / FONTE OFICIAL" for entry in validated))
        for changes in ({"source": "somebody/unverified"}, {"repository": "somebody/different"},
                        {"url": "https://github.com/bucanero/apollo-ps4/releases/latest/download/app.pkg"},
                        {"version": "latest"}, {"release_url": "https://example.org/release"},
                        {"url": "https://www.mediafire.com/file/abc/game.pkg/file"}):
            with self.subTest(changes=changes):
                entry = copy.deepcopy(self.official[0])
                entry.update(changes)
                self.assert_refused(entry)

    def test_external_metadata_and_badge_are_preserved(self):
        entry = external()
        entry["source_badge"] = "PKG / FONTE OFICIAL"  # A forged input badge is ignored.
        validated = self.validate(self.official + [entry])
        result = validated[-1]
        self.assertEqual(result["source_kind"], "external_mediafire")
        self.assertEqual(result["source_badge"], "PKG / MTPS4 / MEDIAFIRE")
        self.assertEqual(result["release_url"], entry["source_url"])
        self.assertEqual(result["size_bytes"], 10518134784)
        self.assertEqual(result["sha256"], "")
        self.assertEqual(CATALOG.CATEGORIES[result["category"]], 3)
        self.assertEqual(CATALOG.CATEGORIES["Jogos homebrew"], 3)
        header = CATALOG.generate_header(validated)
        self.assertIn("const char* sourceBadge;", header)
        self.assertIn('"PKG / MTPS4 / MEDIAFIRE",', header)
        self.assertIn("10518134784ULL", header)

    def test_ps5_updates_dlc_and_unknown_sources_are_refused(self):
        for changes in ({"platform": "PS5"}, {"platform": "unknown"}, {"package_kind": "update"},
                        {"package_kind": "dlc"}, {"package_kind": "unknown"}, {"source_kind": "external"},
                        {"source_kind": "user"}, {"source_kind": None},
                        {"content_id": "EP9000-PPSA00001_00-ABCDEFGHIJKLMNOP"},
                        {"content_id": "UP2047-ZZZZ12345_00-AGONY666AMERICAS"}):
            with self.subTest(changes=changes):
                entry = external(); entry.update(changes)
                self.assert_refused(entry)

    def test_non_pkg_pages_archives_and_other_hosts_are_refused(self):
        urls = (
            "https://www.mediafire.com/file/abc/game.html/file",
            "https://www.mediafire.com/file/abc/game.zip/file",
            "https://www.mediafire.com/file/abc/game.pkg",
            "https://www.mediafire.com/file/abc/game.pkg/file/extra",
            "https://www.mediafire.com/file/abc_key/game.pkg/file",
            "https://mediafire.com/file/abc/game.pkg/file",
            "https://www.mediafire.com.evil.org/file/abc/game.pkg/file",
            "https://dlpsgame.com/game.pkg",
            "https://www.mediafire.com/folder/abc/game.pkg",
        )
        for url in urls:
            with self.subTest(url=url):
                entry = external(); entry["url"] = url
                self.assert_refused(entry)

    def test_url_credentials_ports_queries_fragments_and_encoded_paths_are_refused(self):
        good = external()["url"]
        bad = (good.replace("https://", "http://"), good.replace("www.mediafire.com", "u:p@www.mediafire.com"),
               good.replace("www.mediafire.com", "www.mediafire.com:443"), good + "?token=x", good + "#", good + "#x",
               good.replace(".pkg", ".pkg%2Fother"), good.replace(".pkg", "%5Cgame.pkg"),
               good.replace(".pkg", "%0D.pkg"), good.replace(".pkg", "%ZZ.pkg"))
        for url in bad:
            with self.subTest(url=url):
                entry = external(); entry["url"] = url
                self.assert_refused(entry)

    def test_content_id_and_local_or_remote_filenames_must_agree(self):
        for changes in ({"filename": "Agony.pkg"}, {"filename": CID.replace("CUSA10216", "CUSA10217") + ".pkg"},
                        {"content_id": CID[:-1] + "!"},
                        {"url": external()["url"].replace("CUSA10216", "CUSA10217")},
                        {"url": "https://www.mediafire.com/file/abc/Agony-CUSA10217.pkg/file"}):
            with self.subTest(changes=changes):
                entry = external(); entry.update(changes)
                self.assert_refused(entry)

    def test_reviewed_ps2_wrappers_and_generic_names_are_allowed(self):
        cases = (
            ("UP9000-SLES51507_00-SLES515070000001", "UP9000-SLES51507_00-SLES515070000001-A0100-V0100.pkg"),
            ("HP9000-SLUS21376_00-SCPS560030000001", "[Downloadgameps3.com]-BLACK_SLUS21376_00-SCPS560030000001-A0100-V0100.pkg"),
            ("UP9000-SLUS20249_00-SLUS202490000001", "[Downloadgameps3.com]PS2TOPS4-12.pkg"),
        )
        for content_id, remote_name in cases:
            with self.subTest(content_id=content_id):
                entry = external()
                entry["content_id"] = content_id
                entry["id"] = content_id[7:16]
                entry["filename"] = content_id + "-A0100.pkg"
                entry["header_verification"]["content_id"] = content_id
                entry["url"] = f"https://www.mediafire.com/file/abc123/{quote(remote_name, safe='')}/file"
                result = self.validate([entry])[0]
                self.assertEqual(result["content_id"], content_id)
                self.assertIn(json.dumps(content_id) + ",", CATALOG.generate_header([result]))
        entry = external(); entry["url"] = "https://www.mediafire.com/file/abc123/Agony.pkg/file"
        self.assertEqual(self.validate([entry])[0]["content_id"], CID)
        entry["header_verification"]["content_flags"] = 0x0E000000
        self.assertEqual(self.validate([entry])[0]["content_id"], CID)

    def test_missing_bad_or_patch_header_proofs_are_refused(self):
        entry = external(); del entry["header_verification"]
        self.assert_refused(entry, "header_verification")
        for proof in (None, [], {}, {"magic": "504b0304"}, {"content_type": 0x1B},
                      {"content_type": "26"}, {"content_flags": 0x62300000},
                      {"content_flags": True}, {"package_size": 10518134785},
                      {"package_size": "10518134784"}, {"content_id": CID.replace("10216", "10217")}):
            with self.subTest(proof=proof):
                entry = external()
                if isinstance(proof, dict) and proof:
                    entry["header_verification"].update(proof)
                else:
                    entry["header_verification"] = proof
                self.assert_refused(entry)
        for prefix in ("PPSA", "ZZZZ", "BREW", "SCPS"):
            entry = external(); content_id = CID.replace("CUSA", prefix)
            entry["content_id"] = content_id
            entry["filename"] = content_id + ".pkg"
            entry["header_verification"]["content_id"] = content_id
            entry["url"] = entry["url"].replace("CUSA", prefix)
            self.assert_refused(entry)

    def test_provenance_must_be_honest_and_single_line(self):
        for changes in ({"source": "github.com"}, {"source_url": "http://mtps4store380.blogspot.com/"},
                        {"source_url": "https://u:p@mtps4store380.blogspot.com/"},
                        {"source_url": "https://mtps4store380.blogspot.com:443/"},
                        {"source_label": "FONTE OFICIAL"}, {"source_label": "Official downloads"},
                        {"source_label": "MTPS4\nMEDIAFIRE"}, {"source_label": "x" * 31}, {"source_label": ""},
                        {"repository": "bucanero/apollo-ps4"}, {"release_url": "https://example.org/claim"}):
            with self.subTest(changes=changes):
                entry = external(); entry.update(changes)
                self.assert_refused(entry)

    def test_exact_sizes_hashes_and_runtime_filename_bounds(self):
        for size in (0x438, 5 * 1024 ** 3, 256 * 1024 ** 3):
            entry = external(); entry["size_bytes"] = size
            entry["header_verification"]["package_size"] = size
            self.assertEqual(self.validate([entry])[0]["size_bytes"], size)
        for size in (None, True, 0, 0x437, 10518134784.0, 256 * 1024 ** 3 + 1, 2 ** 64 - 1):
            with self.subTest(size=size):
                entry = external(); entry["size_bytes"] = size
                self.assert_refused(entry)
        for digest in ("a" * 64, "ABCDEF0123456789" * 4):
            entry = external(); entry["sha256"] = digest
            self.assertEqual(self.validate([entry])[0]["sha256"], digest)
        for digest in (True, "", "a" * 63, "g" * 64):
            entry = external(); entry["sha256"] = digest
            self.assert_refused(entry)
        for filename in ("../escape.pkg", CID + "-..pkg", CID + "-" + "x" * 80 + ".pkg"):
            entry = external(); entry["filename"] = filename
            self.assert_refused(entry)

    def test_duplicate_ids_and_cache_filenames_are_refused(self):
        entry = external()
        duplicate = external(); duplicate["id"] = "different-id"
        with self.assertRaisesRegex(ValueError, "duplicate filename"):
            self.validate([entry, duplicate])
        duplicate = external(); duplicate["filename"] = CID + "-different.pkg"
        with self.assertRaisesRegex(ValueError, "duplicate id"):
            self.validate([entry, duplicate])

    def test_cli_generates_mixed_catalog_without_network(self):
        self.validate(self.official + [external()])
        output = Path(self.directory.name) / "ui_catalog.h"
        run = subprocess.run([sys.executable, str(SCRIPT), "--catalog", str(self.path), "--output", str(output)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("16 validated PS4 catalog entries", run.stdout)
        self.assertIn("sourceBadge", output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
