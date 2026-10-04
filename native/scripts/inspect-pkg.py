#!/usr/bin/env python3
"""Inspect a local PS4 PKG header, package kind, and SHA-256 without uploading it.

This checks a held local file, not package signatures, installation compatibility,
or a remote download. An optional HTTPS URL is user supplied and remains
unverified. A successful inspection provides metadata for catalog review.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from urllib.parse import urlsplit


HEADER_BYTES = 0x438
CONTENT_ID_OFFSET = 0x40
CONTENT_ID_BYTES = 36
CONTENT_TYPE_OFFSET = 0x74
CONTENT_FLAGS_OFFSET = 0x78
PACKAGE_SIZE_OFFSET = 0x430
READ_BYTES = 1024 * 1024
CONTENT_ID_RE = re.compile(r"[A-Z]{2}[0-9]{4}-([A-Z]{4}[0-9]{5})_[0-9]{2}-[A-Z0-9]{16}")
# Pinned header/flag reference: maxton/LibOrbisPkg commit
# 643477263b2644e0803e0f58b8726ea4e3f3b7d4, PKG/Enums.cs and PkgReader.cs.
PATCH_FLAGS = 0x61300000  # FIRST_PATCH | PATCHGO | SUBSEQUENT | DELTA | CUMULATIVE
BASE_REQUIRED_FLAGS = 0x0A000000  # supported observed GD_AC + Unk_x8000000 pattern
BASE_ALLOWED_FLAGS = BASE_REQUIRED_FLAGS | 0x04000000  # optionally NON_GAME


class InspectionError(ValueError):
    pass


def validate_url(value: str) -> str:
    """Validate URL syntax only; no request or redirect is performed."""
    if not value or len(value) >= 4096 or any(ord(char) <= 32 or ord(char) >= 127 for char in value):
        raise InspectionError("download URL must be a nonempty ASCII URL without whitespace or controls")
    if "\\" in value or "#" in value:
        raise InspectionError("download URL must not contain backslashes or fragments")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise InspectionError("download URL has an invalid host or port") from error
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or port not in (None, 443) or parsed.fragment):
        raise InspectionError("download URL must use HTTPS without credentials, a fragment, or a non-443 port")
    if not parsed.path.startswith("/") or parsed.path == "/":
        raise InspectionError("download URL must name a resource path, not only a website")
    if len(parsed.hostname) > 253 or any(not label or len(label) > 63 or not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?", label)
           for label in parsed.hostname.split(".")):
        raise InspectionError("download URL must have a valid ASCII hostname")
    if re.search(r"%(?![0-9A-Fa-f]{2})", parsed.path + parsed.query):
        raise InspectionError("download URL contains invalid percent encoding")
    return value


def file_identity(info: os.stat_result) -> tuple[int, ...]:
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def ps4_package_kind(content_type: int, content_flags: int) -> str:
    """Conservatively interpret known PS4 types; unknown flags never imply base."""
    if content_type == 0x1E:
        return "patch"
    if content_type in (0x1B, 0x1C):
        return "addon"
    if content_type != 0x1A:
        return "unknown"
    # GD is also used by patches. Binary patch flags override filename labels.
    if content_flags & PATCH_FLAGS:
        return "patch"
    if (content_flags & BASE_REQUIRED_FLAGS == BASE_REQUIRED_FLAGS
            and not content_flags & ~BASE_ALLOWED_FLAGS):
        return "base"
    return "unknown"


def inspect_pkg(path: Path, url: str | None = None) -> dict:
    """Return a JSON-compatible report, including reasons for refused inputs."""
    path = Path(path)
    report = {
        "original_filename": path.name,
        "platform": "unknown",
        "package_kind": "unknown",
        "importable": False,
        "validation_scope": "local_header_identifier_declared_size_kind_and_sha256",
        "full_package_validation": False,
        "network_verified": False,
    }
    descriptor = None
    file_checked = False
    try:
        if url is not None:
            report["url"] = validate_url(url)
            report["url_source"] = "user_supplied"
        if not hasattr(os, "O_NOFOLLOW"):
            raise InspectionError("this operating system cannot open the input without following symlinks")
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0))
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise InspectionError("input must be a regular file; devices, directories, and pipes are refused")
        report["size_bytes"] = before.st_size
        if before.st_size < HEADER_BYTES:
            raise InspectionError(f"file is truncated: a PKG header needs at least {HEADER_BYTES} bytes")
        header = bytearray()
        while len(header) < HEADER_BYTES:
            part = os.read(descriptor, HEADER_BYTES - len(header))
            if not part:
                raise InspectionError("file changed or ended while reading the PKG header")
            header.extend(part)
        if header[:4] != b"\x7fCNT":
            raise InspectionError("file does not have PKG magic; archives and password-protected downloads must be unpacked first")
        raw_id = bytes(header[CONTENT_ID_OFFSET:CONTENT_ID_OFFSET + CONTENT_ID_BYTES])
        try:
            content_id = raw_id.decode("ascii", errors="strict")
        except UnicodeError as error:
            raise InspectionError("Content ID must contain only canonical ASCII characters") from error
        match = CONTENT_ID_RE.fullmatch(content_id)
        if not match:
            raise InspectionError("Content ID does not have the canonical 36-character package format")
        # PS4/PS5 identification is based only on this header identifier. It
        # does not prove all package structures are complete, signed, or installable.
        title_id = match.group(1)
        report["content_id"] = content_id
        report["title_id"] = title_id
        if title_id.startswith("PPSA"):
            report["platform"] = "PS5"
        elif title_id.startswith("CUSA"):
            report["platform"] = "PS4"
        report["platform_basis"] = ("content_id_title_prefix" if report["platform"] != "unknown"
                                    else "unrecognized_content_id_title_prefix")
        content_type = int.from_bytes(header[CONTENT_TYPE_OFFSET:CONTENT_TYPE_OFFSET + 4], "big")
        content_flags = int.from_bytes(header[CONTENT_FLAGS_OFFSET:CONTENT_FLAGS_OFFSET + 4], "big")
        report["content_type"] = f"0x{content_type:08X}"
        report["content_flags"] = f"0x{content_flags:08X}"
        if report["platform"] == "PS4":
            declared_size = int.from_bytes(header[PACKAGE_SIZE_OFFSET:PACKAGE_SIZE_OFFSET + 8], "big")
            report["header_package_size"] = declared_size
            if declared_size < HEADER_BYTES:
                raise InspectionError("PS4 header package size is zero or smaller than the PKG header")
            if declared_size != before.st_size:
                raise InspectionError("PS4 header package size does not match the complete local file size")
            report["package_kind"] = ps4_package_kind(content_type, content_flags)
        digest = hashlib.sha256(header)
        received = len(header)
        while True:
            part = os.read(descriptor, READ_BYTES)
            if not part:
                break
            received += len(part)
            if received > before.st_size:
                raise InspectionError("file changed size during inspection; report discarded")
            digest.update(part)
        after = os.fstat(descriptor)
        current_path = os.stat(path, follow_symlinks=False)
        if (received != before.st_size or file_identity(before) != file_identity(after)
                or not stat.S_ISREG(current_path.st_mode)
                or file_identity(current_path) != file_identity(after)):
            raise InspectionError("file changed or the path was replaced during inspection; report discarded")
        report["sha256"] = digest.hexdigest()
        # A deterministic ASCII basename preserves the original separately
        # and avoids collisions between releases sharing the same Content ID.
        report["filename"] = f"{content_id}-{report['sha256'][:12]}.pkg"
        file_checked = True
        if report["platform"] == "PS5":
            raise InspectionError("PPSA identifies a PS5 package; it cannot be imported into the PS4 catalog")
        if report["platform"] == "unknown":
            report["catalog_review_required"] = ["platform_evidence", "complete_ps4_base_package", "verified_download_url"]
            raise InspectionError("Content ID uses an unknown platform prefix; PS4 platform evidence is required before import")
        if report["package_kind"] != "base":
            report["catalog_review_required"] = ["complete_ps4_base_package", "verified_download_url"]
            raise InspectionError(f"PS4 package kind is {report['package_kind']}; only known base applications can be imported")
        report["importable"] = True
        report["catalog_review_required"] = ["full_package_integrity", "verified_download_url", "firmware_compatibility"]
    except (InspectionError, OSError) as error:
        report["reason"] = str(error)
        # Platform/kind refusals keep checked metadata for review. A file which
        # failed local checks must never retain a digest or proposed filename.
        if not file_checked:
            report.pop("sha256", None)
            report.pop("filename", None)
    finally:
        if descriptor is not None:
            os.close(descriptor)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pkg", type=Path, help="complete local PKG file to inspect")
    parser.add_argument("--url", help="optional user-supplied direct HTTPS URL; syntax is checked, download is not")
    parser.add_argument("--output", type=Path, help="write JSON to this file instead of standard output")
    args = parser.parse_args()
    report = inspect_pkg(args.pkg, args.url)
    content = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    temporary = None
    try:
        if args.output:
            if args.output.resolve() == args.pkg.resolve():
                raise ValueError("--output must differ from the input PKG")
            if args.output.exists() and args.pkg.exists() and os.path.samefile(args.output, args.pkg):
                raise ValueError("--output must not be an alias of the input PKG")
            # Replace the report pathname atomically; never follow an existing
            # output symlink or truncate a file through a hard-link alias.
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", dir=args.output.parent,
                                             prefix=args.output.name + ".", suffix=".tmp", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(content)
            os.replace(temporary, args.output)
            temporary = None
        else:
            sys.stdout.write(content)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return 0 if report["importable"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
