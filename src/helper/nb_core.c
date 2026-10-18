/*
 * Notebooks helper: the parts that don't talk to LIPC, so they can be unit
 * tested on any machine (see tests/test_core.c).
 */

#include "nb_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


/* ----- growable byte buffer ----- */

int buf_reserve(struct buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return 0;
    }

    size_t cap = b->cap ? b->cap : 4096;

    while (cap < b->len + extra + 1) {
        cap *= 2;
    }

    char *p = realloc(b->data, cap);

    if (!p) {
        return -1;
    }

    b->data = p;
    b->cap = cap;
    return 0;
}

int buf_append(struct buf *b, const char *s, size_t n) {
    if (buf_reserve(b, n) != 0) {
        return -1;
    }

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buf_puts(struct buf *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

void buf_free(struct buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}


/*
 * Append s as a JSON string literal. Bytes >= 0x80 pass through untouched
 * (the page loads data.js as UTF-8); U+2028/U+2029 are escaped because old
 * JavaScript engines treat them as line breaks inside string literals.
 */
int json_append_string(struct buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";

    if (buf_append(b, "\"", 1) != 0) {
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[7];
        size_t en = 0;

        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            en = 2;
        } else if (c == '\n') {
            memcpy(esc, "\\n", 2);
            en = 2;
        } else if (c == '\r') {
            memcpy(esc, "\\r", 2);
            en = 2;
        } else if (c == '\t') {
            memcpy(esc, "\\t", 2);
            en = 2;
        } else if (c < 0x20 || c == 0x7f) {
            memcpy(esc, "\\u00", 4);
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
            en = 6;
        } else if (c == 0xe2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
                   ((unsigned char)s[i + 2] == 0xa8 || (unsigned char)s[i + 2] == 0xa9)) {
            memcpy(esc, (unsigned char)s[i + 2] == 0xa8 ? "\\u2028" : "\\u2029", 6);
            en = 6;
            i += 2;
        } else if (c == '<' && i + 1 < n && s[i + 1] == '/') {
            /* Never let file content close the <script> it's loaded by. */
            memcpy(esc, "<\\", 2);
            en = 2;
        } else {
            esc[0] = (char)c;
            en = 1;
        }

        if (buf_append(b, esc, en) != 0) {
            return -1;
        }
    }

    return buf_append(b, "\"", 1);
}


/* ----- base64 (standard alphabet, padding optional, whitespace ignored) ----- */

static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, size_t n, struct buf *out) {
    unsigned int acc = 0;
    int bits = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];

        if (c == '=') {
            break;
        }

        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }

        int v = b64_value(c);

        if (v < 0) {
            return -1;
        }

        acc = (acc << 6) | (unsigned int)v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            char byte = (char)((acc >> bits) & 0xff);

            if (buf_append(out, &byte, 1) != 0) {
                return -1;
            }
        }
    }

    return 0;
}


/*
 * A note path is exactly "<notebook>/<note>.txt" relative to the notes root.
 * Each part: 1-120 bytes, no slash or backslash, no control characters, and
 * not starting with a dot (so no "..", no hidden files).
 */
static int valid_segment(const char *s, size_t n) {
    if (n == 0 || n > 120 || s[0] == '.' || s[0] == ' ') {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') {
            return 0;
        }
    }

    return 1;
}

int valid_note_path(const char *rel) {
    const char *slash = strchr(rel, '/');

    if (!slash || strchr(slash + 1, '/')) {
        return 0;
    }

    size_t nb_len = (size_t)(slash - rel);
    const char *name = slash + 1;
    size_t name_len = strlen(name);

    if (!valid_segment(rel, nb_len) || !valid_segment(name, name_len)) {
        return 0;
    }

    return name_len > 4 && strcmp(name + name_len - 4, ".txt") == 0;
}


/* ----- files ----- */

int read_file(const char *path, size_t limit, struct buf *out) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        return -1;
    }

    char chunk[8192];
    size_t n;
    int rc = 0;

    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (out->len + n > limit) {
            rc = -2;
            break;
        }

        if (buf_append(out, chunk, n) != 0) {
            rc = -1;
            break;
        }
    }

    fclose(f);
    return rc;
}

static int write_all(int fd, const char *data, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, data, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        data += w;
        n -= (size_t)w;
    }

    return 0;
}

static void fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/*
 * Write to "<path>.tmp", fsync, then rename over <path>, so a note is never
 * left half-written if the Kindle sleeps, crashes or enters USB mode.
 */
int write_file_atomic(const char *dir, const char *path, const char *data, size_t n) {
    char tmp[4096];

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        return -1;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        return -1;
    }

    if (write_all(fd, data, n) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }

    close(fd);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    fsync_dir(dir);
    return 0;
}


