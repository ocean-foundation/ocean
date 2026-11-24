/* Compiler-owned C ABI for os/exec. No shell is inserted on either platform. */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <Availability.h>
#else
extern char **environ;
#endif
#endif

static volatile sig_atomic_t interrupted;
#ifdef _WIN32
static BOOL WINAPI interrupted_console(DWORD event) { if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) { interrupted = 1; return TRUE; } return FALSE; }
#else
static void interrupted_signal(int number) { interrupted = number; }
#endif
void ocean_process_clear_interrupt(void) { interrupted = 0; }
int32_t ocean_process_interrupted(void) { return interrupted; }
void ocean_process_watch(void) {
#ifdef _WIN32
    SetConsoleCtrlHandler(interrupted_console, TRUE);
#else
    struct sigaction action = {0}; action.sa_handler = interrupted_signal;
    sigemptyset(&action.sa_mask); sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
#endif
}
typedef struct {
    const char *path;
    const char *const *argv;
    const char *directory;
    const char *const *environment;
    int64_t capture, combined, exit_code;
    char *output;
    int64_t output_size;
    const char *error;
} OceanProcess;
static int fail(OceanProcess *p, const char *text) { p->error = text; return 1; }
static int append(OceanProcess *p, const char *bytes, size_t count) {
    if (count > SIZE_MAX - (size_t)p->output_size - 1) return 0;
    char *out = realloc(p->output, (size_t)p->output_size + count + 1);
    if (!out) return 0;
    memcpy(out + p->output_size, bytes, count); p->output_size += (int64_t)count;
    out[p->output_size] = 0; p->output = out; return 1;
}
#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *out = n ? malloc((size_t)n * sizeof(*out)) : NULL;
    if (out) MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}
static char *utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = n ? malloc((size_t)n) : NULL;
    if (out) WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}
char *ocean_process_argument(int64_t index) {
    int count = 0; wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return NULL;
    char *out = index >= 0 && index < count ? utf8(args[index]) : NULL;
    LocalFree(args); return out;
}
/* Quoting follows CommandLineToArgvW / the Microsoft CRT, including trailing \. */
static wchar_t *command_line(const char *const *argv) {
    size_t length = 1;
    for (size_t i = 0; argv[i]; i++) length += strlen(argv[i]) * 2 + 4;
    wchar_t *line = calloc(length, sizeof(*line)), *out = line;
    if (!line) return NULL;
    for (size_t i = 0; argv[i]; i++) {
        wchar_t *arg = wide(argv[i]); if (!arg) { free(line); return NULL; }
        if (i) *out++ = L' '; *out++ = L'"'; size_t slash = 0;
        for (const wchar_t *c = arg; ; c++) {
            if (*c == L'\\') { slash++; continue; }
            size_t n = (*c == L'"' || !*c) ? slash * 2 : slash;
            while (n--) *out++ = L'\\'; slash = 0;
            if (!*c) break;
            if (*c == L'"') *out++ = L'\\';
            *out++ = *c;
        }
        *out++ = L'"'; free(arg);
    }
    *out = 0; return line;
}
char *ocean_process_lookpath(const char *name) {
    wchar_t *input = wide(name), found[32768]; if (!input) return NULL;
    DWORD n = SearchPathW(NULL, input, L".exe", 32768, found, NULL); free(input);
    if (!n || n >= 32768 || (GetFileAttributesW(found) & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    /* Reject the implicit current-directory search, matching exec.LookPath. */
    if (!strchr(name, '/') && !strchr(name, '\\') && !strchr(name, ':')) {
        wchar_t cwd[32768]; GetCurrentDirectoryW(32768, cwd);
        wchar_t *slash = wcsrchr(found, L'\\');
        if (slash) { *slash = 0; int same = !_wcsicmp(found, cwd); *slash = L'\\'; if (same) return NULL; }
    }
    return utf8(found);
}
static int compare_env(const void *a, const void *b) { return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b); }
static HANDLE inherited(HANDLE original) {
    HANDLE result = INVALID_HANDLE_VALUE;
    if (!original || original == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &result, 0, TRUE, DUPLICATE_SAME_ACCESS)) return INVALID_HANDLE_VALUE;
    return result;
}
static volatile LONG64 temp_sequence;
static wchar_t *put_hex(wchar_t *p, unsigned long long v) {
    static const wchar_t hex[] = L"0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) *p++ = hex[(v >> shift) & 15];
    return p;
}
/* Atomically creates an inheritable, exclusive, delete-on-close temporary file,
 * supporting long paths via the \\?\ prefix without a reopen race. */
