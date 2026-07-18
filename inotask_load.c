/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_load.h"
#include "inotask_log.h"
#include "inotask_parser.h"
#include "inotask_validate.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Read an entire file into a newly allocated buffer.
 *
 * The returned buffer is NUL-terminated for convenience, though the reported
 * length excludes that trailing byte.
 *
 * @param path Path to the file to read.
 * @param out_len Output byte length of the file contents.
 *
 * @return Heap-allocated buffer on success, or NULL on I/O or allocation
 *         failure.
 */
static char *read_all(const char *path, size_t *out_len)
{
    FILE *fp;
    long end;
    size_t size;
    size_t n;
    char *buf;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    end = ftell(fp);
    if (end < 0) { fclose(fp); return NULL; }
    size = (size_t)end;
    if (size > SIZE_MAX - 1) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (char *)malloc(size + 1);
    if (!buf) { fclose(fp); return NULL; }
    n = fread(buf, 1, size, fp);
    if (n != size || ferror(fp)) { free(buf); fclose(fp); return NULL; }
    buf[n] = '\0';
    fclose(fp);
    *out_len = n;
    return buf;
}

bool it_load_config_file(const char *path, it_config *cfg)
{
    char *buf;
    size_t len;
    it_parse_error pe;
    it_cfg_errc rc;
    it_config_init(cfg);
    buf = read_all(path, &len);
    if (!buf) { it_log_error("cannot read config file: %s", path); return false; }
    if (!it_parse_config(buf, len, cfg, &pe)) {
        it_log_error("%s:%zu:%zu: %s", path, pe.line, pe.col,
                     pe.detail ? pe.detail : "parse error");
        it_parse_error_free(&pe);
        free(buf);
        it_config_free(cfg);
        return false;
    }
    free(buf);
    rc = it_validate_config_v01(cfg);
    if (rc != IT_CFG_OK) {
        it_log_error("%s: validation failed: %s", path, it_cfg_errc_str(rc));
        it_config_free(cfg);
        return false;
    }
    return true;
}
