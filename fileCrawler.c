#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PATH_BUFFER_SIZE 4096
#define MAX_INDEX_WORKERS 8

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_BUSY 5
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

typedef int (__cdecl *sqlite3_open_fn)(const char *, sqlite3 **);
typedef int (__cdecl *sqlite3_close_fn)(sqlite3 *);
typedef int (__cdecl *sqlite3_exec_fn)(sqlite3 *, const char *, int (__cdecl *)(void *, int, char **, char **), void *, char **);
typedef int (__cdecl *sqlite3_prepare_v2_fn)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (__cdecl *sqlite3_bind_text_fn)(sqlite3_stmt *, int, const char *, int, sqlite3_destructor_type);
typedef int (__cdecl *sqlite3_step_fn)(sqlite3_stmt *);
typedef int (__cdecl *sqlite3_finalize_fn)(sqlite3_stmt *);
typedef int (__cdecl *sqlite3_reset_fn)(sqlite3_stmt *);
typedef int (__cdecl *sqlite3_clear_bindings_fn)(sqlite3_stmt *);
typedef const unsigned char *(__cdecl *sqlite3_column_text_fn)(sqlite3_stmt *, int);
typedef const char *(__cdecl *sqlite3_errmsg_fn)(sqlite3 *);
typedef void (__cdecl *sqlite3_free_fn)(void *);

static HMODULE sqlite_module = NULL;
static sqlite3_open_fn p_sqlite3_open = NULL;
static sqlite3_close_fn p_sqlite3_close = NULL;
static sqlite3_exec_fn p_sqlite3_exec = NULL;
static sqlite3_prepare_v2_fn p_sqlite3_prepare_v2 = NULL;
static sqlite3_bind_text_fn p_sqlite3_bind_text = NULL;
static sqlite3_step_fn p_sqlite3_step = NULL;
static sqlite3_finalize_fn p_sqlite3_finalize = NULL;
static sqlite3_reset_fn p_sqlite3_reset = NULL;
static sqlite3_clear_bindings_fn p_sqlite3_clear_bindings = NULL;
static sqlite3_column_text_fn p_sqlite3_column_text = NULL;
static sqlite3_errmsg_fn p_sqlite3_errmsg = NULL;
static sqlite3_free_fn p_sqlite3_free = NULL;

static int sqlite3_open(const char *filename, sqlite3 **db) { return p_sqlite3_open(filename, db); }
static int sqlite3_close(sqlite3 *db) { return p_sqlite3_close(db); }
static int sqlite3_exec(sqlite3 *db, const char *sql, int (__cdecl *callback)(void *, int, char **, char **), void *context, char **error_message) { return p_sqlite3_exec(db, sql, callback, context, error_message); }
static int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int length, sqlite3_stmt **statement, const char **tail) { return p_sqlite3_prepare_v2(db, sql, length, statement, tail); }
static int sqlite3_bind_text(sqlite3_stmt *statement, int index, const char *text, int length, sqlite3_destructor_type destructor) { return p_sqlite3_bind_text(statement, index, text, length, destructor); }
static int sqlite3_step(sqlite3_stmt *statement) { return p_sqlite3_step(statement); }
static int sqlite3_finalize(sqlite3_stmt *statement) { return p_sqlite3_finalize(statement); }
static int sqlite3_reset(sqlite3_stmt *statement) { return p_sqlite3_reset(statement); }
static int sqlite3_clear_bindings(sqlite3_stmt *statement) { return p_sqlite3_clear_bindings(statement); }
static const unsigned char *sqlite3_column_text(sqlite3_stmt *statement, int column) { return p_sqlite3_column_text(statement, column); }
static const char *sqlite3_errmsg(sqlite3 *db) { return p_sqlite3_errmsg(db); }
static void sqlite3_free(void *pointer) { p_sqlite3_free(pointer); }

