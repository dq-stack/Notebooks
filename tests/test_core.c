/* Build and run: tests/run_core_tests.sh */

#include "../src/helper/nb_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_paths(void) {
    CHECK(valid_note_path("Walden/2026-10-18 201503.txt"));
    CHECK(valid_note_path("Loose notes/a.txt"));
    CHECK(valid_note_path("Les Mis\xc3\xa9rables/n.txt"));
    CHECK(valid_note_path("Don't Panic (2nd ed.)/n.txt"));

    CHECK(!valid_note_path("n.txt"));
    CHECK(!valid_note_path("../etc/n.txt"));
    CHECK(!valid_note_path("Walden/../../x.txt"));
    CHECK(!valid_note_path("Walden/.hidden.txt"));
    CHECK(!valid_note_path(".Walden/n.txt"));
    CHECK(!valid_note_path("Walden/sub/n.txt"));
    CHECK(!valid_note_path("Walden/n.sh"));
    CHECK(!valid_note_path("Walden/.txt"));
    CHECK(!valid_note_path("/Walden/n.txt"));
    CHECK(!valid_note_path("Walden/"));
    CHECK(!valid_note_path("Wal\nden/n.txt"));
    CHECK(!valid_note_path("Wal\\den/n.txt"));
}

static void test_base64(void) {
    struct buf out = {0};

    /* "héllo \"q\" 'a'\n" as produced by btoa(unescape(encodeURIComponent(s))) */
    CHECK(base64_decode("aMOpbGxvICJxIiAnYScK", 20, &out) == 0);
    CHECK(out.len == 15 && memcmp(out.data, "h\xc3\xa9llo \"q\" 'a'\n", 15) == 0);
    buf_free(&out);

    CHECK(base64_decode("", 0, &out) == 0 && out.len == 0);
    buf_free(&out);

    CHECK(base64_decode("YQ==", 4, &out) == 0 && out.len == 1 && out.data[0] == 'a');
    buf_free(&out);

    CHECK(base64_decode("Y$Q=", 4, &out) != 0);
    buf_free(&out);
}

static void test_json(void) {
    struct buf b = {0};
    const char in[] = "a\"b\\c\nd\te\x01</script>\xe2\x80\xa8\xc3\xa9";

    CHECK(json_append_string(&b, in, sizeof in - 1) == 0);
    CHECK(strcmp(b.data, "\"a\\\"b\\\\c\\nd\\te\\u0001<\\/script>\\u2028\xc3\xa9\"") == 0);
    buf_free(&b);
}

static void test_files(void) {
    char root[] = "/tmp/nb_test_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char dir[256], path[320];
    snprintf(dir, sizeof dir, "%s/Walden", root);
    snprintf(path, sizeof path, "%s/a.txt", dir);
    CHECK(mkdir(dir, 0755) == 0);

    CHECK(write_file_atomic(dir, path, "first", 5) == 0);
    CHECK(write_file_atomic(dir, path, "line \"2\"\n", 9) == 0);

    char junk[320];
    snprintf(junk, sizeof junk, "%s/.a.txt.swp", dir);
    FILE *f = fopen(junk, "w");
    fputs("ignored", f);
    fclose(f);

    struct buf b = {0};
    CHECK(append_notes_json(&b, root) == 0);
    CHECK(strstr(b.data, "\"path\":\"Walden/a.txt\"") != NULL);
    CHECK(strstr(b.data, "\"text\":\"line \\\"2\\\"\\n\"") != NULL);
    CHECK(strstr(b.data, "ignored") == NULL);
    buf_free(&b);

    char tmp[400];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CHECK(access(tmp, F_OK) != 0);

    unlink(junk);
    unlink(path);
    rmdir(dir);
    rmdir(root);

    CHECK(append_notes_json(&b, "/nonexistent/nb") == 0 && strcmp(b.data, "[]") == 0);
    buf_free(&b);
}

