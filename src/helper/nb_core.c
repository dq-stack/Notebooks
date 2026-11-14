/*
 * Notebooks helper: the parts that don't talk to LIPC, so they can be unit
 * tested on any machine (see tests/test_core.c).
 */

#include "nb_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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


/*
 * ----- deleting a highlight -----
 *
 * A highlight lives in two places: "My Clippings.txt" (the Kindle appends
 * an entry, and another on every edit) and the book's own annotation file,
 * Book.sdr/Book<hash>.azw3r, which is what the reader shows. Deleting means
 * taking it out of both. Both edits are pure: they only build the new file
 * in memory, refuse anything they don't fully understand, and check the
 * result before the caller writes it.
 */

/* Seconds since 1970 for a calendar date and time, read as UTC. */
long long civil_seconds(int y, int mo, int d, int h, int mi, int s) {
    y -= mo <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

static int month_number(const char *s, size_t n) {
    static const char *const names[] = {
        "january", "february", "march", "april", "may", "june", "july",
        "august", "september", "october", "november", "december"
    };

    for (int i = 0; i < 12; i++) {
        if (strlen(names[i]) == n) {
            size_t k = 0;

            while (k < n && (s[k] | 0x20) == names[i][k]) {
                k++;
            }

            if (k == n) {
                return i + 1;
            }
        }
    }

    return 0;
}

/* "location " or "Location " (the Kindle 11th gen on 5.18 capitalises it). */
static const char *find_location(const char *p) {
    for (; *p; p++) {
        if ((p[0] == 'l' || p[0] == 'L') && strncmp(p + 1, "ocation ", 8) == 0) {
            return p;
        }
    }

    return NULL;
}

/*
 * Read a clipping's meta line:
 *   "- Your Highlight on page 11 | location 165-165 | Added on Thursday, 29 October 2026 00:18:26"
 *   "- Your Highlight on page 311 | Location 4769-4769 | Added on Saturday, 14 November 2026 18:49:15"
 *   "- Your Note at location 566 | Added on Thursday, October 1, 2026 12:15:31 PM"
 * kind: CLIP_HIGHLIGHT or CLIP_NOTE; loc: start location (0 if none);
 * added: the "Added on" clock time as civil_seconds (no time zone).
 */
int parse_clip_meta(const char *meta, int *kind, long *loc, long long *added) {
    const char *p = meta;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (strncmp(p, "- Your Highlight", 16) == 0) {
        *kind = CLIP_HIGHLIGHT;
    } else if (strncmp(p, "- Your Note", 11) == 0) {
        *kind = CLIP_NOTE;
    } else {
        return -1;
    }

    const char *l = find_location(p);
    *loc = 0;

    if (l) {
        char *end;
        long v = strtol(l + 9, &end, 10);

        if (end == l + 9 || v <= 0) {
            return -1;
        }

        *loc = v;
    }

    const char *a = strstr(p, "Added on ");

    if (!a) {
        return -1;
    }

    a += 9;

    const char *comma = strchr(a, ',');     /* skip the weekday */

    if (!comma) {
        return -1;
    }

    a = comma + 1;

    while (*a == ' ') {
        a++;
    }

    int d = 0, mo = 0, y = 0, h = 0, mi = 0, s = 0, used = 0;
    char word[16];

    if (sscanf(a, "%2d %15[A-Za-z] %4d %2d:%2d:%2d%n", &d, word, &y, &h, &mi, &s, &used) == 6) {
        mo = month_number(word, strlen(word));
    } else if (sscanf(a, "%15[A-Za-z] %2d, %4d %2d:%2d:%2d%n", word, &d, &y, &h, &mi, &s, &used) == 6) {
        mo = month_number(word, strlen(word));

        const char *t = a + used;

        while (*t == ' ') {
            t++;
        }

        if ((t[0] == 'A' || t[0] == 'P') && t[1] == 'M') {
            h = h % 12 + (t[0] == 'P' ? 12 : 0);
        }
    } else {
        return -1;
    }

    if (mo == 0 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60 || y < 2000 || y > 2200) {
        return -1;
    }

    *added = civil_seconds(y, mo, d, h, mi, s);
    return 0;
}


/* --- KRDS: the Kindle reader's data-store format (.azw3r / .azw3f) --- */

#define KRDS_BEGIN 0xfe
#define KRDS_END 0xff

static const unsigned char krds_magic[8] = {0, 0, 0, 0, 0, 0x1a, 0xb1, 0x26};

struct ktok {
    int type;           /* 0 bool, 1 int, 2 long, 3 utf, 4 double, 5 short, 6 float, 7 byte, 9 char, BEGIN, END */
    size_t start, end;  /* bytes of the whole token */
    long long num;      /* int / long value */
    size_t str, slen;   /* utf text or BEGIN name */
    int null_str;
};

struct ktoks {
    struct ktok *t;
    size_t n, cap;
};

static int ktok_push(struct ktoks *k, const struct ktok *t) {
    if (k->n == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 256;
        struct ktok *p = realloc(k->t, cap * sizeof *p);

        if (!p) {
            return -1;
        }

        k->t = p;
        k->cap = cap;
    }

    k->t[k->n++] = *t;
    return 0;
}

/* Split a whole KRDS file into tokens; -1 unless every byte is understood and BEGIN/END balance. */
static int krds_tokens(const unsigned char *d, size_t n, struct ktoks *k) {
    if (n < 8 || memcmp(d, krds_magic, 8) != 0) {
        return -1;
    }

    size_t p = 8;
    long depth = 0;

    while (p < n) {
        struct ktok t = {0};
        size_t need;

        t.type = d[p];
        t.start = p++;

        switch (t.type) {
        case 0: case 7: need = 1; break;
        case 5: case 9: need = 2; break;
        case 1: case 6: need = 4; break;
        case 2: case 4: need = 8; break;
        case 3:
        case KRDS_BEGIN:
            if (p >= n) {
                return -1;
            }

            if (d[p] == 1 && t.type == 3) {
                t.null_str = 1;
                need = 1;
                break;
            }

            if (d[p] != 0 || p + 3 > n) {
                return -1;
            }

            t.slen = (size_t)be16(d + p + 1);
            t.str = p + 3;
            need = 3 + t.slen;
            break;
        case KRDS_END: need = 0; break;
        default: return -1;
        }

        if (need > n - p) {
            return -1;
        }

        if (t.type == 1) {
            t.num = (long long)(int32_t)be32(d + p);
        } else if (t.type == 2) {
            t.num = (long long)((uint64_t)be32(d + p) << 32 | be32(d + p + 4));
        }

        p += need;
        t.end = p;

        if (t.type == KRDS_BEGIN) {
            depth++;
        } else if (t.type == KRDS_END && --depth < 0) {
            return -1;
        }

        if (ktok_push(k, &t) != 0) {
            return -1;
        }
    }

    return depth == 0 ? 0 : -1;
}

static int ktok_is(const unsigned char *d, const struct ktok *t, int type, const char *name) {
    return t->type == type && (!name || (t->slen == strlen(name) && memcmp(d + t->str, name, t->slen) == 0));
}

/* Index just past the END that closes the BEGIN at i, or 0. */
static size_t ktok_close(const struct ktoks *k, size_t i) {
    long depth = 0;

    for (size_t j = i; j < k->n; j++) {
        if (k->t[j].type == KRDS_BEGIN) {
            depth++;
        } else if (k->t[j].type == KRDS_END && --depth == 0) {
            return j + 1;
        }
    }

    return 0;
}

#define KRDS_MAX_CATS 16

struct krec {
    size_t tok;             /* BEGIN token index */
    size_t start, end;      /* bytes */
    long loc;
    long long ms[2];
    int remove;
};

struct kcat {
    long long type;
    size_t type_tok, count_tok, tree_end_byte;
    struct krec *recs;
    size_t nrec;
};

struct kann {
    size_t cache_tok, cache_end_tok;    /* BEGIN of annotation.cache.object, index past its END */
    size_t ncat_tok;                    /* 0 if the object is empty */
    struct kcat cats[KRDS_MAX_CATS];
    int ncat;
};

static void kann_free(struct kann *a) {
    for (int i = 0; i < a->ncat; i++) {
        free(a->cats[i].recs);
    }
}

static const char *krds_record_name(long long type) {
    return type == 0 ? "annotation.personal.bookmark" :
        type == 1 ? "annotation.personal.highlight" :
        type == 2 ? "annotation.personal.note" : NULL;
}

/*
 * Find annotation.cache.object and check its layout:
 *   int ncat, then ncat x (int type, BEGIN saved.avl.interval.tree, int count,
 *   count x BEGIN annotation.personal.<kind> utf start, utf end, long, long ... END, END)
 * Returns 1 if found and understood, 0 if the file has none, -1 otherwise.
 */
static int krds_annotations(const unsigned char *d, const struct ktoks *k, struct kann *a) {
    memset(a, 0, sizeof *a);

    size_t i;
    long depth = 0;

    for (i = 0; i < k->n; i++) {
        if (depth == 0 && ktok_is(d, &k->t[i], KRDS_BEGIN, "annotation.cache.object")) {
            break;
        }

        if (k->t[i].type == KRDS_BEGIN) {
            depth++;
        } else if (k->t[i].type == KRDS_END) {
            depth--;
        }
    }

    if (i == k->n) {
        return 0;
    }

    a->cache_tok = i;
    a->cache_end_tok = ktok_close(k, i);

    if (!a->cache_end_tok) {
        return -1;
    }

    size_t j = i + 1;
    size_t last = a->cache_end_tok - 1;     /* the END token */

    if (j == last) {
        return 1;                           /* empty object */
    }

    if (k->t[j].type != 1 || k->t[j].num < 0 || k->t[j].num > KRDS_MAX_CATS) {
        return -1;
    }

    a->ncat_tok = j;
    a->ncat = (int)k->t[j].num;
    j++;

    for (int c = 0; c < a->ncat; c++) {
        struct kcat *cat = &a->cats[c];

        if (j + 2 >= last || k->t[j].type != 1 ||
            !ktok_is(d, &k->t[j + 1], KRDS_BEGIN, "saved.avl.interval.tree") || k->t[j + 2].type != 1) {
            a->ncat = c;
            return -1;
        }

        cat->type = k->t[j].num;
        cat->type_tok = j;
        cat->count_tok = j + 2;

        size_t tree_end = ktok_close(k, j + 1);
        long long count = k->t[j + 2].num;

        if (!tree_end || count < 0 || (size_t)count > k->n) {
            a->ncat = c;
            return -1;
        }

        cat->tree_end_byte = k->t[tree_end - 1].end;
        cat->recs = calloc((size_t)count + 1, sizeof *cat->recs);

        if (!cat->recs) {
            a->ncat = c;
            return -1;
        }

        const char *name = krds_record_name(cat->type);
        size_t r = j + 3;

        for (long long e = 0; e < count; e++) {
            if (r >= tree_end - 1 || k->t[r].type != KRDS_BEGIN ||
                (name && !ktok_is(d, &k->t[r], KRDS_BEGIN, name))) {
                a->ncat = c + 1;
                return -1;
            }

            size_t rend = ktok_close(k, r);

            if (!rend || rend > tree_end - 1) {
                a->ncat = c + 1;
                return -1;
            }

            struct krec *rec = &cat->recs[cat->nrec++];
            rec->tok = r;
            rec->start = k->t[r].start;
            rec->end = k->t[rend - 1].end;

            /* highlights and notes: utf start, utf end, long, long, ... */
            if (cat->type == 1 || cat->type == 2) {
                const struct ktok *s = &k->t[r + 1];

                if (rend - r < 6 || s->type != 3 || s->null_str || s->slen == 0 || s->slen > 18 ||
                    k->t[r + 2].type != 3 || k->t[r + 3].type != 2 || k->t[r + 4].type != 2) {
                    a->ncat = c + 1;
                    return -1;
                }

                long long pos = 0;

                for (size_t q = 0; q < s->slen; q++) {
                    unsigned char ch = d[s->str + q];

                    if (ch < '0' || ch > '9') {
                        a->ncat = c + 1;
                        return -1;
                    }

                    pos = pos * 10 + (ch - '0');
                }

                rec->loc = (long)(pos / 150 + 1);
                rec->ms[0] = k->t[r + 3].num;
                rec->ms[1] = k->t[r + 4].num;
            }

            r = rend;
        }

        if (r != tree_end - 1) {
            a->ncat = c + 1;
            return -1;                      /* more records than the count says */
        }

        j = tree_end;
    }

    return j == last ? 1 : -1;
}

/*
 * Same moment? The clipping shows the Kindle's local clock, the record a UTC
 * time in ms. Equal to the second, apart from a whole time-zone offset (a
 * multiple of 15 minutes, at most 14 hours).
 */
static int same_moment(long long local, long long ms) {
    if (ms <= 0) {
        return 0;
    }

    long long diff = local - ms / 1000;
    return diff % 900 == 0 && diff <= 14 * 3600 && diff >= -14 * 3600;
}

static void put_be32(unsigned char *p, long long v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/*
 * Rebuild the file from the original with the marked records left out:
 * tree counts lowered, an emptied tree dropped with its type (the Kindle
 * writes an annotation-free book as "int 0"), the category count lowered.
 */
static int krds_rebuild(const unsigned char *d, size_t n, const struct ktoks *k, const struct kann *a, struct buf *out) {
    size_t p = 0;
    int ncat = a->ncat;

    for (int c = 0; c < a->ncat; c++) {
        size_t gone = 0;

        for (size_t r = 0; r < a->cats[c].nrec; r++) {
            gone += a->cats[c].recs[r].remove;
        }

        if (gone && gone == a->cats[c].nrec) {
            ncat--;
        }
    }

    for (int c = 0; c < a->ncat; c++) {
        const struct kcat *cat = &a->cats[c];
        size_t keep = 0;

        for (size_t r = 0; r < cat->nrec; r++) {
            keep += !cat->recs[r].remove;
        }

        if (keep == cat->nrec) {
            continue;
        }

        if (keep == 0) {
            size_t from = k->t[cat->type_tok].start;

            if (buf_append(out, (const char *)d + p, from - p) != 0) {
                return -1;
            }

            p = cat->tree_end_byte;
            continue;
        }

        size_t count_at = k->t[cat->count_tok].start + 1;
        unsigned char num[4];

        put_be32(num, (long long)keep);

        if (buf_append(out, (const char *)d + p, count_at - p) != 0 || buf_append(out, (const char *)num, 4) != 0) {
            return -1;
        }

        p = count_at + 4;

        for (size_t r = 0; r < cat->nrec; r++) {
            if (!cat->recs[r].remove) {
                continue;
            }

            if (buf_append(out, (const char *)d + p, cat->recs[r].start - p) != 0) {
                return -1;
            }

            p = cat->recs[r].end;
        }
    }

    if (buf_append(out, (const char *)d + p, n - p) != 0) {
        return -1;
    }

    /* finally the category count (it comes before everything changed above) */
    if (ncat != a->ncat) {
        size_t at = k->t[a->ncat_tok].start + 1;
        put_be32((unsigned char *)out->data + at, ncat);
    }

    return 0;
}

/* The new file must parse, keep every other record byte for byte, and change nothing outside the annotations. */
static int krds_verify(const unsigned char *d, size_t n, const struct ktoks *k, const struct kann *a,
                       const unsigned char *o, size_t on) {
    struct ktoks ok = {0};
    struct kann b;
    int rc = -1;

    if (krds_tokens(o, on, &ok) != 0 || krds_annotations(o, &ok, &b) != 1) {
        free(ok.t);
        return -1;
    }

    size_t head = k->t[a->cache_tok].end;
    size_t tail = n - k->t[a->cache_end_tok - 1].end;
    size_t ohead = ok.t[b.cache_tok].end;
    size_t otail = on - ok.t[b.cache_end_tok - 1].end;

    if (head != ohead || tail != otail || memcmp(d, o, head) != 0 || memcmp(d + n - tail, o + on - otail, tail) != 0) {
        goto done;
    }

    int c2 = 0;

    for (int c = 0; c < a->ncat; c++) {
        const struct kcat *cat = &a->cats[c];
        size_t keep = 0;

        for (size_t r = 0; r < cat->nrec; r++) {
            keep += !cat->recs[r].remove;
        }

        if (keep == 0) {
            continue;
        }

        if (c2 >= b.ncat || b.cats[c2].type != cat->type || b.cats[c2].nrec != keep) {
            goto done;
        }

        size_t r2 = 0;

        for (size_t r = 0; r < cat->nrec; r++) {
            if (cat->recs[r].remove) {
                continue;
            }

            const struct krec *x = &cat->recs[r];
            const struct krec *y = &b.cats[c2].recs[r2++];

            if (x->end - x->start != y->end - y->start || memcmp(d + x->start, o + y->start, x->end - x->start) != 0) {
                goto done;
            }
        }

        c2++;
    }

    rc = c2 == b.ncat ? 0 : -1;

done:
    kann_free(&b);
    free(ok.t);
    return rc;
}

/*
 * Remove from a KRDS annotation file the highlights and notes that the given
 * clippings describe. A record is taken out when its kind and location match
 * a clipping and its time matches one of that item's clipping times. For each
 * kind + location: no record there at all is fine (the highlight was only in
 * My Clippings, e.g. made on another Kindle); exactly one matching record is
 * removed; anything else (a record there whose time doesn't match, or two that
 * do) is refused. *removed counts the records taken out (0: out is a copy).
 */
int krds_remove(const char *data, size_t n, const struct clip_target *t, int nt,
                struct buf *out, int *removed, const char **why) {
    const unsigned char *d = (const unsigned char *)data;
    struct ktoks k = {0};
    struct kann a;
    int rc = -1;

    *removed = 0;
    *why = "annotation file not understood";

    if (krds_tokens(d, n, &k) != 0) {
        free(k.t);
        return -1;
    }

    int found = krds_annotations(d, &k, &a);

    if (found < 0) {
        goto done;
    }

    for (int i = 0; found && i < nt; i++) {
        long long type = t[i].kind == CLIP_HIGHLIGHT ? 1 : 2;
        int at_loc = 0, matched = 0;
        struct krec *hit = NULL;

        if (t[i].loc <= 0) {
            *why = "highlight has no location";
            goto done;
        }

        for (int c = 0; c < a.ncat; c++) {
            if (a.cats[c].type != type) {
                continue;
            }

            for (size_t r = 0; r < a.cats[c].nrec; r++) {
                struct krec *rec = &a.cats[c].recs[r];

                if (rec->loc != t[i].loc) {
                    continue;
                }

                at_loc++;

                /* any clipping version of this item may carry the record's time */
                for (int j = 0; j < nt; j++) {
                    if (t[j].kind == t[i].kind && t[j].loc == t[i].loc &&
                        (same_moment(t[j].added, rec->ms[0]) || same_moment(t[j].added, rec->ms[1]))) {
                        if (hit != rec) {
                            matched++;
                        }
                        hit = rec;
                        break;
                    }
                }
            }
        }

        if (at_loc == 0) {
            continue;
        }

        if (matched != 1) {
            *why = matched ? "two highlights match; not deleting" : "the book's highlight there doesn't match";
            goto done;
        }

        hit->remove = 1;
    }

    for (int c = 0; c < a.ncat; c++) {
        for (size_t r = 0; r < a.cats[c].nrec; r++) {
            *removed += a.cats[c].recs[r].remove;
        }
    }

    if (*removed == 0) {
        rc = buf_append(out, data, n);
        goto done;
    }

    *why = "could not rebuild the annotation file";

    if (krds_rebuild(d, n, &k, &a, out) != 0 ||
        krds_verify(d, n, &k, &a, (const unsigned char *)out->data, out->len) != 0) {
        buf_free(out);
        goto done;
    }

    rc = 0;

done:
    kann_free(&a);
    free(k.t);
    return rc;
}


/* --- My Clippings.txt --- */

/* Trim spaces, CR and a UTF-8 byte order mark; returns the trimmed length and moves *s. */
static size_t clip_trim(const char **s, size_t n) {
    const char *p = *s;

    if (n >= 3 && memcmp(p, "\xef\xbb\xbf", 3) == 0) {
        p += 3;
        n -= 3;
    }

    while (n && (*p == ' ' || *p == '\t' || *p == '\r')) {
        p++;
        n--;
    }

    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) {
        n--;
    }

    *s = p;
    return n;
}

static int clip_line_is(const char *line, size_t n, const char *want) {
    n = clip_trim(&line, n);
    const char *w = want;
    size_t wn = clip_trim(&w, strlen(want));
    return n == wn && memcmp(line, w, n) == 0;
}

/*
 * Remove every block whose title line and meta line equal one of the given
 * pairs (compared without byte order mark, CR or surrounding spaces). Each
 * pair must match at least one block, or the file changed under us and
 * nothing is removed. A block runs up to and including its "==========" line.
 */
int clippings_remove(const char *data, size_t n, const char *const *heads, const char *const *metas, int npairs,
                     struct buf *out, int *removed) {
    int *hits = calloc((size_t)npairs + 1, sizeof *hits);
    size_t p = 0;

    *removed = 0;

    if (!hits) {
        return -1;
    }

    while (p < n) {
        size_t start = p, end = n;
        const char *line[2] = {NULL, NULL};
        size_t len[2] = {0, 0};
        int nl = 0;

        /* find this block's end and its first two non-blank lines */
        for (size_t q = p; q < n; ) {
            const char *e = memchr(data + q, '\n', n - q);
            size_t lend = e ? (size_t)(e - data) : n;
            const char *s = data + q;
            size_t sn = lend - q;
            size_t next = e ? lend + 1 : n;

            const char *ts = s;
            size_t tn = clip_trim(&ts, sn);

            if (tn == 10 && memcmp(ts, "==========", 10) == 0) {
                end = next;
                break;
            }

            if (tn && nl < 2) {
                line[nl] = s;
                len[nl] = sn;
                nl++;
            }

            q = next;
        }

        int drop = 0;

        if (nl == 2) {
            for (int i = 0; i < npairs; i++) {
                if (clip_line_is(line[0], len[0], heads[i]) && clip_line_is(line[1], len[1], metas[i])) {
                    hits[i]++;
                    drop = 1;
                }
            }
        }

        if (drop) {
            (*removed)++;
        } else if (buf_append(out, data + start, end - start) != 0) {
            free(hits);
            return -1;
        }

        p = end;
    }

    for (int i = 0; i < npairs; i++) {
        if (!hits[i]) {
            free(hits);
            buf_free(out);
            *removed = 0;
            return -1;
        }
    }

    free(hits);

    if (!out->data && buf_append(out, "", 0) != 0) {
        return -1;
    }

    return 0;
}
