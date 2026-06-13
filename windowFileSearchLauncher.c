#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <process.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_BUFFER_SIZE 4096
#define MAX_RESULTS 200
#define MAX_LIST_TEXT 512
#define HOTKEY_ID 0x1001
#define WM_TRAYICON (WM_APP + 1)
#define TRAY_MENU_SHOW 2001
#define TRAY_MENU_EXIT 2002

#define IDC_AVAILABLE 101
#define IDC_ROOTS 102
#define IDC_ADD_DRIVE 103
#define IDC_ADD_FOLDER 104
#define IDC_REMOVE_ROOT 105
#define IDC_INDEX 106
#define IDC_SEARCH_EDIT 107
#define IDC_SEARCH_BUTTON 108
#define IDC_RESULTS 109
#define IDC_STATUS 110
#define IDC_SCOPE_EDIT 111
#define IDC_SCOPE_BUTTON 112

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);

typedef int (__cdecl *sqlite3_open_fn)(const char *, sqlite3 **);
typedef int (__cdecl *sqlite3_close_fn)(sqlite3 *);
typedef int (__cdecl *sqlite3_exec_fn)(sqlite3 *, const char *, int (__cdecl *)(void *, int, char **, char **), void *, char **);
typedef int (__cdecl *sqlite3_prepare_v2_fn)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (__cdecl *sqlite3_bind_text_fn)(sqlite3_stmt *, int, const char *, int, sqlite3_destructor_type);
typedef int (__cdecl *sqlite3_step_fn)(sqlite3_stmt *);
typedef int (__cdecl *sqlite3_finalize_fn)(sqlite3_stmt *);
typedef const unsigned char *(__cdecl *sqlite3_column_text_fn)(sqlite3_stmt *, int);
typedef const char *(__cdecl *sqlite3_errmsg_fn)(sqlite3 *);
typedef void (__cdecl *sqlite3_free_fn)(void *);

typedef struct {
    char *path;
    char *display;
} SearchResult;

static HINSTANCE g_instance;
static HWND g_main_window;
static HWND g_available_list;
static HWND g_roots_list;
static HWND g_search_edit;
static HWND g_scope_edit;
static HWND g_results_list;
static HWND g_status_label;
static NOTIFYICONDATAA g_tray_icon;
static HMODULE g_sqlite_module;
static sqlite3_open_fn p_sqlite3_open;
static sqlite3_close_fn p_sqlite3_close;
static sqlite3_exec_fn p_sqlite3_exec;
static sqlite3_prepare_v2_fn p_sqlite3_prepare_v2;
static sqlite3_bind_text_fn p_sqlite3_bind_text;
static sqlite3_step_fn p_sqlite3_step;
static sqlite3_finalize_fn p_sqlite3_finalize;
static sqlite3_column_text_fn p_sqlite3_column_text;
static sqlite3_errmsg_fn p_sqlite3_errmsg;
static sqlite3_free_fn p_sqlite3_free;
static SearchResult *g_results;
static size_t g_result_count;

static int sqlite3_open(const char *filename, sqlite3 **db) { return p_sqlite3_open(filename, db); }
static int sqlite3_close(sqlite3 *db) { return p_sqlite3_close(db); }
static int sqlite3_exec(sqlite3 *db, const char *sql, int (__cdecl *callback)(void *, int, char **, char **), void *context, char **error_message) { return p_sqlite3_exec(db, sql, callback, context, error_message); }
static int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int length, sqlite3_stmt **statement, const char **tail) { return p_sqlite3_prepare_v2(db, sql, length, statement, tail); }
static int sqlite3_bind_text(sqlite3_stmt *statement, int index, const char *text, int length, sqlite3_destructor_type destructor) { return p_sqlite3_bind_text(statement, index, text, length, destructor); }
static int sqlite3_step(sqlite3_stmt *statement) { return p_sqlite3_step(statement); }
static int sqlite3_finalize(sqlite3_stmt *statement) { return p_sqlite3_finalize(statement); }
static const unsigned char *sqlite3_column_text(sqlite3_stmt *statement, int column) { return p_sqlite3_column_text(statement, column); }
static const char *sqlite3_errmsg(sqlite3 *db) { return p_sqlite3_errmsg(db); }
static void sqlite3_free(void *pointer) { p_sqlite3_free(pointer); }

