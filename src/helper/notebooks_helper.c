/*
 * notebooks_helper: the only part of Notebooks that touches the file system.
 *
 * The app page (running in the Kindle's mesquite browser) can't write files,
 * so it sends requests here over LIPC by setting string properties:
 *
 *   save     "<id>\n<notebook>/<note>.txt\n<base64 text>"
 *   remove   "<id>\n<notebook>/<note>.txt"
 *   refresh  "<id>"
 *   open     "<id>\n/mnt/us/documents/<book file>"
 *   state    "<id>\n<JSON>" where you are in the app (written to state.js)
 *   ping     anything (keeps the helper alive while the app is open)
 *
 * After every request the helper rewrites /tmp/notebooks/data.js with the
 * library, the Kindle's clippings, every note and an acknowledgement for the
 * request id. The page reloads that file and only reports "Saved" when the
 * note text in it matches what was typed.
 *
 * Nothing here runs a shell with page-supplied text: note paths are checked
 * by valid_note_path() and book paths must be existing files in documents/.
 *
 * The helper also closes the app once you leave it. A Paperwhite 2 has
 * 256 MB of RAM, and a mesquite window left loaded in the background made
 * other apps swap until the Kindle crashed. The page saves where you were
 * (state.js) so reopening still lands in the same place.
 */

#include "nb_core.h"

#include <dirent.h>
#include <errno.h>
#include <lipc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE "com.dqstack.notebooks.helper"
#define NOTES_ROOT "/mnt/us/notebooks"
#define DATA_DIR "/tmp/notebooks"
#define DATA_FILE DATA_DIR "/data.js"
#define PAGES_FILE DATA_DIR "/pages.js"
#define STATE_FILE DATA_DIR "/state.js"
#define APP_ID "com.dqstack.notebooks"
#define LEAVE_GRACE_SECONDS 3
#define START_TIMEOUT_SECONDS 90
#define CLIPPINGS "/mnt/us/documents/My Clippings.txt"
#define DOCUMENTS "/mnt/us/documents/"
#define IDLE_EXIT_SECONDS (15 * 60)
#define MAX_ACKS 32

/* Columns: location, title, author, percent, lastAccess, cdeKey, cdeType */
#define LIBRARY_CMD \
    "sqlite3 -separator '\x1f' /var/local/cc.db " \
    "\"SELECT p_location, p_titles_0_nominal, p_credits_0_name_collation, " \
    "p_percentFinished, p_lastAccess, p_cdeKey, p_cdeType FROM Entries " \
    "WHERE p_type='Entry:Item' AND p_location LIKE '" DOCUMENTS "%';\" 2>/dev/null"

static volatile sig_atomic_t keep_running = 1;
static time_t last_activity;
static FILE *log_file;

static char ack_ids[MAX_ACKS][48];
static char ack_results[MAX_ACKS][96];
static int ack_next;


static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_msg(const char *fmt, ...) {
    if (!log_file) {
        return;
    }

    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_file, "%s ", stamp);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);

    fputc('\n', log_file);
    fflush(log_file);
}


static void on_signal(int sig) {
    (void)sig;
    keep_running = 0;
}


static void remember_ack(const char *id, const char *result) {
    if (!id[0]) {
        return;
    }

    snprintf(ack_ids[ack_next], sizeof ack_ids[0], "%s", id);
    snprintf(ack_results[ack_next], sizeof ack_results[0], "%s", result);
    ack_next = (ack_next + 1) % MAX_ACKS;
}


static void run_command(const char *cmd, struct buf *out) {
    FILE *p = popen(cmd, "r");

    if (p) {
        char chunk[8192];
        size_t n;

        while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) {
            buf_append(out, chunk, n);
        }

        pclose(p);
    }
}

static int append_command_output(struct buf *b, const char *cmd) {
    struct buf out = {0};
    run_command(cmd, &out);

    int rc = json_append_string(b, out.data ? out.data : "", out.len);
    buf_free(&out);
    return rc;
}


/*
 * Page tables for every book that has an APNX file, either next to the book
 * (calibre over USB) or inside its .sdr folder. Written to pages.js only at
 * start-up and on refresh: they don't change when notes are saved.
 */
