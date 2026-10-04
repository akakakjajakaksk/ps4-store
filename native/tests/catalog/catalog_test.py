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
ARCHIVE_CID = "UP3643-CUSA00486_00-HOTLINEMIAMIPS40"
ARCHIVE_ITEM = "ps4-fpkg-collection-english-h"
ARCHIVE_REMOTE_NAME = "Hotline Miami - [US] [EN] [1.01].pkg"
GAMEBATO_CID = "XX0000-GBTX00001_00-GBTXXXXXXXXXXXXX"
GAMEBATO_SHA256 = "529a33a55722c2488c9b190da6eb6132997903c3253091e576538785d3c937bb"
NEW_OFFICIAL_RELEASES = (
    ("ItsJokerZz/FPKGi", "v1.10.0", "FPKGi_v1.10.0-release.pkg", 85458944),
    ("bucanero/PS4CheatsManager", "v1.2.2", "IV0000-CHTM00777_00-PS4CHEATSMANAGER.pkg", 19202048),
    ("EmiiBytee/np2kai-ps4", "v1.0", "IV0000-BREW00984_00-NP2KAIBOOTSTRAP0.pkg", 9109504),
    ("bizkut/ps4-mgba", "ps4-v0.1.0", "mgba-ps4-ps4-v0.1.0.pkg", 6619136),
)
IOQUAKE_ASSETS = (
    ("QUAK03000", 13762560, "4f609f1dab826f558fa99d7389c2d9d48ef3c0a28e9c2aec926835b44fce2cc4"),
    ("QUAK03001", 13762560, "3c1823e5ba2003d25166aaa75868ec68e5a83684fc18b8a31c5f5c52025058af"),
    ("QUAK03002", 13762560, "5ffe1e0c70ab455a693db4af6ac6960e2a2ec430519ea09571bf4e6e09f1d3f0"),
    ("QUAK03003", 13107200, "53a6a447346e88e22684e523193574606374ab247896a127d54188b5c5ba6d57"),
    ("QUAK03004", 13762560, "8e77da8680aa6f51e9676d526291cd127f4f2eea441a527cb9e1ed120ca542c8"),
)
LEGACY_OFFICIAL_IDS = frozenset({
    "APOL00004", "ezremote-client", "homebrew-store", "BREW00050", "KPBR01111",
    "BGEN00041", "BGEN00042", "BGEN00070", "BGEN00071", "PSMP00001",
    "PFBN00001", "PGBA00001", "PGEN00001", "PNES00001", "PSNE00001",
})


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


def archive() -> dict:
    entry = external()
    entry.update({
        "id": "CUSA00486",
        "name": "Hotline Miami",
        "source_kind": "reviewed_direct",
        "provider": "archive",
        "source": "archive.org",
        "source_label": "INTERNET ARCHIVE",
        "source_url": f"https://archive.org/details/{ARCHIVE_ITEM}",
        "content_id": ARCHIVE_CID,
        "filename": f"{ARCHIVE_CID}-BASE.pkg",
        "size_bytes": 161939456,
        "url": f"https://archive.org/download/{ARCHIVE_ITEM}/{quote(ARCHIVE_REMOTE_NAME, safe='')}",
    })
    entry["header_verification"].update({"package_size": entry["size_bytes"], "content_id": ARCHIVE_CID})
    return entry