static HANDLE temp_file(const wchar_t *prefix) {
    wchar_t base[32768], full[32768], path[32768];
    DWORD bn = GetTempPathW(32768, base);
    if (!bn || bn >= 32768) return INVALID_HANDLE_VALUE;
    DWORD fn = GetFullPathNameW(base, 32768, full, NULL);
    if (!fn || fn >= 32768) return INVALID_HANDLE_VALUE;
    size_t pos = 0;
    if (wcsncmp(full, L"\\\\?\\", 4) == 0) {
        if (fn >= 32700) return INVALID_HANDLE_VALUE;
        memcpy(path, full, fn * sizeof(wchar_t)); pos = fn;
    } else if (full[0] == L'\\' && full[1] == L'\\') {
        if (fn + 6 >= 32700) return INVALID_HANDLE_VALUE;
        memcpy(path, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
        memcpy(path + 8, full + 2, (fn - 2) * sizeof(wchar_t)); pos = fn + 6;
    } else {
        if (fn + 4 >= 32700) return INVALID_HANDLE_VALUE;
        memcpy(path, L"\\\\?\\", 4 * sizeof(wchar_t));
        memcpy(path + 4, full, fn * sizeof(wchar_t)); pos = fn + 4;
    }
    if (pos && path[pos - 1] != L'\\') path[pos++] = L'\\';
    size_t plen = wcslen(prefix);
    if (pos + plen + 64 >= 32768) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    unsigned long long pid = (unsigned long long)GetCurrentProcessId();
    for (int attempt = 0; attempt < 64; attempt++) {
        LARGE_INTEGER counter = {0}; QueryPerformanceCounter(&counter);
        unsigned long long seq = (unsigned long long)InterlockedIncrement64(&temp_sequence);
        wchar_t *at = path + pos;
        memcpy(at, prefix, plen * sizeof(wchar_t)); at += plen;
        *at++ = L'-'; at = put_hex(at, pid);
        *at++ = L'-'; at = put_hex(at, (unsigned long long)counter.QuadPart);
        *at++ = L'-'; at = put_hex(at, seq);
        memcpy(at, L".tmp", 5 * sizeof(wchar_t));
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, &security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (file != INVALID_HANDLE_VALUE) return file;
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}
/* Writes input to an inheritable, delete-on-close temporary file positioned at
 * its start, for use as a child's standard input. */
static HANDLE input_file(const char *input, int64_t size) {
    HANDLE file = temp_file(L"oci");
    if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    for (int64_t done = 0; done < size; ) {
        DWORD chunk = size - done > 1 << 30 ? 1 << 30 : (DWORD)(size - done), written = 0;
        if (!WriteFile(file, input + done, chunk, &written, NULL) || !written) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
        done += written;
    }
    LARGE_INTEGER zero = {0};
    if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
    return file;
}
/* Starts p with the given standard-input and capture handles (either may be
 * INVALID_HANDLE_VALUE to inherit). Returns the process handle or NULL. */
static HANDLE spawn(OceanProcess *p, HANDLE input, HANDLE capture) {
    char *resolved = ocean_process_lookpath(p->path);
    if (!resolved) { fail(p, "exec: executable not found"); return NULL; }
    wchar_t *application = wide(resolved), *line = command_line(p->argv);
    wchar_t *directory = p->directory && *p->directory ? wide(p->directory) : NULL;
    free(resolved);
    if (!application || !line || (p->directory && *p->directory && !directory)) { free(application); free(line); free(directory); fail(p, "exec: invalid UTF-8 or allocation failed"); return NULL; }
    wchar_t *environment = NULL;
    if (p->environment) {
        size_t count = 2; for (size_t i = 0; p->environment[i]; i++) count += strlen(p->environment[i]) + 1;
        environment = calloc(count, sizeof(*environment));
        if (!environment) { free(application); free(line); free(directory); fail(p, "exec: allocation failed"); return NULL; }
        size_t total = 0; while (p->environment[total]) total++;
        wchar_t **entries = calloc(total ? total : 1, sizeof(*entries));
        if (!entries) { free(application); free(line); free(directory); free(environment); fail(p, "exec: allocation failed"); return NULL; }
        int valid = 1;
        for (size_t i = 0; i < total; i++) if (!(entries[i] = wide(p->environment[i]))) valid = 0;
        if (!valid) { for (size_t i = 0; i < total; i++) free(entries[i]); free(entries); free(application); free(line); free(directory); free(environment); fail(p, "exec: invalid environment"); return NULL; }
        qsort(entries, total, sizeof(*entries), compare_env);
        wchar_t *at = environment;
        for (size_t i = 0; i < total; i++) { size_t n = wcslen(entries[i])+1; memcpy(at,entries[i],n*sizeof(*at)); at += n; free(entries[i]); }
        free(entries);
    }
    HANDLE handles[3] = {input != INVALID_HANDLE_VALUE ? input : inherited(GetStdHandle(STD_INPUT_HANDLE)), p->capture ? capture : inherited(GetStdHandle(STD_OUTPUT_HANDLE)), p->combined ? capture : inherited(GetStdHandle(STD_ERROR_HANDLE))};
    STARTUPINFOEXW startup = {0}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0]; startup.StartupInfo.hStdOutput = handles[1]; startup.StartupInfo.hStdError = handles[2];
    HANDLE allowed[3]; size_t used = 0;
    for (int i = 0; i < 3; i++) if (handles[i] != INVALID_HANDLE_VALUE) { int duplicate = 0; for (size_t j = 0; j < used; j++) if (allowed[j] == handles[i]) duplicate = 1; if (!duplicate) allowed[used++] = handles[i]; }
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    startup.lpAttributeList = malloc(bytes);
    int initialized = startup.lpAttributeList && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
    int configured = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed, used * sizeof(HANDLE), NULL, NULL);
    PROCESS_INFORMATION process = {0};
    int created = configured && CreateProcessW(application, line, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment, directory, &startup.StartupInfo, &process);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList); free(startup.lpAttributeList);
    for (int i = 0; i < 3; i++) if (handles[i] != INVALID_HANDLE_VALUE && handles[i] != capture && handles[i] != input) CloseHandle(handles[i]);
    free(application); free(line); free(directory); free(environment);
    if (!created) { fail(p, "exec: executable could not be started"); return NULL; }
    CloseHandle(process.hThread);
    return process.hProcess;
}
int32_t ocean_process_run_input(OceanProcess *p, const char *input, int64_t input_size, int64_t has_input) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL;
    HANDLE capture = INVALID_HANDLE_VALUE, stdin_file = INVALID_HANDLE_VALUE;
    if (p->capture) {
        capture = temp_file(L"ocx");
        if (capture == INVALID_HANDLE_VALUE) return fail(p, "exec: cannot create output file");
    }
    if (has_input && (stdin_file = input_file(input, input_size)) == INVALID_HANDLE_VALUE) { if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture); return fail(p, "exec: cannot create input file"); }
    HANDLE child = spawn(p, stdin_file, capture);
    if (stdin_file != INVALID_HANDLE_VALUE) CloseHandle(stdin_file);
    if (!child) { if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture); return 1; }
    DWORD code = 0;
    DWORD waiting;
    do { waiting = WaitForSingleObject(child, 20); if (interrupted) TerminateProcess(child, 130); } while (waiting == WAIT_TIMEOUT);
    int waited = waiting == WAIT_OBJECT_0 && GetExitCodeProcess(child, &code);
    CloseHandle(child); p->exit_code = waited ? (int64_t)code : -1;
    int read_ok = 1;
    if (capture != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero = {0}; read_ok = SetFilePointerEx(capture, zero, NULL, FILE_BEGIN) != 0;
        char buffer[8192]; DWORD n;
        while (read_ok) { if (!ReadFile(capture, buffer, sizeof(buffer), &n, NULL)) { read_ok = 0; break; } if (!n) break; if (!append(p, buffer, n)) read_ok = 0; }
        CloseHandle(capture);
    }
    if (!waited) return fail(p, "exec: wait failed");
    if (!read_ok) return fail(p, "exec: output read failed");
    return code ? fail(p, "exec: process exited unsuccessfully") : 0;
}
/* Starts p without capture; *handle receives the process handle for wait/kill. */
int32_t ocean_process_start(OceanProcess *p, const char *input, int64_t input_size, int64_t has_input, int64_t *handle, int64_t *pid) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL; *handle = 0; *pid = -1;
    HANDLE stdin_file = INVALID_HANDLE_VALUE;
    if (has_input && (stdin_file = input_file(input, input_size)) == INVALID_HANDLE_VALUE) return fail(p, "exec: cannot create input file");
    p->capture = 0; p->combined = 0;
    HANDLE child = spawn(p, stdin_file, INVALID_HANDLE_VALUE);
    if (stdin_file != INVALID_HANDLE_VALUE) CloseHandle(stdin_file);
    if (!child) return 1;
    *handle = (int64_t)(intptr_t)child; *pid = (int64_t)GetProcessId(child);
    return 0;
}
/* Returns 1 when the process has exited (releasing the handle), 0 while it
 * runs (only without block), or -1 on failure. */