static void write_pages(void) {
    struct buf lib = {0};
    struct buf b = {0};
    int first = 1;

    run_command(LIBRARY_CMD, &lib);
    int rc = buf_puts(&b, "window.NB_PAGES={");

    for (char *line = lib.data; line && *line; ) {
        char *end = strchr(line, '\n');
        char *sep = strchr(line, '\x1f');

        if (end) {
            *end = '\0';
        }

        if (sep && (!end || sep < end)) {
            *sep = '\0';

            const char *book = line;
            const char *dot = strrchr(book, '.');
            const char *name = strrchr(book, '/');
            char cand[2][2048];

            if (dot && name && dot > name) {
                int base = (int)(dot - book);
                int stem = (int)(dot - name - 1);

                snprintf(cand[0], sizeof cand[0], "%.*s.apnx", base, book);
                snprintf(cand[1], sizeof cand[1], "%.*s.sdr/%.*s.apnx", base, book, stem, name + 1);

                for (int i = 0; i < 2; i++) {
                    struct buf apnx = {0};

                    if (read_file(cand[i], 4 * 1024 * 1024, &apnx) == 0 && apnx.len) {
                        size_t mark = b.len;

                        rc |= buf_puts(&b, first ? "" : ",");
                        rc |= json_append_string(&b, book, strlen(book));
                        rc |= buf_puts(&b, ":");

                        if (append_apnx_json(&b, apnx.data, apnx.len) == 0) {
                            first = 0;
                            buf_free(&apnx);
                            break;
                        }

                        b.len = mark; /* not a page table we understand */
                        b.data[mark] = '\0';
                    }

                    buf_free(&apnx);
                }
            }
        }

        line = end ? end + 1 : NULL;
    }

    rc |= buf_puts(&b, "};\n");

    if (rc != 0 || write_file_atomic(DATA_DIR, PAGES_FILE, b.data, b.len) != 0) {
        log_msg("ERROR: could not write %s", PAGES_FILE);
    }

    buf_free(&lib);
    buf_free(&b);
}


/* Rebuild data.js; written to a temp file and renamed so the page never reads half of it. */
static void write_data(void) {
    struct buf b = {0};
    struct buf clippings = {0};
    char num[32];

    snprintf(num, sizeof num, "%lld", (long long)time(NULL));

    int rc = buf_puts(&b, "window.NB_DATA={\"generated\":");
    rc |= buf_puts(&b, num);
    rc |= buf_puts(&b, ",\"acks\":{");

    int first = 1;

    for (int i = 0; i < MAX_ACKS; i++) {
        if (!ack_ids[i][0]) {
            continue;
        }

        rc |= buf_puts(&b, first ? "" : ",");
        rc |= json_append_string(&b, ack_ids[i], strlen(ack_ids[i]));
        rc |= buf_puts(&b, ":");
        rc |= json_append_string(&b, ack_results[i], strlen(ack_results[i]));
        first = 0;
    }

    rc |= buf_puts(&b, "},\"library\":");
    rc |= append_command_output(&b, LIBRARY_CMD);

    read_file(CLIPPINGS, 16 * 1024 * 1024, &clippings);
    rc |= buf_puts(&b, ",\"clippings\":");
    rc |= json_append_string(&b, clippings.data ? clippings.data : "", clippings.len);
    buf_free(&clippings);

    rc |= buf_puts(&b, ",\"notes\":");
    rc |= append_notes_json(&b, NOTES_ROOT);
    rc |= buf_puts(&b, "};\n");

    if (rc != 0 || write_file_atomic(DATA_DIR, DATA_FILE, b.data, b.len) != 0) {
        log_msg("ERROR: could not write %s", DATA_FILE);
    }

    buf_free(&b);
}


/*
 * Split "<id>\n<rest>" in place. Returns the rest (or "" when missing) and
 * leaves the id in *id. Ids are short tokens made by the page.
 */
static char *split_id(char *msg, char **id) {
    char *nl = strchr(msg, '\n');

    *id = msg;

    if (!nl) {
        return msg + strlen(msg);
    }

    *nl = '\0';

    if (strlen(msg) >= sizeof ack_ids[0]) {
        msg[sizeof ack_ids[0] - 1] = '\0';
    }

    return nl + 1;
}