def gamebato() -> dict:
    entry = archive()
    entry.update({
        "id": "GBTX00001",
        "name": "GameBaTo",
        "category": "Utilitários",
        "provider": "gamebato",
        "source": "gamebatoapp.ir",
        "source_label": "GAMEBATO",
        "source_url": "https://gamebatoapp.ir/home/en/",
        "url": "https://gamebatoapp.ir/home/app.pkg",
        "content_id": GAMEBATO_CID,
        "filename": f"{GAMEBATO_CID}-SNAPSHOT.pkg",
        "size_bytes": 23068672,
        "version": "site sem versão",
        "sha256": GAMEBATO_SHA256,
    })
    entry["header_verification"].update({"content_id": GAMEBATO_CID, "package_size": entry["size_bytes"]})
    return entry


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
        self.assertTrue(LEGACY_OFFICIAL_IDS.issubset({entry["id"] for entry in self.official}))
        validated = self.validate(self.official)
        self.assertEqual(len(validated), len(self.official))
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

    def test_new_reviewed_official_repositories_keep_pinned_asset_checks(self):
        for source, version, filename, size in NEW_OFFICIAL_RELEASES:
            with self.subTest(source=source):
                self.assertIn(source, CATALOG.VERIFIED_REPOSITORIES)
                entry = copy.deepcopy(self.official[0])
                entry.update({
                    "id": source.split("/")[1], "source": source, "version": version,
                    "filename": filename, "size_bytes": size, "sha256": None,
                    "url": f"https://github.com/{source}/releases/download/{version}/{filename}",
                    "release_url": f"https://github.com/{source}/releases/tag/{version}",
                })
                entry.pop("repository", None)
                result = self.validate([entry])[0]
                self.assertEqual(result["source_badge"], "PKG / FONTE OFICIAL")
                for changes in ({"repository": "somebody/different"}, {"version": "latest"},
                                {"url": entry["url"].replace(f"/download/{version}/", "/download/latest/")},
                                {"release_url": entry["release_url"].replace(f"/tag/{version}", "/tag/other")}):
                    invalid = copy.deepcopy(entry); invalid.update(changes)
                    self.assert_refused(invalid)

    def test_reviewed_ioquake_release_assets_preserve_published_hashes_and_data_requirements(self):
        source = "Mayo1970/ioQuake3-PS4"
        self.assertIn(source, CATALOG.VERIFIED_REPOSITORIES)
        entries = []
        for title_id, size, digest in IOQUAKE_ASSETS:
            filename = f"IV0000-{title_id}_00-IOQ3PS4PORT00000.pkg"
            entry = copy.deepcopy(self.official[0])
            entry.update({
                "id": title_id, "source": source, "version": "1.8", "filename": filename,
                "size_bytes": size, "sha256": digest,
                "url": f"https://github.com/{source}/releases/download/1.8/{filename}",
                "release_url": f"https://github.com/{source}/releases/tag/1.8",
                "requires_data": "Dados pk3 externos necessários; não incluídos no PKG.",
            })
            entry.pop("repository", None)
            entries.append(entry)
        result = self.validate(entries)
        self.assertEqual(len(result), 5)
        header = CATALOG.generate_header(result)
        for actual, (title_id, size, digest) in zip(result, IOQUAKE_ASSETS):
            self.assertEqual(actual["id"], title_id)
            self.assertEqual(actual["size_bytes"], size)
            self.assertEqual(actual["sha256"], digest)
            self.assertEqual(actual["source_badge"], "PKG / FONTE OFICIAL")
            self.assertIn(json.dumps(digest) + ",", header)
            self.assertIn(json.dumps(actual["requires_data"], ensure_ascii=False) + ",", header)
        for changes in ({"version": "latest"}, {"version": "1.7"},
                        {"source": "somebody/ioQuake3-PS4"},
                        {"url": entries[0]["url"].replace("QUAK03000", "QUAK03001")}):
            invalid = copy.deepcopy(entries[0]); invalid.update(changes)
            self.assert_refused(invalid)

    def test_reviewed_archive_metadata_pins_runtime_identity_and_honest_badge(self):
        entry = archive()
        entry["source_badge"] = "PKG / FONTE OFICIAL"
        result = self.validate([entry])[0]
        self.assertEqual(result["source_kind"], "reviewed_direct")
        self.assertEqual(result["source_badge"], "PKG / INTERNET ARCHIVE")
        self.assertEqual(result["release_url"], entry["source_url"])
        self.assertEqual(result["content_id"], ARCHIVE_CID)
        self.assertEqual(result["size_bytes"], 161939456)
        header = CATALOG.generate_header([result])
        self.assertIn(json.dumps(ARCHIVE_CID) + ",", header)
        self.assertIn('"PKG / INTERNET ARCHIVE",', header)
        self.assertIn("161939456ULL", header)
        entry["header_verification"]["content_flags"] = 0x0E000000
        self.assertEqual(self.validate([entry])[0]["content_id"], ARCHIVE_CID)

    def test_archive_provider_provenance_and_ps4_base_evidence_are_mandatory(self):
        for changes in ({"provider": "mediafire"}, {"provider": None}, {"source": "github.com"},
                        {"source_label": "FONTE OFICIAL"}, {"source_label": "Official downloads"},
                        {"source_label": "INTERNET ARCHIVE\n"}, {"repository": "somebody/repository"},
                        {"platform": "PS5"}, {"package_kind": "patch"}, {"package_kind": "addon"},
                        {"source_url": f"https://archive.org/details/{ARCHIVE_ITEM}?x=1"},
                        {"source_url": "https://archive.org/details/some-other-item"},
                        {"release_url": "https://archive.org/details/some-other-item"}):
            with self.subTest(changes=changes):
                entry = archive(); entry.update(changes)
                self.assert_refused(entry)
        for key in ("provider", "header_verification", "content_id", "source_label"):
            entry = archive(); del entry[key]
            self.assert_refused(entry)
        for proof in ({"content_type": 0x1B}, {"content_type": True}, {"content_flags": 0x62300000},
                      {"content_flags": 0x0A000001}, {"package_size": 161939457},
                      {"content_id": ARCHIVE_CID.replace("00486", "00487")}, {"magic": "504b0304"}):
            with self.subTest(proof=proof):
                entry = archive(); entry["header_verification"].update(proof)
                self.assert_refused(entry)
        for prefix in ("PPSA", "SLES", "SLUS", "BREW"):
            entry = archive(); content_id = ARCHIVE_CID.replace("CUSA", prefix)
            entry["content_id"] = content_id
            entry["filename"] = content_id + ".pkg"
            entry["header_verification"]["content_id"] = content_id
            self.assert_refused(entry)

    def test_archive_urls_refuse_other_routes_hosts_and_intermediate_formats(self):
        good = archive()["url"]
        bad = (
            good.replace("https://", "http://"),
            good.replace("archive.org", "u:p@archive.org"),
            good.replace("archive.org", "archive.org:443"),
            good.replace("archive.org", "archive.org.evil.org"),
            good.replace("archive.org", "ia800705.us.archive.org"),
            good.replace(ARCHIVE_ITEM, "another-collection"),
            good.replace("/download/", "/details/"),
            good.replace(".pkg", ".html"), good.replace(".pkg", ".zip"),
            good.replace(".pkg", ".pkg/other"), good + "/", good + "?", good + "?token=a", good + "#",
        )
        for url in bad:
            with self.subTest(url=url):
                entry = archive(); entry["url"] = url
                self.assert_refused(entry)

    def test_archive_decoded_basename_rejects_traversal_controls_and_identity_mismatch(self):
        prefix = f"https://archive.org/download/{ARCHIVE_ITEM}/"
        bad = ("../game.pkg", "%2e%2e%2fgame.pkg", "%252e%252e%252fgame.pkg", "%2Fgame.pkg",
               "%5Cgame.pkg", "%0Agame.pkg", "%00game.pkg", "game%3Ftoken.pkg", "game%23fragment.pkg",
               "game%ZZ.pkg", "game%C3%A9.pkg", ".pkg", "game.PKG",
               ARCHIVE_CID.replace("00486", "00487") + ".pkg", "Hotline-CUSA00487.pkg")
        for basename in bad:
            with self.subTest(basename=basename):
                entry = archive(); entry["url"] = prefix + basename
                self.assert_refused(entry)
        for filename in ("Hotline.pkg", ARCHIVE_CID.replace("00486", "00487") + ".pkg"):
            entry = archive(); entry["filename"] = filename
            self.assert_refused(entry)

    def test_gamebato_mutable_installer_pins_exact_identity_and_hash(self):
        entry = gamebato()
        entry["source_badge"] = "PKG / FONTE OFICIAL"
        result = self.validate([entry])[0]
        self.assertEqual(result["source_kind"], "reviewed_direct")
        self.assertEqual(result["provider"], "gamebato")
        self.assertEqual(result["release_url"], entry["source_url"])
        self.assertEqual(result["source_badge"], "PKG / GAMEBATO")
        self.assertEqual(result["content_id"], GAMEBATO_CID)
        self.assertEqual(result["sha256"], GAMEBATO_SHA256)
        self.assertEqual(result["size_bytes"], 23068672)
        self.assertEqual(result["version"], "site sem versão")
        header = CATALOG.generate_header([result])
        self.assertIn(json.dumps(GAMEBATO_CID) + ",", header)
        self.assertIn(json.dumps(GAMEBATO_SHA256) + ",", header)
        self.assertIn('"PKG / GAMEBATO",', header)

    def test_gamebato_requires_checked_sha256_for_its_mutable_url(self):
        for digest in (None, "", True, "a" * 63, "g" * 64, "a" * 65):
            with self.subTest(digest=digest):
                entry = gamebato(); entry["sha256"] = digest
                self.assert_refused(entry, "SHA-256")
        entry = gamebato(); del entry["sha256"]
        self.assert_refused(entry, "SHA-256")
        entry = gamebato(); entry["sha256"] = GAMEBATO_SHA256.upper()
        self.assertEqual(self.validate([entry])[0]["sha256"], GAMEBATO_SHA256.upper())

    def test_gamebato_routes_provenance_and_exact_cid_cannot_be_generalized(self):
        good = gamebato()["url"]
        for changes in ({"provider": "archive"}, {"provider": "akirabox"},
                        {"source": "somewhere.else"}, {"source_label": "FONTE OFICIAL"},
                        {"source_url": "https://gamebatoapp.ir/"},
                        {"source_url": "https://gamebatoapp.ir/home/en/?x=1"},
                        {"release_url": "https://gamebatoapp.ir/"},
                        {"repository": "gamebato/site"},
                        {"platform": "PS5"}, {"package_kind": "patch"},
                        {"content_id": ARCHIVE_CID}, {"filename": "app.pkg"}):
            with self.subTest(changes=changes):
                entry = gamebato(); entry.update(changes)
                self.assert_refused(entry)
        for url in (good.replace("https://", "http://"), good + "?", good + "?token=a", good + "#",
                    good.replace("gamebatoapp.ir", "www.gamebatoapp.ir"),
                    good.replace("gamebatoapp.ir", "u:p@gamebatoapp.ir"),
                    good.replace("gamebatoapp.ir", "gamebatoapp.ir:443"),
                    good.replace("gamebatoapp.ir", "gamebatoapp.ir.evil.org"),
                    good.replace("/home/", "/home/../home/"), good.replace("app.pkg", "app.zip"),
                    good.replace("app.pkg", "%61pp.pkg"), good.replace("app.pkg", "other.pkg")):
            with self.subTest(url=url):
                entry = gamebato(); entry["url"] = url
                self.assert_refused(entry)
        for proof in ({"content_id": ARCHIVE_CID}, {"package_size": 23068673},
                      {"content_type": 0x1B}, {"content_flags": 0x62300000}):
            entry = gamebato(); entry["header_verification"].update(proof)
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
        self.validate(self.official + [external(), archive(), gamebato()])
        output = Path(self.directory.name) / "ui_catalog.h"
        run = subprocess.run([sys.executable, str(SCRIPT), "--catalog", str(self.path), "--output", str(output)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f"{len(self.official) + 3} validated PS4 catalog entries", run.stdout)
        self.assertIn("sourceBadge", output.read_text(encoding="utf-8"))
        self.assertIn('"PKG / INTERNET ARCHIVE",', output.read_text(encoding="utf-8"))
        self.assertIn('"PKG / GAMEBATO",', output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