static int load_sqlite_api(void) {
    if (sqlite_module != NULL) {
        return 1;
    }

    sqlite_module = LoadLibraryA("sqlite3.dll");
    if (sqlite_module == NULL) {
        fprintf(stderr, "sqlite3.dll was not found. Put it next to the executable or on PATH.\n");
        return 0;
    }

#define LOAD_SQLITE_PROC(name) do { \
    p_##name = (name##_fn)GetProcAddress(sqlite_module, #name); \
    if (p_##name == NULL) { \
        fprintf(stderr, "Missing SQLite symbol: %s\n", #name); \
        return 0; \
    } \
} while (0)

    LOAD_SQLITE_PROC(sqlite3_open);
    LOAD_SQLITE_PROC(sqlite3_close);
    LOAD_SQLITE_PROC(sqlite3_exec);
    LOAD_SQLITE_PROC(sqlite3_prepare_v2);
    LOAD_SQLITE_PROC(sqlite3_bind_text);
    LOAD_SQLITE_PROC(sqlite3_step);
    LOAD_SQLITE_PROC(sqlite3_finalize);
    LOAD_SQLITE_PROC(sqlite3_reset);
    LOAD_SQLITE_PROC(sqlite3_clear_bindings);
    LOAD_SQLITE_PROC(sqlite3_column_text);
    LOAD_SQLITE_PROC(sqlite3_errmsg);
    LOAD_SQLITE_PROC(sqlite3_free);

#undef LOAD_SQLITE_PROC

    return 1;
}

typedef struct {
    char *filename;
    char *path;
} SearchResult;

typedef struct DirectoryNode {
    char *path;
    struct DirectoryNode *next;
} DirectoryNode;

typedef struct {
    DirectoryNode *head;
    DirectoryNode *tail;
    size_t pending;
    int stop;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE available;
} DirectoryQueue;

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert_statement;
    DirectoryQueue *queue;
    CRITICAL_SECTION db_lock;
    volatile LONG file_count;
    int failed;
} IndexContext;

static char *duplicate_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);

    if (copy != NULL) {
        memcpy(copy, text, length);
    }

    return copy;
}

static int get_basename(const char *path, char *buffer, size_t buffer_size) {
    const char *last_backslash = strrchr(path, '\\');
    const char *last_slash = strrchr(path, '/');
    const char *separator = last_backslash;

    if (last_slash != NULL && (separator == NULL || last_slash > separator)) {
        separator = last_slash;
    }

    if (separator == NULL) {
        separator = path;
    } else {
        separator++;
    }

    if (snprintf(buffer, buffer_size, "%s", separator) >= (int)buffer_size) {
        return 0;
    }

    return 1;
}

static int join_path(char *destination, size_t destination_size, const char *parent, const char *child);

static int get_app_data_directory(char *buffer, size_t buffer_size) {
    DWORD written;
    char local_app_data[PATH_BUFFER_SIZE];

    written = GetEnvironmentVariableA("LOCALAPPDATA", local_app_data, (DWORD)sizeof(local_app_data));
    if (written == 0 || written >= buffer_size) {
        return 0;
    }

    local_app_data[written] = '\0';

    if (!join_path(buffer, buffer_size, local_app_data, "WindowFileSearch")) {
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

static int get_database_path(char *buffer, size_t buffer_size) {
    char data_directory[PATH_BUFFER_SIZE];

    if (!get_app_data_directory(data_directory, sizeof(data_directory))) {
        return 0;
    }

    return join_path(buffer, buffer_size, data_directory, "index.db");
}

static int execute_sql(sqlite3 *db, const char *sql) {
    char *error_message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error_message);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", error_message != NULL ? error_message : sqlite3_errmsg(db));
        sqlite3_free(error_message);
        return 0;
    }

    return 1;
}

