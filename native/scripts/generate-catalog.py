#!/usr/bin/env python3
"""Validate the verified PS4 catalog and generate its UTF-8 C++ header."""

from __future__ import annotations

import argparse
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
    "Mídia": 4,
}

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


def validate_entry(entry: dict, index: int, seen_ids: set[str]) -> dict:
    if not isinstance(entry, dict):
        raise ValueError(f"entry {index} must be an object")
    entry = dict(entry)
    for field, _ in TEXT_FIELDS:
        if field == "sha256":
            continue
        text_field(entry, field, allow_empty=field == "requires_data")

    app_id = entry["id"]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", app_id):
        raise ValueError("id must contain only ASCII letters, digits, hyphens or underscores")
    if app_id in seen_ids:
        raise ValueError(f"duplicate id: {app_id}")
    seen_ids.add(app_id)

    if not isinstance(entry.get("category"), str) or entry["category"] not in CATEGORIES:
        raise ValueError(f"unsupported category: {entry.get('category')!r}")
    if len(entry["caption"]) > 30:
        raise ValueError("caption must contain at most 30 characters")
    filename = entry["filename"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*\.pkg", filename):
        raise ValueError("filename must be a safe ASCII basename ending in .pkg")

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
    if asset != filename:
        raise ValueError("url asset filename must match filename")

    release_parts = github_path(entry["release_url"], "release_url")
    if (len(release_parts) != 5 or release_parts[2:4] != ["releases", "tag"]
            or "/".join(release_parts[:2]) != source
            or unquote(release_parts[4]) != tag):
        raise ValueError("release_url must match the pinned asset repository and tag")

    size = entry.get("size_bytes")
    if type(size) is not int or not 0 < size <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("size_bytes must be a positive uint64 integer")
    digest = entry.get("sha256")
    if digest is None:
        entry["sha256"] = ""
    elif not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise ValueError("sha256 must be absent, null, or exactly 64 hexadecimal characters")

    art = entry.get("art")
    if type(art) is not int or not 0 <= art <= 3:
        raise ValueError("art must be an integer from 0 to 3")
    return entry


def read_catalog(catalog_path: Path) -> list[dict]:
    with catalog_path.open("r", encoding="utf-8", errors="strict") as stream:
        catalog = json.load(stream)
    if (not isinstance(catalog, dict) or type(catalog.get("schema_version")) is not int
            or catalog["schema_version"] != 1):
        raise ValueError("catalog must be an object with schema_version 1")
    entries = catalog.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("catalog entries must be a nonempty array")
    seen_ids: set[str] = set()
    validated = []
    for index, entry in enumerate(entries, 1):
        try:
            validated.append(validate_entry(entry, index, seen_ids))
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
        "    uint64_t sizeBytes;",
        "    int category, art;",
        "};",
        "",
        "// Categories: 0 all (filter only), 1 utilities, 2 emulators,",
        "// 3 homebrew games, 4 media. Art: 0 Apollo, 1 games/emulators,",
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
    args = parser.parse_args()
    try:
        if args.catalog.resolve() == args.output.resolve():
            raise ValueError("--output must be different from --catalog")
        entries = read_catalog(args.catalog)
        write_header(args.output, generate_header(entries))
    except (OSError, ValueError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Generated {args.output}: {len(entries)} verified PS4 packages.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