static void test_url(void) {
    char out[256];

    CHECK(url_encode_path("/mnt/us/documents/Stewart, Aubrey/Plutarch's Lives - Aubrey Stewart.azw3", out, sizeof out) == 0);
    CHECK(strcmp(out, "/mnt/us/documents/Stewart%2C%20Aubrey/Plutarch%27s%20Lives%20-%20Aubrey%20Stewart.azw3") == 0);
    CHECK(url_encode_path("/a/Mis\xc3\xa9rables%.mobi", out, sizeof out) == 0);
    CHECK(strcmp(out, "/a/Mis%C3%A9rables%25.mobi") == 0);
    CHECK(url_encode_path("/aaaa", out, 4) != 0);
}

static void put32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void test_apnx(void) {
    /* version, page-header offset (12 + 2 byte content header), content header len, "{}" */
    unsigned char f[64] = {0};
    struct buf b = {0};

    put32(f, 0x00010001); put32(f + 4, 14); put32(f + 8, 2);
    f[12] = '{'; f[13] = '}';
    f[14] = 0; f[15] = 1;      /* unknown */
    f[16] = 0; f[17] = 0;      /* page header length */
    f[18] = 0; f[19] = 3;      /* 3 pages */
    f[20] = 0; f[21] = 32;     /* 32-bit entries */
    put32(f + 22, 0); put32(f + 26, 2300); put32(f + 30, 4600);

    CHECK(append_apnx_json(&b, (char *)f, 34) == 0 && strcmp(b.data, "{\"step\":2300,\"count\":3}") == 0);
    buf_free(&b);

    put32(f + 30, 5000);
    CHECK(append_apnx_json(&b, (char *)f, 34) == 0 && strcmp(b.data, "[0,2300,5000]") == 0);
    buf_free(&b);

    CHECK(append_apnx_json(&b, (char *)f, 30) != 0); /* truncated */
    buf_free(&b);
}


/* ----- deleting highlights ----- */

static void kput(struct buf *b, const void *p, size_t n) { buf_append(b, (const char *)p, n); }
static void kint(struct buf *b, long v) { unsigned char x[5] = {1, (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v}; kput(b, x, 5); }
static void klong(struct buf *b, long long v) {
    unsigned char x[9] = {2};
    for (int i = 0; i < 8; i++) x[1 + i] = (unsigned char)(v >> (56 - 8 * i));
    kput(b, x, 9);
}
static void kstr(struct buf *b, int type, const char *s) {
    size_t n = strlen(s);
    unsigned char x[4] = {(unsigned char)type, 0, (unsigned char)(n >> 8), (unsigned char)n};
    kput(b, x, 4);
    kput(b, s, n);
}
static void kend(struct buf *b) { unsigned char x = 0xff; kput(b, &x, 1); }
static void krec(struct buf *b, const char *name, const char *start, long long ms, const char *text) {
    kstr(b, 0xfe, name);
    kstr(b, 3, start); kstr(b, 3, start); klong(b, ms); klong(b, ms); kstr(b, 3, "0");
    if (text) kstr(b, 3, text);
    kend(b);
}

/* A book file shaped like the Kindle's: prefs, annotations (2 highlights, 1 note, 1 bookmark), page table. */
static void make_krds(struct buf *b, int first, int second) {
    static const unsigned char magic[8] = {0, 0, 0, 0, 0, 0x1a, 0xb1, 0x26};
    kput(b, magic, 8);
    klong(b, 1);
    kint(b, 3);
    kstr(b, 0xfe, "font.prefs"); kstr(b, 3, "bookerly"); kint(b, 3); kend(b);
    kstr(b, 0xfe, "annotation.cache.object");
    kint(b, 3);
    kint(b, 1); kstr(b, 0xfe, "saved.avl.interval.tree"); kint(b, first + second);
    /* location 4285, created 1 Nov 2026 21:49:57 at UTC+11; location 165, 29 Oct 00:18:26 */
    if (first) krec(b, "annotation.personal.highlight", "642740", 1793530197488LL, NULL);
    if (second) krec(b, "annotation.personal.highlight", "24621", 1793193506661LL, NULL);
    kend(b);
    kint(b, 2); kstr(b, 0xfe, "saved.avl.interval.tree"); kint(b, 1);
    krec(b, "annotation.personal.note", "642940", 1793530300000LL, "a note");
    kend(b);
    kint(b, 0); kstr(b, 0xfe, "saved.avl.interval.tree"); kint(b, 1);
    krec(b, "annotation.personal.bookmark", "700000", 1793530400000LL, NULL);
    kend(b);
    kend(b);
    kstr(b, 0xfe, "apnx.key"); kstr(b, 3, "1"); kend(b);
}

static int count_of(const struct buf *b, const char *needle) {
    int c = 0;
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= b->len; i++) c += memcmp(b->data + i, needle, n) == 0;
    return c;
}

