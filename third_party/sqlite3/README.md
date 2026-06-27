# Vendored SQLite

- **Version:** 3.45.3 (amalgamation)
- **Source:** https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip
- **License:** Public Domain (https://www.sqlite.org/copyright.html)
- **Files:** `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h`

Vendored intentionally instead of pulling SQLite via Conan: the amalgamation is a
single public-domain translation unit that builds identically on Linux, macOS,
and Windows as a **static** library, eliminating shared-library / runtime-DLL
resolution differences across platforms. Built by `CMakeLists.txt` here as
`SQLite::SQLite3`.

To update: download the new amalgamation zip, replace the three files, bump the
version above.
