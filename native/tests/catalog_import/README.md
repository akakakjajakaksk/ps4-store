# Local PKG inspection

Run the metadata regression checks without downloading anything:

```sh
python3 native/tests/catalog_import/inspect_test.py
```

Inspect a complete file and write its metadata for later catalog review:

```sh
python3 native/scripts/inspect-pkg.py "/path/to/user-file.pkg" --output report.json
python3 native/scripts/inspect-pkg.py "/path/to/user-file.pkg" --url "https://example.org/files/game.pkg"
```

The inspector holds one regular, non-symlink file descriptor, reads the PKG
magic and canonical Content ID, and hashes the exact file in chunks of at
most 1 MiB. For CUSA/PS4 files it first checks that the big-endian package
size at header offset `0x430` equals the exact local file size and is at least
`0x438` bytes. A size mismatch is refused before hashing. It refuses files
changed or replaced during inspection. The
original basename is preserved separately from a proposed ASCII basename.
PPSA identifiers are reported as PS5 and refused for the PS4 catalog. CUSA
identifies PS4. Other prefixes, including homebrew IDs, remain `unknown` and
are refused pending explicit platform evidence; an arbitrary identifier does
not establish that a package belongs to PS4.

The PS4 header content type at `0x74` and flags at `0x78` identify a conservative
base-application candidate. Type GD (`0x1A`) alone does not prove a base app:
documented patch flags override it. The supported base flag patterns are
`0x0A000000` and `0x0E000000`, containing GD_AC and the observed
`Unk_x8000000` bit, with optional NON_GAME. Other patterns remain `unknown`.
DP (`0x1E`) is a patch; AC/AL (`0x1B`/`0x1C`) are add-on/theme package types.
Patches, add-ons, and unknown kinds are refused for the base-application-only
catalog, retaining the checked file hash and proposed filename for review.
Platform refusals also retain those checked fields; failed local file checks
never do.

Header offsets and flags follow the pinned reference
[LibOrbisPkg PkgReader.cs](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/PkgReader.cs#L117)
and [Enums.cs](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/Enums.cs#L20).

`importable: true` means local header and file checks passed for a PS4 base
candidate requiring catalog review. It does not validate all package structures,
signatures, firmware compatibility, or successful installation. The report
keeps `full_package_validation: false` and lists the remaining review steps.
A supplied URL is checked only for HTTPS syntax: `network_verified` always
remains false. The tool neither extracts files nor uploads or downloads them.