static void test_clip_meta(void) {
    int kind; long loc; long long t;

    CHECK(parse_clip_meta("- Your Highlight on page 280 | location 4285-4287 | Added on Sunday, 1 November 2026 21:49:57", &kind, &loc, &t) == 0);
    CHECK(kind == CLIP_HIGHLIGHT && loc == 4285 && t == civil_seconds(2026, 11, 1, 21, 49, 57));
    CHECK(parse_clip_meta("- Your Note at location 566 | Added on Thursday, October 1, 2026 12:15:31 PM", &kind, &loc, &t) == 0);
    CHECK(kind == CLIP_NOTE && loc == 566 && t == civil_seconds(2026, 10, 1, 12, 15, 31));
    CHECK(parse_clip_meta("- Your Note at location 566 | Added on Thursday, October 1, 2026 12:15:31 AM", &kind, &loc, &t) == 0);
    CHECK(t == civil_seconds(2026, 10, 1, 0, 15, 31));
    /* Firmware 5.18 (Kindle 11th gen) writes "Location" with a capital. */
    CHECK(parse_clip_meta("- Your Highlight on page 311 | Location 4769-4769 | Added on Saturday, 14 November 2026 18:49:15", &kind, &loc, &t) == 0);
    CHECK(kind == CLIP_HIGHLIGHT && loc == 4769 && t == civil_seconds(2026, 11, 14, 18, 49, 15));
    CHECK(parse_clip_meta("- Your Bookmark at location 5 | Added on Thursday, 1 October 2026 12:15:31", &kind, &loc, &t) != 0);
    CHECK(parse_clip_meta("- Your Highlight at location 5 | Added on Thursday, 1 Smarch 2026 12:15:31", &kind, &loc, &t) != 0);
    CHECK(civil_seconds(1970, 1, 1, 0, 0, 0) == 0 && civil_seconds(2026, 11, 1, 10, 49, 57) == 1793530197LL);
}

