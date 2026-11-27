/* Savage Wind Co-op.exe — Win32 launcher, no console.
 *
 *   windres -O coff launcher/coop_launcher.rc -o coop_launcher_rc.o
 *   clang -O2 -municode -mwindows launcher/coop_launcher.c coop_launcher_rc.o -o "Savage Wind Co-op.exe"
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef wchar_t Path[1024];
static const wchar_t title[] = L"Savage Wind Co-op 0.1.0";
static const wchar_t setup_text[] =
    L"Setting up co-op from your game files (about a minute, only once)...";
static const wchar_t ini_name[] = L"savagewind.ini";
static Path test_log;
/* Only the current unpublished copy is ever removed on failure. */
static Path staging;

static void append_bytes(const wchar_t *path, const char *text) {
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(file);
}

static void message(const wchar_t *text, UINT icon) {
    if (*test_log) {
        char utf8[16384];
        if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, NULL, NULL)) {
            append_bytes(test_log, utf8);
            append_bytes(test_log, "\r\n");
        }
        return;
    }
    MessageBoxW(NULL, text, title, MB_OK | icon | MB_SETFOREGROUND);
}

static void remove_stage(const wchar_t *path);

static void fail(const wchar_t *format, ...) {
    wchar_t text[4096];
    va_list args;
    va_start(args, format);
    vswprintf(text, _countof(text), format, args);
    va_end(args);
    text[_countof(text) - 1] = 0;
    if (*staging) remove_stage(staging);
    message(text, MB_ICONERROR);
    ExitProcess(1);
}

static void file_error(const wchar_t *action, const wchar_t *path) {
    DWORD code = GetLastError();
    fail(L"Could not %ls:\n%ls\n\nWindows error %lu.", action, path, (unsigned long)code);
}

static void join(Path path, const wchar_t *base, const wchar_t *name) {
    const wchar_t *separator = base[wcslen(base) - 1] == L'\\' ? L"" : L"\\";
    if (swprintf(path, sizeof(Path) / sizeof(wchar_t), L"%ls%ls%ls", base, separator, name) < 0)
        fail(L"A co-op file path is too long.");
}

static void parent(Path path) {
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) { *path = 0; return; }
    if (slash == path + 2 && path[1] == L':') {
        if (slash[1]) slash[1] = 0;
        else *path = 0;
        return;
    }
    *slash = 0;
}

static BOOL missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static DWORD attributes(const wchar_t *path) {
    DWORD result = GetFileAttributesW(path);
    if (result == INVALID_FILE_ATTRIBUTES && !missing(GetLastError()))
        file_error(L"check this file or folder", path);
    return result;
}

static BOOL exists(const wchar_t *path) {
    return attributes(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL is_file(const wchar_t *path) {
    DWORD attr = attributes(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void no_link(const wchar_t *path, DWORD attr) {
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        fail(L"Linked data paths are not supported:\n%ls", path);
}

static void check_ancestors(const wchar_t *path) {
    Path ancestor;
    wcscpy(ancestor, path);
    while (*ancestor) {
        no_link(ancestor, attributes(ancestor));
        parent(ancestor);
    }
}

static BOOL dot_entry(const WIN32_FIND_DATAW *entry) {
    return !wcscmp(entry->cFileName, L".") || !wcscmp(entry->cFileName, L"..");
}

static HANDLE find_files(const wchar_t *dir, const wchar_t *pattern, WIN32_FIND_DATAW *entry) {
    Path search;
    join(search, dir, pattern);
    HANDLE find = FindFirstFileW(search, entry);
    if (find == INVALID_HANDLE_VALUE && !missing(GetLastError()))
        file_error(L"list files in this folder", dir);
    return find;
}

static BOOL next_file(HANDLE find, WIN32_FIND_DATAW *entry, const wchar_t *dir) {
    if (FindNextFileW(find, entry)) return TRUE;
    if (GetLastError() != ERROR_NO_MORE_FILES) file_error(L"list files in this folder", dir);
    FindClose(find);
    return FALSE;
}

static void check_tree(const wchar_t *path) {
    DWORD attr = attributes(path);
    no_link(path, attr);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    WIN32_FIND_DATAW entry;
    HANDLE find = find_files(path, L"*", &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (dot_entry(&entry)) continue;
        Path child;
        join(child, path, entry.cFileName);
        check_tree(child);
    } while (next_file(find, &entry, path));
}

static void make_dir(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL)) return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD attr = attributes(path);
        no_link(path, attr);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
        SetLastError(ERROR_ALREADY_EXISTS);
    }
    file_error(L"create this folder", path);
}

static void remove_stage(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            Path search;
            if (swprintf(search, _countof(search), L"%ls\\*", path) < 0) return;
            WIN32_FIND_DATAW entry;
            HANDLE find = FindFirstFileW(search, &entry);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (dot_entry(&entry)) continue;
                    Path child;
                    if (swprintf(child, _countof(child), L"%ls\\%ls", path, entry.cFileName) >= 0)
                        remove_stage(child);
                } while (FindNextFileW(find, &entry));
                FindClose(find);
            }
        }
        RemoveDirectoryW(path);
    } else {
        if ((attr & FILE_ATTRIBUTE_READONLY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT))
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path);
    }
}

