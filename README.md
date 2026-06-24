# Windows File Search Engine

A lightweight Windows file search engine written in C that indexes filenames and file paths into a local SQLite database, enabling instant searches without rescanning the filesystem on every query.

For the end-user guide, see [README-USER.md](README-USER.md).

## Overview

Windows File Search Engine recursively crawls the filesystem, stores file metadata in a SQLite database, and performs fast filename-based searches against the index.

The project uses multithreaded directory traversal to accelerate indexing and supports searching across the entire machine or within a specific drive/folder scope.

## Features

* Recursive filesystem traversal using Win32 APIs
* Multithreaded directory crawling
* SQLite-backed file indexing
* Automatic indexing of all logical drives
* Fast filename search using indexed database lookups
* Scope-limited search within a specific drive or folder
* Open search results using the default Windows application
* Automatic database creation and management

## Tech Stack

* C
* Win32 API
* SQLite3
* Windows Shell API
* Multithreading

## Architecture

```text
Filesystem
    ↓
Windows Directory Crawler
    ↓
SQLite Database (index.db)
    ↓
Filename Search Engine
    ↓
Open File Handler
```

The database is stored per user at `%LOCALAPPDATA%\WindowFileSearch\index.db`.
It is generated locally and intentionally excluded from Git, together with its
SQLite WAL, shared-memory, and journal files.

## Performance Benchmark

Measured on June 24, 2026 on Windows 11 build 26200. The machine exposes 20
logical processors; the indexer caps itself at 8 worker threads. The benchmark
indexed every logical drive visible to the process into a fresh disposable
database. Results will vary with storage, filesystem cache state, permissions,
and the number and shape of directories.

### Full-PC indexing

| Run | Cache state | Indexed files | Time | Throughput |
| --- | --- | ---: | ---: | ---: |
| 1 | Cold / first traversal | 368,971 | 24.757 s | 14,904 files/s |
| 2 | Warm filesystem cache | 368,971 | 8.089 s | 45,616 files/s |
| 3 | Warm filesystem cache | 368,971 | 7.657 s | 48,188 files/s |

The resulting SQLite database and active WAL files occupied about 78.38 MiB,
or roughly 223 bytes per indexed file. The two warm runs averaged 7.873 seconds
and 46,902 files/s. This reproduces the previously observed "about 7 seconds"
result, but the cold 24.757-second run is the more realistic expectation after
a restart or when filesystem metadata is not already cached.

Indexing measurements include directory traversal, extracting each filename and
path, deleting the previous rows, inserting all new rows, committing SQLite,
and process startup. File contents are never opened or read.

### Search latency

Each case below was warmed twice and then measured 15 times through the actual
`fileCrawler.exe search` command. The timings include process startup, opening
SQLite, running the ranked query, collecting up to 200 results, formatting
them, and exiting after the automated `0` selection. `p50` is the median;
`p95` uses the nearest-rank method (the slowest result in a 15-run sample).

| Search case | Query | Scope | Matching rows | p50 | p95 |
| --- | --- | --- | ---: | ---: | ---: |
| Exact common filename | `comctl32.dll.mui` | All drives | 1,804 | 251.88 ms | 264.51 ms |
| Specific partial name | `ui-strings` | All drives | 1,287 | 236.30 ms | 246.22 ms |
| JPG extension | `.jpg` | All drives | 4,468 | 271.81 ms | 294.83 ms |
| PNG extension | `.png` | All drives | 45,422 | 614.83 ms | 680.88 ms |
| DLL extension | `.dll` | All drives | 78,559 | 885.77 ms | 1,018.90 ms |
| Scoped DLL extension | `.dll` | `C:\Windows` | 60,500 | 739.87 ms | 845.12 ms |

The broad `.dll` query matches about 21% of the complete index, so SQLite has
far more rows to evaluate and sort. Specific searches are faster. The launcher
keeps SQLite loaded and does not print every result to a console, so its
perceived response can be slightly better than these end-to-end CLI numbers.

The current `LIKE '%term%'` search is flexible but cannot use a normal
left-anchored filename index efficiently. A future FTS/trigram index would
improve broad contains searches substantially.

### Benchmark method

The full-PC benchmark used three consecutive commands against a disposable
`%LOCALAPPDATA%\WindowFileSearch\index.db`:

```powershell
$oldLocalAppData = $env:LOCALAPPDATA
$env:LOCALAPPDATA = "$PWD\.benchmark-localappdata"
New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
1..3 | ForEach-Object { Measure-Command { .\fileCrawler.exe index } }
$env:LOCALAPPDATA = $oldLocalAppData
```

Search p50 and p95 values came from 15 timed runs after two warm-up runs.
The local benchmark database was deleted afterward and is excluded by
`.gitignore`.

## Why Indexing Is Fast

