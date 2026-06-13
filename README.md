# Windows File Search Engine

Version 1 indexes filenames and full paths into a local SQLite database, then searches that database instead of rescanning the filesystem on every query. Indexing now uses multiple worker threads to crawl directories faster, and the default search-time rebuild covers all logical drives.

## Features

- Recursive filesystem crawling from a user-provided root path or all logical drives.
- Multithreaded indexing with multiple directory workers.
- SQLite-backed storage in `index.db`.
- Filename search with `LIKE '%term%'`.
- Result selection and opening with the system default application.
- Automatic indexing of all logical drives when search runs and the database is empty.

## Build

This workspace uses a single C source file. Build it with a Windows C compiler and link against Shell32 for file opening.

Example with MinGW:

```bash
gcc fileCrawler.c -o fileCrawler.exe -lshell32
```

At runtime, the program loads `sqlite3.dll` dynamically. Make sure SQLite is installed and `sqlite3.dll` is available on `PATH` or in the same folder as the executable.

## Usage

Index a tree:

```bash
fileCrawler.exe index C:\Users\User
```

Index the whole machine:

```bash
fileCrawler.exe index
```

Search indexed filenames:

```bash
fileCrawler.exe search notes
```

Scope search to one drive or folder:

```bash
fileCrawler.exe search lab E:\
fileCrawler.exe search lab E:\Projects
```

If you rename the executable to `indexer.exe` or `search.exe`, it also accepts the shorter single-argument form:

```bash
indexer.exe C:\Users\User
search.exe notes E:\
```

If you run search before indexing, the program will automatically index all logical drives first.

## Database Schema

The program creates the database and table automatically:

```sql
CREATE TABLE files (
	id INTEGER PRIMARY KEY,
	filename TEXT,
	path TEXT
);

CREATE INDEX idx_filename ON files(filename);
```

## Workflow

1. Crawl a root directory or all logical drives.
2. Insert each regular file into SQLite.
3. Search the `files` table by filename.
4. Pick a result number to open the file in its default app.

## Notes

- Multiple worker threads are used for directory crawling, but SQLite writes are serialized to keep the database consistent.
- Full all-drive reindex benchmark on this machine after deleting `index.db`: 7.16 seconds.
   