static void copy_contents(const wchar_t *src, const wchar_t *dst, BOOL recurse);

static void copy_item(const wchar_t *src, const wchar_t *dst) {
    DWORD attr = attributes(src);
    no_link(src, attr);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        make_dir(dst);
        copy_contents(src, dst, TRUE);
    } else if (!CopyFileW(src, dst, TRUE)) {
        file_error(L"copy this file", src);
    }
}

static void copy_contents(const wchar_t *src, const wchar_t *dst, BOOL recurse) {
    WIN32_FIND_DATAW entry;
    HANDLE find = find_files(src, L"*", &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (dot_entry(&entry) || (!recurse && (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) continue;
        Path from, to;
        join(from, src, entry.cFileName);
        join(to, dst, entry.cFileName);
        copy_item(from, to);
    } while (next_file(find, &entry, src));
}

static void start_stage(const wchar_t *dst, BOOL directory) {
    Path path;
    if (swprintf(path, _countof(path), L"%ls.copy-%lu-%llu", dst,
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64()) < 0)
        fail(L"A co-op file path is too long.");
    if (directory) {
        if (!CreateDirectoryW(path, NULL)) file_error(L"create a temporary copy folder", path);
    } else {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) file_error(L"create a temporary file", path);
        CloseHandle(file);
    }
    wcscpy(staging, path);
}

static void publish_stage(const wchar_t *dst, DWORD flags) {
    if (!MoveFileExW(staging, dst, flags)) file_error(L"finish copying to this path", dst);
    *staging = 0;
}

/* A missing source is optional, except for directories initialized empty (save and cache). */
static void stage_copy(const wchar_t *src, const wchar_t *dst, DWORD attr, BOOL directory, BOOL recurse) {
    no_link(src, attr);
    start_stage(dst, directory);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (directory) {
        copy_contents(src, staging, recurse);
    } else if (!CopyFileW(src, staging, FALSE)) {
        file_error(L"copy this file", src);
    }
}

static void copy_once(const wchar_t *src, const wchar_t *dst) {
    if (exists(dst)) return;
    DWORD attr = attributes(src);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    stage_copy(src, dst, attr, (attr & FILE_ATTRIBUTE_DIRECTORY) != 0, TRUE);
    publish_stage(dst, 0);
}

static void copy_named(const wchar_t *src, const wchar_t *dst, const wchar_t *name) {
    Path from, to;
    join(from, src, name);
    join(to, dst, name);
    copy_once(from, to);
}

static void copy_fonts(const wchar_t *game, const wchar_t *dst) {
    WIN32_FIND_DATAW entry;
    HANDLE find = find_files(game, L"*.ttf", &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) copy_named(game, dst, entry.cFileName);
    } while (next_file(find, &entry, game));
}

static char *read_bytes(const wchar_t *path, size_t *length, size_t tail) {
    FILE *file = _wfopen(path, L"rb");
    if (!file) return NULL;
    if (_fseeki64(file, 0, SEEK_END)) { fclose(file); return NULL; }
    __int64 size = _ftelli64(file);
    if (size < 0 || (!tail && size > 8 * 1024 * 1024)) { fclose(file); return NULL; }
    __int64 offset = tail && size > (__int64)tail ? size - (__int64)tail : 0;
    if (_fseeki64(file, offset, SEEK_SET)) { fclose(file); return NULL; }
    *length = (size_t)(size - offset);
    char *bytes = malloc(*length + 1);
    if (!bytes) fail(L"There is not enough memory to start co-op.");
    size_t got = fread(bytes, 1, *length, file);
    fclose(file);
    if (got != *length) { free(bytes); return NULL; }
    bytes[got] = 0;
    return bytes;
}