static void free_results(void) {
    size_t index;

    for (index = 0; index < g_result_count; ++index) {
        free(g_results[index].path);
        free(g_results[index].display);
    }

    free(g_results);
    g_results = NULL;
    g_result_count = 0;
}

static void show_main_window(void) {
    ShowWindow(g_main_window, SW_SHOW);
    ShowWindow(g_main_window, SW_RESTORE);
    SetForegroundWindow(g_main_window);
}

static void add_tray_icon(void) {
    ZeroMemory(&g_tray_icon, sizeof(g_tray_icon));
    g_tray_icon.cbSize = sizeof(g_tray_icon);
    g_tray_icon.hWnd = g_main_window;
    g_tray_icon.uID = 1;
    g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray_icon.uCallbackMessage = WM_TRAYICON;
    g_tray_icon.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strncpy(g_tray_icon.szTip, "Window File Search", sizeof(g_tray_icon.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_tray_icon);
}

static void remove_tray_icon(void) {
    if (g_tray_icon.cbSize != 0) {
        Shell_NotifyIconA(NIM_DELETE, &g_tray_icon);
    }
}

static void show_tray_menu(HWND window) {
    POINT cursor;
    HMENU menu = CreatePopupMenu();

    AppendMenuA(menu, MF_STRING, TRAY_MENU_SHOW, "Show");
    AppendMenuA(menu, MF_STRING, TRAY_MENU_EXIT, "Exit");

    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, NULL);
    DestroyMenu(menu);
}

static char *duplicate_string(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        text = "";
    }

    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }

    return copy;
}

static void lowercase_in_place(char *text) {
    if (text == NULL) {
        return;
    }

    for (; *text != '\0'; ++text) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static void trim_in_place(char *text) {
    size_t start = 0;
    size_t end;

    if (text == NULL) {
        return;
    }

    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') {
        start++;
    }

    if (start > 0) {
        memmove(text, text + start, strlen(text + start) + 1);
    }

    end = strlen(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        text[end - 1] = '\0';
        end--;
    }
}

static void normalize_slashes_in_place(char *text) {
    if (text == NULL) {
        return;
    }

    for (char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\\';
        }
    }
}

static void build_scope_pattern(const char *scope, char *buffer, size_t buffer_size) {
    char normalized[PATH_BUFFER_SIZE];
    size_t length;

    if (scope == NULL) {
        buffer[0] = '\0';
        return;
    }

    snprintf(normalized, sizeof(normalized), "%s", scope);
    trim_in_place(normalized);
    normalize_slashes_in_place(normalized);

    if (normalized[0] == '\0') {
        buffer[0] = '\0';
        return;
    }

    length = strlen(normalized);
    if (length > 0 && (normalized[length - 1] == '\\')) {
        snprintf(buffer, buffer_size, "%s%%", normalized);
    } else if (length == 2 && normalized[1] == ':') {
        snprintf(buffer, buffer_size, "%s\\%%", normalized);
    } else {
        snprintf(buffer, buffer_size, "%s\\%%", normalized);
    }

    lowercase_in_place(buffer);
}

static int join_path(char *destination, size_t destination_size, const char *parent, const char *child) {
    int written;
    size_t parent_length = strlen(parent);

    if (parent_length > 0 && (parent[parent_length - 1] == '\\' || parent[parent_length - 1] == '/')) {
        written = snprintf(destination, destination_size, "%s%s", parent, child);
    } else {
        written = snprintf(destination, destination_size, "%s\\%s", parent, child);
    }

    return written >= 0 && (size_t)written < destination_size;
}

static int get_local_appdata(char *buffer, size_t buffer_size) {
    DWORD written = GetEnvironmentVariableA("LOCALAPPDATA", buffer, (DWORD)buffer_size);
    if (written == 0 || written >= buffer_size) {
        return 0;
    }

    buffer[written] = '\0';
    return 1;
}

