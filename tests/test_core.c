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

int main(void) {
    test_url();
    test_paths();
    test_base64();
    test_json();
    test_files();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    puts("core: all tests passed");
    return 0;
}