static sqlite3 *open_database(void) {
    sqlite3 *db = NULL;
    char database_path[PATH_BUFFER_SIZE];

    if (!load_sqlite_api()) {
        return NULL;
    }

    if (!get_database_path(database_path, sizeof(database_path))) {
        fprintf(stderr, "Failed to resolve database path.\n");
        return NULL;
    }

    if (sqlite3_open(database_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open %s: %s\n", database_path, db != NULL ? sqlite3_errmsg(db) : "unknown error");
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    if (!execute_sql(db, "PRAGMA busy_timeout = 5000;") ||
        !execute_sql(db, "PRAGMA journal_mode = WAL;") ||
        !execute_sql(db, "CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, filename TEXT, path TEXT);") ||
        !execute_sql(db, "CREATE INDEX IF NOT EXISTS idx_filename ON files(filename);")) {
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

static int database_has_files(sqlite3 *db) {
    sqlite3_stmt *statement = NULL;
    int has_files = 0;

    if (sqlite3_prepare_v2(db, "SELECT 1 FROM files LIMIT 1;", -1, &statement, NULL) != SQLITE_OK) {
        return 0;
    }

    if (sqlite3_step(statement) == SQLITE_ROW) {
        has_files = 1;
    }

    sqlite3_finalize(statement);
    return has_files;
}

static int path_is_directory(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int join_path(char *destination, size_t destination_size, const char *parent, const char *child) {
    size_t parent_length = strlen(parent);
    int written;

    if (parent_length > 0 && (parent[parent_length - 1] == '\\' || parent[parent_length - 1] == '/')) {
        written = snprintf(destination, destination_size, "%s%s", parent, child);
    } else {
        written = snprintf(destination, destination_size, "%s\\%s", parent, child);
    }

    return written >= 0 && (size_t)written < destination_size;
}

static int should_index_drive_type(UINT drive_type) {
    return drive_type == DRIVE_FIXED ||
           drive_type == DRIVE_REMOVABLE ||
           drive_type == DRIVE_REMOTE ||
           drive_type == DRIVE_CDROM ||
           drive_type == DRIVE_RAMDISK;
}

static void queue_init(DirectoryQueue *queue) {
    ZeroMemory(queue, sizeof(*queue));
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->available);
}

static void queue_destroy(DirectoryQueue *queue) {
    DirectoryNode *node;

    EnterCriticalSection(&queue->lock);
    node = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->pending = 0;
    LeaveCriticalSection(&queue->lock);

    while (node != NULL) {
        DirectoryNode *next_node = node->next;
        free(node->path);
        free(node);
        node = next_node;
    }

    DeleteCriticalSection(&queue->lock);
}

static void queue_abort(DirectoryQueue *queue) {
    EnterCriticalSection(&queue->lock);
    queue->stop = 1;
    WakeAllConditionVariable(&queue->available);
    LeaveCriticalSection(&queue->lock);
}

static int queue_push(DirectoryQueue *queue, const char *path) {
    DirectoryNode *node = (DirectoryNode *)malloc(sizeof(*node));

    if (node == NULL) {
        return 0;
    }

    node->path = duplicate_string(path);
    if (node->path == NULL) {
        free(node);
        return 0;
    }

    node->next = NULL;

    EnterCriticalSection(&queue->lock);
    if (queue->stop) {
        LeaveCriticalSection(&queue->lock);
        free(node->path);
        free(node);
        return 0;
    }

    if (queue->tail != NULL) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }

    queue->tail = node;
    queue->pending++;
    WakeConditionVariable(&queue->available);
    LeaveCriticalSection(&queue->lock);
    return 1;
}

static DirectoryNode *queue_pop(DirectoryQueue *queue) {
    DirectoryNode *node;

    EnterCriticalSection(&queue->lock);
    while (queue->head == NULL && queue->pending > 0 && !queue->stop) {
        SleepConditionVariableCS(&queue->available, &queue->lock, INFINITE);
    }

    if (queue->head == NULL) {
        LeaveCriticalSection(&queue->lock);
        return NULL;
    }

    node = queue->head;
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }

    LeaveCriticalSection(&queue->lock);
    return node;
}

static void queue_mark_complete(DirectoryQueue *queue) {
    EnterCriticalSection(&queue->lock);
    if (queue->pending > 0) {
        queue->pending--;
    }
    if (queue->pending == 0) {
        WakeAllConditionVariable(&queue->available);
    }
    LeaveCriticalSection(&queue->lock);
}

static int queue_all_logical_drives(DirectoryQueue *queue) {
    DWORD drive_mask = GetLogicalDrives();

    if (drive_mask == 0) {
        return 0;
    }

    for (int drive_index = 0; drive_index < 26; ++drive_index) {
        char root_path[4];
        UINT drive_type;

        if ((drive_mask & (1UL << drive_index)) == 0) {
            continue;
        }

        root_path[0] = (char)('A' + drive_index);
        root_path[1] = ':';
        root_path[2] = '\\';
        root_path[3] = '\0';

        drive_type = GetDriveTypeA(root_path);
        if (!should_index_drive_type(drive_type)) {
            continue;
        }

        if (!queue_push(queue, root_path)) {
            return 0;
        }
    }

    return 1;
}

static int insert_file_row(IndexContext *context, const char *full_path) {
    char filename[PATH_BUFFER_SIZE];
    int rc;

    if (!get_basename(full_path, filename, sizeof(filename))) {
        return 0;
    }

    EnterCriticalSection(&context->db_lock);
    rc = sqlite3_bind_text(context->insert_statement, 1, filename, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(context->insert_statement, 2, full_path, -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(context->insert_statement);
    }

    if (rc == SQLITE_DONE) {
        InterlockedIncrement(&context->file_count);
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert file: %s\n", full_path);
    }

    sqlite3_reset(context->insert_statement);
    sqlite3_clear_bindings(context->insert_statement);
    LeaveCriticalSection(&context->db_lock);
    return rc == SQLITE_DONE;
}

static void mark_index_failure(IndexContext *context) {
    EnterCriticalSection(&context->queue->lock);
    context->failed = 1;
    context->queue->stop = 1;
    WakeAllConditionVariable(&context->queue->available);
    LeaveCriticalSection(&context->queue->lock);
}

static void crawl_directory_contents(IndexContext *context, const char *directory_path) {
    char search_pattern[PATH_BUFFER_SIZE];
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle;

    if (snprintf(search_pattern, sizeof(search_pattern), "%s\\*", directory_path) >= (int)sizeof(search_pattern)) {
        return;
    }

    find_handle = FindFirstFileA(search_pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        char full_path[PATH_BUFFER_SIZE];

        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }

        if (!join_path(full_path, sizeof(full_path), directory_path, find_data.cFileName)) {
            continue;
        }

        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                if (!queue_push(context->queue, full_path)) {
                    fprintf(stderr, "Out of memory while queuing directory: %s\n", full_path);
                    mark_index_failure(context);
                    break;
                }
            }
            continue;
        }

        if (!insert_file_row(context, full_path) && context->failed) {
            break;
        }
    } while (!context->queue->stop && FindNextFileA(find_handle, &find_data) != 0);

    FindClose(find_handle);
}