static int ensure_data_dir(char *buffer, size_t buffer_size) {
    char data_dir[PATH_BUFFER_SIZE];

    if (!get_local_appdata(data_dir, sizeof(data_dir))) {
        return 0;
    }

    if (!join_path(buffer, buffer_size, data_dir, "WindowFileSearch")) {
        return 0;
    }

    if (CreateDirectoryA(buffer, NULL) == 0) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return 0;
        }
    }

    return 1;
}

static int get_data_path(const char *file_name, char *buffer, size_t buffer_size) {
    char data_dir[PATH_BUFFER_SIZE];

    if (!ensure_data_dir(data_dir, sizeof(data_dir))) {
        return 0;
    }

    return join_path(buffer, buffer_size, data_dir, file_name);
}

static int load_sqlite(void) {
    if (g_sqlite_module != NULL) {
        return 1;
    }

    g_sqlite_module = LoadLibraryA("sqlite3.dll");
    if (g_sqlite_module == NULL) {
        MessageBoxA(NULL, "sqlite3.dll was not found. Put it next to the launcher or on PATH.", "Window File Search", MB_ICONERROR);
        return 0;
    }

#define LOAD_PROC(name) do { \
    p_##name = (name##_fn)GetProcAddress(g_sqlite_module, #name); \
    if (p_##name == NULL) { \
        MessageBoxA(NULL, "Missing SQLite symbol.", "Window File Search", MB_ICONERROR); \
        return 0; \
    } \
} while (0)

    LOAD_PROC(sqlite3_open);
    LOAD_PROC(sqlite3_close);
    LOAD_PROC(sqlite3_exec);
    LOAD_PROC(sqlite3_prepare_v2);
    LOAD_PROC(sqlite3_bind_text);
    LOAD_PROC(sqlite3_step);
    LOAD_PROC(sqlite3_finalize);
    LOAD_PROC(sqlite3_column_text);
    LOAD_PROC(sqlite3_errmsg);
    LOAD_PROC(sqlite3_free);

#undef LOAD_PROC
    return 1;
}

static void set_status(HWND window, const char *text) {
    SetWindowTextA(g_status_label, text);
    UpdateWindow(g_status_label);
}

static int listbox_contains(HWND listbox, const char *text) {
    int count = (int)SendMessageA(listbox, LB_GETCOUNT, 0, 0);

    for (int index = 0; index < count; ++index) {
        char item[MAX_LIST_TEXT];
        SendMessageA(listbox, LB_GETTEXT, (WPARAM)index, (LPARAM)item);
        if (_stricmp(item, text) == 0) {
            return 1;
        }
    }

    return 0;
}

static void add_root(HWND listbox, const char *path) {
    if (path == NULL || path[0] == '\0') {
        return;
    }

    if (!listbox_contains(listbox, path)) {
        SendMessageA(listbox, LB_ADDSTRING, 0, (LPARAM)path);
    }
}

static void populate_available_drives(HWND listbox) {
    DWORD mask = GetLogicalDrives();

    SendMessageA(listbox, LB_RESETCONTENT, 0, 0);

    for (int index = 0; index < 26; ++index) {
        if ((mask & (1UL << index)) == 0) {
            continue;
        }

        char root[4];
        char display[32];
        UINT drive_type;

        root[0] = (char)('A' + index);
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';

        drive_type = GetDriveTypeA(root);
        snprintf(display, sizeof(display), "%s", root);
        if (drive_type == DRIVE_FIXED) {
            strcat(display, " (fixed)");
        } else if (drive_type == DRIVE_REMOVABLE) {
            strcat(display, " (removable)");
        } else if (drive_type == DRIVE_CDROM) {
            strcat(display, " (cdrom)");
        }

        SendMessageA(listbox, LB_ADDSTRING, 0, (LPARAM)display);
    }
}

