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

## Performance

Benchmark on a Windows machine after deleting the existing per-user index and performing a full rebuild:

* Indexed files: 491,593
* Database size: ~100 MB
* Full indexing time: 7.16 seconds
* Indexing throughput: ~68,000 files/sec
* Search latency: effectively instantaneous for common queries

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
* Ranking and relevance scoring

## Learning Objectives

This project explores:

* Filesystem traversal
* Multithreaded systems programming
* SQLite integration in C
* Database indexing concepts
* Search engine fundamentals
* Windows systems programming