static unsigned __stdcall index_worker_thread(void *parameter) {
    IndexContext *context = (IndexContext *)parameter;

    for (;;) {
        DirectoryNode *node = queue_pop(context->queue);

        if (node == NULL) {
            break;
        }

        crawl_directory_contents(context, node->path);
        free(node->path);
        free(node);
        queue_mark_complete(context->queue);
    }

    return 0;
}

static size_t determine_worker_count(void) {
    SYSTEM_INFO system_info;
    size_t worker_count;

    GetSystemInfo(&system_info);
    worker_count = system_info.dwNumberOfProcessors;
    if (worker_count < 2) {
        worker_count = 2;
    }
    if (worker_count > MAX_INDEX_WORKERS) {
        worker_count = MAX_INDEX_WORKERS;
    }

    return worker_count;
}

static int run_indexing(int all_drives, int root_count, const char **roots) {
    sqlite3 *db = open_database();
    sqlite3_stmt *insert_statement = NULL;
    IndexContext context;
    HANDLE worker_handles[MAX_INDEX_WORKERS];
    size_t worker_count;
    size_t worker_index;
    int success = 1;

    if (db == NULL) {
        return 1;
    }

    ZeroMemory(&context, sizeof(context));
    context.db = db;
    context.file_count = 0;
    context.failed = 0;
    context.queue = (DirectoryQueue *)malloc(sizeof(DirectoryQueue));

    if (context.queue == NULL) {
        sqlite3_close(db);
        return 1;
    }

    queue_init(context.queue);
    InitializeCriticalSection(&context.db_lock);

    if (!execute_sql(db, "BEGIN IMMEDIATE TRANSACTION;") || !execute_sql(db, "DELETE FROM files;")) {
        DeleteCriticalSection(&context.db_lock);
        queue_destroy(context.queue);
        free(context.queue);
        sqlite3_close(db);
        return 1;
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO files (filename, path) VALUES (?, ?);", -1, &insert_statement, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare insert statement: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        DeleteCriticalSection(&context.db_lock);
        queue_destroy(context.queue);
        free(context.queue);
        sqlite3_close(db);
        return 1;
    }

    context.insert_statement = insert_statement;

    if (all_drives) {
        if (!queue_all_logical_drives(context.queue)) {
            fprintf(stderr, "Failed to queue logical drives.\n");
            context.failed = 1;
            queue_abort(context.queue);
            success = 0;
        }
    } else {
        for (size_t root_index = 0; root_index < (size_t)root_count; ++root_index) {
            const char *root_path = roots[root_index];

            if (!path_is_directory(root_path)) {
                fprintf(stderr, "Root path is not an accessible directory: %s\n", root_path);
                context.failed = 1;
                queue_abort(context.queue);
                success = 0;
                break;
            }

            if (!queue_push(context.queue, root_path)) {
                fprintf(stderr, "Failed to queue root directory: %s\n", root_path);
                context.failed = 1;
                queue_abort(context.queue);
                success = 0;
                break;
            }
        }
    }

    worker_count = determine_worker_count();
    for (worker_index = 0; worker_index < worker_count; ++worker_index) {
        unsigned thread_id;
        uintptr_t handle = _beginthreadex(NULL, 0, index_worker_thread, &context, 0, &thread_id);

        if (handle == 0) {
            fprintf(stderr, "Failed to create indexing worker thread.\n");
            queue_abort(context.queue);
            success = 0;
            worker_count = worker_index;
            break;
        }

        worker_handles[worker_index] = (HANDLE)handle;
    }

    if (worker_count > 0) {
        WaitForMultipleObjects((DWORD)worker_count, worker_handles, TRUE, INFINITE);
        for (worker_index = 0; worker_index < worker_count; ++worker_index) {
            CloseHandle(worker_handles[worker_index]);
        }
    }

    if (context.failed || !success) {
        sqlite3_finalize(insert_statement);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        DeleteCriticalSection(&context.db_lock);
        queue_destroy(context.queue);
        free(context.queue);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_finalize(insert_statement);
    if (!execute_sql(db, "COMMIT;")) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        DeleteCriticalSection(&context.db_lock);
        queue_destroy(context.queue);
        free(context.queue);
        sqlite3_close(db);
        return 1;
    }

    DeleteCriticalSection(&context.db_lock);
    queue_destroy(context.queue);
    free(context.queue);
    sqlite3_close(db);

    if (all_drives) {
        printf("Indexed %ld files across all logical drives using %zu worker threads\n", context.file_count, worker_count);
    } else {
        printf("Indexed %ld files into the user database using %zu worker threads\n", context.file_count, worker_count);
    }

    return 0;
}