static int get_listbox_text(HWND listbox, int index, char *buffer, size_t buffer_size) {
    LRESULT copied = SendMessageA(listbox, LB_GETTEXT, (WPARAM)index, (LPARAM)buffer);
    if (copied == LB_ERR) {
        return 0;
    }

    buffer[buffer_size - 1] = '\0';
    return 1;
}

static int save_roots(HWND roots_list) {
    char roots_path[PATH_BUFFER_SIZE];
    FILE *file;
    int count;

    if (!get_data_path("roots.txt", roots_path, sizeof(roots_path))) {
        return 0;
    }

    file = fopen(roots_path, "w");
    if (file == NULL) {
        return 0;
    }

    count = (int)SendMessageA(roots_list, LB_GETCOUNT, 0, 0);
    for (int index = 0; index < count; ++index) {
        char item[MAX_LIST_TEXT];
        SendMessageA(roots_list, LB_GETTEXT, (WPARAM)index, (LPARAM)item);
        fprintf(file, "%s\n", item);
    }

    fclose(file);
    return 1;
}

static void load_roots(HWND roots_list) {
    char roots_path[PATH_BUFFER_SIZE];
    FILE *file;
    char line[PATH_BUFFER_SIZE];

    if (!get_data_path("roots.txt", roots_path, sizeof(roots_path))) {
        return;
    }

    file = fopen(roots_path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[length - 1] = '\0';
            length--;
        }

        if (line[0] != '\0') {
            add_root(roots_list, line);
        }
    }

    fclose(file);
}

static int browse_for_folder(HWND owner, char *buffer, size_t buffer_size) {
    BROWSEINFOA info;
    LPITEMIDLIST item_id_list;
    char display_name[MAX_PATH];

    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = owner;
    info.pszDisplayName = display_name;
    info.lpszTitle = "Select a folder to index";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    item_id_list = SHBrowseForFolderA(&info);
    if (item_id_list == NULL) {
        return 0;
    }

    buffer[0] = '\0';
    if (!SHGetPathFromIDListA(item_id_list, buffer)) {
        CoTaskMemFree(item_id_list);
        return 0;
    }

    CoTaskMemFree(item_id_list);
    return buffer[0] != '\0';
}

static int get_file_crawler_path(char *buffer, size_t buffer_size) {
    char module_path[PATH_BUFFER_SIZE];
    char *last_slash;

    if (GetModuleFileNameA(NULL, module_path, (DWORD)sizeof(module_path)) == 0) {
        return 0;
    }

    last_slash = strrchr(module_path, '\\');
    if (last_slash != NULL) {
        *(last_slash + 1) = '\0';
    }

    return join_path(buffer, buffer_size, module_path, "fileCrawler.exe");
}

static int run_process_and_wait(const char *command_line) {
    STARTUPINFOA startup;
    PROCESS_INFORMATION process_info;
    char mutable_command[PATH_BUFFER_SIZE * 2];
    DWORD wait_result;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process_info, sizeof(process_info));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    strncpy(mutable_command, command_line, sizeof(mutable_command) - 1);
    mutable_command[sizeof(mutable_command) - 1] = '\0';

    if (!CreateProcessA(NULL, mutable_command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process_info)) {
        return 0;
    }

    for (;;) {
        MSG message;
        wait_result = MsgWaitForMultipleObjects(1, &process_info.hProcess, FALSE, 100, QS_ALLEVENTS);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }

        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return 1;
}