static wchar_t *read_text(const wchar_t *path, size_t tail) {
    size_t size;
    char *bytes = read_bytes(path, &size, tail);
    if (!bytes) return NULL;
    const unsigned char *data = (const unsigned char *)bytes;
    wchar_t *wide = calloc(size + 1, sizeof *wide);
    if (!wide) fail(L"There is not enough memory to start co-op.");
    if (size >= 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff))) {
        BOOL little = data[0] == 0xff;
        for (size_t i = 2; i + 1 < size; i += 2)
            wide[(i - 2) / 2] = little ? data[i] | data[i + 1] << 8 : data[i] << 8 | data[i + 1];
    } else {
        if (size >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) { data += 3; size -= 3; }
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, (int)size,
                                 wide, (int)size))
            MultiByteToWideChar(CP_ACP, 0, (const char *)data, (int)size, wide, (int)size);
    }
    free(bytes);
    return wide;
}

static wchar_t *trim(wchar_t *text) {
    while (iswspace(*text)) ++text;
    wchar_t *end = text + wcslen(text);
    while (end > text && iswspace(end[-1])) *--end = 0;
    return text;
}

static BOOL env_one(const wchar_t *name) {
    wchar_t value[2];
    return GetEnvironmentVariableW(name, value, _countof(value)) == 1 && *value == L'1';
}

static BOOL valid_profile(const wchar_t *name) {
    if (!_wcsicmp(name, L"play")) return TRUE;
    if (_wcsnicmp(name, L"play-", 5) || !name[5]) return FALSE;
    for (name += 5; *name; ++name) {
        wchar_t c = *name;
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')) return FALSE;
    }
    return TRUE;
}

static void check_data(const wchar_t *data) {
    check_ancestors(data);
    DWORD attr = attributes(data);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        fail(L"The co-op data folder is blocked by a file:\n%ls", data);
    check_tree(data);
}

static BOOL dependencies(const wchar_t *dir) {
    static const wchar_t *names[] = {
        L"SDL3.dll", L"avcodec-63.dll", L"avformat-63.dll", L"avutil-61.dll", L"swscale-10.dll",
        L"libgcc_s_seh-1.dll", L"libstdc++-6.dll", L"libwinpthread-1.dll", L"libshaderc_shared.dll", L"libiconv-2.dll"
    };
    for (size_t i = 0; i < _countof(names); ++i) {
        Path path;
        join(path, dir, names[i]);
        if (!is_file(path)) return FALSE;
    }
    return TRUE;
}

static const wchar_t *choose_program(const wchar_t *coop, const wchar_t *game, Path exe, Path library) {
    static const wchar_t runtime[] = L"C:\\msys64\\ucrt64\\bin";
    join(exe, coop, L"upstream\\build\\savagewind.exe");
    if (is_file(exe) && dependencies(runtime)) {
        join(library, coop, L"upstream\\build\\swgame.dll");
        if (!is_file(library)) join(library, game, L"extracted\\swgame.dll");
        return runtime;
    }
    join(exe, coop, L"savagewind.exe");
    if (!is_file(exe) || !dependencies(game))
        fail(L"Co-op program or required DLLs are missing. Extract the latest co-op ZIP "
             L"inside a working Savage Wind installation.");
    join(library, game, L"extracted\\swgame.dll");
    Path want, have;
    join(want, coop, L"gamelib.id");
    join(have, game, L"extracted\\swgame.dll.id");
    if (is_file(want) && is_file(have)) {
        wchar_t *expected = read_text(want, 0), *installed = read_text(have, 0);
        if (!expected || !installed) fail(L"Could not read the Savage Wind game library version.");
        if (_wcsicmp(trim(expected), trim(installed)))
            fail(L"This co-op zip needs a different Savage Wind version than the one installed "
                 L"(game library %ls, the zip needs %ls). Update normal Savage Wind (or use the "
                 L"matching co-op zip), start it once normally, then try again.",
                 trim(installed), trim(expected));
        free(expected);
        free(installed);
    }
    copy_fonts(game, coop);
    copy_named(game, coop, L"colour-luts");
    copy_named(game, coop, L"icon.png");
    return game;
}

