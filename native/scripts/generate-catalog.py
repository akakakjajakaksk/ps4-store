#!/usr/bin/env python3
"""Validate reviewed PS4 catalog metadata and generate its UTF-8 C++ header."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from urllib.parse import unquote, urlsplit


CATEGORIES = {
    "Utilitários": 1,
    "Emuladores": 2,
    "Jogos homebrew": 3,
    "Jogos": 3,
    "Mídia": 4,
}
MAX_PACKAGE_BYTES = 256 * 1024 ** 3
CONTENT_ID_RE = re.compile(r"[A-Z]{2}[0-9]{4}-((?:CUSA|SLES|SLUS)[0-9]{5})_[0-9]{2}-[A-Z0-9]{16}")
CUSA_CONTENT_ID_RE = re.compile(r"[A-Z]{2}[0-9]{4}-(CUSA[0-9]{5})_[0-9]{2}-[A-Z0-9]{16}")
DEFAULT_ARCHIVE_POLICY = Path(__file__).resolve().parents[1] / "archive_sources.json"
# Resolve the sibling explicitly so importlib-based callers and other working
# directories use this project's policy reader without changing sys.path.
_archive_policy_spec = importlib.util.spec_from_file_location(
    "peppy_archive_policy", Path(__file__).resolve().with_name("archive_policy.py"))
_archive_policy_module = importlib.util.module_from_spec(_archive_policy_spec)
_archive_policy_spec.loader.exec_module(_archive_policy_module)
load_archive_policy = _archive_policy_module.load_archive_policy
GAMEBATO_SOURCE_URL = "https://gamebatoapp.ir/home/en/"
GAMEBATO_DOWNLOAD_URL = "https://gamebatoapp.ir/home/app.pkg"
GAMEBATO_CONTENT_ID = "XX0000-GBTX00001_00-GBTXXXXXXXXXXXXX"
# Third-party mirrors are reviewed individually and never become official sources.
REVIEWED_GITHUB_MIRRORS = {
    "https://raw.githubusercontent.com/Niklas080208/ps4-aio-apps/bf32bd6cf497ee9070f3f2f962c63ed19330e8bc/pkgs/itemzflow.pkg": {
        "source": "Niklas080208/ps4-aio-apps",
        "source_url": "https://github.com/Niklas080208/ps4-aio-apps/blob/bf32bd6cf497ee9070f3f2f962c63ed19330e8bc/apps.json",
        "content_id": "IV0002-ITEM00001_00-STOREUPD00000000",
        "size_bytes": 27131904,
    },
}
# These PS1/PS2 conversions have reviewed PS4-container headers. Their donor
# Title IDs do not make them native PS4 games or authorize other legacy IDs.
# Sizes are the observed header/HTTP totals in portuguese-wrapper-probes.json.
REVIEWED_ARCHIVE_CONVERSIONS = {
    "o-bom-de-guerra-1": {
        "UP9000-SCUS97399_00-SCUS973990000001": ("PS2 conversion for PS4", 3279749120),
    },
    "parasite-eve-ii-pt-br": {
        "UP9000-SLUS01042_00-SLUS010420000000": ("PS1 conversion for PS4", 976355328),
        "UP9000-CUSA00927_00-SCUS942400000000": ("PS1 conversion for PS4", 411697152),
        "UP9000-CUSA00755_00-SCUS942400000000": ("PS1 conversion for PS4", 944766976),
        "UP9000-CUSA00924_00-SCUS942400000000": ("PS1 conversion for PS4", 468910080),
        "UP9000-CUSA01088_00-SCUS942400000000": ("PS1 conversion for PS4", 245760000),
        "UP9000-CUSA00925_00-SCUS942400000000": ("PS1 conversion for PS4", 397803520),
        "UP9000-CUSA14511_00-SCUS942400000000": ("PS1 conversion for PS4", 248578048),
    },
}
REMOTE_CONTENT_ID_RE = re.compile(r"(?<![A-Z0-9])[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}(?![A-Z0-9_])")
REMOTE_TITLE_ID_RE = re.compile(r"(?<![A-Z0-9])[A-Z]{4}[0-9]{5}(?![A-Z0-9])")

# Add a repository only after checking its author's official release metadata.
VERIFIED_REPOSITORIES = frozenset({
    "bucanero/apollo-ps4",
    "cy33hc/ps4-ezremote-client",
    "LightningMods/PS4-Store",
    "0x199/ps4-ipi",
    "Backporter/ps4_remote_pkg_installer-OOSDK",
    "gen04177/freedoom-ps4",
    "gen04177/rarch-ps4",
    "buckethatboy3/pplay-Ps4-Media-Player",
    "Cpasjuste/pemu",
    "ItsJokerZz/FPKGi",
    "bucanero/PS4CheatsManager",
    "EmiiBytee/np2kai-ps4",
    "bizkut/ps4-mgba",
    "Mayo1970/ioQuake3-PS4",
    "kalaposfos13/ps4-homebrew-base",
    "GoldHEN/GoldHEN_Cheat_Manager",
    "Ninedark9/PS4PackageLink",
})

TEXT_FIELDS = (
    ("id", "id"),
    ("name", "name"),
    ("caption", "caption"),
    ("description", "description"),
    ("version", "version"),
    ("developer", "developer"),
    ("firmware", "firmware"),
    ("release_url", "releaseUrl"),
    ("url", "url"),
    ("filename", "filename"),
    ("sha256", "sha256"),
    ("requires_data", "requiresData"),
    ("source_badge", "sourceBadge"),
    ("content_id", "contentId"),
)


def text_field(entry: dict, name: str, allow_empty: bool = False) -> str:
    value = entry.get(name)
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        raise ValueError(f"{name} must be a {'possibly empty ' if allow_empty else 'nonempty '}string")
    value.encode("utf-8", errors="strict")
    if any(ord(char) < 32 and char not in "\n\r\t" for char in value):
        raise ValueError(f"{name} contains a forbidden control character")
    if "\x7f" in value:
        raise ValueError(f"{name} contains a forbidden control character")
    return value


def github_path(value: str, name: str) -> list[str]:
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or parsed.netloc != "github.com"
            or parsed.query or parsed.fragment):
        raise ValueError(f"{name} must be a plain HTTPS github.com URL")
    if not parsed.path.startswith("/"):
        raise ValueError(f"{name} has an invalid GitHub path")
    return parsed.path[1:].split("/")


def plain_https_url(value: str, name: str):
    """Accept metadata URL syntax, without making any network claim."""
    if len(value) >= 4096 or any(ord(char) <= 32 or ord(char) >= 127 for char in value):
        raise ValueError(f"{name} must be an ASCII HTTPS URL without whitespace or controls")
    if "\\" in value or "#" in value or re.search(r"%(?![0-9A-Fa-f]{2})", value):
        raise ValueError(f"{name} contains a fragment, backslash, or invalid percent encoding")
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or not parsed.hostname or parsed.netloc != parsed.hostname
            or not parsed.path.startswith("/")):
        raise ValueError(f"{name} must use HTTPS without credentials or a port")
    if len(parsed.hostname) > 253 or any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?", label)
                                        or len(label) > 63 for label in parsed.hostname.split(".")):
        raise ValueError(f"{name} must have a valid ASCII hostname")
    return parsed


def validate_official(entry: dict) -> None:
    source = text_field(entry, "source")
    if source not in VERIFIED_REPOSITORIES:
        raise ValueError(f"source is not a verified official repository: {source}")
    if entry.get("repository", source) != source:
        raise ValueError("source and repository disagree")
    parts = github_path(entry["url"], "url")
    if len(parts) != 6 or parts[2:4] != ["releases", "download"]:
        raise ValueError("url must point to a pinned GitHub release PKG asset")
    if "/".join(parts[:2]) != source:
        raise ValueError("url repository must match source")
    tag, asset = unquote(parts[4]), unquote(parts[5])
    if not tag or tag.lower() == "latest" or tag != entry["version"]:
        raise ValueError("url release tag must be pinned and match version")
    if asset != entry["filename"]:
        raise ValueError("url asset filename must match filename")
    release_parts = github_path(entry["release_url"], "release_url")
    if (len(release_parts) != 5 or release_parts[2:4] != ["releases", "tag"]
            or "/".join(release_parts[:2]) != source
            or unquote(release_parts[4]) != tag):
        raise ValueError("release_url must match the pinned asset repository and tag")
    entry["source_badge"] = "PKG / FONTE OFICIAL"


def validate_reviewed_base(entry: dict, legacy_title_ids: bool = True,
                          required_content_id: str | None = None) -> str:
    """Validate catalog header evidence without claiming a full PKG check."""
    source_kind = entry["source_kind"]
    if entry.get("platform") != "PS4" or entry.get("package_kind") != "base":
        raise ValueError(f"{source_kind} requires a PS4 base package")
    content_id = text_field(entry, "content_id")
    content_id_re = CONTENT_ID_RE if legacy_title_ids else CUSA_CONTENT_ID_RE
    if required_content_id is not None:
        if content_id != required_content_id:
            raise ValueError(f"{source_kind} content_id must match the reviewed provider's exact Content ID")
    elif not content_id_re.fullmatch(content_id):
        prefixes = "CUSA, SLES, or SLUS" if legacy_title_ids else "CUSA"
        raise ValueError(f"{source_kind} content_id must be a canonical {prefixes} Content ID")
    proof = entry.get("header_verification")
    if not isinstance(proof, dict):
        raise ValueError(f"{source_kind} requires explicit reviewed PKG header_verification")
    if (proof.get("magic") != "7f434e54" or type(proof.get("content_type")) is not int
            or proof["content_type"] != 0x1A or type(proof.get("content_flags")) is not int
            or proof["content_flags"] not in (0x0A000000, 0x0E000000)):
        raise ValueError("header_verification must identify a PS4 game base package, without patch flags")
    if (type(proof.get("package_size")) is not int or proof["package_size"] != entry.get("size_bytes")
            or proof.get("content_id") != content_id):
        raise ValueError("header_verification package_size and content_id must match the catalog metadata exactly")
    filename = entry["filename"]
    if not filename.startswith(content_id) or not re.fullmatch(re.escape(content_id) + r"(?:-[A-Za-z0-9_.-]+)?\.pkg", filename):
        raise ValueError("external filename must begin with its declared Content ID")
    return content_id


def validate_remote_identity(remote_name: str, content_id: str) -> None:
    """Filename identities cannot contradict the reviewed binary header."""
    remote_ids = REMOTE_CONTENT_ID_RE.findall(remote_name)
    if remote_ids:
        if any(value != content_id for value in remote_ids):
            raise ValueError("remote PKG basename contains a different Content ID")
    else:
        remote_titles = REMOTE_TITLE_ID_RE.findall(remote_name)
        if any(value != content_id[7:16] for value in remote_titles):
            raise ValueError("remote PKG basename contains a different Title ID")
        # Generic names require the reviewed header evidence; the emitted
        # contentId also pins the binary header during the native download.


def validate_mediafire(entry: dict) -> None:
    content_id = validate_reviewed_base(entry)
    label = text_field(entry, "source_label")
    if len(label) > 30 or any(ord(char) < 32 for char in label) or re.search(r"\b(?:oficial|official)\b", label, re.IGNORECASE):
        raise ValueError("external source_label must be a single line of at most 30 characters without an official-source claim")
    provenance = plain_https_url(text_field(entry, "source_url"), "source_url")
    if text_field(entry, "source") != provenance.hostname:
        raise ValueError("external source must equal the source_url provenance hostname")
    if "repository" in entry:
        raise ValueError("external provenance must not claim an official repository")
    if entry["release_url"] != entry["source_url"]:
        raise ValueError("external release_url must equal its source_url provenance page")
    parsed = plain_https_url(entry["url"], "url")
    if parsed.netloc != "www.mediafire.com" or parsed.query:
        raise ValueError("external URL must be a plain HTTPS www.mediafire.com file page")
    parts = parsed.path.split("/")
    if (len(parts) != 5 or parts[0] or parts[1] != "file" or parts[4] != "file"
            or not re.fullmatch(r"[A-Za-z0-9]{1,64}", parts[2])):
        raise ValueError("external URL must use /file/<key>/<encoded-pkg-basename>/file")
    remote_name = unquote(parts[3], errors="strict")
    if (not remote_name.endswith(".pkg") or any(ord(char) < 32 or ord(char) == 127 for char in remote_name)
            or "/" in remote_name or "\\" in remote_name):
        raise ValueError("MediaFire page must name one complete PKG, not HTML or an archive")
    validate_remote_identity(remote_name, content_id)
    entry["source_badge"] = "PKG / " + label
    if len(entry["source_badge"]) > 52:
        raise ValueError("source badge must contain at most 52 characters")


def validate_archive_direct(entry: dict, archive_policy: dict | None = None) -> None:
    """Pin reviewed collections, base-header evidence and conversion origins."""
    if archive_policy is None:
        archive_policy = load_archive_policy(DEFAULT_ARCHIVE_POLICY)
    provenance = plain_https_url(text_field(entry, "source_url"), "source_url")
    source_parts = provenance.path.split("/")
    if (provenance.netloc != "archive.org" or "?" in entry["source_url"]
            or len(source_parts) != 3 or source_parts[:2] != ["", "details"]
            or source_parts[2] not in archive_policy["collections"]):
        raise ValueError("archive provenance must name an exact reviewed collection from archive_sources.json")
    collection = source_parts[2]
    conversions = REVIEWED_ARCHIVE_CONVERSIONS.get(collection)
    if conversions is not None:
        content_id = text_field(entry, "content_id")
        conversion = conversions.get(content_id)
        if conversion is None:
            raise ValueError("archive conversion Content ID must match the individually reviewed collection package")
        origin, size = conversion
        if entry.get("content_origin") != origin or entry.get("size_bytes") != size:
            raise ValueError("archive conversion requires its reviewed content_origin and exact package size")
        content_id = validate_reviewed_base(entry, required_content_id=content_id)
    else:
        if entry.get("content_origin", "native PS4") != "native PS4":
            raise ValueError("archive conversions require an individually reviewed conversion collection and Content ID")
        content_id = validate_reviewed_base(entry, legacy_title_ids=False)
    if (text_field(entry, "source") != "archive.org"
            or text_field(entry, "source_label") != "INTERNET ARCHIVE"
            or entry["release_url"] != entry["source_url"] or "repository" in entry):
        raise ValueError("archive provenance must use the reviewed collection and INTERNET ARCHIVE label without an official repository claim")
    parsed = plain_https_url(entry["url"], "url")
    if "?" in entry["url"]:
        raise ValueError("archive URL must be plain HTTPS without a query")
    parts = parsed.path.split("/")
    if parsed.netloc == "archive.org":
        correct_route = len(parts) == 4 and parts[:3] == ["", "download", collection]
    elif parsed.netloc in archive_policy["cdn_hosts"]:
        correct_route = (len(parts) == 5 and parts[0] == ""
                         and re.fullmatch(r"[0-9]{1,10}", parts[1]) is not None
                         and parts[2:4] == ["items", collection])
    else:
        raise ValueError("archive URL host must be archive.org or an exact reviewed CDN host")
    if not correct_route:
        raise ValueError("archive URL must name one PKG directly in the reviewed collection")
    raw_basename = parts[-1]
    remote_name = unquote(raw_basename, errors="strict")
    if (not raw_basename.endswith(".pkg") or len(remote_name) <= 4 or remote_name.startswith(".")
            or any(ord(char) < 32 or 127 <= ord(char) <= 159 or char in '/\\%?#"<>`'
                   for char in remote_name)):
        raise ValueError("archive URL must name a complete UTF-8 PKG basename without controls or encoded separators")
    validate_remote_identity(remote_name, content_id)
    entry["source_badge"] = "PKG / INTERNET ARCHIVE"


def validate_gamebato_direct(entry: dict) -> None:
    """Pin the reviewed mutable site installer by exact URL, CID and SHA-256."""
    validate_reviewed_base(entry, required_content_id=GAMEBATO_CONTENT_ID)
    if (text_field(entry, "source") != "gamebatoapp.ir"
            or text_field(entry, "source_label") != "GAMEBATO"
            or text_field(entry, "source_url") != GAMEBATO_SOURCE_URL
            or entry["release_url"] != GAMEBATO_SOURCE_URL
            or entry["url"] != GAMEBATO_DOWNLOAD_URL or "repository" in entry):
        raise ValueError("gamebato provenance and download must match the reviewed site installer exactly, without an official repository claim")
    digest = entry.get("sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise ValueError("gamebato's mutable URL requires a checked 64-character SHA-256 digest")
    entry["source_badge"] = "PKG / GAMEBATO"


def validate_github_mirror(entry: dict) -> None:
    """Allow a single individually reviewed commit-pinned package mirror."""
    reviewed = REVIEWED_GITHUB_MIRRORS.get(entry["url"])
    if reviewed is None:
        raise ValueError("github_mirror URL must match an individually reviewed commit-pinned package")
    validate_reviewed_base(entry, required_content_id=reviewed["content_id"])
    if (text_field(entry, "source") != reviewed["source"]
            or text_field(entry, "source_label") != "ESPELHO NÃO OFICIAL"
            or text_field(entry, "source_url") != reviewed["source_url"]
            or entry["release_url"] != reviewed["source_url"]
            or entry["size_bytes"] != reviewed["size_bytes"] or "repository" in entry):
        raise ValueError("github_mirror provenance, size and nonofficial label must match the reviewed mirror without an official repository claim")
    digest = entry.get("sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise ValueError("github_mirror requires a checked 64-character SHA-256 digest")
    entry["source_badge"] = "PKG / ESPELHO NÃO OFICIAL"


def validate_reviewed_direct(entry: dict, archive_policy: dict | None = None) -> None:
    provider = entry.get("provider")
    if provider == "archive":
        validate_archive_direct(entry, archive_policy)
    elif provider == "gamebato":
        validate_gamebato_direct(entry)
    elif provider == "github_mirror":
        validate_github_mirror(entry)
    else:
        raise ValueError("reviewed_direct requires an explicitly reviewed archive, gamebato or github_mirror provider")


def validate_entry(entry: dict, index: int, seen_ids: set[str], seen_filenames: set[str] | None = None,
                   archive_policy: dict | None = None) -> dict:
    if not isinstance(entry, dict):
        raise ValueError(f"entry {index} must be an object")
    entry = dict(entry)
    source_kind = entry.get("source_kind", "official")
    if source_kind not in ("official", "external_mediafire", "reviewed_direct"):
        raise ValueError(f"unsupported source_kind: {source_kind!r}")
    entry["source_kind"] = source_kind
    if source_kind != "official":
        entry.setdefault("release_url", entry.get("source_url"))
    else:
        entry.setdefault("content_id", "")
    for field, _ in TEXT_FIELDS:
        if field in ("sha256", "source_badge"):
            continue
        text_field(entry, field, allow_empty=(field == "requires_data"
                                           or (field == "content_id" and source_kind == "official")))

    app_id = entry["id"]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", app_id):
        raise ValueError("id must contain only ASCII letters, digits, hyphens or underscores")
    if app_id in seen_ids:
        raise ValueError(f"duplicate id: {app_id}")

    if not isinstance(entry.get("category"), str) or entry["category"] not in CATEGORIES:
        raise ValueError(f"unsupported category: {entry.get('category')!r}")
    if len(entry["caption"]) > 30:
        raise ValueError("caption must contain at most 30 characters")
    filename = entry["filename"]
    if len(filename) > 95 or ".." in filename or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*\.pkg", filename):
        raise ValueError("filename must be a safe ASCII basename of at most 95 characters ending in .pkg")
    if seen_filenames is not None and filename in seen_filenames:
        raise ValueError(f"duplicate filename: {filename}")

    if source_kind == "official":
        validate_official(entry)
    elif source_kind == "external_mediafire":
        validate_mediafire(entry)
    else:
        validate_reviewed_direct(entry, archive_policy)

    size = entry.get("size_bytes")
    if type(size) is not int or not 0x438 <= size <= MAX_PACKAGE_BYTES:
        raise ValueError("size_bytes must be an exact integer from PKG header size through 256 GiB")
    digest = entry.get("sha256")
    if digest is None:
        entry["sha256"] = ""
    elif not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise ValueError("sha256 must be absent, null, or exactly 64 hexadecimal characters")

    art = entry.get("art")
    if type(art) is not int or not 0 <= art <= 3:
        raise ValueError("art must be an integer from 0 to 3")
    seen_ids.add(app_id)
    if seen_filenames is not None:
        seen_filenames.add(filename)
    return entry


def read_catalog(catalog_path: Path, archive_policy_path: Path | None = None) -> list[dict]:
    with catalog_path.open("r", encoding="utf-8", errors="strict") as stream:
        catalog = json.load(stream)
    if (not isinstance(catalog, dict) or type(catalog.get("schema_version")) is not int
            or catalog["schema_version"] != 1):
        raise ValueError("catalog must be an object with schema_version 1")
    entries = catalog.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("catalog entries must be a nonempty array")
    seen_ids: set[str] = set()
    seen_filenames: set[str] = set()
    validated = []
    archive_policy = None
    for index, entry in enumerate(entries, 1):
        try:
            if (isinstance(entry, dict) and entry.get("source_kind") == "reviewed_direct"
                    and entry.get("provider") == "archive" and archive_policy is None):
                archive_policy = load_archive_policy(archive_policy_path or DEFAULT_ARCHIVE_POLICY)
            validated.append(validate_entry(entry, index, seen_ids, seen_filenames, archive_policy))
        except (ValueError, UnicodeError) as error:
            raise ValueError(f"entry {index}: {error}") from error
    return validated


def cpp_string(value: str) -> str:
    # JSON handles quotes, slashes and newline/tab escapes. Escape question marks
    # as well so older C++ modes cannot reinterpret trigraphs inside literals.
    return json.dumps(value, ensure_ascii=False).replace("?", "\\?")


def generate_header(entries: list[dict]) -> str:
    lines = [
        "// Generated by native/scripts/generate-catalog.py; do not edit.",
        "// Metadata evidence remains in native/catalog.json.",
        "#ifndef PEPPY_UI_CATALOG_H",
        "#define PEPPY_UI_CATALOG_H",
        "",
        "#include <stdint.h>",
        "",
        "struct UiApp {",
        "    const char* id;",
        "    const char* name;",
        "    const char* caption;",
        "    const char* description;",
        "    const char* version;",
        "    const char* developer;",
        "    const char* firmware;",
        "    const char* releaseUrl;",
        "    const char* url;",
        "    const char* filename;",
        "    const char* sha256;",
        "    const char* requiresData;",
        "    const char* sourceBadge;",
        "    const char* contentId;",
        "    uint64_t sizeBytes;",
        "    int category, art;",
        "};",
        "",
        "// Categories: 0 all (filter only), 1 utilities, 2 emulators,",
        "// 3 games (including homebrew), 4 media. Art: 0 Apollo, 1 games/emulators,",
        "// 2 file tools/installers, 3 store/media.",
        "static const UiApp UI_APPS[] = {",
    ]
    for entry in entries:
        lines.append("    {")
        lines.extend(f"        {cpp_string(entry[field])}," for field, _ in TEXT_FIELDS)
        lines.append(f"        {entry['size_bytes']}ULL, {CATEGORIES[entry['category']]}, {entry['art']}")
        lines.append("    },")
    lines.extend([
        "};",
        "static const int UI_APP_COUNT = static_cast<int>(sizeof(UI_APPS) / sizeof(UI_APPS[0]));",
        "",
        "#endif // PEPPY_UI_CATALOG_H",
        "",
    ])
    return "\n".join(lines)


def write_header(output_path: Path, content: str) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", newline="\n", dir=output_path.parent,
            prefix=output_path.name + ".", suffix=".tmp", delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(content)
        os.replace(temporary_path, output_path)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path, help="verified UTF-8 catalog.json")
    parser.add_argument("--output", required=True, type=Path, help="destination ui_catalog.h")
    parser.add_argument("--archive-policy", type=Path, help="reviewed archive_sources.json; defaults to this project's native file")
    args = parser.parse_args()
    try:
        if args.catalog.resolve() == args.output.resolve():
            raise ValueError("--output must be different from --catalog")
        entries = read_catalog(args.catalog, args.archive_policy)
        write_header(args.output, generate_header(entries))
    except (OSError, ValueError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Generated {args.output}: {len(entries)} validated PS4 catalog entries.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