int32_t ocean_process_wait(int64_t handle, int64_t block, int64_t *exit_code, int64_t *signal_number) {
    HANDLE child = (HANDLE)(intptr_t)handle; *exit_code = -1; *signal_number = 0;
    DWORD waiting = WaitForSingleObject(child, block ? INFINITE : 0);
    if (waiting == WAIT_TIMEOUT) return 0;
    DWORD code = 0;
    int waited = waiting == WAIT_OBJECT_0 && GetExitCodeProcess(child, &code);
    CloseHandle(child);
    if (!waited) return -1;
    *exit_code = (int64_t)code; return 1;
}
int32_t ocean_process_kill(int64_t handle) { return TerminateProcess((HANDLE)(intptr_t)handle, 1) ? 0 : -1; }
int32_t ocean_process_run(OceanProcess *p) { return ocean_process_run_input(p, NULL, 0, 0); }
#else
char *ocean_process_argument(int64_t index) { (void)index; return NULL; }
char *ocean_process_lookpath(const char *name) {
    const char *path = getenv("PATH"); if (!path) path = "/bin:/usr/bin";
    int explicit = strchr(name, '/') != NULL;
    for (const char *at = path; ; ) {
        const char *end = strchr(at, ':'); size_t n = end ? (size_t)(end - at) : strlen(at);
        char *candidate = explicit ? strdup(name) : malloc(n + strlen(name) + 3);
        if (!candidate) return NULL;
        if (!explicit) { memcpy(candidate, at, n); candidate[n] = '/'; strcpy(candidate + n + 1, name); if (!n) { free(candidate); candidate = malloc(strlen(name) + 3); if (!candidate) return NULL; sprintf(candidate, "./%s", name); } }
        if (!access(candidate, X_OK)) { DIR *dir = opendir(candidate); if (dir) closedir(dir); else { if (!explicit && candidate[0] != '/') { free(candidate); return NULL; } return candidate; } }
        free(candidate); if (explicit || !end) return NULL; at = end + 1;
    }
}
// New SDKs deprecate the _np spelling, but older macOS releases still need it.
static int add_directory(posix_spawn_file_actions_t *actions, const char *directory) {
#ifdef __APPLE__
#if defined(__MAC_26_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_26_0
    if (__builtin_available(macOS 26.0, *))
        return posix_spawn_file_actions_addchdir(actions, directory);
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    int result = posix_spawn_file_actions_addchdir_np(actions, directory);
#pragma clang diagnostic pop
    return result;
#else
    return posix_spawn_file_actions_addchdir_np(actions, directory);
#endif
}
/* Creates an unlinked, close-on-exec temporary file descriptor >= 3 so it
 * cannot leak to concurrently spawned children or collide with fds 0..2. */
