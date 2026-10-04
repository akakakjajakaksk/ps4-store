#!/usr/bin/env python3
"""Catalog provenance and package metadata regression checks, without network."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
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
POLICY_SCRIPT = SCRIPT.with_name("archive_policy.py")
POLICY_SPEC = importlib.util.spec_from_file_location("peppy_archive_policy_test", POLICY_SCRIPT)
POLICY = importlib.util.module_from_spec(POLICY_SPEC)
POLICY_SPEC.loader.exec_module(POLICY)
CID = "UP2047-CUSA10216_00-AGONY666AMERICAS"
REMOTE_NAME = f"[DLPSGAME.COM]-{CID}-A0100-V0100.pkg"
ARCHIVE_CID = "UP3643-CUSA00486_00-HOTLINEMIAMIPS40"
ARCHIVE_ITEM = "ps4-fpkg-collection-english-h"
ARCHIVE_REMOTE_NAME = "Hotline Miami - [US] [EN] [1.01].pkg"
CONVERSION_CASES = (
    ("o-bom-de-guerra-1", "O_Bom_de_Guerra_1.pkg", "UP9000-SCUS97399_00-SCUS973990000001", "PS2 conversion for PS4", 3279749120),
    ("parasite-eve-ii-pt-br", "Parasite Eve II (PT-BR).pkg", "UP9000-SLUS01042_00-SLUS010420000000", "PS1 conversion for PS4", 976355328),
    ("parasite-eve-ii-pt-br", "Resident Evil 1 BR CUSA00927 PS1.pkg", "UP9000-CUSA00927_00-SCUS942400000000", "PS1 conversion for PS4", 411697152),
    ("parasite-eve-ii-pt-br", "Resident Evil 2 BR CUSA00755 PS1.pkg", "UP9000-CUSA00755_00-SCUS942400000000", "PS1 conversion for PS4", 944766976),
    ("parasite-eve-ii-pt-br", "Resident Evil Nemesis BR CUSA00924 PS1.pkg", "UP9000-CUSA00924_00-SCUS942400000000", "PS1 conversion for PS4", 468910080),
    ("parasite-eve-ii-pt-br", "Resident Evil Survivor BR CUSA01088 PS1.pkg", "UP9000-CUSA01088_00-SCUS942400000000", "PS1 conversion for PS4", 245760000),
    ("parasite-eve-ii-pt-br", "Silent Hill BR CUSA00925 PS1.pkg", "UP9000-CUSA00925_00-SCUS942400000000", "PS1 conversion for PS4", 397803520),
    ("parasite-eve-ii-pt-br", "Yu-Gi-Oh! Forbidden Memories (BR)PS1.pkg", "UP9000-CUSA14511_00-SCUS942400000000", "PS1 conversion for PS4", 248578048),
)
GAMEBATO_CID = "XX0000-GBTX00001_00-GBTXXXXXXXXXXXXX"
GAMEBATO_SHA256 = "529a33a55722c2488c9b190da6eb6132997903c3253091e576538785d3c937bb"
MIRROR_SOURCE = "Niklas080208/ps4-aio-apps"
MIRROR_COMMIT = "bf32bd6cf497ee9070f3f2f962c63ed19330e8bc"
MIRROR_CID = "IV0002-ITEM00001_00-STOREUPD00000000"
# Synthetic hash metadata exercises required pinning; no test downloads a PKG.
MIRROR_TEST_SHA256 = "0123456789abcdef" * 4
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
        "content_origin": "native PS4",
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


def archive_conversion(case) -> dict:
    collection, remote_name, content_id, origin, size = case
    entry = archive()
    entry.update({
        "id": content_id[7:16], "name": remote_name[:-4],
        "source_url": f"https://archive.org/details/{collection}",
        "url": f"https://archive.org/download/{collection}/{quote(remote_name, safe='')}",
        "content_id": content_id, "content_origin": origin,
        "filename": content_id + "-BASE.pkg", "size_bytes": size,
    })
    entry["header_verification"].update({"content_id": content_id, "package_size": size})
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


def github_mirror() -> dict:
    entry = archive()
    entry.update({
        "id": "ITEM00001-mirror",
        "name": "Itemzflow",
        "category": "Utilitários",
        "provider": "github_mirror",
        "source": MIRROR_SOURCE,
        "source_label": "ESPELHO NÃO OFICIAL",
        "source_url": f"https://github.com/{MIRROR_SOURCE}/blob/{MIRROR_COMMIT}/apps.json",
        "url": f"https://raw.githubusercontent.com/{MIRROR_SOURCE}/{MIRROR_COMMIT}/pkgs/itemzflow.pkg",
        "content_id": MIRROR_CID,
        "filename": f"{MIRROR_CID}-MIRROR.pkg",
        "size_bytes": 27131904,
        "version": "espelho sem versão",
        "sha256": MIRROR_TEST_SHA256,
    })
    entry["header_verification"].update({"content_id": MIRROR_CID, "package_size": entry["size_bytes"]})
    return entry


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="peppy-catalog-tests-")
        self.path = Path(self.directory.name) / "catalog.json"
        self.policy_path = Path(self.directory.name) / "archive_sources.json"
        self.policy_data = {"schema_version": 1, "collections": [ARCHIVE_ITEM],
                            "cdn_hosts": ["ia800705.us.archive.org", "dn721707.ca.archive.org"]}
        self.policy_path.write_text(json.dumps(self.policy_data), encoding="utf-8")
        raw = json.loads((NATIVE / "catalog.json").read_text(encoding="utf-8"))
        self.official = [copy.deepcopy(entry) for entry in raw["entries"]
                         if entry.get("source_kind", "official") == "official"]

    def tearDown(self):
        self.directory.cleanup()

    def validate(self, entries):
        self.path.write_text(json.dumps({"schema_version": 1, "entries": entries}), encoding="utf-8")
        return CATALOG.read_catalog(self.path, self.policy_path)

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
               "game%ZZ.pkg", ".pkg", "game.PKG", ".hidden.pkg", "%2ehidden.pkg",
               'game".pkg', "game%22.pkg", "game%3C.pkg", "game%3E.pkg", "game%60.pkg",
               "game%2Epkg", "game.%70kg", "game.p%6Bg", "game.pk%67",
               ARCHIVE_CID.replace("00486", "00487") + ".pkg", "Hotline-CUSA00487.pkg")
        for basename in bad:
            with self.subTest(basename=basename):
                entry = archive(); entry["url"] = prefix + basename
                self.assert_refused(entry)
        for filename in ("Hotline.pkg", ARCHIVE_CID.replace("00486", "00487") + ".pkg"):
            entry = archive(); entry["filename"] = filename
            self.assert_refused(entry)

    def test_archive_percent_encoded_utf8_accepts_scalar_names_and_internal_double_dots(self):
        prefix = f"https://archive.org/download/{ARCHIVE_ITEM}/"
        # Scalar validation matches the native decoder, without treating valid
        # Unicode noncharacters as malformed UTF-8 or allowing raw URL Unicode.
        names = ("Ragnarök.pkg", "ŌKAMI.pkg", "NINJA GAIDEN Σ.pkg", "NINJA GAIDEN Σ2.pkg",
                 "Zone M∀RS.pkg", "Nier ver….pkg", "game..backup.pkg", "game..pkg",
                 "game\uffff.pkg", "game\U0010ffff.pkg")
        for name in names:
            with self.subTest(name=name):
                entry = archive(); entry["url"] = prefix + quote(name, safe="")
                self.assertEqual(self.validate([entry])[0]["url"], entry["url"])
        entry = archive(); entry["url"] = prefix + "Ragnarök.pkg"
        self.assert_refused(entry, "ASCII HTTPS URL")

    def test_archive_percent_encoded_utf8_rejects_invalid_sequences_and_c0_c1_controls(self):
        prefix = f"https://archive.org/download/{ARCHIVE_ITEM}/game"
        bad = ("%80", "%C0%80", "%C0%AF", "%C1%BF", "%C2", "%C2%20", "%E0%80%AF",
               "%E2%82", "%ED%A0%80", "%ED%BF%BF", "%F0%80%80%AF", "%F4%90%80%80",
               "%F5%80%80%80", "%FE", "%FF", "%00", "%1F", "%7F", "%C2%80", "%C2%85", "%C2%9F")
        for encoded in bad:
            with self.subTest(encoded=encoded):
                entry = archive(); entry["url"] = prefix + encoded + ".pkg"
                self.assert_refused(entry)

    def test_archive_exact_cdn_routes_preserve_origin_provenance_and_header_identity(self):
        for host in self.policy_data["cdn_hosts"]:
            for directory in ("0", "13", "0123456789"):
                with self.subTest(host=host, directory=directory):
                    entry = archive()
                    entry["url"] = f"https://{host}/{directory}/items/{ARCHIVE_ITEM}/{quote(ARCHIVE_REMOTE_NAME, safe='')}"
                    result = self.validate([entry])[0]
                    self.assertEqual(result["content_id"], ARCHIVE_CID)
                    self.assertEqual(result["source_url"], f"https://archive.org/details/{ARCHIVE_ITEM}")
                    self.assertEqual(result["source_badge"], "PKG / INTERNET ARCHIVE")
        entry = archive()
        entry["url"] = f"https://dn721707.ca.archive.org/0/items/{ARCHIVE_ITEM}/Ragnar%C3%B6k..pkg"
        self.assertEqual(self.validate([entry])[0]["url"], entry["url"])

    def test_archive_cdn_routes_reject_unapproved_hosts_wrong_items_and_path_shapes(self):
        host = "ia800705.us.archive.org"
        good = f"https://{host}/13/items/{ARCHIVE_ITEM}/game.pkg"
        bad = [good.replace(host, value) for value in (
            "never-reviewed.archive.org", host + ".evil.org", host + ".", "u:p@" + host,
            host + ":443", "archive.org")]
        bad += [good.replace("/13/", "/" + value + "/")
                for value in ("", "-1", "1.0", "abc", "%31", "12345678901")]
        bad += [good.replace("/items/", "/download/"), good.replace(ARCHIVE_ITEM, "never-reviewed-item"),
                good.replace(ARCHIVE_ITEM, "%70" + ARCHIVE_ITEM[1:]), good.replace("game.pkg", "sub/game.pkg"),
                good.replace("game.pkg", "game.zip"), good + "/", good + "?", good + "?x=1", good + "#"]
        for url in bad:
            with self.subTest(url=url):
                entry = archive(); entry["url"] = url
                self.assert_refused(entry)
        self.policy_data["collections"].append("second-reviewed-item")
        self.policy_path.write_text(json.dumps(self.policy_data), encoding="utf-8")
        entry = archive(); entry["url"] = good.replace(ARCHIVE_ITEM, "second-reviewed-item")
        self.assert_refused(entry, "reviewed collection")
        entry = archive(); entry["url"] = good; entry["source_url"] = f"https://{host}/details/{ARCHIVE_ITEM}"
        self.assert_refused(entry, "provenance")

    def approve_conversion_fixtures(self):
        self.policy_data["collections"] += ["o-bom-de-guerra-1", "parasite-eve-ii-pt-br"]
        self.policy_path.write_text(json.dumps(self.policy_data), encoding="utf-8")

    def test_eight_reviewed_conversions_preserve_distinct_content_origin_and_runtime_identity(self):
        self.approve_conversion_fixtures()
        result = self.validate([archive_conversion(case) for case in CONVERSION_CASES])
        self.assertEqual(len(result), 8)
        self.assertEqual(len({entry["content_id"] for entry in result}), 8)
        self.assertEqual(sum(entry["content_origin"] == "PS1 conversion for PS4" for entry in result), 7)
        self.assertEqual(sum(entry["content_origin"] == "PS2 conversion for PS4" for entry in result), 1)
        for actual, (_, _, content_id, origin, size) in zip(result, CONVERSION_CASES):
            with self.subTest(content_id=content_id):
                self.assertEqual(actual["content_id"], content_id)
                self.assertEqual(actual["content_origin"], origin)
                self.assertEqual(actual["size_bytes"], size)
                self.assertEqual(actual["source_badge"], "PKG / INTERNET ARCHIVE")
                self.assertIn(json.dumps(content_id) + ",", CATALOG.generate_header([actual]))

    def test_conversion_origin_exact_size_and_ps4_base_proof_are_mandatory_including_cusa_donors(self):
        self.approve_conversion_fixtures()
        for case in CONVERSION_CASES:
            for origin in (None, "", "native PS4", "PS1 conversion for PS5", "PS2 conversion for PS4"
                           if case[3].startswith("PS1") else "PS1 conversion for PS4"):
                with self.subTest(content_id=case[2], origin=origin):
                    entry = archive_conversion(case); entry["content_origin"] = origin
                    self.assert_refused(entry, "content_origin")
            entry = archive_conversion(case); del entry["content_origin"]
            self.assert_refused(entry, "content_origin")
            entry = archive_conversion(case); entry["size_bytes"] += 1
            entry["header_verification"]["package_size"] += 1
            self.assert_refused(entry, "exact package size")
            for changes in ({"platform": "PS5"}, {"package_kind": "patch"}, {"header_verification": None}):
                entry = archive_conversion(case); entry.update(changes)
                self.assert_refused(entry)
            for proof in ({"content_type": 0x1B}, {"content_flags": 0x62300000}, {"package_size": case[4] + 1},
                          {"content_id": ARCHIVE_CID}):
                entry = archive_conversion(case); entry["header_verification"].update(proof)
                self.assert_refused(entry)

    def test_conversion_cids_are_source_specific_and_do_not_expand_general_archive_id_rules(self):
        self.approve_conversion_fixtures()
        for case in CONVERSION_CASES:
            entry = archive_conversion(case)
            wrong = "parasite-eve-ii-pt-br" if case[0] == "o-bom-de-guerra-1" else "o-bom-de-guerra-1"
            entry["source_url"] = entry["source_url"].replace(case[0], wrong)
            entry["url"] = entry["url"].replace(case[0], wrong)
            self.assert_refused(entry, "individually reviewed")
            entry = archive_conversion(case)
            entry["source_url"] = entry["source_url"].replace(case[0], ARCHIVE_ITEM)
            entry["url"] = entry["url"].replace(case[0], ARCHIVE_ITEM)
            self.assert_refused(entry, "individually reviewed")
        for collection in ("o-bom-de-guerra-1", "parasite-eve-ii-pt-br"):
            entry = archive()
            entry["source_url"] = entry["source_url"].replace(ARCHIVE_ITEM, collection)
            entry["url"] = entry["url"].replace(ARCHIVE_ITEM, collection)
            self.assert_refused(entry, "individually reviewed")
        for prefix in ("SCUS", "SLES", "SLUS"):
            entry = archive(); cid = ARCHIVE_CID.replace("CUSA", prefix)
            entry["content_id"] = cid; entry["filename"] = cid + "-BASE.pkg"
            entry["header_verification"]["content_id"] = cid
            self.assert_refused(entry, "canonical CUSA")
        entry = archive(); entry["content_origin"] = "PS1 conversion for PS4"
        self.assert_refused(entry, "individually reviewed")
        entry = archive(); del entry["content_origin"]
        self.assertEqual(self.validate([entry])[0]["content_id"], ARCHIVE_CID)

    def test_archive_policy_allows_only_approved_collection_with_matching_provenance(self):
        collection = "reviewed-fixture-second-collection"
        entry = archive()
        entry["source_url"] = entry["source_url"].replace(ARCHIVE_ITEM, collection)
        entry["url"] = entry["url"].replace(ARCHIVE_ITEM, collection)
        self.assert_refused(entry, "exact reviewed collection")
        self.policy_data["collections"].append(collection)
        self.policy_path.write_text(json.dumps(self.policy_data), encoding="utf-8")
        result = self.validate([entry])[0]
        self.assertEqual(result["source_url"], entry["source_url"])
        self.assertEqual(result["source_badge"], "PKG / INTERNET ARCHIVE")
        entry["url"] = entry["url"].replace(collection, ARCHIVE_ITEM)
        self.assert_refused(entry, "reviewed collection")

    def test_archive_policy_schema_duplicates_string_limits_and_domains_are_strict(self):
        malformed = [None, [], {}, {**self.policy_data, "schema_version": True},
                     {**self.policy_data, "schema_version": 2}, {**self.policy_data, "unknown": []}]
        for value in ([], ARCHIVE_ITEM, [ARCHIVE_ITEM, ARCHIVE_ITEM], ["sample"] * 129,
                      [".."], ["sample/other"], ["sample%2fother"], ["sample?"], ["sample#"],
                      ["sample\n"], ["sampleé"], ["x" * 129], [False]):
            malformed.append({**self.policy_data, "collections": value})
        for value in ([], "ia800705.us.archive.org", ["ia800705.us.archive.org"] * 2,
                      [f"host-{i}.archive.org" for i in range(129)],
                      ["archive.org"], ["archive.org.evil.org"], ["unreviewed.org"],
                      ["IA800705.us.archive.org"], ["ia800705.us.archive.org."],
                      ["ia800705.us.archive.org:443"], ["u@ia800705.us.archive.org"],
                      ["ia800705..archive.org"], ["-bad.archive.org"], ["bad-.archive.org"],
                      ["x" * 64 + ".archive.org"], [False]):
            malformed.append({**self.policy_data, "cdn_hosts": value})
        for value in malformed:
            with self.subTest(value=value):
                self.policy_path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(ValueError):
                    POLICY.load_archive_policy(self.policy_path)
        self.policy_path.write_text('{"schema_version":1,"collections":["one"],"collections":["two"],'
                                    '"cdn_hosts":["host.archive.org"]}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            POLICY.load_archive_policy(self.policy_path)

    def test_archive_policy_read_and_list_caps_accept_exact_boundaries(self):
        data = {"schema_version": 1, "collections": [f"fixture-{i}" for i in range(128)],
                "cdn_hosts": [f"host-{i}.archive.org" for i in range(128)]}
        raw = json.dumps(data).encode("utf-8")
        self.policy_path.write_bytes(raw + b" " * (65536 - len(raw)))
        result = POLICY.load_archive_policy(self.policy_path)
        self.assertEqual(len(result["collections"]), 128)
        self.assertEqual(len(result["cdn_hosts"]), 128)
        with self.policy_path.open("ab") as stream:
            stream.write(b" ")
        with self.assertRaisesRegex(ValueError, "64 KiB"):
            POLICY.load_archive_policy(self.policy_path)

    def test_archive_policy_header_cli_is_deterministic_and_check_never_writes(self):
        output = Path(self.directory.name) / "archive_sources.h"
        command = [sys.executable, str(POLICY_SCRIPT), "--policy", str(self.policy_path), "--output", str(output)]
        run = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        expected = POLICY.generate_archive_header(POLICY.load_archive_policy(self.policy_path))
        self.assertEqual(output.read_text(encoding="utf-8"), expected)
        run = subprocess.run(command + ["--check"], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        stale = output.read_bytes() + b"// stale\n"
        output.write_bytes(stale)
        run = subprocess.run(command + ["--check"], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 1)
        self.assertEqual(output.read_bytes(), stale)
        original = self.policy_path.read_bytes()
        aliases = [self.policy_path, Path(self.directory.name) / "policy-hardlink"]
        os.link(self.policy_path, aliases[1])
        for destination in aliases:
            run = subprocess.run([sys.executable, str(POLICY_SCRIPT), "--policy", str(self.policy_path),
                                  "--output", str(destination)], capture_output=True, text=True, check=False)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(self.policy_path.read_bytes(), original)

    def test_generated_archive_header_enforces_exact_collection_and_dns_host_spans(self):
        compiler = shutil.which("g++")
        if compiler is None:
            self.skipTest("g++ is required to check the generated native helper")
        root = Path(self.directory.name)
        (root / "archive_sources.h").write_text(
            POLICY.generate_archive_header(POLICY.load_archive_policy(self.policy_path)), encoding="utf-8")
        source = root / "policy_test.cpp"
        source.write_text('''#include "archive_sources.h"
#include <assert.h>
#include <string.h>
#define APPROVED_HOST(value) approvedCdnHost(value, sizeof(value) - 1)
int main() {
    using namespace peppyArchiveSources;
    static_assert(COLLECTION_COUNT == 1 && CDN_HOST_COUNT == 2, "policy count");
    const char* collection = "ps4-fpkg-collection-english-h";
    assert(approvedCollection(collection, strlen(collection)));
    assert(!approvedCollection(collection, strlen(collection) - 1));
    assert(!approvedCollection(collection, strlen(collection) + 1));
    assert(!approvedCollection("PS4-fpkg-collection-english-h", strlen(collection)));
    assert(!approvedCollection("unreviewed-fixture", 18));
    assert(!approvedCollection(0, 0));
    const char* host = "IA800705.US.ARCHIVE.ORG";
    assert(approvedCdnHost(host, strlen(host)));
    assert(APPROVED_HOST("dn721707.ca.archive.org"));
    assert(!APPROVED_HOST("ia800705.us.archive.org."));
    assert(!APPROVED_HOST("ia800705.us.archive.org:443"));
    assert(!APPROVED_HOST("ia800705.us.archive.org.evil.org"));
    assert(!APPROVED_HOST("u@ia800705.us.archive.org"));
    assert(!APPROVED_HOST("never-reviewed.archive.org"));
    assert(!approvedCdnHost(0, 0));
}
''', encoding="utf-8")
        output = root / "policy_test"
        run = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(output)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        run = subprocess.run([str(output)], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_five_hundred_fixture_records_have_distinct_identity_and_emit_the_last_record(self):
        # Synthetic records belong only to this temporary regression fixture;
        # no production count or game claim is derived from them.
        entries = []
        for index in range(500):
            entry = archive()
            cid = f"UP0000-CUSA{index:05d}_00-ABCDEFGHIJKLMNOP"
            entry.update({"id": f"fixture-{index}", "name": f"Regression fixture {index}",
                          "content_id": cid, "filename": cid + "-BASE.pkg", "size_bytes": 4096 + index,
                          "url": f"https://archive.org/download/{ARCHIVE_ITEM}/Regression-fixture-{index}.pkg"})
            entry["header_verification"].update({"content_id": cid, "package_size": entry["size_bytes"]})
            entries.append(entry)
        result = self.validate(entries)
        self.assertEqual(len(result), 500)
        self.assertEqual(len({entry["id"] for entry in result}), 500)
        self.assertEqual(len({entry["content_id"] for entry in result}), 500)
        header = CATALOG.generate_header(result)
        self.assertEqual(header.count("    {\n"), 500)
        self.assertIn(json.dumps(entries[-1]["id"]) + ",", header)
        self.assertIn(json.dumps(entries[-1]["content_id"]) + ",", header)
        self.assertIn("4595ULL, 3, 1", header)
        self.assertIn("sizeof(UI_APPS) / sizeof(UI_APPS[0])", header)

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

    def test_reviewed_github_mirror_emits_nonofficial_badge_and_runtime_hash_identity(self):
        self.assertNotIn(MIRROR_SOURCE, CATALOG.VERIFIED_REPOSITORIES)
        entry = github_mirror()
        entry["source_badge"] = "PKG / FONTE OFICIAL"
        result = self.validate([entry])[0]
        self.assertEqual(result["source_kind"], "reviewed_direct")
        self.assertEqual(result["provider"], "github_mirror")
        self.assertEqual(result["source"], MIRROR_SOURCE)
        self.assertEqual(result["release_url"], entry["source_url"])
        self.assertEqual(result["source_badge"], "PKG / ESPELHO NÃO OFICIAL")
        self.assertEqual(result["content_id"], MIRROR_CID)
        self.assertEqual(result["sha256"], MIRROR_TEST_SHA256)
        self.assertEqual(result["size_bytes"], 27131904)
        self.assertEqual(result["version"], "espelho sem versão")
        header = CATALOG.generate_header([result])
        self.assertIn(json.dumps(MIRROR_CID) + ",", header)
        self.assertIn(json.dumps(MIRROR_TEST_SHA256) + ",", header)
        self.assertIn('"PKG / ESPELHO NÃO OFICIAL",', header)

    def test_github_mirror_requires_reviewed_url_provenance_and_no_official_claim(self):
        for changes in ({"source": "LightningMods/Itemzflow"}, {"source_kind": "official"},
                        {"source_label": "FONTE OFICIAL"}, {"source_label": "ESPELHO OFICIAL"},
                        {"repository": MIRROR_SOURCE}, {"provider": "github"},
                        {"source_url": github_mirror()["source_url"].replace(MIRROR_COMMIT, "main")},
                        {"release_url": "https://github.com/LightningMods/Itemzflow/releases"},
                        {"source_url": github_mirror()["source_url"] + "?"},
                        {"platform": "PS5"}, {"package_kind": "patch"}, {"content_id": ARCHIVE_CID},
                        {"filename": "itemzflow.pkg"}):
            with self.subTest(changes=changes):
                entry = github_mirror(); entry.update(changes)
                self.assert_refused(entry)
        good = github_mirror()["url"]
        for url in (good.replace(MIRROR_COMMIT, "main"), good.replace(MIRROR_COMMIT, "master"),
                    good.replace(MIRROR_COMMIT, MIRROR_COMMIT[:-1] + "a"),
                    good.replace(MIRROR_SOURCE, "somebody/ps4-aio-apps"),
                    good.replace("itemzflow.pkg", "other.pkg"), good.replace("itemzflow.pkg", "itemzflow.zip"),
                    good.replace("https://", "http://"), good + "?", good + "?token=a", good + "#",
                    good.replace("raw.githubusercontent.com", "u:p@raw.githubusercontent.com"),
                    good.replace("raw.githubusercontent.com", "raw.githubusercontent.com:443"),
                    good.replace("raw.githubusercontent.com", "raw.githubusercontent.com.evil.org"),
                    good.replace("/pkgs/", "/pkgs/../pkgs/")):
            with self.subTest(url=url):
                entry = github_mirror(); entry["url"] = url
                self.assert_refused(entry)

    def test_github_mirror_refuses_missing_hash_and_wrong_or_nonbase_header_evidence(self):
        for digest in (None, "", True, "a" * 63, "a" * 65, "g" * 64):
            with self.subTest(digest=digest):
                entry = github_mirror(); entry["sha256"] = digest
                self.assert_refused(entry, "SHA-256")
        entry = github_mirror(); del entry["sha256"]
        self.assert_refused(entry, "SHA-256")
        for proof in ({"content_id": MIRROR_CID.replace("ITEM", "PPSA")}, {"package_size": 27131905},
                      {"content_type": 0x1B}, {"content_flags": 0x62300000}, {"magic": "504b0304"}):
            with self.subTest(proof=proof):
                entry = github_mirror(); entry["header_verification"].update(proof)
                self.assert_refused(entry)
        entry = github_mirror()
        entry["size_bytes"] += 1
        entry["header_verification"]["package_size"] = entry["size_bytes"]
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
        self.validate(self.official + [external(), archive(), gamebato(), github_mirror()])
        output = Path(self.directory.name) / "ui_catalog.h"
        run = subprocess.run([sys.executable, str(SCRIPT), "--catalog", str(self.path), "--output", str(output),
                              "--archive-policy", str(self.policy_path)],
                             capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn(f"{len(self.official) + 4} validated PS4 catalog entries", run.stdout)
        self.assertIn("sourceBadge", output.read_text(encoding="utf-8"))
        self.assertIn('"PKG / INTERNET ARCHIVE",', output.read_text(encoding="utf-8"))
        self.assertIn('"PKG / GAMEBATO",', output.read_text(encoding="utf-8"))
        self.assertIn('"PKG / ESPELHO NÃO OFICIAL",', output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
