/* Savage Wind Co-op.exe — Win32 launcher, no console.
 *
 *   windres -O coff launcher/coop_launcher.rc -o coop_launcher_rc.o
 *   clang -O2 -municode -mwindows launcher/coop_launcher.c coop_launcher_rc.o -lwinhttp -lbcrypt -o "Savage Wind Co-op.exe"
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#ifndef COOP_VERSION_FALLBACK
#define COOP_VERSION_FALLBACK L"0.2.0"
#endif

typedef wchar_t Path[1024];
static wchar_t title[128] = L"Savage Wind Co-op " COOP_VERSION_FALLBACK;
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

static BOOL check_gamelib_match(const wchar_t *want, const wchar_t *have, BOOL require_want,
                                wchar_t *err, size_t err_cap) {
    if (!is_file(want)) {
        if (!require_want) return TRUE;
        swprintf(err, err_cap, L"The update package is missing gamelib.id.");
        return FALSE;
    }
    if (!is_file(have)) return TRUE;
    wchar_t *expected = read_text(want, 0), *installed = read_text(have, 0);
    if (!expected || !installed) {
        free(expected);
        free(installed);
        swprintf(err, err_cap, L"Could not read the Savage Wind game library version.");
        return FALSE;
    }
    BOOL ok = !_wcsicmp(trim(expected), trim(installed));
    if (!ok) {
        swprintf(err, err_cap,
                 L"This co-op zip needs a different Savage Wind version than the one installed "
                 L"(game library %ls, the zip needs %ls). Update normal Savage Wind (or use the "
                 L"matching co-op zip), start it once normally, then try again.",
                 trim(installed), trim(expected));
    }
    free(expected);
    free(installed);
    return ok;
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
    wchar_t err[1024];
    if (!check_gamelib_match(want, have, FALSE, err, _countof(err))) fail(L"%ls", err);
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

static void pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static HWND progress_window(HINSTANCE inst, const wchar_t *heading_text, const wchar_t *body_text) {
    /* Resolve the progress control at runtime so the launcher stays lightweight. */
    static BOOL controls_ready = FALSE, class_ready = FALSE;
    if (!controls_ready) {
        HMODULE controls = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!controls) return NULL;
        BOOL (WINAPI *init)(const INITCOMMONCONTROLSEX *) =
            (BOOL (WINAPI *)(const INITCOMMONCONTROLSEX *))GetProcAddress(controls, "InitCommonControlsEx");
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
        if (!init || !init(&icc)) return NULL;
        controls_ready = TRUE;
    }
    if (!class_ready) {
        WNDCLASSW cls = { .lpfnWndProc = setup_window_proc, .hInstance = inst,
                          .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)),
                          .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
                          .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                          .lpszClassName = L"SavageWindCoopSetup" };
        if (!RegisterClassW(&cls)) return NULL;
        class_ready = TRUE;
    }
    /* Layout in 96-dpi pixels, scaled to the screen (the manifest makes the program DPI aware: sharp text) */
    HDC screen = GetDC(NULL);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(NULL, screen);
#define S(v) MulDiv((v), dpi, 96)
    RECT frame = {0, 0, S(480), S(156)};
    AdjustWindowRect(&frame, WS_CAPTION, FALSE);
    int width = frame.right - frame.left, height = frame.bottom - frame.top;
    HWND window = CreateWindowExW(0, L"SavageWindCoopSetup", title, WS_CAPTION,
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
    HWND heading = CreateWindowExW(0, L"STATIC", heading_text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                   S(92), S(22), S(364), S(28), window, (HMENU)1, inst, NULL);
    SendMessageW(heading, WM_SETFONT, (WPARAM)heading_font, TRUE);
    HWND body = CreateWindowExW(0, L"STATIC", body_text,
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

static HWND setup_window(HINSTANCE inst) {
    return progress_window(inst, L"Setting up Savage Wind Co-op",
                           L"Making the co-op files from your own game files.\n"
                           L"This takes about a minute, only the first time.");
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

/* ---- Auto-updater ------------------------------------------------------- */

#define UPDATER_API_URL L"https://api.github.com/repos/Jaumjvrs/SavageWind-Coop/releases/latest"
#define UPDATER_USER_AGENT L"SavageWind-Coop-Updater"

static BOOL safe_join(Path out, const wchar_t *base, const wchar_t *name) {
    size_t len = wcslen(base);
    if (!len) return FALSE;
    const wchar_t *sep = base[len - 1] == L'\\' ? L"" : L"\\";
    return swprintf(out, sizeof(Path) / sizeof(wchar_t), L"%ls%ls%ls", base, sep, name) >= 0;
}

static BOOL ensure_local_dir_safe(const wchar_t *coop, Path local) {
    if (!safe_join(local, coop, L"local")) return FALSE;
    DWORD attr = GetFileAttributesW(local);
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
    return CreateDirectoryW(local, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void updater_log(const wchar_t *coop, const char *fmt, ...) {
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof body, fmt, args);
    va_end(args);
    body[sizeof body - 1] = 0;

    Path local, log_path;
    if (ensure_local_dir_safe(coop, local) && safe_join(log_path, local, L"updater.log")) {
        DWORD attr = GetFileAttributesW(log_path);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char line[1200];
            snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d [updater] %s\r\n",
                     t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, body);
            append_bytes(log_path, line);
        }
    }
    if (*test_log) {
        char line[1200];
        snprintf(line, sizeof line, "[updater] %s\r\n", body);
        append_bytes(test_log, line);
    }
}

static const char *json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_push(char *out, size_t cap, size_t *pos, unsigned cp) {
    if (!out || !cap) return;
    if (cp < 0x80) {
        if (*pos + 1 < cap) out[(*pos)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*pos + 2 < cap) {
            out[(*pos)++] = (char)(0xC0 | (cp >> 6));
            out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (*pos + 3 < cap) {
            out[(*pos)++] = (char)(0xE0 | (cp >> 12));
            out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0x10FFFF) {
        if (*pos + 4 < cap) {
            out[(*pos)++] = (char)(0xF0 | (cp >> 18));
            out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static BOOL json_parse_string(const char **pp, char *out, size_t cap) {
    const char *p = json_ws(*pp);
    if (*p != '"') return FALSE;
    ++p;
    size_t pos = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p) return FALSE;
            char ch = *p++;
            switch (ch) {
                case '"': case '\\': case '/': if (out && pos + 1 < cap) out[pos++] = ch; break;
                case 'b': if (out && pos + 1 < cap) out[pos++] = '\b'; break;
                case 'f': if (out && pos + 1 < cap) out[pos++] = '\f'; break;
                case 'n': if (out && pos + 1 < cap) out[pos++] = '\n'; break;
                case 'r': if (out && pos + 1 < cap) out[pos++] = '\r'; break;
                case 't': if (out && pos + 1 < cap) out[pos++] = '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        int d = hex_val(p[i]);
                        if (d < 0) return FALSE;
                        cp = (cp << 4) | (unsigned)d;
                    }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                        unsigned lo = 0;
                        BOOL ok = TRUE;
                        for (int i = 0; i < 4; ++i) {
                            int d = hex_val(p[2 + i]);
                            if (d < 0) { ok = FALSE; break; }
                            lo = (lo << 4) | (unsigned)d;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            p += 6;
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        }
                    }
                    utf8_push(out, cap, &pos, cp);
                    break;
                }
                default: return FALSE;
            }
        } else {
            if ((unsigned char)*p < 0x20) return FALSE;
            if (out && pos + 1 < cap) out[pos++] = *p;
            ++p;
        }
    }
    if (*p != '"') return FALSE;
    if (out && cap) out[pos] = 0;
    *pp = p + 1;
    return TRUE;
}

static BOOL json_skip_value(const char **pp, int depth) {
    if (depth > 32) return FALSE;
    const char *p = json_ws(*pp);
    if (*p == '"') return json_parse_string(&p, NULL, 0) ? (*pp = p, TRUE) : FALSE;
    if (*p == '{') {
        ++p;
        p = json_ws(p);
        if (*p == '}') { *pp = p + 1; return TRUE; }
        for (;;) {
            if (!json_parse_string(&p, NULL, 0)) return FALSE;
            p = json_ws(p);
            if (*p != ':') return FALSE;
            ++p;
            if (!json_skip_value(&p, depth + 1)) return FALSE;
            p = json_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { *pp = p + 1; return TRUE; }
            return FALSE;
        }
    }
    if (*p == '[') {
        ++p;
        p = json_ws(p);
        if (*p == ']') { *pp = p + 1; return TRUE; }
        for (;;) {
            if (!json_skip_value(&p, depth + 1)) return FALSE;
            p = json_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { *pp = p + 1; return TRUE; }
            return FALSE;
        }
    }
    if (!strncmp(p, "true", 4)) { *pp = p + 4; return TRUE; }
    if (!strncmp(p, "false", 5)) { *pp = p + 5; return TRUE; }
    if (!strncmp(p, "null", 4)) { *pp = p + 4; return TRUE; }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-') ++p;
        if (*p < '0' || *p > '9') return FALSE;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p == '.') {
            ++p;
            if (*p < '0' || *p > '9') return FALSE;
            while (*p >= '0' && *p <= '9') ++p;
        }
        if (*p == 'e' || *p == 'E') {
            ++p;
            if (*p == '+' || *p == '-') ++p;
            if (*p < '0' || *p > '9') return FALSE;
            while (*p >= '0' && *p <= '9') ++p;
        }
        *pp = p;
        return TRUE;
    }
    return FALSE;
}