static int temp_fd(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    char *path = malloc(n + 16);
    if (!path) return -1;
    memcpy(path, dir, n);
    memcpy(path + n, "/ocean-XXXXXX", 14);
    int fd = mkostemp(path, O_CLOEXEC);
    if (fd >= 0) unlink(path);
    free(path);
    if (fd < 0) return -1;
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        if (moved < 0) return -1;
        fd = moved;
    }
    return fd;
}
/* A rewound temporary file holding input, for a child's standard input. */
static int input_file(const char *input, int64_t size) {
    int fd = temp_fd();
    if (fd < 0) return -1;
    for (int64_t done = 0; done < size; ) {
        ssize_t w = write(fd, input + done, (size_t)(size - done));
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (w == 0) { close(fd); return -1; }
        done += w;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    return fd;
}
/* Starts p with optional standard-input and capture fds (>= 3); returns the pid or -1. */
static pid_t spawn(OceanProcess *p, int input, int capture) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { fail(p, "exec: cannot initialize spawn actions"); return -1; }
    int failed = 0;
    if (input >= 0) failed = posix_spawn_file_actions_adddup2(&actions, input, 0);
    if (!failed && capture >= 0) {
        failed = posix_spawn_file_actions_adddup2(&actions, capture, 1);
        if (!failed && p->combined) failed = posix_spawn_file_actions_adddup2(&actions, capture, 2);
    }
    if (!failed && p->directory && *p->directory) failed = add_directory(&actions, p->directory);
#ifdef __APPLE__
    char **env = p->environment ? (char **)p->environment : *_NSGetEnviron();
#else
    char **env = p->environment ? (char **)p->environment : environ;
#endif
    pid_t child = -1;
    if (!failed) failed = posix_spawnp(&child, p->path, &actions, NULL, (char *const *)p->argv, env);
    posix_spawn_file_actions_destroy(&actions);
    if (failed) { fail(p, "exec: executable could not be started"); return -1; }
    return child;
}
int32_t ocean_process_run_input(OceanProcess *p, const char *input, int64_t input_size, int64_t has_input) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL;
    int file = p->capture ? temp_fd() : -1;
    if (p->capture && file < 0) return fail(p, "exec: executable could not be started");
    int stdin_file = has_input ? input_file(input, input_size) : -1;
    if (has_input && stdin_file < 0) { if (file >= 0) close(file); return fail(p, "exec: cannot create input file"); }
    pid_t child = spawn(p, stdin_file, file);
    if (stdin_file >= 0) close(stdin_file);
    if (child < 0) { if (file >= 0) close(file); return 1; }
    int status;
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) { if (file >= 0) close(file); return fail(p, "exec: wait failed"); } if (interrupted) kill(child, SIGTERM); }
    p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int read_ok = 1;
    if (file >= 0) {
        if (lseek(file, 0, SEEK_SET) < 0) read_ok = 0;
        char buffer[8192];
        while (read_ok) {
            ssize_t n = read(file, buffer, sizeof(buffer));
            if (n < 0) { if (errno == EINTR) continue; read_ok = 0; break; }
            if (n == 0) break;
            if (!append(p, buffer, (size_t)n)) read_ok = 0;
        }
        close(file);
    }
    if (!read_ok) return fail(p, "exec: output read failed");
    return p->exit_code ? fail(p, "exec: process exited unsuccessfully") : 0;
}
/* Starts p without capture; *handle receives the pid for wait/kill. */
int32_t ocean_process_start(OceanProcess *p, const char *input, int64_t input_size, int64_t has_input, int64_t *handle, int64_t *pid) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL; *handle = 0; *pid = -1;
    int stdin_file = has_input ? input_file(input, input_size) : -1;
    if (has_input && stdin_file < 0) return fail(p, "exec: cannot create input file");
    p->capture = 0; p->combined = 0;
    pid_t child = spawn(p, stdin_file, -1);
    if (stdin_file >= 0) close(stdin_file);
    if (child < 0) return 1;
    *handle = child; *pid = child;
    return 0;
}
/* Returns 1 when the process has exited (reaping it), 0 while it runs (only
 * without block), or -1 on failure. A signal exit reports exit code -1. */
int32_t ocean_process_wait(int64_t handle, int64_t block, int64_t *exit_code, int64_t *signal_number) {
    *exit_code = -1; *signal_number = 0;
    int status; pid_t result;
    while ((result = waitpid((pid_t)handle, &status, block ? 0 : WNOHANG)) < 0) if (errno != EINTR) return -1;
    if (result == 0) return 0;
    if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) *signal_number = WTERMSIG(status);
    return 1;
}
int32_t ocean_process_kill(int64_t handle) { return kill((pid_t)handle, SIGKILL) ? -1 : 0; }
int32_t ocean_process_run(OceanProcess *p) { return ocean_process_run_input(p, NULL, 0, 0); }
#endif