static int index_selected_roots(HWND window) {
    char crawler_path[PATH_BUFFER_SIZE];
    char command[PATH_BUFFER_SIZE * 2];
    int count;
    char roots_file[PATH_BUFFER_SIZE];

    if (!save_roots(g_roots_list)) {
        set_status(window, "Unable to save roots.");
        return 0;
    }

    if (!get_file_crawler_path(crawler_path, sizeof(crawler_path))) {
        set_status(window, "Unable to locate fileCrawler.exe.");
        return 0;
    }

    count = (int)SendMessageA(g_roots_list, LB_GETCOUNT, 0, 0);
    if (count == 0) {
        snprintf(command, sizeof(command), "\"%s\" index", crawler_path);
    } else {
        size_t offset = (size_t)snprintf(command, sizeof(command), "\"%s\" index", crawler_path);

        for (int index = 0; index < count; ++index) {
            char root[MAX_LIST_TEXT];
            SendMessageA(g_roots_list, LB_GETTEXT, (WPARAM)index, (LPARAM)root);
            if (offset + strlen(root) + 3 >= sizeof(command)) {
                break;
            }
            offset += (size_t)snprintf(command + offset, sizeof(command) - offset, " \"%s\"", root);
        }
    }

    set_status(window, "Indexing selected roots...");
    if (!run_process_and_wait(command)) {
        set_status(window, "Indexing failed.");
        return 0;
    }

    if (!get_data_path("roots.txt", roots_file, sizeof(roots_file))) {
        set_status(window, "Indexing completed, but could not save roots.");
        return 1;
    }

    set_status(window, "Indexing completed.");
    MessageBoxA(window, "Indexing finished.", "Window File Search", MB_OK | MB_ICONINFORMATION);
    return 1;
}

