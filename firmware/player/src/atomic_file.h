/**
 * @file atomic_file.h
 * @brief Crash-safe small-file save/load for the SD card (FAT).
 *
 * A plain fopen("w") truncates the file first, so a reset / power loss / SD hiccup during the
 * write leaves an empty file and the settings silently fall back to defaults. Here the new
 * content goes to "<path>.tmp" first and only replaces the real file once fully written.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/** Write str to path via path.tmp. Returns true on success. */
static inline bool atomic_file_write(const char *path, const char *str)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    const size_t len = strlen(str);
    bool ok = fwrite(str, 1u, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    fsync(fileno(f));
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }

    remove(path);   /* FAT rename does not reliably overwrite; the .tmp copy is a load fallback */
    return rename(tmp, path) == 0;
}

/**
 * Read a whole file of 1..max_size bytes into a malloc'ed, NUL-terminated buffer (caller frees).
 * Falls back to path.tmp when path is missing/empty (interrupted save). Returns nullptr if neither exists.
 */
static inline char *atomic_file_read(const char *path, size_t max_size)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const char *cands[2] = { path, tmp };
    for (int i = 0; i < 2; i++) {
        struct stat st = {};
        if (stat(cands[i], &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max_size) continue;
        FILE *f = fopen(cands[i], "r");
        if (!f) continue;
        char *buf = (char *)malloc((size_t)st.st_size + 1u);
        if (!buf) { fclose(f); return nullptr; }
        size_t n = fread(buf, 1u, (size_t)st.st_size, f);
        fclose(f);
        buf[n] = '\0';
        return buf;
    }
    return nullptr;
}