static const char *do_save(char *args) {
    char *nl = strchr(args, '\n');

    if (!nl) {
        return "error: bad request";
    }

    *nl = '\0';
    const char *rel = args;
    const char *b64 = nl + 1;

    if (!valid_note_path(rel)) {
        return "error: bad note path";
    }

    struct buf text = {0};

    if (base64_decode(b64, strlen(b64), &text) != 0 || text.len > NB_MAX_NOTE_BYTES) {
        buf_free(&text);
        return "error: bad note text";
    }

    char dir[1024], path[1024];
    const char *slash = strchr(rel, '/');

    snprintf(dir, sizeof dir, "%s/%.*s", NOTES_ROOT, (int)(slash - rel), rel);
    snprintf(path, sizeof path, "%s/%s", NOTES_ROOT, rel);

    mkdir(NOTES_ROOT, 0755);

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        buf_free(&text);
        return "error: could not create notebook";
    }

    int rc = write_file_atomic(dir, path, text.data ? text.data : "", text.len);
    log_msg("save %s (%zu bytes): %s", rel, text.len, rc == 0 ? "ok" : strerror(errno));
    buf_free(&text);

    return rc == 0 ? "ok" : "error: could not write note";
}


static const char *do_remove(const char *rel) {
    if (!valid_note_path(rel)) {
        return "error: bad note path";
    }

    char dir[1024], path[1024];
    const char *slash = strchr(rel, '/');

    snprintf(dir, sizeof dir, "%s/%.*s", NOTES_ROOT, (int)(slash - rel), rel);
    snprintf(path, sizeof path, "%s/%s", NOTES_ROOT, rel);

    if (unlink(path) != 0 && errno != ENOENT) {
        return "error: could not delete note";
    }

    rmdir(dir); /* only succeeds once the notebook is empty */
    log_msg("remove %s", rel);
    return "ok";
}


static const char *do_open(const char *book) {
    struct stat st;

    if (strncmp(book, DOCUMENTS, strlen(DOCUMENTS)) != 0 || strstr(book, "/../") ||
        stat(book, &st) != 0 || !S_ISREG(st.st_mode)) {
        return "error: book not found";
    }

    char encoded[3200];
    char url[3300];

    if (url_encode_path(book, encoded, sizeof encoded) != 0) {
        return "error: book path too long";
    }

    snprintf(url, sizeof url, "app://com.lab126.booklet.reader%s", encoded);

    pid_t pid = fork();

    if (pid == 0) {
        execlp("lipc-set-prop", "lipc-set-prop", "com.lab126.appmgrd", "start", url, (char *)NULL);
        _exit(127);
    }

    int status = 0;

    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_msg("open %s: failed", book);
        return "error: could not open book";
    }

    log_msg("open %s", book);
    return "ok";
}


static const char *do_state(const char *json) {
    struct buf b = {0};
    size_t n = strlen(json);

    if (n == 0 || n > 8192 || json[0] != '{') {
        return "error: bad state";
    }

    int rc = buf_puts(&b, "window.NB_STATE=");
    rc |= buf_append(&b, json, n);
    rc |= buf_puts(&b, ";\n");

    if (rc != 0 || write_file_atomic(DATA_DIR, STATE_FILE, b.data, b.len) != 0) {
        buf_free(&b);
        return "error: could not write state";
    }

    buf_free(&b);
    return "ok";
}


/* Kill the mesquite process showing Notebooks ("mesquite -l com.dqstack.notebooks ..."). */
static void close_app_window(void) {
    static const char want[] = "-l\0" APP_ID;
    DIR *proc = opendir("/proc");
    struct dirent *e;

    if (!proc) {
        return;
    }

    while ((e = readdir(proc)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') {
            continue;
        }

        char path[64];
        struct buf cmd = {0};

        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);

        /* cmdline is NUL-separated: look for mesquite with argv "-l" then our id */
        if (read_file(path, 4096, &cmd) == 0 && cmd.len && strstr(cmd.data, "mesquite")) {
            for (size_t i = 0; i + sizeof want - 1 <= cmd.len; i++) {
                if (memcmp(cmd.data + i, want, sizeof want - 1) == 0) {
                    log_msg("closing app window (pid %s)", e->d_name);
                    kill((pid_t)atoi(e->d_name), SIGTERM);
                    break;
                }
            }
        }

        buf_free(&cmd);
    }

    closedir(proc);
}