static sqlite3 *open_database(void) {
    sqlite3 *db = NULL;
    char database_path[PATH_BUFFER_SIZE];

    if (!load_sqlite()) {
        return NULL;
    }

    if (!get_data_path("index.db", database_path, sizeof(database_path))) {
        return NULL;
    }

    if (sqlite3_open(database_path, &db) != SQLITE_OK) {
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    return db;
}

static int search_database(const char *term, const char *scope) {
    sqlite3 *db = open_database();
    sqlite3_stmt *statement = NULL;
    char search_pattern[PATH_BUFFER_SIZE];
    char scope_pattern[PATH_BUFFER_SIZE];
    char exact_term[PATH_BUFFER_SIZE];
    char extension_term[PATH_BUFFER_SIZE];
    int rc;

    free_results();
    SendMessageA(g_results_list, LB_RESETCONTENT, 0, 0);

    if (db == NULL) {
        set_status(g_main_window, "Search unavailable because the database could not be opened.");
        return 0;
    }

    if (snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", term) >= (int)sizeof(search_pattern)) {
        sqlite3_close(db);
        set_status(g_main_window, "Search term too long.");
        return 0;
    }

    lowercase_in_place(search_pattern);
    build_scope_pattern(scope, scope_pattern, sizeof(scope_pattern));

    snprintf(exact_term, sizeof(exact_term), "%s", term);
    lowercase_in_place(exact_term);

    if (term[0] == '.') {
        snprintf(extension_term, sizeof(extension_term), "%s", term + 1);
    } else {
        snprintf(extension_term, sizeof(extension_term), "%s", term);
    }
    lowercase_in_place(extension_term);

    if (scope_pattern[0] != '\0') {
        if (sqlite3_prepare_v2(db,
            "SELECT filename, path, ("
            " CASE WHEN lower(filename) = ?1 THEN 1200 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE ?2 || '%' THEN 180 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE '%' || ?3 || '.%' THEN 220 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE '%.' || ?4 THEN 140 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\downloads\\%' THEN 110 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\desktop\\%' THEN 90 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\documents\\%' THEN 85 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\pictures\\%' THEN 70 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\music\\%' THEN 65 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\program files\\%' OR lower(path) LIKE '%\\windows\\%' THEN -400 ELSE 0 END"
            ") AS score"
            " FROM files WHERE lower(filename) LIKE ?5 AND lower(path) LIKE ?6"
            " ORDER BY score DESC, length(path) ASC, filename COLLATE NOCASE LIMIT 200;",
            -1, &statement, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            set_status(g_main_window, "Failed to prepare search query.");
            return 0;
        }

        if (sqlite3_bind_text(statement, 1, exact_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 2, exact_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 3, extension_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 4, extension_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 5, search_pattern, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 6, scope_pattern, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_finalize(statement);
            sqlite3_close(db);
            set_status(g_main_window, "Failed to bind search query.");
            return 0;
        }
    } else {
        if (sqlite3_prepare_v2(db,
            "SELECT filename, path, ("
            " CASE WHEN lower(filename) = ?1 THEN 1200 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE ?2 || '%' THEN 180 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE '%' || ?3 || '.%' THEN 220 ELSE 0 END"
            " + CASE WHEN lower(filename) LIKE '%.' || ?4 THEN 140 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\downloads\\%' THEN 110 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\desktop\\%' THEN 90 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\documents\\%' THEN 85 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\pictures\\%' THEN 70 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\music\\%' THEN 65 ELSE 0 END"
            " + CASE WHEN lower(path) LIKE '%\\program files\\%' OR lower(path) LIKE '%\\windows\\%' THEN -400 ELSE 0 END"
            ") AS score"
            " FROM files WHERE lower(filename) LIKE ?5"
            " ORDER BY score DESC, length(path) ASC, filename COLLATE NOCASE LIMIT 200;",
            -1, &statement, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            set_status(g_main_window, "Failed to prepare search query.");
            return 0;
        }

        if (sqlite3_bind_text(statement, 1, exact_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 2, exact_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 3, extension_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 4, extension_term, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement, 5, search_pattern, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            sqlite3_finalize(statement);
            sqlite3_close(db);
            set_status(g_main_window, "Failed to bind search query.");
            return 0;
        }
    }

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *filename_text = sqlite3_column_text(statement, 0);
        const unsigned char *path_text = sqlite3_column_text(statement, 1);
        SearchResult *new_results;
        char display[MAX_LIST_TEXT];

        new_results = (SearchResult *)realloc(g_results, (g_result_count + 1) * sizeof(*g_results));
        if (new_results == NULL) {
            free_results();
            sqlite3_finalize(statement);
            sqlite3_close(db);
            set_status(g_main_window, "Out of memory while collecting results.");
            return 0;
        }

        g_results = new_results;
        g_results[g_result_count].path = duplicate_string((const char *)path_text);
        g_results[g_result_count].display = NULL;

        if (g_results[g_result_count].path == NULL) {
            free_results();
            sqlite3_finalize(statement);
            sqlite3_close(db);
            set_status(g_main_window, "Out of memory while copying results.");
            return 0;
        }

        snprintf(display, sizeof(display), "%s", (const char *)filename_text);
        strncat(display, " | ", sizeof(display) - strlen(display) - 1);
        strncat(display, (const char *)path_text, sizeof(display) - strlen(display) - 1);
        g_results[g_result_count].display = duplicate_string(display);
        if (g_results[g_result_count].display == NULL) {
            free_results();
            sqlite3_finalize(statement);
            sqlite3_close(db);
            set_status(g_main_window, "Out of memory while copying display text.");
            return 0;
        }

        SendMessageA(g_results_list, LB_ADDSTRING, 0, (LPARAM)display);
        SendMessageA(g_results_list, LB_SETITEMDATA, (WPARAM)g_result_count, (LPARAM)g_result_count);
        g_result_count++;
    }

    sqlite3_finalize(statement);
    sqlite3_close(db);

    if (g_result_count == 0) {
        set_status(g_main_window, "No matches.");
    } else {
        char status[128];
        snprintf(status, sizeof(status), "Found %zu result(s).", g_result_count);
        set_status(g_main_window, status);
    }

    return 1;
}

static void open_result_by_index(size_t index) {
    if (index >= g_result_count) {
        return;
    }

    ShellExecuteA(NULL, "open", g_results[index].path, NULL, NULL, SW_SHOWNORMAL);
}

static void add_selected_available_drives_to_roots(void) {
    int selection = (int)SendMessageA(g_available_list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) {
        MessageBoxA(g_main_window, "Select a drive first.", "Window File Search", MB_OK | MB_ICONINFORMATION);
        return;
    }

    char item[MAX_LIST_TEXT];
    SendMessageA(g_available_list, LB_GETTEXT, (WPARAM)selection, (LPARAM)item);
    char *paren = strstr(item, " (");
    if (paren != NULL) {
        *paren = '\0';
    }

    add_root(g_roots_list, item);
}

static void remove_selected_root(void) {
    int selection = (int)SendMessageA(g_roots_list, LB_GETCURSEL, 0, 0);
    if (selection != LB_ERR) {
        SendMessageA(g_roots_list, LB_DELETESTRING, (WPARAM)selection, 0);
    }
}

static void add_folder_root(void) {
    char folder[MAX_PATH];

    if (!browse_for_folder(g_main_window, folder, sizeof(folder))) {
        return;
    }

    add_root(g_roots_list, folder);
}

static void focus_search(void) {
    ShowWindow(g_main_window, SW_SHOW);
    SetForegroundWindow(g_main_window);
    SetFocus(g_search_edit);
    SendMessageA(g_search_edit, EM_SETSEL, 0, -1);
}

static void apply_scope_from_selection(void) {
    int selection = (int)SendMessageA(g_roots_list, LB_GETCURSEL, 0, 0);
    char folder[MAX_PATH];

    if (selection != LB_ERR) {
        SendMessageA(g_roots_list, LB_GETTEXT, (WPARAM)selection, (LPARAM)folder);
        SetWindowTextA(g_scope_edit, folder);
        return;
    }

    if (browse_for_folder(g_main_window, folder, sizeof(folder))) {
        SetWindowTextA(g_scope_edit, folder);
    }
}

static void on_search(void) {
    char term[PATH_BUFFER_SIZE];
    char scope[PATH_BUFFER_SIZE];
    GetWindowTextA(g_search_edit, term, sizeof(term));
    GetWindowTextA(g_scope_edit, scope, sizeof(scope));

    if (term[0] == '\0') {
        set_status(g_main_window, "Type a search term first.");
        return;
    }

    search_database(term, scope);
}

static void resize_controls(HWND window, int width, int height) {
    MoveWindow(g_available_list, 10, 30, 180, height - 180, TRUE);
    MoveWindow(g_roots_list, 200, 30, 220, height - 180, TRUE);
    MoveWindow(GetDlgItem(window, IDC_ADD_DRIVE), 10, height - 140, 180, 24, TRUE);
    MoveWindow(GetDlgItem(window, IDC_ADD_FOLDER), 10, height - 110, 180, 24, TRUE);
    MoveWindow(GetDlgItem(window, IDC_REMOVE_ROOT), 200, height - 140, 220, 24, TRUE);
    MoveWindow(GetDlgItem(window, IDC_INDEX), 200, height - 110, 220, 24, TRUE);
    MoveWindow(g_search_edit, 10, height - 75, width - 420, 24, TRUE);
    MoveWindow(g_scope_edit, width - 400, height - 75, 280, 24, TRUE);
    MoveWindow(GetDlgItem(window, IDC_SEARCH_BUTTON), width - 105, height - 75, 90, 24, TRUE);
    MoveWindow(g_results_list, 10, height - 270, width - 20, 160, TRUE);
    MoveWindow(g_status_label, 10, height - 40, width - 20, 20, TRUE);
}

static void create_controls(HWND window) {
    CreateWindowA("STATIC", "Available drives", WS_CHILD | WS_VISIBLE, 10, 8, 150, 18, window, NULL, g_instance, NULL);
    CreateWindowA("STATIC", "Selected roots", WS_CHILD | WS_VISIBLE, 200, 8, 150, 18, window, NULL, g_instance, NULL);
    g_available_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 10, 30, 180, 200, window, (HMENU)IDC_AVAILABLE, g_instance, NULL);
    g_roots_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 200, 30, 220, 200, window, (HMENU)IDC_ROOTS, g_instance, NULL);
    CreateWindowA("BUTTON", "Add Drive", WS_CHILD | WS_VISIBLE, 10, 240, 180, 24, window, (HMENU)IDC_ADD_DRIVE, g_instance, NULL);
    CreateWindowA("BUTTON", "Add Folder", WS_CHILD | WS_VISIBLE, 10, 270, 180, 24, window, (HMENU)IDC_ADD_FOLDER, g_instance, NULL);
    CreateWindowA("BUTTON", "Remove Root", WS_CHILD | WS_VISIBLE, 200, 240, 220, 24, window, (HMENU)IDC_REMOVE_ROOT, g_instance, NULL);
    CreateWindowA("BUTTON", "Index Selected", WS_CHILD | WS_VISIBLE, 200, 270, 220, 24, window, (HMENU)IDC_INDEX, g_instance, NULL);
    CreateWindowA("STATIC", "Search", WS_CHILD | WS_VISIBLE, 10, 430, 80, 18, window, NULL, g_instance, NULL);
    CreateWindowA("STATIC", "Scope (optional)", WS_CHILD | WS_VISIBLE, 420, 430, 140, 18, window, NULL, g_instance, NULL);
    g_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 450, 400, 24, window, (HMENU)IDC_SEARCH_EDIT, g_instance, NULL);
    g_scope_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 420, 450, 280, 24, window, (HMENU)IDC_SCOPE_EDIT, g_instance, NULL);
    CreateWindowA("BUTTON", "Search", WS_CHILD | WS_VISIBLE, 710, 450, 90, 24, window, (HMENU)IDC_SEARCH_BUTTON, g_instance, NULL);
    g_results_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 10, 300, 790, 120, window, (HMENU)IDC_RESULTS, g_instance, NULL);
    g_status_label = CreateWindowA("STATIC", "Ready.", WS_CHILD | WS_VISIBLE, 10, 560, 790, 20, window, (HMENU)IDC_STATUS, g_instance, NULL);
}