static void migrate_files(const wchar_t *from, const wchar_t *to, BOOL staged) {
    /* (main agent review) "New save" profiles (save-profiles + the active one in save_profile.txt) and screenshots */
    static const wchar_t *names[] = { L"save", ini_name, L"save-profiles", L"save_profile.txt", L"screenshots" };
    for (size_t i = 0; i < _countof(names); ++i) {
        Path src, dst;
        join(src, from, names[i]);
        join(dst, to, names[i]);
        if (staged) {
            if (exists(src)) copy_item(src, dst);
        } else {
            copy_once(src, dst);
        }
    }
}

static void migrate_profile(const wchar_t *from, const wchar_t *to) {
    Path split_from, split_to, split_save, split_ini;
    join(split_from, from, L"split-player2");
    no_link(split_from, attributes(split_from));
    join(split_save, split_from, L"save");
    join(split_ini, split_from, ini_name);
    BOOL has_split = exists(split_save) || exists(split_ini);

    check_data(to);
    BOOL staged = !exists(to);
    if (staged) start_stage(to, TRUE);
    const wchar_t *destination = staged ? staging : to;
    migrate_files(from, destination, staged);
    if (has_split) {
        join(split_to, destination, L"split-player2");
        make_dir(split_to);
        migrate_files(split_from, split_to, staged);
    }
    if (staged) publish_stage(to, 0);
}

static void migrate(const wchar_t *coop, const wchar_t *game) {
    Path legacy, old_play, new_play, old_save;
    join(legacy, game, L"coop-dev");
    join(old_play, legacy, L"play");
    join(new_play, coop, L"play");
    if (!_wcsicmp(coop, legacy) || exists(new_play) || !exists(old_play)) return;
    check_ancestors(old_play);
    join(old_save, old_play, L"save");
    DWORD attr = attributes(old_save);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    no_link(old_save, attr);

    WIN32_FIND_DATAW entry;
    HANDLE find = find_files(legacy, L"play-*", &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !valid_profile(entry.cFileName)) continue;
            Path from, to;
            join(from, legacy, entry.cFileName);
            join(to, coop, entry.cFileName);
            no_link(from, entry.dwFileAttributes);
            migrate_profile(from, to);
        } while (next_file(find, &entry, legacy));
    }
    /* Publish the default profile last. If interrupted, the next launch can finish the migration. */
    migrate_profile(old_play, new_play);
    message(L"Your co-op saves were copied from the old coop-dev folder. "
            L"When everything works, you can delete that old folder.", MB_ICONINFORMATION);
}

static BOOL ini_key(wchar_t *line, const wchar_t *key, wchar_t **value) {
    while (iswspace(*line)) ++line;
    size_t len = wcslen(key);
    if (_wcsnicmp(line, key, len)) return FALSE;
    line += len;
    while (iswspace(*line)) ++line;
    if (*line != L'=') return FALSE;
    if (value) *value = line + 1;
    return TRUE;
}

static void write_ini(const wchar_t *path, const wchar_t *text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (!size) fail(L"Could not encode the co-op settings:\n%ls", path);
    char *utf8 = malloc(size);
    if (!utf8) fail(L"There is not enough memory to start co-op.");
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, size, NULL, NULL);
    start_stage(path, FALSE);
    FILE *file = _wfopen(staging, L"wb");
    if (!file) fail(L"Could not open the co-op settings for writing:\n%ls", path);
    size_t written = fwrite(utf8, 1, (size_t)size - 1, file);
    int result = fclose(file);
    free(utf8);
    if (result || written != (size_t)size - 1) fail(L"Could not write the co-op settings:\n%ls", path);
    publish_stage(path, MOVEFILE_REPLACE_EXISTING);
}