/* Is Notebooks the app in front? -1 if appmgrd couldn't be asked. */
static int app_in_front(LIPC *query) {
    char *active = NULL;

    if (!query || LipcGetStringProperty(query, "com.lab126.appmgrd", "activeApp", &active) != LIPC_OK || !active) {
        return -1;
    }

    int front = strcmp(active, APP_ID) == 0;
    LipcFreeString(active);
    return front;
}


static LIPCcode on_request(LIPC *lipc, const char *property, void *value, void *data) {
    (void)lipc;
    (void)data;

    char *msg = strdup(LIPC_SETTER_VTOS(value) ? LIPC_SETTER_VTOS(value) : "");

    if (!msg) {
        return LIPC_ERROR_INTERNAL;
    }

    char *id;
    char *args = split_id(msg, &id);
    const char *result = "ok";

    last_activity = time(NULL);

    if (strcmp(property, "save") == 0) {
        result = do_save(args);
    } else if (strcmp(property, "remove") == 0) {
        result = do_remove(args);
    } else if (strcmp(property, "open") == 0) {
        result = do_open(args);
    } else if (strcmp(property, "state") == 0) {
        do_state(args);
        free(msg);
        return LIPC_OK;     /* no ack needed, and no data.js rebuild */
    }

    if (strcmp(property, "refresh") == 0) {
        write_pages();
    }

    if (strcmp(property, "ping") != 0) {
        remember_ack(id, result);
        write_data();
    }

    free(msg);
    return LIPC_OK;
}


int main(int argc, char **argv) {
    const char *log_path = argc > 1 ? argv[1] : DATA_DIR "/helper.log";

    mkdir(DATA_DIR, 0755);
    log_file = fopen(log_path, "a");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    LIPCcode code;
    LIPC *lipc = LipcOpenEx(SERVICE, &code);

    if (!lipc || code != LIPC_OK) {
        log_msg("ERROR: LipcOpenEx(%s) failed: %d", SERVICE, (int)code);
        return 1;
    }

    const char *props[] = {"save", "remove", "refresh", "open", "state", "ping"};

    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (LipcRegisterStringProperty(lipc, props[i], NULL, on_request, NULL) != LIPC_OK) {
            log_msg("ERROR: could not register %s", props[i]);
            LipcClose(lipc);
            return 1;
        }
    }

    last_activity = time(NULL);
    write_pages();
    write_data();
    log_msg("ready (pid %d)", (int)getpid());

    /*
     * LIPC callbacks run on the library's own thread. This loop watches,
     * on a separate connection, which app is in front: once Notebooks has
     * been shown and then left for LEAVE_GRACE_SECONDS, close its window
     * and exit, so nothing heavy stays loaded.
     */
    LIPC *query = LipcOpenNoName();
    time_t started = time(NULL);
    time_t left_at = 0;
    int seen = 0;
    const char *why = "signal";

    while (keep_running) {
        sleep(2);

        int front = app_in_front(query);

        if (front == 1) {
            seen = 1;
            left_at = 0;
        } else if (front == 0 && seen) {
            if (!left_at) {
                left_at = time(NULL);
            } else if (time(NULL) - left_at >= LEAVE_GRACE_SECONDS) {
                close_app_window();
                why = "app left";
                break;
            }
        } else if (front == 0 && time(NULL) - started > START_TIMEOUT_SECONDS) {
            why = "app never opened";
            break;
        }

        if (time(NULL) - last_activity >= IDLE_EXIT_SECONDS) {
            why = "idle";
            break;
        }
    }

    log_msg("exiting (%s)", why);

    if (query) {
        LipcClose(query);
    }

    LipcClose(lipc);
    return 0;
}