static BOOL json_extract_top_string(const char *json, const char *want_key, char *out, size_t cap) {
    if (!json) return FALSE;
    if ((unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
        json += 3;
    const char *p = json_ws(json);
    if (*p != '{') return FALSE;
    ++p;
    p = json_ws(p);
    if (*p == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_string(&p, key, sizeof key)) return FALSE;
        p = json_ws(p);
        if (*p != ':') return FALSE;
        ++p;
        p = json_ws(p);
        if (!strcmp(key, want_key) && *p == '"') {
            return json_parse_string(&p, out, cap);
        }
        if (!json_skip_value(&p, 1)) return FALSE;
        p = json_ws(p);
        if (*p == ',') { ++p; continue; }
        return FALSE;
    }
}

typedef struct {
    char tag[64];
    char body[4096];
    char zip_url[1024];
    char sha_url[1024];
} ReleaseInfo;

static BOOL parse_asset_object(const char **pp, ReleaseInfo *info) {
    const char *p = json_ws(*pp);
    if (*p != '{') return FALSE;
    ++p;
    p = json_ws(p);
    char name[256] = {0}, url[1024] = {0};
    if (*p != '}') {
        for (;;) {
            char key[128];
            if (!json_parse_string(&p, key, sizeof key)) return FALSE;
            p = json_ws(p);
            if (*p != ':') return FALSE;
            ++p;
            p = json_ws(p);
            if (!strcmp(key, "name") && *p == '"') {
                if (!json_parse_string(&p, name, sizeof name)) return FALSE;
            } else if (!strcmp(key, "browser_download_url") && *p == '"') {
                if (!json_parse_string(&p, url, sizeof url)) return FALSE;
            } else {
                if (!json_skip_value(&p, 2)) return FALSE;
            }
            p = json_ws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') break;
            return FALSE;
        }
    }
    ++p;
    if (!strcmp(name, "SavageWind-Coop.zip") && *url) {
        strncpy(info->zip_url, url, sizeof(info->zip_url) - 1);
        info->zip_url[sizeof(info->zip_url) - 1] = 0;
    } else if (!strcmp(name, "SavageWind-Coop.zip.sha256") && *url) {
        strncpy(info->sha_url, url, sizeof(info->sha_url) - 1);
        info->sha_url[sizeof(info->sha_url) - 1] = 0;
    }
    *pp = p;
    return TRUE;
}

static BOOL parse_release_json(const char *json, ReleaseInfo *info) {
    memset(info, 0, sizeof *info);
    if (!json) return FALSE;
    if ((unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
        json += 3;
    const char *p = json_ws(json);
    if (*p != '{') return FALSE;
    ++p;
    p = json_ws(p);
    if (*p == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_string(&p, key, sizeof key)) return FALSE;
        p = json_ws(p);
        if (*p != ':') return FALSE;
        ++p;
        p = json_ws(p);
        if (!strcmp(key, "tag_name") && *p == '"') {
            if (!json_parse_string(&p, info->tag, sizeof info->tag)) return FALSE;
        } else if (!strcmp(key, "body") && *p == '"') {
            if (!json_parse_string(&p, info->body, sizeof info->body)) return FALSE;
        } else if (!strcmp(key, "assets") && *p == '[') {
            ++p;
            p = json_ws(p);
            if (*p != ']') {
                for (;;) {
                    if (*p == '{') {
                        if (!parse_asset_object(&p, info)) return FALSE;
                    } else {
                        if (!json_skip_value(&p, 2)) return FALSE;
                    }
                    p = json_ws(p);
                    if (*p == ',') { ++p; p = json_ws(p); continue; }
                    if (*p == ']') break;
                    return FALSE;
                }
            }
            ++p;
        } else {
            if (!json_skip_value(&p, 1)) return FALSE;
        }
        p = json_ws(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') break;
        return FALSE;
    }
    return info->tag[0] != 0;
}

static void utf8_to_wide(const char *src, wchar_t *dst, size_t cap) {
    if (!cap) return;
    dst[0] = 0;
    if (!src || !*src) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (!n) dst[0] = 0;
    dst[cap - 1] = 0;
}

static void load_current_version(const wchar_t *coop, wchar_t *out, size_t cap) {
    wcsncpy(out, COOP_VERSION_FALLBACK, cap - 1);
    out[cap - 1] = 0;
    Path manifest;
    if (!safe_join(manifest, coop, L"manifest.json")) return;
    size_t len = 0;
    char *raw = read_bytes(manifest, &len, 0);
    if (!raw) return;
    char ver[64] = {0};
    if (json_extract_top_string(raw, "version", ver, sizeof ver) && ver[0]) {
        wchar_t wide[64];
        utf8_to_wide(ver, wide, _countof(wide));
        wchar_t *t = trim(wide);
        if (*t) {
            wcsncpy(out, t, cap - 1);
            out[cap - 1] = 0;
        }
    }
    free(raw);
}

static BOOL parse_version(const wchar_t *s, unsigned parts[4]) {
    parts[0] = parts[1] = parts[2] = parts[3] = 0;
    while (iswspace(*s)) ++s;
    if (*s == L'v' || *s == L'V') ++s;
    if (*s < L'0' || *s > L'9') return FALSE;
    int idx = 0;
    while (idx < 4) {
        if (*s < L'0' || *s > L'9') return FALSE;
        unsigned val = 0;
        while (*s >= L'0' && *s <= L'9') {
            if (val > 99999) return FALSE;
            val = val * 10 + (unsigned)(*s - L'0');
            ++s;
        }
        parts[idx++] = val;
        if (*s == L'.') { ++s; continue; }
        break;
    }
    return *s == 0 || *s == L'-' || *s == L'+' || iswspace(*s);
}

static BOOL compare_versions(const wchar_t *a, const wchar_t *b, int *out_cmp) {
    unsigned va[4], vb[4];
    if (!parse_version(a, va) || !parse_version(b, vb)) return FALSE;
    for (int i = 0; i < 4; ++i) {
        if (va[i] != vb[i]) { *out_cmp = va[i] < vb[i] ? -1 : 1; return TRUE; }
    }
    *out_cmp = 0;
    return TRUE;
}

static void strip_v_prefix(const wchar_t *in, wchar_t *out, size_t cap) {
    while (iswspace(*in)) ++in;
    if (*in == L'v' || *in == L'V') ++in;
    wcsncpy(out, in, cap - 1);
    out[cap - 1] = 0;
    wchar_t *end = out + wcslen(out);
    while (end > out && iswspace(end[-1])) *--end = 0;
}

static void read_updater_ini(const wchar_t *coop, BOOL *enabled, wchar_t *skip_ver, size_t skip_cap) {
    *enabled = TRUE;
    if (skip_cap) skip_ver[0] = 0;
    Path local, ini;
    if (!safe_join(local, coop, L"local") || !safe_join(ini, local, L"updater.ini")) return;
    DWORD attr = GetFileAttributesW(ini);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        return;
    wchar_t *raw = read_text(ini, 0);
    if (!raw) return;
    for (wchar_t *line = raw; *line;) {
        wchar_t *end = wcschr(line, L'\n');
        if (end) {
            *end = 0;
            if (end > line && end[-1] == L'\r') end[-1] = 0;
        }
        wchar_t *val = NULL;
        if (ini_key(line, L"enabled", &val) || ini_key(line, L"auto_update", &val)) {
            wchar_t *t = trim(val);
            if (!_wcsicmp(t, L"0") || !_wcsicmp(t, L"false") || !_wcsicmp(t, L"no") || !_wcsicmp(t, L"off"))
                *enabled = FALSE;
        } else if (ini_key(line, L"skip_version", &val)) {
            wchar_t *t = trim(val);
            if (skip_cap) { wcsncpy(skip_ver, t, skip_cap - 1); skip_ver[skip_cap - 1] = 0; }
        }
        if (!end) break;
        line = end + 1;
    }
    free(raw);
}

static void write_skip_version(const wchar_t *coop, const wchar_t *version) {
    Path local, ini, tmp;
    if (!ensure_local_dir_safe(coop, local) || !safe_join(ini, local, L"updater.ini")) return;
    if (swprintf(tmp, _countof(tmp), L"%ls.tmp-%lu", ini, (unsigned long)GetCurrentProcessId()) < 0) return;
    BOOL enabled = TRUE;
    wchar_t old_skip[64];
    read_updater_ini(coop, &enabled, old_skip, _countof(old_skip));
    char ver_utf8[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, version, -1, ver_utf8, sizeof ver_utf8, NULL, NULL);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return;
    if (!enabled) fprintf(f, "enabled = 0\r\n");
    fprintf(f, "skip_version = %s\r\n", ver_utf8);
    fclose(f);
    if (!MoveFileExW(tmp, ini, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
}

static BOOL allowed_host(const wchar_t *host, BOOL is_api, BOOL allow_local) {
    if (allow_local && !_wcsicmp(host, L"127.0.0.1")) return TRUE;
    if (is_api && !_wcsicmp(host, L"api.github.com")) return TRUE;
    if (!is_api && (!_wcsicmp(host, L"github.com") ||
                    !_wcsicmp(host, L"objects.githubusercontent.com") ||
                    !_wcsicmp(host, L"release-assets.githubusercontent.com")))
        return TRUE;
    return FALSE;
}

static BOOL crack_and_validate_url(const wchar_t *url, BOOL is_api, BOOL allow_local,
                                   INTERNET_SCHEME *scheme, wchar_t *host, size_t host_cap,
                                   INTERNET_PORT *port, wchar_t *path, size_t path_cap) {
    wchar_t extra[512] = {0};
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)host_cap;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)path_cap;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = (DWORD)_countof(extra);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return FALSE;
    if (!host[0]) return FALSE;
    if (!allowed_host(host, is_api, allow_local)) return FALSE;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
        /* always allowed for allowlisted hosts */
    } else if (allow_local && uc.nScheme == INTERNET_SCHEME_HTTP && !_wcsicmp(host, L"127.0.0.1")) {
        /* local test server only via SAVAGEWIND_COOP_UPDATE_URL */
    } else {
        return FALSE;
    }
    if (!path[0]) wcscpy(path, L"/");
    if (extra[0]) {
        if (wcslen(path) + wcslen(extra) + 1 >= path_cap) return FALSE;
        wcscat(path, extra);
    }
    *scheme = uc.nScheme;
    *port = uc.nPort;
    return TRUE;
}

static BOOL http_get(const wchar_t *initial_url, BOOL is_api, BOOL allow_local, size_t max_bytes,
                     char **out_buf, size_t *out_len, HANDLE out_file, BCRYPT_HASH_HANDLE hash_handle,
                     DWORD *out_status, char *err_msg, size_t err_cap) {
    if (out_buf) *out_buf = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;

    wchar_t cur_url[1536];
    wcsncpy(cur_url, initial_url, _countof(cur_url) - 1);
    cur_url[_countof(cur_url) - 1] = 0;

    HINTERNET session = WinHttpOpen(UPDATER_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        snprintf(err_msg, err_cap, "WinHttpOpen failed (%lu)", (unsigned long)GetLastError());
        return FALSE;
    }
    if (is_api) WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    else WinHttpSetTimeouts(session, 10000, 10000, 10000, 30000);

    BOOL ok = FALSE;
    for (int hop = 0; hop < 6; ++hop) {
        INTERNET_SCHEME scheme = 0;
        INTERNET_PORT port = 0;
        wchar_t host[256] = {0}, path[1280] = {0};
        if (!crack_and_validate_url(cur_url, is_api, allow_local, &scheme,
                                    host, _countof(host), &port, path, _countof(path))) {
            snprintf(err_msg, err_cap, "rejected URL (scheme or host not allowed)");
            break;
        }
        HINTERNET conn = WinHttpConnect(session, host, port, 0);
        if (!conn) {
            snprintf(err_msg, err_cap, "WinHttpConnect failed (%lu)", (unsigned long)GetLastError());
            break;
        }
        DWORD flags = (scheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            snprintf(err_msg, err_cap, "WinHttpOpenRequest failed (%lu)", (unsigned long)GetLastError());
            WinHttpCloseHandle(conn);
            break;
        }
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        if (is_api) {
            WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, NULL)) {
            snprintf(err_msg, err_cap, "HTTP request failed (%lu)", (unsigned long)GetLastError());
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        DWORD status = 0, status_sz = sizeof status;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX)) {
            snprintf(err_msg, err_cap, "HTTP status query failed (%lu)", (unsigned long)GetLastError());
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        if (out_status) *out_status = status;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t loc[1536] = {0};
            DWORD loc_sz = sizeof loc;
            BOOL got_loc = WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                               loc, &loc_sz, WINHTTP_NO_HEADER_INDEX);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            if (!got_loc || !loc[0]) {
                snprintf(err_msg, err_cap, "HTTP redirect missing Location header");
                break;
            }
            if (loc[0] == L'/') {
                if (swprintf(cur_url, _countof(cur_url), L"%ls://%ls:%u%ls",
                             scheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http",
                             host, (unsigned)port, loc) < 0) {
                    snprintf(err_msg, err_cap, "redirect URL too long");
                    break;
                }
            } else {
                wcsncpy(cur_url, loc, _countof(cur_url) - 1);
                cur_url[_countof(cur_url) - 1] = 0;
            }
            continue;
        }
        if (status != 200) {
            snprintf(err_msg, err_cap, "HTTP status %lu", (unsigned long)status);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }

        size_t total = 0, cap = 0;
        char *buf = NULL;
        BYTE chunk[65536];
        BOOL read_ok = TRUE;
        for (;;) {
            pump_messages();
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) {
                snprintf(err_msg, err_cap, "HTTP read check failed (%lu)", (unsigned long)GetLastError());
                read_ok = FALSE;
                break;
            }
            if (!avail) break;
            DWORD to_read = avail < sizeof chunk ? avail : (DWORD)sizeof chunk;
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk, to_read, &got)) {
                snprintf(err_msg, err_cap, "HTTP read failed (%lu)", (unsigned long)GetLastError());
                read_ok = FALSE;
                break;
            }
            if (!got) break;
            if (total + got > max_bytes) {
                snprintf(err_msg, err_cap, "HTTP response exceeded size limit");
                read_ok = FALSE;
                break;
            }
            if (hash_handle) {
                NTSTATUS st = BCryptHashData(hash_handle, chunk, got, 0);
                if (st < 0) {
                    snprintf(err_msg, err_cap, "SHA-256 hash step failed (0x%08lX)", (unsigned long)st);
                    read_ok = FALSE;
                    break;
                }
            }
            if (out_file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                if (!WriteFile(out_file, chunk, got, &written, NULL) || written != got) {
                    snprintf(err_msg, err_cap, "disk write failed (%lu)", (unsigned long)GetLastError());
                    read_ok = FALSE;
                    break;
                }
            }
            if (out_buf) {
                if (total + got + 1 > cap) {
                    size_t next_cap = cap ? cap * 2 : 16384;
                    while (next_cap < total + got + 1) next_cap *= 2;
                    char *grown = realloc(buf, next_cap);
                    if (!grown) {
                        snprintf(err_msg, err_cap, "out of memory");
                        read_ok = FALSE;
                        break;
                    }
                    buf = grown;
                    cap = next_cap;
                }
                memcpy(buf + total, chunk, got);
            }
            total += got;
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        if (read_ok) {
            if (out_buf) {
                if (!buf) {
                    buf = calloc(1, 1);
                    if (!buf) { snprintf(err_msg, err_cap, "out of memory"); break; }
                } else {
                    buf[total] = 0;
                }
                *out_buf = buf;
            }
            if (out_len) *out_len = total;
            ok = TRUE;
        } else {
            free(buf);
        }
        break;
    }
    WinHttpCloseHandle(session);
    return ok;
}

