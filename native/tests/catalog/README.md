# Catalog provenance regression checks

```sh
python3 native/tests/catalog/catalog_test.py
```

The checks preserve the fifteen official GitHub release entries and their
repository, pinned version, filename, and release-page validation. They also
cover reviewed `external_mediafire` entries for PS4 base packages, provenance
badges, canonical CUSA/SLES/SLUS Content IDs, remote/local filename agreement, exact
64-bit sizes, SHA-256 syntax, duplicate cache paths, and malformed URLs.

Catalog schema remains version 1. Entries without `source_kind` use the
official GitHub path. External entries keep the common presentation fields
and additionally require:

- `source_kind: "external_mediafire"`, `platform: "PS4"`, `package_kind: "base"`;
- `content_id`, a canonical 36-character CUSA, SLES, or SLUS Content ID;
- `header_verification`, recording reviewed `magic: "7f434e54"`, integer
  `content_type: 26`, integer `content_flags: 167772160` or `234881024`,
  `package_size` equal to `size_bytes`, and `content_id` equal to the entry;
- `source`, the hostname of the HTTPS `source_url` provenance page;
- `source_label`, a single line up to 30 characters without an official-source claim;
- `url`, a pinned `https://www.mediafire.com/file/<alphanumeric-key>/<encoded-PKG-basename>/file` page;
- `filename`, an ASCII cache basename up to 95 characters beginning with the declared Content ID;
- `size_bytes`, an exact integer between the PKG header size and 256 GiB;
- optional published `sha256`, exactly 64 hexadecimal characters when present.

The decoded MediaFire basename must agree with the header's canonical Content
ID when it contains one, or with its nine-character Title ID when only that
identifier is present. Generic filenames are permitted with the explicit
header evidence; the generated `contentId` supports a further comparison with
the header received by the native downloader. Prefixes such as
`[DLPSGAME.COM]` and version suffixes can differ from the local cache basename.
`release_url` defaults to `source_url` and must match it
if supplied. `repository` is reserved for official entries. The generated
`sourceBadge` is derived from provenance and ignores an arbitrary input badge.
`Jogos` and the existing `Jogos homebrew` category both map to category 3.

The generator makes no network requests and does not verify full package
structures, signatures, publisher identity, or firmware compatibility. The
source metadata, header, exact size, and base-package classification must be
reviewed before adding a real entry to `catalog.json`.