static void borderless_ini(const wchar_t *path) {
    if (!exists(path)) {
        write_ini(path, L"settings_version = 4\r\nfullscreen_mode = borderless\r\n");
        return;
    }
    wchar_t *raw = read_text(path, 0);
    if (!raw) fail(L"Could not read the co-op settings:\n%ls", path);
    const wchar_t *newline = wcsstr(raw, L"\r\n") || !wcschr(raw, L'\n') ? L"\r\n" : L"\n";
    size_t length = wcslen(raw), capacity = 2;
    for (const wchar_t *p = raw; *p; ++p) if (*p == L'\n') ++capacity;
    const wchar_t **lines = calloc(capacity, sizeof *lines);
    if (!lines) fail(L"There is not enough memory to start co-op.");
    static const wchar_t mode[] = L"fullscreen_mode = borderless";
    BOOL found = FALSE, changed = FALSE;
    size_t count = 0, insertion = 0;
    for (wchar_t *line = raw; *line;) {
        wchar_t *end = wcschr(line, L'\n');
        if (end) {
            *end = 0;
            if (end > line && end[-1] == L'\r') end[-1] = 0;
        }
        wchar_t *value;
        lines[count++] = line;
        if (!insertion && ini_key(line, L"fullscreen", NULL)) insertion = count;
        if (ini_key(line, L"fullscreen_mode", &value)) {
            found = TRUE;
            /* Check the trimmed value without modifying lines that will be kept. */
            while (iswspace(*value)) ++value;
            size_t len = wcslen(value);
            while (len && iswspace(value[len - 1])) --len;
            if (!len || (len == 4 && !_wcsnicmp(value, L"auto", len)) ||
                        (len == 9 && !_wcsnicmp(value, L"exclusive", len))) {
                lines[count - 1] = mode;
                changed = TRUE;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    if (!found) {
        if (!insertion) insertion = count;
        memmove(lines + insertion + 1, lines + insertion, (count - insertion) * sizeof *lines);
        lines[insertion] = mode;
        ++count;
        changed = TRUE;
    }
    if (changed) {
        wchar_t *out = malloc((length + count * (_countof(mode) + 2) + 1) * sizeof *out);
        if (!out) fail(L"There is not enough memory to start co-op.");
        wchar_t *p = out;
        for (size_t i = 0; i < count; ++i) {
            wcscpy(p, lines[i]);
            p += wcslen(lines[i]);
            wcscpy(p, newline);
            p += wcslen(newline);
        }
        write_ini(path, out);
        free(out);
    }
    free(lines);
    free(raw);
}

static void prepare_data(const wchar_t *game, const wchar_t *data, Path save, Path ini,
                         BOOL fresh, BOOL exclusive) {
    make_dir(data);
    copy_fonts(game, data);
    copy_named(game, data, L"colour-luts");
    copy_named(game, data, ini_name);
    join(ini, data, ini_name);
    if (!exclusive && !env_one(L"SAVAGEWIND_COOP_EXCLUSIVE")) borderless_ini(ini);

    Path src, cache;
    join(src, game, L"cache");
    join(cache, data, L"cache");
    if (!exists(cache)) {
        stage_copy(src, cache, attributes(src), TRUE, FALSE);
        publish_stage(cache, 0);
    }

    join(src, game, L"save");
    join(save, data, L"save");
    BOOL has_save = exists(save);
    if (has_save && !fresh) return;
    stage_copy(src, save, attributes(src), TRUE, TRUE);
    if (!has_save) {
        publish_stage(save, 0);
        return;
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    Path backup;
    if (swprintf(backup, _countof(backup), L"%ls-old-%04d%02d%02d-%02d%02d%02d", save,
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) < 0)
        fail(L"A co-op save path is too long.");
    if (!MoveFileExW(save, backup, 0)) file_error(L"keep a backup of the previous co-op save", save);
    if (!MoveFileExW(staging, save, 0)) {
        DWORD code = GetLastError();
        if (!MoveFileExW(backup, save, 0))
            fail(L"Could not replace your co-op save. Your previous save is here:\n%ls", backup);
        SetLastError(code);
        file_error(L"replace the co-op save", save);
    }
    *staging = 0;
}

static HANDLE child_log(const wchar_t *path) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) file_error(L"open this log", path);
    return file;
}

static HANDLE start_child(const wchar_t *exe, wchar_t *command, const wchar_t *cwd,
                          HANDLE out, HANDLE err) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) file_error(L"open the Windows input device", L"NUL");
    STARTUPINFOW si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES,
                        .hStdInput = nul, .hStdOutput = out, .hStdError = err };
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    DWORD error = GetLastError();
    CloseHandle(nul);
    CloseHandle(out);
    if (err != out) CloseHandle(err);
    if (!started) {
        SetLastError(error);
        file_error(L"start this program", exe);
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static DWORD child_exit_code(HANDLE child) {
    DWORD code;
    if (!GetExitCodeProcess(child, &code))
        fail(L"Could not read how the program exited. Windows error %lu.", (unsigned long)GetLastError());
    CloseHandle(child);
    return code;
}

static LRESULT CALLBACK setup_window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) return 0;
    if (msg == WM_CTLCOLORSTATIC) {  /* (the texts and the icon on the window's white background) */
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, GetWindowLongPtrW((HWND)lp, GWLP_ID) == 2 ? RGB(0x5f, 0x63, 0x68) : RGB(0x20, 0x21, 0x24));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(window, msg, wp, lp);
}