static BOOL parse_sha256_hex(const char *text, BYTE out[32]) {
    if (!text) return FALSE;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    for (int i = 0; i < 32; ++i) {
        int hi = hex_val(text[i * 2]);
        int lo = hex_val(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return FALSE;
        out[i] = (BYTE)((hi << 4) | lo);
    }
    char after = text[64];
    return after == 0 || after == ' ' || after == '\t' || after == '\r' || after == '\n' || after == '*';
}

static BOOL download_and_verify_zip(const wchar_t *zip_url, const BYTE expected[32], BOOL allow_local,
                                    const wchar_t *dst_path, char *err_msg, size_t err_cap) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (st < 0) { snprintf(err_msg, err_cap, "BCrypt open failed (0x%08lX)", (unsigned long)st); goto done; }
    st = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    if (st < 0) { snprintf(err_msg, err_cap, "BCrypt create hash failed (0x%08lX)", (unsigned long)st); goto done; }

    file = CreateFileW(dst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        snprintf(err_msg, err_cap, "could not create download file (%lu)", (unsigned long)GetLastError());
        goto done;
    }

    if (!http_get(zip_url, FALSE, allow_local, 256u * 1024u * 1024u, NULL, NULL, file, hash, NULL, err_msg, err_cap))
        goto done;

    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;

    BYTE actual[32];
    st = BCryptFinishHash(hash, actual, sizeof actual, 0);
    if (st < 0) { snprintf(err_msg, err_cap, "BCrypt finish hash failed (0x%08lX)", (unsigned long)st); goto done; }

    unsigned diff = 0;
    for (int i = 0; i < 32; ++i) diff |= (unsigned)(actual[i] ^ expected[i]);
    if (diff != 0) {
        snprintf(err_msg, err_cap, "SHA-256 hash mismatch");
        goto done;
    }
    ok = TRUE;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static BOOL check_tree_safe(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return TRUE;
    Path search;
    if (!safe_join(search, path, L"*")) return FALSE;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(search, &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    BOOL ok = TRUE;
    do {
        if (dot_entry(&entry)) continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = FALSE; break; }
        Path child;
        if (!safe_join(child, path, entry.cFileName) || !check_tree_safe(child)) { ok = FALSE; break; }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return ok;
}

static BOOL run_tar_extract(const wchar_t *zip_path, const wchar_t *dst_dir, const wchar_t *log_path,
                            HWND progress_hwnd, char *err_msg, size_t err_cap) {
    wchar_t sysdir[MAX_PATH];
    UINT n = GetSystemDirectoryW(sysdir, _countof(sysdir));
    if (!n || n >= _countof(sysdir)) { snprintf(err_msg, err_cap, "could not find System32"); return FALSE; }
    Path tar_exe;
    if (!safe_join(tar_exe, sysdir, L"tar.exe")) { snprintf(err_msg, err_cap, "tar path too long"); return FALSE; }
    if (GetFileAttributesW(tar_exe) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err_msg, err_cap, "Windows tar.exe not found");
        return FALSE;
    }
    wchar_t cmd[3 * (sizeof(Path) / sizeof(wchar_t)) + 32];
    if (swprintf(cmd, _countof(cmd), L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar_exe, zip_path, dst_dir) < 0) {
        snprintf(err_msg, err_cap, "tar command too long");
        return FALSE;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    HANDLE out = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (nul == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        snprintf(err_msg, err_cap, "could not open tar log");
        return FALSE;
    }
    STARTUPINFOW si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES,
                        .hStdInput = nul, .hStdOutput = out, .hStdError = out };
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessW(tar_exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dst_dir, &si, &pi);
    DWORD start_err = GetLastError();
    CloseHandle(nul);
    CloseHandle(out);
    if (!started) {
        snprintf(err_msg, err_cap, "could not start tar.exe (%lu)", (unsigned long)start_err);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    if (progress_hwnd) {
        DWORD res;
        while ((res = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT)) == WAIT_OBJECT_0 + 1)
            pump_messages();
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        snprintf(err_msg, err_cap, "tar.exe exited with code %lu", (unsigned long)code);
        return FALSE;
    }
    return TRUE;
}

static void format_release_notes(const char *body_utf8, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (!body_utf8 || !*body_utf8 || !cap) return;
    wchar_t wide[2048];
    utf8_to_wide(body_utf8, wide, _countof(wide));
    const wchar_t *p = wide;
    while (iswspace(*p)) ++p;
    size_t pos = 0;
    int lines = 0;
    BOOL truncated = FALSE;
    while (*p) {
        if (*p == L'\r') { ++p; continue; }
        if (*p == L'\n') {
            if (++lines >= 8) { truncated = TRUE; break; }
            if (pos + 2 >= cap || pos >= 480) { truncated = TRUE; break; }
            out[pos++] = L'\r';
            out[pos++] = L'\n';
            ++p;
            continue;
        }
        if (pos + 4 >= cap || pos >= 480) { truncated = TRUE; break; }
        out[pos++] = *p++;
    }
    while (pos > 0 && iswspace(out[pos - 1])) --pos;
    if (truncated && pos + 4 < cap) {
        out[pos++] = L'.'; out[pos++] = L'.'; out[pos++] = L'.';
    }
    out[pos] = 0;
}

enum { UPD_UPDATE = 101, UPD_LATER = 102, UPD_SKIP = 103 };

static int prompt_update(const wchar_t *coop, const wchar_t *remote_ver, const wchar_t *current_ver,
                         const char *body_utf8) {
    if (env_one(L"SAVAGEWIND_COOP_UPDATE_AUTOYES")) {
        updater_log(coop, "auto-accepting update (SAVAGEWIND_COOP_UPDATE_AUTOYES=1)");
        return UPD_UPDATE;
    }
    wchar_t heading[256];
    swprintf(heading, _countof(heading), L"Version %ls is available (you have %ls). Update now?",
             remote_ver, current_ver);
    wchar_t notes[600];
    format_release_notes(body_utf8, notes, _countof(notes));

    HMODULE controls = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (controls) {
        typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
        TaskDialogIndirectFn td = (TaskDialogIndirectFn)GetProcAddress(controls, "TaskDialogIndirect");
        if (td) {
            TASKDIALOG_BUTTON buttons[3] = {
                { UPD_UPDATE, L"Update" },
                { UPD_LATER,  L"Not now" },
                { UPD_SKIP,   L"Skip this version" }
            };
            TASKDIALOGCONFIG cfg;
            memset(&cfg, 0, sizeof cfg);
            cfg.cbSize = sizeof cfg;
            cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
            cfg.pszWindowTitle = title;
            cfg.pszMainIcon = TD_INFORMATION_ICON;
            cfg.pszMainInstruction = heading;
            cfg.pszContent = notes[0] ? notes : L"A new version of Savage Wind Co-op is ready to download.";
            cfg.cButtons = 3;
            cfg.pButtons = buttons;
            cfg.nDefaultButton = UPD_UPDATE;
            int pressed = UPD_LATER;
            if (SUCCEEDED(td(&cfg, &pressed, NULL, NULL))) {
                if (pressed == UPD_UPDATE || pressed == UPD_SKIP) return pressed;
                return UPD_LATER;
            }
        }
    }

    wchar_t fallback[1024];
    swprintf(fallback, _countof(fallback),
             L"%ls\n\n%ls\n\nYes = Update, No = Not now, Cancel = Skip this version.",
             heading, notes[0] ? notes : L"");
    int r = MessageBoxW(NULL, fallback, title, MB_YESNOCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (r == IDYES) return UPD_UPDATE;
    if (r == IDCANCEL) return UPD_SKIP;
    return UPD_LATER;
}

static BOOL is_protected_entry(const wchar_t *name) {
    return !_wcsicmp(name, L"local") || !_wcsicmp(name, L"play") || !_wcsnicmp(name, L"play-", 5);
}

static void cleanup_updater_leftovers(const wchar_t *coop, const wchar_t *self_exe) {
    Path old_exe;
    if (swprintf(old_exe, _countof(old_exe), L"%ls.old", self_exe) >= 0) {
        for (int tries = 0; tries < 10; ++tries) {
            DWORD attr = GetFileAttributesW(old_exe);
            if (attr == INVALID_FILE_ATTRIBUTES) break;
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT) break;
            if (DeleteFileW(old_exe)) break;
            DWORD err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
            Sleep(100);
        }
    }
    Path local, search;
    if (!safe_join(local, coop, L"local") || !safe_join(search, local, L"update-stage-*")) return;
    DWORD lattr = GetFileAttributesW(local);
    if (lattr == INVALID_FILE_ATTRIBUTES || (lattr & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(search, &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (dot_entry(&entry)) continue;
        Path item;
        if (safe_join(item, local, entry.cFileName)) remove_stage(item);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

typedef struct {
    wchar_t name[MAX_PATH];
    BOOL had_old;
    BOOL is_self;
} SwappedEntry;

static BOOL apply_update(HINSTANCE inst, const wchar_t *coop, const wchar_t *game, const wchar_t *self_exe,
                         const ReleaseInfo *rel, const wchar_t *remote_ver, BOOL allow_local, HANDLE mutex) {
    char err[256] = {0};
    Path local, stage_dir, zip_path, extract_dir, new_coop, backup_dir, tar_log;
    if (!ensure_local_dir_safe(coop, local)) {
        updater_log(coop, "update aborted: could not create local folder");
        return FALSE;
    }
    if (swprintf(stage_dir, _countof(stage_dir), L"%ls\\update-stage-%lu-%llu", local,
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64()) < 0)
        return FALSE;
    if (!CreateDirectoryW(stage_dir, NULL)) {
        updater_log(coop, "update aborted: could not create staging folder (%lu)", (unsigned long)GetLastError());
        return FALSE;
    }

    HWND win = *test_log ? NULL : progress_window(inst, L"Updating Savage Wind Co-op",
                                                  L"Downloading and checking the new version.\n"
                                                  L"Your saves and settings will be kept.");
    BOOL swapped_and_restarting = FALSE;

    if (!rel->zip_url[0] || !rel->sha_url[0]) {
        updater_log(coop, "update refused: release is missing zip or sha256 asset");
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"Could not update: this release is missing the zip or its SHA-256 verification file. "
                L"Your existing files were not changed.", MB_ICONERROR);
        goto cleanup;
    }

    wchar_t sha_url_w[1024], zip_url_w[1024];
    utf8_to_wide(rel->sha_url, sha_url_w, _countof(sha_url_w));
    utf8_to_wide(rel->zip_url, zip_url_w, _countof(zip_url_w));

    char *sha_text = NULL;
    size_t sha_len = 0;
    if (!http_get(sha_url_w, FALSE, allow_local, 4096, &sha_text, &sha_len, INVALID_HANDLE_VALUE, NULL,
                  NULL, err, sizeof err)) {
        updater_log(coop, "update failed downloading sha256: %s", err);
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"Could not download the update verification hash. Your existing files were not changed.", MB_ICONERROR);
        goto cleanup;
    }
    BYTE expected_hash[32];
    BOOL hash_parsed = parse_sha256_hex(sha_text, expected_hash);
    free(sha_text);
    if (!hash_parsed) {
        updater_log(coop, "update refused: invalid sha256 file format");
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"Could not update: the SHA-256 verification file is invalid. "
                L"Your existing files were not changed.", MB_ICONERROR);
        goto cleanup;
    }

    if (!safe_join(zip_path, stage_dir, L"SavageWind-Coop.zip") ||
        !safe_join(extract_dir, stage_dir, L"extract") ||
        !safe_join(backup_dir, stage_dir, L"backup") ||
        !safe_join(tar_log, stage_dir, L"tar.log")) {
        updater_log(coop, "update aborted: path too long");
        goto cleanup;
    }

    if (!download_and_verify_zip(zip_url_w, expected_hash, allow_local, zip_path, err, sizeof err)) {
        updater_log(coop, "update refused during download/verify: %s", err);
        if (win) { DestroyWindow(win); win = NULL; }
        wchar_t msg[512];
        wchar_t err_w[256];
        utf8_to_wide(err, err_w, _countof(err_w));
        swprintf(msg, _countof(msg),
                 L"Update download failed verification (%ls).\n\nYour existing files were not changed.", err_w);
        message(msg, MB_ICONERROR);
        goto cleanup;
    }

    if (!CreateDirectoryW(extract_dir, NULL) || !CreateDirectoryW(backup_dir, NULL)) {
        updater_log(coop, "update aborted: could not create extract/backup dirs");
        goto cleanup;
    }
    if (!run_tar_extract(zip_path, extract_dir, tar_log, win, err, sizeof err)) {
        updater_log(coop, "update failed extracting zip: %s", err);
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"Could not extract the update archive. Your existing files were not changed.", MB_ICONERROR);
        goto cleanup;
    }

    if (!safe_join(new_coop, extract_dir, L"Co-op") || !check_tree_safe(new_coop)) {
        updater_log(coop, "update refused: extracted Co-op folder missing or contains links");
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"The update archive has an unexpected layout. Your existing files were not changed.", MB_ICONERROR);
        goto cleanup;
    }

    /* Check required files and gamelib.id before touching anything in Co-op\. */
    static const wchar_t *req_files[] = {
        L"Savage Wind Co-op.exe", L"savagewind.exe", L"gamelib.id", L"manifest.json"
    };
    for (size_t i = 0; i < _countof(req_files); ++i) {
        Path req;
        if (!safe_join(req, new_coop, req_files[i]) || !is_file(req)) {
            updater_log(coop, "update refused: missing required file in extracted zip");
            if (win) { DestroyWindow(win); win = NULL; }
            message(L"The update archive is incomplete. Your existing files were not changed.", MB_ICONERROR);
            goto cleanup;
        }
    }

    Path new_id, have_id;
    wchar_t id_err[1024];
    if (!safe_join(new_id, new_coop, L"gamelib.id") ||
        !safe_join(have_id, game, L"extracted\\swgame.dll.id") ||
        !check_gamelib_match(new_id, have_id, TRUE, id_err, _countof(id_err))) {
        updater_log(coop, "update refused: gamelib.id mismatch with installed Savage Wind");
        if (win) { DestroyWindow(win); win = NULL; }
        message(id_err, MB_ICONERROR);
        goto cleanup;
    }

    /* Swap top-level entries from extracted Co-op\ into live Co-op\, keeping backups for rollback. */
    const wchar_t *self_name = wcsrchr(self_exe, L'\\');
    self_name = self_name ? self_name + 1 : self_exe;
    Path self_old;
    if (swprintf(self_old, _countof(self_old), L"%ls.old", self_exe) < 0) goto cleanup;

    SwappedEntry swapped[128];
    size_t n_swapped = 0;
    BOOL swap_ok = TRUE;

    Path search;
    if (!safe_join(search, new_coop, L"*")) { swap_ok = FALSE; }
    WIN32_FIND_DATAW entry;
    HANDLE find = swap_ok ? FindFirstFileW(search, &entry) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) swap_ok = FALSE;
    if (swap_ok) {
        do {
            if (dot_entry(&entry) || is_protected_entry(entry.cFileName)) continue;
            if (n_swapped >= _countof(swapped)) { swap_ok = FALSE; break; }
            Path src_p, dst_p, bak_p;
            if (!safe_join(src_p, new_coop, entry.cFileName) ||
                !safe_join(dst_p, coop, entry.cFileName) ||
                !safe_join(bak_p, backup_dir, entry.cFileName)) {
                swap_ok = FALSE;
                break;
            }
            DWORD dst_attr = GetFileAttributesW(dst_p);
            if (dst_attr != INVALID_FILE_ATTRIBUTES && (dst_attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                swap_ok = FALSE;
                break;
            }
            BOOL is_self = !_wcsicmp(entry.cFileName, self_name);
            BOOL had_old = dst_attr != INVALID_FILE_ATTRIBUTES;
            if (had_old) {
                if (is_self) {
                    DeleteFileW(self_old);
                    if (!MoveFileExW(dst_p, self_old, MOVEFILE_REPLACE_EXISTING)) {
                        updater_log(coop, "update swap failed renaming running exe (%lu)", (unsigned long)GetLastError());
                        swap_ok = FALSE;
                        break;
                    }
                } else {
                    if (!MoveFileExW(dst_p, bak_p, 0)) {
                        updater_log(coop, "update swap failed backing up entry (%lu)", (unsigned long)GetLastError());
                        swap_ok = FALSE;
                        break;
                    }
                }
            }
            if (!MoveFileExW(src_p, dst_p, 0)) {
                DWORD move_err = GetLastError();
                if (had_old) MoveFileExW(is_self ? self_old : bak_p, dst_p, MOVEFILE_REPLACE_EXISTING);
                updater_log(coop, "update swap failed placing new entry (%lu)", (unsigned long)move_err);
                swap_ok = FALSE;
                break;
            }
            wcsncpy(swapped[n_swapped].name, entry.cFileName, _countof(swapped[n_swapped].name) - 1);
            swapped[n_swapped].name[_countof(swapped[n_swapped].name) - 1] = 0;
            swapped[n_swapped].had_old = had_old;
            swapped[n_swapped].is_self = is_self;
            ++n_swapped;
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    if (!swap_ok) {
        for (size_t i = n_swapped; i > 0; --i) {
            SwappedEntry *e = &swapped[i - 1];
            Path src_p, dst_p, bak_p;
            if (safe_join(src_p, new_coop, e->name) &&
                safe_join(dst_p, coop, e->name) &&
                safe_join(bak_p, backup_dir, e->name)) {
                if (!MoveFileExW(dst_p, src_p, MOVEFILE_REPLACE_EXISTING)) remove_stage(dst_p);
                if (e->had_old) MoveFileExW(e->is_self ? self_old : bak_p, dst_p, MOVEFILE_REPLACE_EXISTING);
            }
        }
        if (win) { DestroyWindow(win); win = NULL; }
        message(L"Could not replace the co-op files (a file may be in use). "
                L"Your existing files were restored.", MB_ICONERROR);
        goto cleanup;
    }

    if (win) { DestroyWindow(win); win = NULL; }
    remove_stage(stage_dir);

    char ver_utf8[64] = {0};
    WideCharToMultiByte(CP_UTF8, 0, remote_ver, -1, ver_utf8, sizeof ver_utf8, NULL, NULL);
    updater_log(coop, "updated to %s; restarting launcher", ver_utf8);

    /* Release the single-instance mutex before starting the updated launcher. */
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    const wchar_t *raw_cmd = GetCommandLineW();
    wchar_t *cmd_copy = _wcsdup(raw_cmd ? raw_cmd : self_exe);
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    BOOL restarted = CreateProcessW(self_exe, cmd_copy, NULL, NULL, FALSE, 0, NULL, coop, &si, &pi);
    free(cmd_copy);
    if (restarted) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        swapped_and_restarting = TRUE;
        ExitProcess(0);
    }
    updater_log(coop, "update applied but restart failed (%lu)", (unsigned long)GetLastError());
    message(L"Update finished! Please start Savage Wind Co-op.exe again.", MB_ICONINFORMATION);
    ExitProcess(0);

cleanup:
    if (win) DestroyWindow(win);
    remove_stage(stage_dir);
    return swapped_and_restarting;
}

static void check_for_update(HINSTANCE inst, const wchar_t *coop, const wchar_t *game,
                             const wchar_t *self_exe, const wchar_t *current_ver, HANDLE mutex) {
    wchar_t env_upd[16] = {0};
    if (GetEnvironmentVariableW(L"SAVAGEWIND_COOP_UPDATE", env_upd, _countof(env_upd)) == 1 && env_upd[0] == L'0') {
        updater_log(coop, "update check disabled by SAVAGEWIND_COOP_UPDATE=0");
        return;
    }

    wchar_t override_url[1024] = {0};
    DWORD override_len = GetEnvironmentVariableW(L"SAVAGEWIND_COOP_UPDATE_URL", override_url, _countof(override_url));
    BOOL allow_local = override_len > 0 && override_len < _countof(override_url);

    /* Skip auto-update in a source dev checkout unless a test override URL is explicitly set. */
    Path dev_exe;
    if (!allow_local && safe_join(dev_exe, coop, L"upstream\\build\\savagewind.exe") && is_file(dev_exe)) {
        updater_log(coop, "dev build detected; skipping update check");
        return;
    }

    BOOL enabled = TRUE;
    wchar_t skip_ver[64] = {0};
    read_updater_ini(coop, &enabled, skip_ver, _countof(skip_ver));
    if (!enabled) {
        updater_log(coop, "update check disabled by updater.ini");
        return;
    }

    wchar_t api_url[1280];
    if (allow_local) {
        /* Accept either a full URL to the JSON or a base URL (append /releases/latest if no path). */
        const wchar_t *after_scheme = wcsstr(override_url, L"://");
        const wchar_t *slash = after_scheme ? wcschr(after_scheme + 3, L'/') : NULL;
        if (!slash || !slash[1]) {
            size_t len = wcslen(override_url);
            const wchar_t *sep = (len && override_url[len - 1] == L'/') ? L"" : L"/";
            swprintf(api_url, _countof(api_url), L"%ls%lsreleases/latest", override_url, sep);
        } else {
            wcsncpy(api_url, override_url, _countof(api_url) - 1);
            api_url[_countof(api_url) - 1] = 0;
        }
    } else {
        wcscpy(api_url, UPDATER_API_URL);
    }

    char *json = NULL;
    size_t json_len = 0;
    char err[256] = {0};
    if (!http_get(api_url, TRUE, allow_local, 512u * 1024u, &json, &json_len, INVALID_HANDLE_VALUE, NULL,
                  NULL, err, sizeof err)) {
        updater_log(coop, "update check skipped: %s", err);
        return;
    }

    ReleaseInfo rel;
    BOOL parsed = parse_release_json(json, &rel);
    free(json);
    if (!parsed) {
        updater_log(coop, "update check skipped: could not parse release JSON");
        return;
    }

    wchar_t tag_w[64], remote_ver[64];
    utf8_to_wide(rel.tag, tag_w, _countof(tag_w));
    strip_v_prefix(tag_w, remote_ver, _countof(remote_ver));

    int cmp = 0;
    if (!compare_versions(remote_ver, current_ver, &cmp)) {
        updater_log(coop, "update check skipped: unrecognized version tag '%s'", rel.tag);
        return;
    }
    if (cmp <= 0) {
        char cur_u[64] = {0}, rem_u[64] = {0};
        WideCharToMultiByte(CP_UTF8, 0, current_ver, -1, cur_u, sizeof cur_u, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, remote_ver, -1, rem_u, sizeof rem_u, NULL, NULL);
        updater_log(coop, "up to date (current %s, latest %s)", cur_u, rem_u);
        return;
    }
    if (skip_ver[0]) {
        int scmp = 0;
        if (compare_versions(remote_ver, skip_ver, &scmp) && scmp == 0) {
            updater_log(coop, "skipping version %s (remembered)", rel.tag);
            return;
        }
    }

    int choice = prompt_update(coop, remote_ver, current_ver, rel.body);
    if (choice == UPD_SKIP) {
        write_skip_version(coop, remote_ver);
        updater_log(coop, "user skipped version %s", rel.tag);
        return;
    }
    if (choice != UPD_UPDATE) {
        updater_log(coop, "user postponed update to %s", rel.tag);
        return;
    }

    apply_update(inst, coop, game, self_exe, &rel, remote_ver, allow_local, mutex);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE previous, PWSTR command, int show) {
    (void)previous;
    (void)command;
    (void)show;
    Path self_exe, coop, game;
    DWORD length = GetModuleFileNameW(NULL, self_exe, _countof(self_exe));
    if (!length || length >= _countof(self_exe)) fail(L"Could not find this program's folder.");
    wcscpy(coop, self_exe);
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

    wchar_t current_ver[64];
    load_current_version(coop, current_ver, _countof(current_ver));
    swprintf(title, _countof(title), L"Savage Wind Co-op %ls", current_ver);

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

    cleanup_updater_leftovers(coop, self_exe);
    check_for_update(inst, coop, game, self_exe, current_ver, mutex);

    const wchar_t *dlls = choose_program(coop, game, exe, library);
    require_file(library);
    if (*test_log && env_one(L"SAVAGEWIND_COOP_LAUNCHER_TEST_NO_GAME")) {
        updater_log(coop, "test: reached launch point, skipping game start");
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }
    prepare_overlay(inst, coop, romfs, overlay);
    migrate(coop, game);
    prepare_data(game, data, save, ini, fresh, exclusive);
    game_environment(dlls, data, save, ini, library, overlay);
    int code = run_game(exe, extracted, data);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