The speed comes from a combination of inexpensive metadata-only work,
parallel traversal, and batched database writes:

1. The program asks Windows for directory entries using `FindFirstFile` and
   `FindNextFile`. It records names and paths without reading file contents.
2. A shared FIFO work queue starts with each logical drive. Whenever a worker
   discovers a normal subdirectory, it appends that directory to the queue.
3. Up to `min(logical processors, 8)` workers pull different directories from
   the queue concurrently. On this machine that means 8 workers.
4. Workers sleep on a condition variable when the queue is temporarily empty,
   rather than busy-waiting and wasting CPU.
5. A `pending` counter includes queued and currently processed directories.
   When it reaches zero, every worker knows the crawl is complete and exits.
6. Reparse-point directories are skipped, preventing junction/symlink loops and
   duplicate traversal.
7. All inserts use one prepared SQLite statement inside one
   `BEGIN IMMEDIATE ... COMMIT` transaction. Avoiding one transaction per file
   removes hundreds of thousands of disk flushes.

Traversal is parallel, while SQLite insertion is serialized by a short
critical section. Each worker briefly takes the database lock, binds filename
and path, executes the prepared insert, resets it, and releases the lock. This
keeps one SQLite connection safe while other workers continue discovering
directories. On fast storage, the single insert lock can eventually become the
bottleneck; a dedicated writer thread with batched inserts is a possible future
optimization.

## Result Ranking

Ranking does use a numerical score, but it is a simple weighted sum rather than
machine learning or an expensive mathematical model. For each matching row:

```text
score =
    2000 if filename exactly equals the query
  + 1200 if filename has the same base name (report -> report.pdf)
  +  700 if filename starts with the query
  +  450 if the query starts after a space, underscore, or hyphen
  +  350 for a strong name/extension boundary match
  +  300 if the file extension equals the query
  +   90 if the path is in Downloads
  +   80 if the path is on Desktop
  +   75 if the path is in Documents
  +   55 if the path is in Pictures or Music
  -  250 if the path is in .git or node_modules
  -  350 if the path is in Windows or Program Files
```

Several bonuses can apply to the same result. SQLite then sorts by:

```text
score descending,
filename length ascending,
path length ascending,
filename alphabetically
```

In practical terms, results appear in this order:

1. Exact filenames
2. Files with the same base name, such as `report.pdf` for `report`
3. Filenames that start with the search text
4. Word-boundary and extension matches
5. Other filename contains matches

Downloads, Desktop, Documents, Pictures, and Music receive a small boost.
Generated/system-heavy locations such as `.git`, `node_modules`, Windows, and
Program Files are pushed down. The launcher displays labels such as
`[Exact name]` and `[Starts with]` so the ordering is visible to the user.

The score calculation itself is cheap integer addition. Search time is
dominated by finding rows for `LIKE '%term%'` and sorting the matching rows,
not by the scoring arithmetic.

## Build

Compile using a Windows C compiler.

Example using MinGW:

```bash
gcc fileCrawler.c -o fileCrawler.exe -lshell32
```

At runtime, the application dynamically loads `sqlite3.dll`.

Place `sqlite3.dll` either:

* beside the executable, or
* somewhere on the system PATH

## Usage

### Index a specific directory

```bash
fileCrawler.exe index C:\Users\User
```

### Index all logical drives

```bash
fileCrawler.exe index
```

### Search indexed filenames

```bash
fileCrawler.exe search notes
```

### Scope search to a drive

```bash
fileCrawler.exe search report D:\
```

### Scope search to a folder

```bash
fileCrawler.exe search report D:\Projects
```

If no index exists, the application automatically indexes all logical drives before executing the search.

## Database Schema

```sql
CREATE TABLE files (
    id INTEGER PRIMARY KEY,
    filename TEXT,
    path TEXT
);

CREATE INDEX idx_filename
ON files(filename);
```

## Workflow

1. Crawl a root directory or all logical drives.
2. Extract filename and full path metadata.
3. Store metadata in SQLite.
4. Search indexed filenames.
5. Open selected results directly from the search interface.

## Design Notes

* Directory traversal is parallelized using multiple worker threads.
* SQLite writes are synchronized to maintain database consistency.
* File metadata is indexed once and reused across searches.
* Search operations query SQLite instead of rescanning the filesystem.
* The user-facing launcher lives in [windowFileSearchLauncher.c](windowFileSearchLauncher.c).

## Future Improvements

* Incremental indexing
* Real-time filesystem monitoring
* Fuzzy filename matching
* Content indexing
* Launcher polish
* NTFS MFT-based indexing

## Learning Objectives

This project explores:

* Filesystem traversal
* Multithreaded systems programming
* SQLite integration in C
* Database indexing concepts
* Search engine fundamentals
* Windows systems programming