/* (0.2.0: a friendlier setup window with the icon, bigger) the setup window's fonts, made once */
static HFONT setup_font(int pixels, int weight) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static HWND setup_window(HINSTANCE inst) {
    /* Resolve the progress control at runtime so the launcher retains its original build command. */
    HMODULE controls = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!controls) return NULL;
    BOOL (WINAPI *init)(const INITCOMMONCONTROLSEX *) =
        (BOOL (WINAPI *)(const INITCOMMONCONTROLSEX *))GetProcAddress(controls, "InitCommonControlsEx");
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    if (!init || !init(&icc)) return NULL;

    WNDCLASSW cls = { .lpfnWndProc = setup_window_proc, .hInstance = inst,
                      .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)),
                      .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
                      .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                      .lpszClassName = L"SavageWindCoopSetup" };
    if (!RegisterClassW(&cls)) return NULL;
    /* Layout in 96-dpi pixels, scaled to the screen (the manifest makes the program DPI aware: sharp text) */
    HDC screen = GetDC(NULL);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(NULL, screen);
#define S(v) MulDiv((v), dpi, 96)
    RECT frame = {0, 0, S(480), S(156)};
    AdjustWindowRect(&frame, WS_CAPTION, FALSE);
    int width = frame.right - frame.left, height = frame.bottom - frame.top;
    HWND window = CreateWindowExW(0, cls.lpszClassName, title, WS_CAPTION,
                                 (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                 (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                                 width, height, NULL, NULL, inst, NULL);
    if (!window) return NULL;
    static HFONT heading_font, body_font;
    if (!heading_font) heading_font = setup_font(S(20), FW_SEMIBOLD);
    if (!body_font) body_font = setup_font(S(15), FW_NORMAL);
    HWND icon = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE,
                                S(24), S(24), S(48), S(48), window, NULL, inst, NULL);
    HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), 0);
    if (big) SendMessageW(icon, STM_SETICON, (WPARAM)big, 0);
    HWND heading = CreateWindowExW(0, L"STATIC", L"Setting up Savage Wind Co-op", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                   S(92), S(22), S(364), S(28), window, (HMENU)1, inst, NULL);
    SendMessageW(heading, WM_SETFONT, (WPARAM)heading_font, TRUE);
    HWND body = CreateWindowExW(0, L"STATIC",
                                L"Making the co-op files from your own game files.\n"
                                L"This takes about a minute, only the first time.",
                                WS_CHILD | WS_VISIBLE | SS_NOPREFIX, S(92), S(52), S(364), S(44), window, (HMENU)2,
                                inst, NULL);
    SendMessageW(body, WM_SETFONT, (WPARAM)body_font, TRUE);
    HWND progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                                    S(24), S(112), S(432), S(16), window, NULL, inst, NULL);
#undef S
    SendMessageW(progress, PBM_SETMARQUEE, TRUE, 30);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    return window;
}