/*
 * Append every note under root as JSON objects:
 *   {"path":"Walden/2026-10-18 201503.txt","mtime":1792300000,"text":"..."}
 * Hidden entries and anything that isn't a valid note path are skipped.
 */
int append_notes_json(struct buf *b, const char *root) {
    DIR *top = opendir(root);
    int first = 1;

    if (buf_puts(b, "[") != 0) {
        return -1;
    }

    if (!top) {
        return buf_puts(b, "]");
    }

    struct dirent *nb;

    while ((nb = readdir(top)) != NULL) {
        if (nb->d_name[0] == '.') {
            continue;
        }

        char nb_path[4096];
        snprintf(nb_path, sizeof nb_path, "%s/%s", root, nb->d_name);

        struct stat st;

        if (stat(nb_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        DIR *sub = opendir(nb_path);

        if (!sub) {
            continue;
        }

        struct dirent *note;

        while ((note = readdir(sub)) != NULL) {
            char rel[600];

            if (snprintf(rel, sizeof rel, "%s/%s", nb->d_name, note->d_name) >= (int)sizeof rel ||
                !valid_note_path(rel)) {
                continue;
            }

            char full[4096];
            snprintf(full, sizeof full, "%s/%s", root, rel);

            struct stat nst;
            struct buf text = {0};

            if (stat(full, &nst) != 0 || !S_ISREG(nst.st_mode) ||
                read_file(full, NB_MAX_NOTE_BYTES, &text) != 0) {
                buf_free(&text);
                continue;
            }

            char mtime[32];
            snprintf(mtime, sizeof mtime, "%lld", (long long)nst.st_mtime);

            int rc = buf_puts(b, first ? "{\"path\":" : ",{\"path\":");
            rc |= json_append_string(b, rel, strlen(rel));
            rc |= buf_puts(b, ",\"mtime\":");
            rc |= buf_puts(b, mtime);
            rc |= buf_puts(b, ",\"text\":");
            rc |= json_append_string(b, text.data ? text.data : "", text.len);
            rc |= buf_puts(b, "}");

            buf_free(&text);
            first = 0;

            if (rc != 0) {
                closedir(sub);
                closedir(top);
                return -1;
            }
        }

        closedir(sub);
    }

    closedir(top);
    return buf_puts(b, "]");
}


/*
 * Percent-encode a book path for an app:// URL. The reader refuses raw
 * paths with spaces, commas or apostrophes ("could not be started").
 */
int url_encode_path(const char *in, char *out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        int keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '/' || *p == '-' ||
                   *p == '_' || *p == '.' || *p == '~';

        if (o + (keep ? 1 : 3) >= out_size) {
            return -1;
        }

        if (keep) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }

    out[o] = '\0';
    return 0;
}


/*
 * Read the page table from an APNX file (the page numbers calibre and the
 * Kindle store make). Entries are text positions where each page starts;
 * Kindle location L covers positions from (L - 1) * 150.
 *
 * Appends either {"step":N,"count":M} when pages are evenly spaced (what
 * calibre writes) or the full list [p0,p1,...]. Returns -1 if the file
 * isn't a page table we understand.
 */
static unsigned int be16(const unsigned char *p) {
    return (unsigned int)p[0] << 8 | p[1];
}

static unsigned long be32(const unsigned char *p) {
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

int append_apnx_json(struct buf *b, const char *data, size_t n) {
    const unsigned char *d = (const unsigned char *)data;

    if (n < 12) {
        return -1;
    }

    unsigned long off = be32(d + 4);

    if (off + 8 > n) {
        return -1;
    }

    unsigned int header_len = be16(d + off + 2);
    unsigned int count = be16(d + off + 4);
    unsigned int bits = be16(d + off + 6);
    unsigned long start = off + 8 + header_len;

    if (bits != 32 || count == 0 || start + 4UL * count > n) {
        return -1;
    }

    const unsigned char *pages = d + start;
    unsigned long step = count > 1 ? be32(pages + 4) - be32(pages) : 0;
    int even = be32(pages) == 0 && step > 0;

    for (unsigned int i = 1; even && i < count; i++) {
        if (be32(pages + 4 * i) != step * i) {
            even = 0;
        }
    }

    char num[48];

    if (even) {
        snprintf(num, sizeof num, "{\"step\":%lu,\"count\":%u}", step, count);
        return buf_puts(b, num);
    }

    if (buf_puts(b, "[") != 0) {
        return -1;
    }

    for (unsigned int i = 0; i < count; i++) {
        snprintf(num, sizeof num, i ? ",%lu" : "%lu", be32(pages + 4 * i));

        if (buf_puts(b, num) != 0) {
            return -1;
        }
    }

    return buf_puts(b, "]");
}
