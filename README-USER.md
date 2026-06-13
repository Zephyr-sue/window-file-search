# Window File Search - User Guide

This is the user-facing version of the project. It gives you a small Windows app with a shortcut so you can search files without opening a console.

## Recommended Shortcut

Use `Ctrl+Alt+F`.

That hotkey opens the search window while the launcher is running in the tray.

## What You Need

Keep these files together in one folder:

- `windowFileSearchLauncher.exe`
- `fileCrawler.exe`
- `sqlite3.dll`

The launcher uses the SQLite database stored per user under:

`%LOCALAPPDATA%\WindowFileSearch\index.db`

## First Run

1. Start `windowFileSearchLauncher.exe`.
2. Select the drives you want to index.
3. Use **Add Drive** or **Add Folder** to add folders.
4. Press **Index Selected**.
5. Wait for indexing to finish.

After that, your selections are saved in your user profile and you can search right away.

When you close the launcher window, it stays running in the system tray instead of quitting.

## Searching

1. Press `Ctrl+Alt+F`.
2. Type a filename search term.
3. (Optional) Type a drive or folder path in **Scope (optional)** to limit the search to that area.
4. Press **Search**.
5. Double-click a result to open it.

You can also right-click the tray icon and choose **Show** to bring the window back.

Search results are ranked to favor exact filename matches, common user folders like Downloads/Desktop/Documents, and file extensions such as `.pdf`, `.doc`, `.jpg`, `.mp3`, and `.c`. Results from `Windows` and `Program Files` are pushed down.

If `Ctrl+Alt+F` does not open the window, make sure `windowFileSearchLauncher.exe` is still running in the tray. The hotkey only works while the launcher process is active.

## Updating the Index

If you add new files later:

1. Open the launcher.
2. Add any new drive or folder.
3. Press **Index Selected** again.

## Notes

- The launcher is the app that stays open and handles the shortcut.
- Closing the window hides it to the tray; use the tray menu to exit.
- The indexer writes to the per-user SQLite database, so each Windows account gets its own search index.
- Search is filename-based only in Version 1.
- If no index exists, the app can rebuild it from the roots you selected.

## Build

If you are building from source:

```bash
gcc fileCrawler.c -o fileCrawler.exe -lshell32 -lole32
gcc windowFileSearchLauncher.c -o windowFileSearchLauncher.exe -mwindows -lshell32 -lcomctl32 -lole32 -luuid
```

## Suggested Startup Setup

For the shortcut to be always available, put `windowFileSearchLauncher.exe` in your Windows Startup folder or create a startup shortcut for it.

To exit completely, right-click the tray icon and choose **Exit**.
