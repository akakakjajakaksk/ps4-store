# Catalog search tests

Run from the project root:

```sh
sh native/tests/catalog_search/run.sh
SANITIZE=1 sh native/tests/catalog_search/run.sh
```

The runner builds the header-only search and editor helpers as pure C++11 with
optimization, fortified libc checks and warnings as errors. `SANITIZE=1` also
enables AddressSanitizer and UndefinedBehaviorSanitizer. Set `CXX` to choose a
compiler. Leak detection defaults off because LeakSanitizer cannot run under the
managed environment's ptrace supervision; set `ASAN_OPTIONS` to override this
default on another host. Temporary binaries are removed on exit.

Fixtures cover case, Portuguese accent folding and decomposed accents in fields
and queries, AND
matching across names, title IDs and Content IDs, whitespace, empty/null input,
negative results, fields longer than display buffers, queries longer than the
editor limit and malformed/truncated UTF-8. Editor checks cover every keyboard
character, four-row navigation and
wrapping, changed-state return values, draft/committed query isolation,
apply/cancel, the 64-byte limit and erasing complete UTF-8 code points.
