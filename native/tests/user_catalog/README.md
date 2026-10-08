# Catálogo pessoal de PKGs

Run `sh native/tests/user_catalog/run.sh` from the repository root. Use
`SANITIZE=1 sh native/tests/user_catalog/run.sh` for Address/UndefinedBehavior
sanitizers. Tests use bounded synthetic PKG/SFO byte fixtures and a range-reader
adapter; they make no Internet requests and do not install console packages.

Checks cover public HTTPS syntax, misleading author URLs, base/update/DLC/theme
header classification, mismatched Content IDs/sizes, malformed SFO tables and
UTF-8, stable response totals, optional/encrypted metadata, duplicate revisions,
1024-entry bounds, mixed URL lists, atomic persistence, corrupted/forged records
and symlinks. Full package signatures, licensing, network DNS/TLS enforcement
and actual firmware compatibility are outside these core tests.

Both runs also build the PS4 metadata adapter against an independent 120-byte
kernel-layout fixture (size at offset 72, blocks at 80, 16-bit mode/nlink). POSIX
`fstat/lstat` deliberately return ENOSYS in that build; persistence must use
`sceKernelFstat` and symlink-safe open instead. Native error preservation is
checked separately. This catches the incompatible OpenOrbis musl stat layout.
