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

#endif
