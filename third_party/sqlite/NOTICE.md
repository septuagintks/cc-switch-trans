# SQLite 3.53.3 Source And Public-Domain Notice

ccs-trans vendors the official SQLite amalgamation version 3.53.3 as a static
C library.

## Source Evidence

- Download page: https://www.sqlite.org/download.html
- Source archive: https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip
- Archive size: 2,945,929 bytes
- Official SHA3-256: `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`
- Verified archive SHA-256: `646421e12aac110282ef8cc68f1a62d4bb15fc7b8f09da0b53e29ee690500431`
- `SQLITE_VERSION_NUMBER`: `3053003`
- `SQLITE_SOURCE_ID`: `2026-06-26 20:14:12 d4c0e51e4aeb96955b99185ab9cde75c339e2c29c3f3f12428d364a10d782c62`

Vendored file SHA-256 values:

```text
sqlite3.c     87497ab605bedd0dbee27a209c1eeff8c89b229b13f921a7efdbb81a13f779fd
sqlite3.h     4ff81af4849acabc76fc8349abb926814395072617ca18e08800abf734ab7612
sqlite3ext.h  ac9645e5c9ff0cf176efdd6e75cb5e98f46295d38e02db5c4d208826a39ab4be
```

The project compiles this source with:

```text
SQLITE_THREADSAFE=1
SQLITE_DQS=0
SQLITE_DEFAULT_FOREIGN_KEYS=1
SQLITE_ENABLE_API_ARMOR
SQLITE_LIKE_DOESNT_MATCH_BLOBS
SQLITE_OMIT_DEPRECATED
SQLITE_OMIT_LOAD_EXTENSION
```

No SQLite shell, loadable extension, FTS, or other optional module is
vendored or enabled by ccs-trans.

## Public Domain

The SQLite authors state at https://www.sqlite.org/copyright.html:

> All of the code and documentation in SQLite has been dedicated to the
> public domain by the authors. Anyone is free to copy, modify, publish, use,
> compile, sell, or distribute the original SQLite code, either in source
> code form or as a compiled binary, for any purpose, commercial or
> non-commercial, and by any means.

The amalgamation carries this blessing in place of a legal notice:

> May you do good and not evil.
>
> May you find forgiveness for yourself and forgive others.
>
> May you share freely, never taking more than you give.