static void test_krds(void) {
    struct buf in = {0}, out = {0}, two = {0}, one = {0};
    struct clip_target hl = {CLIP_HIGHLIGHT, 4285, civil_seconds(2026, 11, 1, 21, 49, 57)};
    struct clip_target hl2 = {CLIP_HIGHLIGHT, 165, civil_seconds(2026, 10, 29, 0, 18, 26)};
    int removed;
    const char *why;

    struct buf only2 = {0};

    make_krds(&two, 1, 1);
    make_krds(&one, 1, 0);
    make_krds(&only2, 0, 1);

    /* one of two highlights: the other, the note and the bookmark stay; count lowered */
    CHECK(krds_remove(two.data, two.len, &hl, 1, &out, &removed, &why) == 0 && removed == 1);
    CHECK(out.len == only2.len && memcmp(out.data, only2.data, only2.len) == 0);
    buf_free(&out);
    CHECK(krds_remove(two.data, two.len, &hl2, 1, &out, &removed, &why) == 0 && removed == 1);
    CHECK(out.len == one.len && memcmp(out.data, one.data, one.len) == 0);
    buf_free(&out);

    /* the last highlight: its whole list goes and the category count drops to 2 */
    CHECK(krds_remove(one.data, one.len, &hl, 1, &out, &removed, &why) == 0 && removed == 1);
    CHECK(count_of(&out, "annotation.personal.highlight") == 0 && count_of(&out, "annotation.personal.note") == 1 &&
          count_of(&out, "annotation.personal.bookmark") == 1 && count_of(&out, "saved.avl.interval.tree") == 2);
    buf_free(&out);

    /* a time-zone offset is fine; one second out is not */
    hl.added += 3600;
    CHECK(krds_remove(two.data, two.len, &hl, 1, &out, &removed, &why) == 0 && removed == 1);
    buf_free(&out);
    hl.added -= 3599;
    CHECK(krds_remove(two.data, two.len, &hl, 1, &out, &removed, &why) != 0 && out.len == 0);
    buf_free(&out);
    hl.added -= 1;

    /* an older clipping version carrying the record's time still matches */
    struct clip_target versions[2] = {{CLIP_HIGHLIGHT, 4285, civil_seconds(2026, 11, 2, 9, 0, 1)}, hl};
    CHECK(krds_remove(two.data, two.len, versions, 2, &out, &removed, &why) == 0 && removed == 1);
    buf_free(&out);

    /* only in My Clippings: the file is left as it was */
    struct clip_target elsewhere = {CLIP_HIGHLIGHT, 2611, civil_seconds(2026, 10, 30, 21, 57, 40)};
    CHECK(krds_remove(two.data, two.len, &elsewhere, 1, &out, &removed, &why) == 0 && removed == 0);
    CHECK(out.len == two.len && memcmp(out.data, two.data, two.len) == 0);
    buf_free(&out);

    /* highlight with its note; then everything (annotations become "int 0") */
    struct clip_target both[2] = {hl, {CLIP_NOTE, 4287, civil_seconds(2026, 11, 1, 21, 51, 40)}};
    CHECK(krds_remove(one.data, one.len, both, 2, &out, &removed, &why) == 0 && removed == 2);
    CHECK(count_of(&out, "annotation.personal.") == 1);
    buf_free(&out);

    /* damaged files are refused */
    buf_append(&in, two.data, two.len - 1);
    CHECK(krds_remove(in.data, in.len, &hl, 1, &out, &removed, &why) != 0);
    buf_free(&out);
    in.data[9] = 0x42;  /* unknown token type */
    CHECK(krds_remove(in.data, in.len, &hl, 1, &out, &removed, &why) != 0);
    buf_free(&out);
    buf_free(&in);

    buf_free(&two);
    buf_free(&one);
    buf_free(&only2);
}

static void test_clippings(void) {
    const char *file =
        "\xef\xbb\xbfOpen (Andre Agassi)\r\n- Your Highlight on page 11 | location 165-165 | Added on Thursday, 29 October 2026 00:18:26\r\n\r\nfirst\r\n==========\r\n"
        "\xef\xbb\xbfOpen (Andre Agassi)\r\n- Your Highlight on page 11 | location 165-166 | Added on Thursday, 29 October 2026 00:19:00\r\n\r\nfirst, longer\r\n==========\r\n"
        "\xef\xbb\xbfWalden (Thoreau)\r\n- Your Highlight on page 11 | location 165-165 | Added on Thursday, 29 October 2026 00:18:26\r\n\r\nother book\r\n==========\r\n";
    const char *keep = "\xef\xbb\xbfWalden (Thoreau)\r\n- Your Highlight on page 11 | location 165-165 | Added on Thursday, 29 October 2026 00:18:26\r\n\r\nother book\r\n==========\r\n";
    const char *heads[2] = {"\xef\xbb\xbfOpen (Andre Agassi)", "Open (Andre Agassi)"};
    const char *metas[2] = {"- Your Highlight on page 11 | location 165-165 | Added on Thursday, 29 October 2026 00:18:26",
                            "- Your Highlight on page 11 | location 165-166 | Added on Thursday, 29 October 2026 00:19:00"};
    struct buf out = {0};
    int removed;

    CHECK(clippings_remove(file, strlen(file), heads, metas, 2, &out, &removed) == 0 && removed == 2);
    CHECK(out.len == strlen(keep) && memcmp(out.data, keep, out.len) == 0);
    buf_free(&out);

    /* a pair that isn't there: the file changed, so nothing is removed */
    const char *gone[1] = {"- Your Highlight on page 12 | location 170-171 | Added on Thursday, 29 October 2026 00:18:26"};
    CHECK(clippings_remove(file, strlen(file), heads, gone, 1, &out, &removed) != 0 && removed == 0 && out.len == 0);
    buf_free(&out);
}

int main(void) {
    test_apnx();
    test_url();
    test_paths();
    test_base64();
    test_json();
    test_files();
    test_clip_meta();
    test_krds();
    test_clippings();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    puts("core: all tests passed");
    return 0;
}