static void wait_setup(HWND window, HANDLE child) {
    if (window) {
        DWORD result;
        while ((result = MsgWaitForMultipleObjects(1, &child, FALSE, INFINITE, QS_ALLINPUT)) == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        DestroyWindow(window);
        if (result == WAIT_OBJECT_0) return;
    }
    WaitForSingleObject(child, INFINITE);
}

static BOOL current_stamp(const wchar_t *script, const wchar_t *stamp) {
    if (!is_file(stamp)) return FALSE;
    wchar_t *source = read_text(script, 0), *saved = read_text(stamp, 0);
    BOOL current = FALSE;
    /* Read the simple literal assignment in build_overlay.py without starting Python on every launch. */
    static const wchar_t definition[] = L"\nOVERLAY_VERSION = '";
    wchar_t *version = source ? wcsstr(source, definition) : NULL;
    if (version && saved) {
        version += _countof(definition) - 1;
        wchar_t *end = wcschr(version, L'\'');
        if (end) {
            *end = 0;
            saved[wcscspn(saved, L"\r\n")] = 0;
            current = !_wcsicmp(version, saved);
        }
    }
    free(source);
    free(saved);
    return current;
}

static void require_file(const wchar_t *path) {
    if (!is_file(path)) fail(L"A required co-op file is missing:\n%ls", path);
}

static void prepare_overlay(HINSTANCE inst, const wchar_t *coop, const wchar_t *romfs, Path overlay) {
    Path local, script, stamp, tables, actor, sword;
    join(local, coop, L"local");
    join(overlay, local, L"overlay");
    join(script, coop, L"build_overlay.py");
    join(stamp, overlay, L"coop_overlay_version.txt");
    join(tables, overlay, L"coop_astree.dat");
    join(actor, overlay, L"Actor\\Pack\\Dm_CoopLinkE.sbactorpack");
    join(sword, overlay, L"Actor\\Pack\\Dm_CoopSword.sbactorpack");
    no_link(local, attributes(local));
    no_link(overlay, attributes(overlay));

    if (!current_stamp(script, stamp) || !is_file(tables) || !is_file(actor)) {
        Path python, log;
        join(python, coop, L"py\\python.exe");
        if (!is_file(python)) join(python, coop, L"tools-ext\\oead-venv\\Scripts\\python.exe");
        if (!is_file(python))
            fail(L"The Python files for co-op setup are missing. Extract the full co-op ZIP and try again.");
        require_file(script);
        check_tree(overlay);
        make_dir(local);
        if (exists(stamp) && !DeleteFileW(stamp)) file_error(L"reset the co-op setup version", stamp);
        join(log, local, L"overlay-setup.log");
        no_link(log, attributes(log));
        wchar_t command[4 * _countof(local) + 64];
        swprintf(command, _countof(command), L"\"%ls\" -I -X utf8 \"%ls\" --play --game \"%ls\" --out \"%ls\"",
                 python, script, romfs, overlay);
        if (*test_log) message(setup_text, MB_ICONINFORMATION);
        HWND window = setup_window(inst);
        HANDLE out = child_log(log);
        HANDLE child = start_child(python, command, coop, out, out);
        wait_setup(window, child);
        DWORD code = child_exit_code(child);
        if (code || !current_stamp(script, stamp)) {
            wchar_t *tail = read_text(log, 2600);
            fail(L"Co-op setup could not finish (code %lu).\n\n%ls\n\nSetup log:\n%ls",
                 (unsigned long)code, tail && *tail ? tail : L"No output from setup.", log);
        }
    }
    require_file(tables);
    require_file(actor);
    require_file(sword);
}

static void set_env(const wchar_t *key, const wchar_t *value) {
    if (!SetEnvironmentVariableW(key, value) && (value || GetLastError() != ERROR_ENVVAR_NOT_FOUND))
        fail(L"Could not set up the game environment. Windows error %lu.", (unsigned long)GetLastError());
}

static void game_environment(const wchar_t *dlls, const wchar_t *data, const wchar_t *save,
                             const wchar_t *ini, const wchar_t *library, const wchar_t *overlay) {
    static const wchar_t *clear[] = {
        L"SAVAGEWIND_SAVE_DIR", L"SAVAGEWIND_HIDDEN", L"SAVAGEWIND_TEST_WARP", L"SAVAGEWIND_INPUT",
        L"SAVAGEWIND_LOG_FILES", L"SAVAGEWIND_SPLIT", L"SAVAGEWIND_PAD", L"SAVAGEWIND_NO_AUDIO",
        L"SAVAGEWIND_WINDOW", L"SAVAGEWIND_COOP_MENU", L"SAVAGEWIND_COOP_PORT", L"SAVAGEWIND_COOP_PEER",
        L"SAVAGEWIND_COOP_CODE", L"SAVAGEWIND_COOP_NAME", L"SAVAGEWIND_COOP_BIND", L"SAVAGEWIND_COOP_OPEN"
    };
    for (size_t i = 0; i < _countof(clear); ++i) set_env(clear[i], NULL);
    if (!*test_log) set_env(L"SAVAGEWIND_CONTROL", NULL);
    set_env(L"SAVAGEWIND_DATA_ROOT", data);
    set_env(L"SAVAGEWIND_DEFAULT_SAVE_DIR", save);
    set_env(L"SAVAGEWIND_SETTINGS", ini);
    set_env(L"SAVAGEWIND_GAME_LIB", library);
    set_env(L"SAVAGEWIND_NO_LAUNCHER", L"1");
    set_env(L"SAVAGEWIND_ROMFS_OVERLAY", overlay);

    DWORD length = GetEnvironmentVariableW(L"PATH", NULL, 0);
    size_t prefix = wcslen(dlls);
    wchar_t *path = calloc(prefix + length + 1, sizeof *path);
    if (!path) fail(L"There is not enough memory to start co-op.");
    wcscpy(path, dlls);
    if (length) {
        path[prefix] = L';';
        GetEnvironmentVariableW(L"PATH", path + prefix + 1, length);
    }
    set_env(L"PATH", path);
    free(path);
}

static int run_game(const wchar_t *exe, const wchar_t *extracted, const wchar_t *data) {
    Path log, previous, stdout_log, exit_log;
    join(log, data, L"savagewind.log");
    join(previous, data, L"savagewind.prev.log");
    join(stdout_log, data, L"stdout.log");
    join(exit_log, data, L"exit.log");
    if (exists(log) && !MoveFileExW(log, previous, MOVEFILE_REPLACE_EXISTING))
        file_error(L"keep the previous game log", log);
    wchar_t command[2 * _countof(log) + 16];
    swprintf(command, _countof(command), L"\"%ls\" \"%ls\" --play", exe, extracted);
    HANDLE err = child_log(log);
    HANDLE out = child_log(stdout_log);
    HANDLE child = start_child(exe, command, data, out, err);
    BOOL early = WaitForSingleObject(child, 2500) == WAIT_OBJECT_0;
    if (!early) WaitForSingleObject(child, INFINITE);
    DWORD code = child_exit_code(child);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[128];
    snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d game exited, code %ld (0x%08lX)\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (long)code, (unsigned long)code);
    append_bytes(exit_log, line);
    if (early) fail(L"The game exited during startup (code %ld). See:\n%ls", (long)code, log);
    return (int)code;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE previous, PWSTR command, int show) {
    (void)previous;
    (void)command;
    (void)show;
    Path coop, game;
    DWORD length = GetModuleFileNameW(NULL, coop, _countof(coop));
    if (!length || length >= _countof(coop)) fail(L"Could not find this program's folder.");
    parent(coop);
    wcscpy(game, coop);
    parent(game);
    if (!*game) fail(L"Put the Co-op folder inside your Savage Wind folder.");
    check_ancestors(coop);
    if (env_one(L"SAVAGEWIND_COOP_LAUNCHER_TEST")) {
        Path log;
        join(log, coop, L"launcher-test.log");
        no_link(log, attributes(log));
        wcscpy(test_log, log);
    }

    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) fail(L"Could not read the co-op launcher options.");
    const wchar_t *profile = L"play";
    BOOL fresh = FALSE, exclusive = FALSE;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"-Profile")) {
            if (++i == argc || !valid_profile(argv[i]))
                fail(L"Profile must be play or play-NAME (letters, digits, underscores and hyphens only).");
            profile = argv[i];
        } else if (!_wcsicmp(argv[i], L"-FreshSave")) {
            fresh = TRUE;
        } else if (!_wcsicmp(argv[i], L"-Exclusive")) {
            exclusive = TRUE;
        } else {
            fail(L"Unknown option: %ls\n\nAvailable options: -Profile NAME, -FreshSave, -Exclusive.", argv[i]);
        }
    }
    Path data;
    join(data, coop, profile);
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\SavageWindCoopLauncher");
    DWORD error = GetLastError();
    if (!mutex) fail(L"Could not reserve the co-op game. Windows error %lu.", (unsigned long)error);
    if (error == ERROR_ALREADY_EXISTS) fail(L"Savage Wind Co-op is already running");
    check_data(data);

    Path exe, library, extracted, romfs, title_pack, overlay, save, ini;
    join(extracted, game, L"extracted");
    join(romfs, extracted, L"romfs");
    join(title_pack, romfs, L"Pack\\TitleBG.pack");
    if (!is_file(title_pack))
        fail(L"Put the Co-op folder inside your Savage Wind folder, next to savagewind.exe, "
             L"and make sure Savage Wind itself works first");
    const wchar_t *dlls = choose_program(coop, game, exe, library);
    require_file(library);
    prepare_overlay(inst, coop, romfs, overlay);
    migrate(coop, game);
    prepare_data(game, data, save, ini, fresh, exclusive);
    game_environment(dlls, data, save, ini, library, overlay);
    int code = run_game(exe, extracted, data);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
