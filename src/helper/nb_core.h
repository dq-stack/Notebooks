#ifndef NB_CORE_H
#define NB_CORE_H

#include <stddef.h>

#define NB_MAX_NOTE_BYTES (512 * 1024)

struct buf {
    char *data;
    size_t len;
    size_t cap;
};

int buf_reserve(struct buf *b, size_t extra);
int buf_append(struct buf *b, const char *s, size_t n);
int buf_puts(struct buf *b, const char *s);
void buf_free(struct buf *b);

int json_append_string(struct buf *b, const char *s, size_t n);
int base64_decode(const char *in, size_t n, struct buf *out);
int valid_note_path(const char *rel);

int read_file(const char *path, size_t limit, struct buf *out);
int write_file_atomic(const char *dir, const char *path, const char *data, size_t n);
int append_notes_json(struct buf *b, const char *root);
int url_encode_path(const char *in, char *out, size_t out_size);
int append_apnx_json(struct buf *b, const char *data, size_t n);

/* Deleting a highlight: from My Clippings.txt and the book's .azw3r file. */
#define CLIP_HIGHLIGHT 1
#define CLIP_NOTE 2

struct clip_target {
    int kind;           /* CLIP_HIGHLIGHT or CLIP_NOTE */
    long loc;           /* Kindle location the clipping starts at */
    long long added;    /* "Added on" clock time, as civil_seconds() */
};

long long civil_seconds(int y, int mo, int d, int h, int mi, int s);
int parse_clip_meta(const char *meta, int *kind, long *loc, long long *added);
int krds_remove(const char *data, size_t n, const struct clip_target *t, int nt,
                struct buf *out, int *removed, const char **why);
int clippings_remove(const char *data, size_t n, const char *const *heads, const char *const *metas, int npairs,
                     struct buf *out, int *removed);

#endif