static LRESULT CALLBACK main_wnd_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        create_controls(window);
        populate_available_drives(g_available_list);
        load_roots(g_roots_list);
        RegisterHotKey(window, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'F');
        add_tray_icon();
        set_status(window, "Ctrl+Alt+F opens the search box.");
        return 0;
    case WM_SIZE:
        resize_controls(window, LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case TRAY_MENU_SHOW:
            show_main_window();
            return 0;
        case TRAY_MENU_EXIT:
            DestroyWindow(window);
            return 0;
        case IDC_ADD_DRIVE:
            add_selected_available_drives_to_roots();
            return 0;
        case IDC_ADD_FOLDER:
            add_folder_root();
            return 0;
        case IDC_REMOVE_ROOT:
            remove_selected_root();
            return 0;
        case IDC_INDEX:
            index_selected_roots(window);
            return 0;
        case IDC_SEARCH_BUTTON:
            on_search();
            return 0;
        case IDC_RESULTS:
            if (HIWORD(w_param) == LBN_DBLCLK) {
                int selection = (int)SendMessageA(g_results_list, LB_GETCURSEL, 0, 0);
                if (selection != LB_ERR) {
                    open_result_by_index((size_t)selection);
                }
            }
            return 0;
        }
        return 0;
    case WM_HOTKEY:
        if (w_param == HOTKEY_ID) {
            show_main_window();
            focus_search();
        }
        return 0;
    case WM_TRAYICON:
        if (l_param == WM_LBUTTONDBLCLK) {
            show_main_window();
            focus_search();
        } else if (l_param == WM_RBUTTONUP) {
            show_tray_menu(window);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        set_status(window, "Running in the tray. Use Ctrl+Alt+F or the tray icon.");
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(window, HOTKEY_ID);
        remove_tray_icon();
        free_results();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(window, message, w_param, l_param);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR command_line, int show_command) {
    WNDCLASSA wc;
    HWND window;
    MSG msg;

    (void)prev_instance;
    (void)command_line;

    g_instance = instance;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WindowFileSearchLauncher";

    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "Unable to register window class.", "Window File Search", MB_ICONERROR);
        return 1;
    }

    window = CreateWindowExA(0, wc.lpszClassName, "Window File Search", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 820, 640, NULL, NULL, instance, NULL);
    if (window == NULL) {
        MessageBoxA(NULL, "Unable to create window.", "Window File Search", MB_ICONERROR);
        return 1;
    }

    g_main_window = window;
    ShowWindow(window, show_command);
    UpdateWindow(window);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