static int index_directory(const char *root_path) {
    const char *roots[] = { root_path };
    return run_indexing(0, 1, roots);
}

static int index_all_drives(void);

static int index_multiple_roots(int root_count, const char **roots) {
    if (root_count <= 0 || roots == NULL) {
        return index_all_drives();
    }

    return run_indexing(0, root_count, roots);
}

static int index_all_drives(void) {
    return run_indexing(1, 0, NULL);
}

static int open_file_with_default_app(const char *path) {
    HINSTANCE result = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)result > 32) ? 0 : 1;
}

static void free_search_results(SearchResult *results, size_t result_count) {
    size_t index;

    for (index = 0; index < result_count; ++index) {
        free(results[index].filename);
        free(results[index].path);
    }

    free(results);
}

static void build_scope_pattern(const char *scope, char *buffer, size_t buffer_size) {
    size_t scope_length;

    if (scope == NULL || scope[0] == '\0') {
        buffer[0] = '\0';
        return;
    }

    scope_length = strlen(scope);
    if (scope_length > 0 && (scope[scope_length - 1] == '\\' || scope[scope_length - 1] == '/')) {
        snprintf(buffer, buffer_size, "%s%%", scope);
        return;
    }

    if (scope_length == 2 && scope[1] == ':') {
        snprintf(buffer, buffer_size, "%s\\%%", scope);
        return;
    }

    snprintf(buffer, buffer_size, "%s\\%%", scope);
}

