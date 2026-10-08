# Runtime user catalog

`user_catalog.h/.cpp` stores up to 1024 personal or remote PKG entries. The
render/controller thread owns its visible catalog. An import worker probes a
separate catalog through `UserCatalogRangeReader`; publish the completed result
only while the UI/download/install tasks are idle. These classes do not provide
concurrent mutation locks. The downloader pins Content ID, exact package size
and base/update/DLC kind with the shared `user_pkg_header.h` helpers.

An import reads at most 100000 bytes in three requests: a 1080-byte header,
up to 32768 bytes of the package entry table, and up to 65536 bytes of plaintext
`param.sfo`. `TITLE` and `APP_VER` populate the displayed title/version when
available; an encrypted/missing/over-budget SFO retains a Title ID placeholder
and unknown version. Valid SFO identity fields must match the package header.
These checks do not establish full package integrity, licensing, signatures or
firmware compatibility. A supplied full-file SHA-256 can be retained for the
downloader to verify; the probe cannot produce a full-file hash from metadata.

The callback must enforce verified HTTPS, exact Content-Range framing, public
DNS/connect destinations, supported redirects and bounded reads. The core URL
validator checks syntax and rejects IP literals/local DNS suffixes; it cannot
pin a connection or prevent DNS rebinding. The native hub adapter therefore
uses its supported public provider policy. Ordinary HTML, shorteners and RAR
archives are not direct PKG files. No website scraping, login bypass or
password-protected archive extraction is performed by the core.

Kind values match the existing FTP/installer ABI: base 0, update 1, DLC 2.
Theme detection additionally requires an AC (`0x1B`) package and IRO tag 1
(SHAREfactory) or 2 (system theme). AL (`0x1C`) remains an add-on without data;
an IRO tag does not turn it into a theme. SHAREfactory themes with no existing
dependency note receive `Tema SHAREfactory; requer SHAREfactory para usar.`
Category/rating labels and known release version text can be supplied by an
administrator's remote catalog. Labels do not override kind/header checks.
The exact category names for personal entries are `Base do usuário`,
`Update do usuário`, and `DLC do usuário`.

Only release URLs under explicitly recognized author GitHub repositories
receive recognized-author provenance. Other links retain community/unknown
provenance. The code never infers piracy or ownership from a Content ID or
claims a URL alone proves authorization. It keeps original package identities
and author art; generated filenames contain only canonical Content ID, kind,
and a safe version segment. Additional-data requirements, descriptions and
optional published hashes remain intact when persisting remote metadata.

Default storage is `/data/peppy-store/user-catalog.dat`; the caller creates its
parent directory. Schema 2 encodes fields explicitly, bounds every length,
verifies corruption checksum and metadata on load, and rejects duplicate
identities. It never serializes pointers, credentials, premium entitlements or
raw structs. Writes use a private temporary file, file fsync and atomic rename.
Failed reads preserve the current in-memory catalog. The checksum detects
corruption, not malicious authentication; signed server access belongs to the
hub backend. File/parent directory trust and power-loss filesystem durability
remain platform responsibilities.

Native persistence inspects files with `sceKernelFstat` and an explicit
120-byte PS4 metadata layout (size at offset 72); it never passes musl's
incompatible POSIX stat struct to the kernel. Symlink-safe `O_NOFOLLOW` open
replaces the unavailable OpenOrbis `lstat/fstatat`. Host checks additionally
exercise this native path with raw-offset metadata fixtures and disabled
POSIX stat wrappers.

Host checks: `sh native/tests/user_catalog/run.sh`; sanitized run with
`SANITIZE=1`. Tests include malformed headers/SFOs/UTF-8, response changes,
private redirect syntax, duplicate versions, limits, URL lists, forged storage
records, symlinks and 1500 deterministic metadata mutation cases.