static int search_files(const char *term, const char *scope) {
    sqlite3 *db;
    sqlite3_stmt *search_statement = NULL;
    SearchResult *results = NULL;
    size_t result_count = 0;
    size_t result_capacity = 0;
    char search_pattern[PATH_BUFFER_SIZE];
    char scope_pattern[PATH_BUFFER_SIZE];
    char selection_buffer[64];
    long selection;

    db = open_database();
    if (db == NULL) {
        return 1;
    }

    if (!database_has_files(db)) {
        sqlite3_close(db);
        printf("No index found. Indexing all logical drives automatically...\n");
        if (index_all_drives() != 0) {
            return 1;
        }
        db = open_database();
        if (db == NULL) {
            return 1;
        }
    }

    if (snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", term) >= (int)sizeof(search_pattern)) {
        fprintf(stderr, "Search term is too long.\n");
        sqlite3_close(db);
        return 1;
    }

    build_scope_pattern(scope, scope_pattern, sizeof(scope_pattern));

    if (scope_pattern[0] != '\0') {
        if (sqlite3_prepare_v2(db, "SELECT filename, path FROM files WHERE filename LIKE ? AND path LIKE ? ORDER BY filename COLLATE NOCASE, path COLLATE NOCASE;", -1, &search_statement, NULL) != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare search statement: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
            return 1;
        }
    } else {
        if (sqlite3_prepare_v2(db, "SELECT filename, path FROM files WHERE filename LIKE ? ORDER BY filename COLLATE NOCASE;", -1, &search_statement, NULL) != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare search statement: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
            return 1;
        }
    }

    if (sqlite3_bind_text(search_statement, 1, search_pattern, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        fprintf(stderr, "Failed to bind search term: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(search_statement);
        sqlite3_close(db);
        return 1;
    }

    if (scope_pattern[0] != '\0' && sqlite3_bind_text(search_statement, 2, scope_pattern, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        fprintf(stderr, "Failed to bind scope: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(search_statement);
        sqlite3_close(db);
        return 1;
    }

    while (sqlite3_step(search_statement) == SQLITE_ROW) {
        const unsigned char *filename_text = sqlite3_column_text(search_statement, 0);
        const unsigned char *path_text = sqlite3_column_text(search_statement, 1);
        SearchResult *new_results;

        if (result_count == result_capacity) {
            size_t new_capacity = result_capacity == 0 ? 16 : result_capacity * 2;
            new_results = (SearchResult *)realloc(results, new_capacity * sizeof(*results));

            if (new_results == NULL) {
                fprintf(stderr, "Out of memory while collecting results.\n");
                free_search_results(results, result_count);
                sqlite3_finalize(search_statement);
                sqlite3_close(db);
                return 1;
            }

            results = new_results;
            result_capacity = new_capacity;
        }

        results[result_count].filename = duplicate_string((const char *)filename_text);
        results[result_count].path = duplicate_string((const char *)path_text);

        if (results[result_count].filename == NULL || results[result_count].path == NULL) {
            fprintf(stderr, "Out of memory while copying results.\n");
            free(results[result_count].filename);
            free(results[result_count].path);
            free_search_results(results, result_count);
            sqlite3_finalize(search_statement);
            sqlite3_close(db);
            return 1;
        }

        result_count++;
    }

    sqlite3_finalize(search_statement);

    if (result_count == 0) {
        printf("No matches for '%s'.\n", term);
        sqlite3_close(db);
        free(results);
        return 0;
    }

    for (size_t index = 0; index < result_count; ++index) {
        printf("%zu. %s\n   %s\n", index + 1, results[index].filename, results[index].path);
    }

    printf("Select a result number to open (0 to exit): ");
    if (fgets(selection_buffer, sizeof(selection_buffer), stdin) == NULL) {
        free_search_results(results, result_count);
        sqlite3_close(db);
        return 0;
    }

    selection = strtol(selection_buffer, NULL, 10);
    if (selection > 0 && (size_t)selection <= result_count) {
        if (open_file_with_default_app(results[selection - 1].path) != 0) {
            fprintf(stderr, "Failed to open file: %s\n", results[selection - 1].path);
        }
    }

    free_search_results(results, result_count);
    sqlite3_close(db);
    return 0;
}

static int is_executable_name(const char *argv0, const char *expected_name) {
    const char *basename = strrchr(argv0, '\\');

    if (basename == NULL) {
        basename = strrchr(argv0, '/');
    }

    basename = basename == NULL ? argv0 : basename + 1;
    return _stricmp(basename, expected_name) == 0;
}

static void print_usage(void) {
    printf("Usage:\n");
    printf("  indexer.exe [root_path]\n");
    printf("  search.exe <filename_term> [scope_path]\n");
    printf("  fileCrawler.exe index [root_path]\n");
    printf("  fileCrawler.exe search <filename_term> [scope_path]\n");
    printf("\n");
    printf("If the database is empty, search will auto-index all logical drives.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (is_executable_name(argv[0], "indexer.exe") || is_executable_name(argv[0], "indexer")) {
        if (argc < 2) {
            return index_all_drives();
        }

        return index_directory(argv[1]);
    }

    if (is_executable_name(argv[0], "search.exe") || is_executable_name(argv[0], "search")) {
        return search_files(argv[1], argc >= 3 ? argv[2] : NULL);
    }

    if (_stricmp(argv[1], "index") == 0) {
        if (argc < 3) {
            return index_all_drives();
        }

        if (argc == 3) {
            return index_directory(argv[2]);
        }

        return index_multiple_roots(argc - 2, (const char **)&argv[2]);
    }

    if (_stricmp(argv[1], "search") == 0) {
        if (argc < 3) {
            print_usage();
            return 1;
        }

        return search_files(argv[2], argc >= 4 ? argv[3] : NULL);
    }

    print_usage();
    return 1;
}
