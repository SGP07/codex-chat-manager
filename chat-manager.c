#define _GNU_SOURCE
#include <ncurses.h>
#include <json-c/json.h>
#include <locale.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <sqlite3.h>
#include <fts.h>
#include <zstd.h>
#include <wchar.h>

static FILE *in, *out;
static pid_t child;
static int seq;
static int rpc_error_code;
static char error[512];
typedef struct {
    char *id, *title, *cwd, *project;
    time_t updated;
    int selected;
} Chat;
static Chat *chats;
static size_t count;
static int archived, color_enabled;
static json_object *projects;
static char status[512];
static char codex_home[PATH_MAX];
static void associate_projects(void);
enum { HEADER = 1, INFO, SELECTED, VISUAL };
static void colors(void) {
    if (!has_colors() || start_color() == ERR)
        return;
    int bg = use_default_colors() == OK ? -1 : COLOR_BLACK;
    init_pair(HEADER, COLOR_CYAN, bg);
    init_pair(INFO, COLOR_BLUE, bg);
    init_pair(SELECTED, COLOR_GREEN, bg);
    init_pair(VISUAL, COLOR_YELLOW, bg);
    color_enabled = 1;
}

static int style(int pair) {
    return color_enabled ? COLOR_PAIR(pair) | A_BOLD : A_BOLD;
}

static json_object *field(json_object *o, const char *k) {
    json_object *v = NULL;
    if (o)
        json_object_object_get_ex(o, k, &v);
    return v;
}

static const char *str(json_object *o, const char *k) {
    json_object *v = field(o, k);
    return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}

static json_object *rpc(const char *method, json_object *params) {
    rpc_error_code = 0;
    json_object *q = json_object_new_object();
    int id = ++seq;
    json_object_object_add(q, "id", json_object_new_int(id));
    json_object_object_add(q, "method", json_object_new_string(method));
    json_object_object_add(q, "params", params);
    if (fprintf(out, "%s\n", json_object_to_json_string_ext(q, JSON_C_TO_STRING_PLAIN)) < 0 ||
        fflush(out)) {
        snprintf(error, sizeof error, "Connection to Codex failed");
        json_object_put(q);
        return NULL;
    }
    json_object_put(q);
    char *line = NULL;
    size_t cap = 0;
    json_object *answer = NULL;
    while (getline(&line, &cap, in) >= 0) {
        json_object *o = json_tokener_parse(line), *v = field(o, "id");
        if (v && json_object_get_int(v) == id) {
            json_object *e = field(o, "error");
            if (e) {
                rpc_error_code = json_object_get_int(field(e, "code"));
                snprintf(error, sizeof error, "%s", str(e, "message"));
            } else {
                answer = field(o, "result");
                if (answer)
                    json_object_get(answer);
                else
                    snprintf(error, sizeof error, "Invalid response from Codex");
            }
            json_object_put(o);
            break;
        }
        if (o)
            json_object_put(o);
    }
    free(line);
    if (!answer && !*error)
        snprintf(error, sizeof error, "Codex app-server closed the connection");
    return answer;
}

static int start_server(void) {
    int a[2], b[2];
    if (pipe(a) || pipe(b))
        return 0;
    child = fork();
    if (child < 0)
        return 0;
    if (!child) {
        dup2(a[0], STDIN_FILENO);
        dup2(b[1], STDOUT_FILENO);
        close(a[0]);
        close(a[1]);
        close(b[0]);
        close(b[1]);
        execlp("codex", "codex", "app-server", (char *)NULL);
        _exit(127);
    }
    close(a[0]);
    close(b[1]);
    out = fdopen(a[1], "w");
    in = fdopen(b[0], "r");
    if (!out || !in)
        return 0;
    signal(SIGPIPE, SIG_IGN);
    json_object *p = json_object_new_object(), *info = json_object_new_object(),
                *caps = json_object_new_object();
    json_object_object_add(info, "name", json_object_new_string("chat_manager"));
    json_object_object_add(info, "title", json_object_new_string("Chat Manager"));
    json_object_object_add(info, "version", json_object_new_string("1.0"));
    json_object_object_add(p, "clientInfo", info);
    json_object_object_add(caps, "experimentalApi", json_object_new_boolean(1));
    json_object_object_add(p, "capabilities", caps);
    json_object *r = rpc("initialize", p);
    if (!r)
        return 0;
    snprintf(codex_home, sizeof codex_home, "%s", str(r, "codexHome"));
    json_object_put(r);
    if (!*codex_home) {
        const char *h = getenv("CODEX_HOME");
        if (h && *h)
            snprintf(codex_home, sizeof codex_home, "%s", h);
        else
            snprintf(codex_home, sizeof codex_home, "%s/.codex", getenv("HOME"));
    }
    fprintf(out, "{\"method\":\"initialized\",\"params\":{}}\n");
    fflush(out);
    return 1;
}

static void clear_chats(void) {
    for (size_t i = 0; i < count; i++) {
        free(chats[i].id);
        free(chats[i].title);
        free(chats[i].cwd);
        free(chats[i].project);
    }
    free(chats);
    chats = NULL;
    count = 0;
}

static int load(void) {
    clear_chats();
    char *cursor = NULL;
    do {
        json_object *p = json_object_new_object();
        json_object_object_add(p, "limit", json_object_new_int(100));
        json_object_object_add(p, "archived", json_object_new_boolean(archived));
        json_object_object_add(p, "sortKey", json_object_new_string("updated_at"));
        json_object_object_add(p, "useStateDbOnly", json_object_new_boolean(1));
        if (cursor)
            json_object_object_add(p, "cursor", json_object_new_string(cursor));
        free(cursor);
        cursor = NULL;
        json_object *r = rpc("thread/list", p);
        if (!r)
            return 0;
        json_object *data = field(r, "data");
        if (!data || !json_object_is_type(data, json_type_array)) {
            json_object_put(r);
            snprintf(error, sizeof error, "Invalid thread list");
            return 0;
        }
        for (size_t i = 0; i < json_object_array_length(data); i++) {
            json_object *t = json_object_array_get_idx(data, i);
            const char *id = str(t, "id"), *title = str(t, "name");
            if (!*id)
                continue;
            if (!*title)
                title = str(t, "preview");
            Chat *n = realloc(chats, (count + 1) * sizeof(*chats));
            if (!n) {
                json_object_put(r);
                snprintf(error, sizeof error, "Out of memory");
                return 0;
            }
            chats = n;
            Chat *c = &chats[count++];
            *c = (Chat){strdup(id),
                        strdup(*title ? title : "(untitled)"),
                        strdup(str(t, "cwd")),
                        strdup(str(t, "projectId")),
                        (time_t)json_object_get_int64(field(t, "updatedAt")),
                        0};
        }
        const char *next = str(r, "nextCursor");
        if (*next)
            cursor = strdup(next);
        json_object_put(r);
    } while (cursor);
    associate_projects();
    return 1;
}

static int match(Chat *c, const char *filter) {
    return !*filter || strcasestr(c->title, filter) || strcasestr(c->cwd, filter);
}

static void prompt(const char *label, char *buf, int size) {
    echo();
    curs_set(1);
    int result;
    do {
        int h, w;
        getmaxyx(stdscr, h, w);
        move(h - 1, 0);
        clrtoeol();
        mvaddnstr(h - 1, 0, label, w - 1);
        result = getnstr(buf, size - 1);
    } while (result == KEY_RESIZE);
    noecho();
    curs_set(0);
}

static void message(void) {
    int h, w;
    getmaxyx(stdscr, h, w);
    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 0, *error ? error : "Done", w - 1);
    refresh();
    getch();
    error[0] = 0;
}

static int selected_count(void) {
    int n = 0;
    for (size_t i = 0; i < count; i++)
        n += chats[i].selected;
    return n;
}

static pid_t desktop_pid(void) {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[PATH_MAX], link[512];
    int len;
    if (config && config[0] == '/')
        len = snprintf(path, sizeof path, "%s/Codex/SingletonLock", config);
    else if (home && home[0] == '/')
        len = snprintf(path, sizeof path, "%s/.config/Codex/SingletonLock", home);
    else
        return -1;
    if (len < 0 || len >= (int)sizeof path)
        return -1;
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n < 0)
        return errno == ENOENT ? 0 : -1;
    if (n == (ssize_t)sizeof link - 1)
        return -1;
    link[n] = 0;
    char *dash = strrchr(link, '-'), *end = NULL;
    if (!dash)
        return -1;
    errno = 0;
    long pid = strtol(dash + 1, &end, 10);
    if (errno || *end || pid <= 1 || pid > INT_MAX)
        return -1;
    if (kill((pid_t)pid, 0) < 0 && errno == ESRCH)
        return 0;
    return (pid_t)pid;
}

static int desktop_running(void) {
    return desktop_pid() != 0;
}

static int is_id(json_object *ids, const char *s) {
    for (size_t i = 0; i < json_object_array_length(ids); i++)
        if (!strcmp(s, json_object_get_string(json_object_array_get_idx(ids, i))))
            return 1;
    return 0;
}

static void remove_ids(json_object *v, json_object *ids) {
    if (!v)
        return;
    if (json_object_is_type(v, json_type_array)) {
        for (size_t i = json_object_array_length(v); i > 0; i--) {
            json_object *item = json_object_array_get_idx(v, i - 1);
            if (json_object_is_type(item, json_type_string) &&
                is_id(ids, json_object_get_string(item)))
                json_object_array_del_idx(v, i - 1, 1);
            else if (item && (json_object_is_type(item, json_type_array) ||
                              json_object_is_type(item, json_type_object)))
                remove_ids(item, ids);
        }
    } else if (json_object_is_type(v, json_type_object)) {
        json_object_object_foreach(v, key, val) {
            if (is_id(ids, key))
                json_object_object_del(v, key);
            else if (val && (json_object_is_type(val, json_type_array) ||
                             json_object_is_type(val, json_type_object)))
                remove_ids(val, ids);
        }
    }
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

static int cleanup_state(json_object *ids) {
    char path[PATH_MAX], backup[PATH_MAX], tmp[PATH_MAX] = "";
    struct stat before, now;
    if (snprintf(path, sizeof path, "%s/.codex-global-state.json", codex_home) >=
        (int)sizeof path) {
        snprintf(error, sizeof error, "Codex home path too long");
        return 0;
    }
    if (desktop_running()) {
        snprintf(error, sizeof error, "Desktop is running; metadata cleanup postponed");
        return 0;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        snprintf(error, sizeof error, "Cannot open desktop state: %s", strerror(errno));
        return 0;
    }
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        before.st_size > 64 * 1024 * 1024) {
        close(fd);
        snprintf(error, sizeof error, "Invalid desktop state file");
        return 0;
    }
    size_t len = (size_t)before.st_size, got = 0;
    char *raw = malloc(len + 1);
    if (!raw) {
        close(fd);
        snprintf(error, sizeof error, "Out of memory");
        return 0;
    }
    while (got < len) {
        ssize_t n = read(fd, raw + got, len - got);
        if (n <= 0) {
            free(raw);
            close(fd);
            snprintf(error, sizeof error, "Could not read desktop state");
            return 0;
        }
        got += (size_t)n;
    }
    close(fd);
    raw[len] = 0;
    json_tokener *tok = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tok, raw, (int)len);
    enum json_tokener_error je = json_tokener_get_error(tok);
    json_tokener_free(tok);
    if (je != json_tokener_success || !root || !json_object_is_type(root, json_type_object)) {
        if (root)
            json_object_put(root);
        free(raw);
        snprintf(error, sizeof error, "Desktop state is not a valid JSON object");
        return 0;
    }
    const char *fields[] = {"projectless-thread-ids",
                            "pinned-thread-ids",
                            "thread-project-assignments",
                            "thread-project-membership-host-ids",
                            "thread-writable-roots",
                            "thread-workspace-root-hints",
                            "thread-projectless-output-directories",
                            "sidebar-project-thread-orders",
                            "electron-thread-read-state-v1",
                            NULL};
    for (int i = 0; fields[i]; i++)
        remove_ids(field(root, fields[i]), ids);
    json_object *migration = field(root, "app-server-projects-migration-by-host");
    remove_ids(migration, ids);
    json_object *atoms = field(root, "electron-persisted-atom-state");
    const char *afields[] = {"thread-descriptions-v1", "prompt-history",
                             "heartbeat-thread-permissions-by-id", NULL};
    for (int i = 0; afields[i]; i++)
        remove_ids(field(atoms, afields[i]), ids);
    json_object *bindings = field(atoms, "client-thread-bindings-v1");
    if (bindings && json_object_is_type(bindings, json_type_object)) {
        json_object_object_foreach(bindings, k, v) {
            if (v && json_object_is_type(v, json_type_string) &&
                is_id(ids, json_object_get_string(v)))
                json_object_object_del(bindings, k);
        }
    }
    if (atoms && json_object_is_type(atoms, json_type_object)) {
        const char *prefixes[] = {"thread-reference-capability:", "thread-tab-routes-v1:",
                                  "thread-client-id-v1:local%3A", NULL};
        for (size_t i = 0; i < json_object_array_length(ids); i++) {
            const char *id = json_object_get_string(json_object_array_get_idx(ids, i));
            char key[256];
            for (int j = 0; prefixes[j]; j++) {
                snprintf(key, sizeof key, "%s%s", prefixes[j], id);
                json_object_object_del(atoms, key);
            }
        }
    }
    int ok = 0, backupfd = -1, tmpfd = -1;
    if (snprintf(backup, sizeof backup, "%s/.codex-global-state.chat-manager-backup.XXXXXX",
                 codex_home) >= (int)sizeof backup ||
        snprintf(tmp, sizeof tmp, "%s/.codex-global-state.chat-manager-tmp.XXXXXX", codex_home) >=
            (int)sizeof tmp) {
        snprintf(error, sizeof error, "Codex home path too long");
        goto done;
    }
    backupfd = mkstemp(backup);
    if (backupfd < 0 || fchmod(backupfd, 0600) || !write_all(backupfd, raw, len) ||
        fsync(backupfd)) {
        snprintf(error, sizeof error, "Could not back up desktop state: %s", strerror(errno));
        goto done;
    }
    close(backupfd);
    backupfd = -1;
    tmpfd = mkstemp(tmp);
    if (tmpfd < 0 || fchmod(tmpfd, before.st_mode & 0777)) {
        snprintf(error, sizeof error, "Could not create updated desktop state: %s",
                 strerror(errno));
        goto done;
    }
    const char *serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (!serialized || !write_all(tmpfd, serialized, strlen(serialized)) || fsync(tmpfd)) {
        snprintf(error, sizeof error, "Could not write desktop state: %s", strerror(errno));
        goto done;
    }
    close(tmpfd);
    tmpfd = -1;
    if (desktop_running() || stat(path, &now) || before.st_ino != now.st_ino ||
        before.st_size != now.st_size || before.st_mtim.tv_sec != now.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != now.st_mtim.tv_nsec) {
        snprintf(error, sizeof error,
                 "Desktop state changed or desktop started; cleanup postponed");
        goto done;
    }
    if (rename(tmp, path)) {
        snprintf(error, sizeof error, "Could not replace desktop state: %s", strerror(errno));
        goto done;
    }
    ok = 1;
done:
    if (backupfd >= 0)
        close(backupfd);
    if (tmpfd >= 0)
        close(tmpfd);
    if (!ok && *tmp)
        unlink(tmp);
    json_object_put(root);
    free(raw);
    return ok;
}

static int cleanup_catalog(json_object *ids) {
    char path[PATH_MAX], backup[PATH_MAX];
    struct stat st;
    if (snprintf(path, sizeof path, "%s/sqlite/codex.db", codex_home) >= (int)sizeof path ||
        snprintf(backup, sizeof backup, "%s/sqlite/codex.sidebar-backup.XXXXXX", codex_home) >=
            (int)sizeof backup) {
        snprintf(error, sizeof error, "Catalog path too long");
        return 0;
    }
    if (stat(path, &st)) {
        if (errno == ENOENT)
            return 1;
        snprintf(error, sizeof error, "Cannot inspect desktop catalog: %s", strerror(errno));
        return 0;
    }
    if (desktop_running()) {
        snprintf(error, sizeof error, "Desktop is running; catalog cleanup postponed");
        return 0;
    }
    sqlite3 *db = NULL, *dest = NULL;
    sqlite3_stmt *mark = NULL, *del = NULL;
    sqlite3_backup *copy = NULL;
    int fd = -1, ok = 0, changed = 0;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        snprintf(error, sizeof error, "Cannot open desktop catalog");
        goto done;
    }
    sqlite3_busy_timeout(db, 5000);
    fd = mkstemp(backup);
    if (fd < 0) {
        snprintf(error, sizeof error, "Cannot create catalog backup: %s", strerror(errno));
        goto done;
    }
    close(fd);
    fd = -1;
    if (sqlite3_open_v2(backup, &dest, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
        (copy = sqlite3_backup_init(dest, "main", db, "main")) == NULL) {
        snprintf(error, sizeof error, "Cannot back up desktop catalog");
        goto done;
    }
    int rc = sqlite3_backup_step(copy, -1);
    sqlite3_backup_finish(copy);
    copy = NULL;
    if (rc != SQLITE_DONE) {
        snprintf(error, sizeof error, "Cannot back up desktop catalog");
        goto done;
    }
    sqlite3_close(dest);
    dest = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(error, sizeof error, "Cannot lock desktop catalog: %.250s", sqlite3_errmsg(db));
        goto done;
    }
    if (desktop_running()) {
        snprintf(error, sizeof error, "Desktop started; catalog cleanup postponed");
        goto rollback;
    }
    const char *mark_sql =
        "INSERT INTO local_thread_catalog_scan_entries(host_id,thread_id,removed) SELECT "
        "'local',?1,1 WHERE EXISTS(SELECT 1 FROM local_thread_catalog_scan_checkpoints WHERE "
        "host_id='local') ON CONFLICT(host_id,thread_id) DO UPDATE SET removed=1";
    if (sqlite3_prepare_v2(db, mark_sql, -1, &mark, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(
            db, "DELETE FROM local_thread_catalog WHERE host_id='local' AND thread_id=?1", -1, &del,
            NULL) != SQLITE_OK) {
        snprintf(error, sizeof error, "Unsupported desktop catalog schema: %.250s",
                 sqlite3_errmsg(db));
        goto rollback;
    }
    for (size_t i = 0; i < json_object_array_length(ids); i++) {
        const char *id = json_object_get_string(json_object_array_get_idx(ids, i));
        sqlite3_bind_text(mark, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(mark) != SQLITE_DONE) {
            snprintf(error, sizeof error, "Cannot update catalog scan state: %.250s",
                     sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_reset(mark);
        sqlite3_clear_bindings(mark);
        sqlite3_bind_text(del, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(del) != SQLITE_DONE) {
            snprintf(error, sizeof error, "Cannot remove catalog row: %.250s", sqlite3_errmsg(db));
            goto rollback;
        }
        changed += sqlite3_changes(db);
        sqlite3_reset(del);
        sqlite3_clear_bindings(del);
    }
    if (changed && sqlite3_exec(db,
                                "UPDATE local_thread_catalog_metadata SET "
                                "catalog_revision=catalog_revision+1 WHERE id=1",
                                NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(error, sizeof error, "Cannot update catalog revision: %.250s", sqlite3_errmsg(db));
        goto rollback;
    }
    if (desktop_running() || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(error, sizeof error, "Catalog commit failed or desktop started");
        goto rollback;
    }
    ok = 1;
    goto done;
rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
done:
    if (mark)
        sqlite3_finalize(mark);
    if (del)
        sqlite3_finalize(del);
    if (copy)
        sqlite3_backup_finish(copy);
    if (dest)
        sqlite3_close(dest);
    if (db)
        sqlite3_close(db);
    if (fd >= 0)
        close(fd);
    return ok;
}

static int pending_open(void) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/.chat-manager-pending-deletions", codex_home) >=
        (int)sizeof path) {
        snprintf(error, sizeof error, "Codex home path too long");
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        snprintf(error, sizeof error, "Cannot open cleanup journal: %s", strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        snprintf(error, sizeof error, "Another cleanup is running");
        close(fd);
        return -1;
    }
    return fd;
}

static json_object *pending_read(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0)
        return NULL;
    FILE *f = fdopen(dup(fd), "r");
    if (!f)
        return NULL;
    json_object *a = json_object_new_array();
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (n > 0)
            json_object_array_add(a, json_object_new_string(line));
    }
    free(line);
    if (ferror(f)) {
        fclose(f);
        json_object_put(a);
        return NULL;
    }
    fclose(f);
    return a;
}

static int pending_flush(int fd) {
    json_object *ids = pending_read(fd);
    if (!ids) {
        snprintf(error, sizeof error, "Could not read cleanup journal");
        return 0;
    }
    if (!json_object_array_length(ids)) {
        json_object_put(ids);
        return 1;
    }
    int ok = cleanup_state(ids) && cleanup_catalog(ids);
    json_object_put(ids);
    if (!ok)
        return 0;
    if (ftruncate(fd, 0) || fsync(fd)) {
        snprintf(error, sizeof error, "Could not clear cleanup journal");
        return 0;
    }
    return 1;
}

static json_object *compressed_rollout_header(FILE *file) {
    ZSTD_DStream *decoder = ZSTD_createDStream();
    json_tokener *parser = json_tokener_new();
    json_object *header = NULL;
    if (!decoder || !parser || ZSTD_isError(ZSTD_initDStream(decoder)))
        goto done;
    char encoded[4096], decoded[4096];
    size_t n;
    while ((n = fread(encoded, 1, sizeof encoded, file))) {
        ZSTD_inBuffer input = {encoded, n, 0};
        ZSTD_outBuffer output;
        do {
            output = (ZSTD_outBuffer){decoded, sizeof decoded, 0};
            if (ZSTD_isError(ZSTD_decompressStream(decoder, &output, &input)))
                goto done;
            if (output.pos) {
                const char *newline = memchr(decoded, '\n', output.pos);
                int len = newline ? (int)(newline - decoded) + 1 : (int)output.pos;
                header = json_tokener_parse_ex(parser, decoded, len);
                if (newline || json_tokener_get_error(parser) != json_tokener_continue)
                    goto done;
            }
        } while (input.pos < input.size || output.pos == sizeof decoded);
    }
done:
    if (parser)
        json_tokener_free(parser);
    ZSTD_freeDStream(decoder);
    return header;
}

static json_object *rollout_header(const char *path, int compressed) {
    FILE *file = fopen(path, "r");
    if (!file)
        return NULL;
    json_object *header;
    if (compressed) {
        header = compressed_rollout_header(file);
    } else {
        char *line = NULL;
        size_t cap = 0;
        header = getline(&line, &cap, file) >= 0 ? json_tokener_parse(line) : NULL;
        free(line);
    }
    fclose(file);
    return header;
}

static int valid_rollout_uuid(const char *id) {
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)id[i])) {
            return 0;
        }
    }
    return 1;
}

static int parse_rollout_name(const char *name, char rollout[37], int *compressed) {
    size_t len = strlen(name), zst_len = sizeof ".jsonl.zst" - 1;
    *compressed = len >= zst_len && !strcmp(name + len - zst_len, ".jsonl.zst");
    const char *suffix = *compressed ? ".jsonl.zst" : ".jsonl";
    size_t suffix_len = strlen(suffix);
    if (strncmp(name, "rollout-", 8) || len < 28 + 36 + suffix_len ||
        strcmp(name + len - suffix_len, suffix))
        return 0;
    struct tm timestamp = {0};
    if (strptime(name + 8, "%Y-%m-%dT%H-%M-%S-", &timestamp) != name + 28)
        return 0;
    const char *ids = name + 28;
    size_t id_len = len - suffix_len - 28;
    if (id_len == 73 && ids[36] == '_' && valid_rollout_uuid(ids))
        ids += 37;
    else if (id_len != 36)
        return 0;
    if (!valid_rollout_uuid(ids))
        return 0;
    for (int i = 0; i < 36; i++)
        rollout[i] = (char)tolower((unsigned char)ids[i]);
    rollout[36] = '\0';
    return 1;
}

static json_object *index_rollouts(void) {
    json_object *index = json_object_new_object();
    const char *dirs[] = {"sessions", "archived_sessions"};
    for (int a = 0; a < 2; a++) {
        char root[PATH_MAX];
        if (snprintf(root, sizeof root, "%s/%s", codex_home, dirs[a]) >= (int)sizeof root)
            continue;
        char *paths[] = {root, NULL};
        FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
        if (!tree)
            continue;
        for (FTSENT *entry; (entry = fts_read(tree));) {
            char rollout[37];
            int compressed;
            if (entry->fts_info != FTS_F ||
                !parse_rollout_name(entry->fts_name, rollout, &compressed) || field(index, rollout))
                continue;
            json_object *record = rollout_header(entry->fts_accpath, compressed);
            json_object *meta = field(record, "payload");
            const char *id = str(meta, "id"), *base = str(field(meta, "history_base"), "thread_id");
            if (!strcmp(str(record, "type"), "session_meta") && strlen(id) == 36 &&
                valid_rollout_uuid(id)) {
                json_object *ref = json_object_new_object();
                json_object_object_add(ref, "id", json_object_new_string(id));
                json_object_object_add(ref, "base", json_object_new_string(base));
                json_object_object_add(ref, "archived", json_object_new_boolean(a));
                json_object_object_add(index, rollout, ref);
            }
            json_object_put(record);
        }
        fts_close(tree);
    }
    return index;
}

static json_object *dependent_forks(json_object *index, json_object *plan) {
    json_object *forks = json_object_new_object();
    int added;
    do {
        added = 0;
        json_object_object_foreach(index, key, ref) {
            (void)key;
            const char *id = str(ref, "id");
            const char *source = str(field(index, str(ref, "base")), "id");
            if (*source && !is_id(plan, id) && !field(forks, id) &&
                (is_id(plan, source) || field(forks, source))) {
                json_object_object_add(forks, id, json_object_get(ref));
                added = 1;
            }
        }
    } while (added);
    return forks;
}

static int review_forks(json_object *forks) {
    json_object *labels = json_object_new_array();
    json_object_object_foreach(forks, id, ref) {
        json_object *params = json_object_new_object();
        json_object_object_add(params, "threadId", json_object_new_string(id));
        json_object *result = rpc("thread/read", params);
        const char *name = str(field(result, "thread"), "name");
        int archived_fork = json_object_get_boolean(field(ref, "archived"));
        char label[512];
        snprintf(label, sizeof label, "%s%.450s", archived_fork ? "[archived] " : "",
                 *name ? name : id);
        for (char *p = label; *p; p++)
            if ((unsigned char)*p < 32)
                *p = ' ';
        json_object_array_add(labels, json_object_new_string(label));
        json_object_put(result);
    }
    error[0] = 0;
    flushinp();
    int key, top = 0, total = json_object_array_length(labels);
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        erase();
        if (h < 6 || w < 45) {
            mvaddnstr(0, 0, "Enlarge the terminal or press Esc.", w - 1);
            refresh();
            if (getch() == 27) {
                key = 27;
                break;
            }
            continue;
        }
        if (top > total - (h - 3))
            top = total > h - 3 ? total - (h - 3) : 0;
        mvaddnstr(0, 0, "These forks still use the selected history.", w - 1);
        for (int row = 0; row < h - 3 && top + row < total; row++)
            mvaddnstr(row + 1, 0, json_object_get_string(json_object_array_get_idx(labels, top + row)),
                      w - 1);
        mvaddnstr(h - 2, 0, "Include these forks in the deletion?", w - 1);
        mvaddnstr(h - 1, 0, "y include  n leave out  Esc cancel  j/k", w - 1);
        refresh();
        key = getch();
        if (key == 'y' || key == 'Y' || key == 'n' || key == 'N' || key == 27 || key == ERR)
            break;
        if ((key == 'j' || key == KEY_DOWN) && top + h - 3 < total)
            top++;
        else if ((key == 'k' || key == KEY_UP) && top > 0)
            top--;
    }
    int h, w;
    getmaxyx(stdscr, h, w);
    if (h >= 2 && w > 0) {
        move(h - 2, 0);
        clrtoeol();
    }
    json_object_put(labels);
    flushinp();
    return key;
}

static int plan_visit(const char *id, json_object *plan, json_object *visiting) {
    if (is_id(plan, id))
        return 1;
    if (is_id(visiting, id)) {
        snprintf(error, sizeof error, "Cycle in related-chat links");
        return 0;
    }
    json_object_array_add(visiting, json_object_new_string(id));
    const char *filters[] = {"attachedToThreadId", "parentThreadId"};
    for (int f = 0; f < 2; f++)
        for (int archived_flag = 0; archived_flag < 2; archived_flag++) {
            char *cursor = NULL;
            do {
                json_object *p = json_object_new_object();
                json_object_object_add(p, filters[f], json_object_new_string(id));
                json_object_object_add(p, "archived", json_object_new_boolean(archived_flag));
                json_object_object_add(p, "limit", json_object_new_int(100));
                json_object_object_add(p, "useStateDbOnly", json_object_new_boolean(1));
                json_object_object_add(p, "modelProviders", json_object_new_array());
                json_object *kinds = json_object_new_array();
                const char *names[] = {"cli",
                                       "vscode",
                                       "exec",
                                       "appServer",
                                       "subAgent",
                                       "subAgentReview",
                                       "subAgentCompact",
                                       "subAgentThreadSpawn",
                                       "subAgentOther",
                                       "unknown",
                                       NULL};
                for (int k = 0; names[k]; k++)
                    json_object_array_add(kinds, json_object_new_string(names[k]));
                json_object_object_add(p, "sourceKinds", kinds);
                if (cursor)
                    json_object_object_add(p, "cursor", json_object_new_string(cursor));
                free(cursor);
                cursor = NULL;
                json_object *r = rpc("thread/list", p);
                if (!r)
                    return 0;
                json_object *data = field(r, "data");
                if (!data || !json_object_is_type(data, json_type_array)) {
                    json_object_put(r);
                    snprintf(error, sizeof error, "Invalid related-chat list");
                    return 0;
                }
                for (size_t i = 0; i < json_object_array_length(data); i++) {
                    const char *child = str(json_object_array_get_idx(data, i), "id");
                    if (!*child || !plan_visit(child, plan, visiting)) {
                        json_object_put(r);
                        if (!*error)
                            snprintf(error, sizeof error, "Invalid related-chat ID");
                        return 0;
                    }
                }
                const char *next = str(r, "nextCursor");
                if (*next)
                    cursor = strdup(next);
                json_object_put(r);
            } while (cursor);
        }
    json_object_array_del_idx(visiting, json_object_array_length(visiting) - 1, 1);
    json_object_array_add(plan, json_object_new_string(id));
    return 1;
}

static int shutdown_desktop(void);
static void delete_selected(void) {
    int quit_before_delete = desktop_running();
    if (quit_before_delete) {
        int h, w;
        getmaxyx(stdscr, h, w);
        mvaddnstr(h - 2, 0, "The ChatGPT desktop app is open.", w - 1);
        clrtoeol();
        move(h - 1, 0);
        clrtoeol();
        mvaddnstr(h - 1, 0, "Quit it? Running chats may stop. [y/N] ", w - 1);
        refresh();
        int key = getch();
        mvhline(h - 2, 0, ACS_HLINE, w);
        if (key != 'y' && key != 'Y') {
            snprintf(status, sizeof status, "Deletion cancelled; desktop left open.");
            return;
        }
        if (!shutdown_desktop())
            return;
    }
    json_object *plan = json_object_new_array(), *visiting = json_object_new_array();
    int selected = selected_count();
    error[0] = 0;
    for (size_t i = 0; i < count; i++)
        if (chats[i].selected && !plan_visit(chats[i].id, plan, visiting)) {
            snprintf(status, sizeof status, "Could not inspect related chats: %.300s", error);
            json_object_put(visiting);
            json_object_put(plan);
            return;
        }
    json_object *rollouts = index_rollouts();
    for (;;) {
        json_object *forks = dependent_forks(rollouts, plan);
        if (!json_object_object_length(forks)) {
            json_object_put(forks);
            break;
        }
        int choice = review_forks(forks);
        int ok = choice == 'y' || choice == 'Y';
        if (ok) {
            json_object_object_foreach(forks, id, ref) {
                (void)ref;
                if (!plan_visit(id, plan, visiting)) {
                    ok = 0;
                    break;
                }
            }
        }
        json_object_put(forks);
        if (choice == 'n' || choice == 'N')
            break;
        if (!ok) {
            if (*error)
                snprintf(status, sizeof status, "Could not inspect related chats: %.300s", error);
            else
                snprintf(status, sizeof status, "Deletion cancelled.");
            json_object_put(rollouts);
            json_object_put(visiting);
            json_object_put(plan);
            return;
        }
    }
    json_object_put(rollouts);
    json_object_put(visiting);
    int total = (int)json_object_array_length(plan);
    char answer[32] = "", label[200];
    snprintf(label, sizeof label,
             "Delete %d selected + %d related chats permanently? Type DELETE: ", selected,
             total - selected);
    if (quit_before_delete)
        flushinp();
    prompt(label, answer, sizeof answer);
    if (strcmp(answer, "DELETE")) {
        json_object_put(plan);
        snprintf(status, sizeof status, "Deletion cancelled.");
        return;
    }
    if (desktop_running()) {
        json_object_put(plan);
        snprintf(status, sizeof status, "Quit the ChatGPT desktop app before deleting.");
        return;
    }
    int fd = pending_open();
    if (fd < 0) {
        snprintf(status, sizeof status, "Cannot delete: %.350s", error);
        json_object_put(plan);
        return;
    }
    if (!pending_flush(fd)) {
        snprintf(status, sizeof status, "Previous cleanup failed: %.350s", error);
        close(fd);
        json_object_put(plan);
        return;
    }
    int ok = 0, failed = 0, journal_failed = 0, previous_ok;
    char first[300] = "";
    json_object *failids = json_object_new_array();
    do {
        previous_ok = ok;
        json_object *deferred = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(plan); i++) {
            json_object *entry = json_object_array_get_idx(plan, i);
            const char *id = json_object_get_string(entry);
            json_object *p = json_object_new_object();
            json_object_object_add(p, "threadId", json_object_new_string(id));
            error[0] = 0;
            json_object *r = rpc("thread/delete", p);
            if (!r) {
                if (rpc_error_code == -32600 && strstr(error, ": forked history still references it")) {
                    json_object_array_add(deferred, json_object_get(entry));
                    continue;
                }
                failed++;
                json_object_array_add(failids, json_object_get(entry));
                if (!*first)
                    snprintf(first, sizeof first, "%.250s", error);
                continue;
            }
            json_object_put(r);
            ok++;
            if (lseek(fd, 0, SEEK_END) < 0 || !write_all(fd, id, strlen(id)) ||
                !write_all(fd, "\n", 1) || fsync(fd)) {
                failed++;
                journal_failed = 1;
                snprintf(first, sizeof first, "Could not journal deleted chat %.80s", id);
                break;
            }
        }
        json_object_put(plan);
        plan = deferred;
    } while (!journal_failed && ok > previous_ok && json_object_array_length(plan));

    int blocked = (int)json_object_array_length(plan);
    char summary[100];
    if (blocked)
        snprintf(summary, sizeof summary, "%d deleted, %d blocked by forks, %d failed",
                 ok, blocked, failed);
    else
        snprintf(summary, sizeof summary, "%d deleted, %d failed", ok, failed);
    int cleaned = pending_flush(fd);
    char cleanup_error[300] = "";
    if (!cleaned)
        snprintf(cleanup_error, sizeof cleanup_error, "%.250s", error);
    close(fd);
    error[0] = 0;
    if (!load()) {
        snprintf(status, sizeof status, "%s; reload error: %.250s", summary, error);
        error[0] = 0;
    } else {
        for (size_t i = 0; i < count; i++)
            if (is_id(failids, chats[i].id) || is_id(plan, chats[i].id))
                chats[i].selected = 1;
        if (!cleaned)
            snprintf(status, sizeof status, "%s; sidebar cleanup pending: %.250s", summary,
                     cleanup_error);
        else
            snprintf(status, sizeof status, "%s%s%.250s", summary, failed ? ": " : "", first);
    }
    json_object_put(plan);
    json_object_put(failids);
}

static int same_executable(const struct stat *actual, const char *path) {
    struct stat expected;
    return stat(path, &expected) == 0 && actual->st_dev == expected.st_dev &&
           actual->st_ino == expected.st_ino;
}

static int matches_desktop_on_path(const struct stat *actual) {
    const char *dir = getenv("PATH");
    while (dir && *dir) {
        size_t len = strcspn(dir, ":");
        char path[PATH_MAX], launcher[PATH_MAX];
        if (*dir == '/' && len <= sizeof path - sizeof "/chatgpt") {
            snprintf(path, sizeof path, "%.*s/chatgpt", (int)len, dir);
            if (access(path, X_OK) == 0 && realpath(path, launcher)) {
                if (same_executable(actual, launcher))
                    return 1;
                /* The Linux launcher runs ChatGPT from its own directory. */
                *strrchr(launcher, '/') = '\0';
                int n = snprintf(path, sizeof path, "%s/ChatGPT", launcher);
                if (n >= 0 && n < (int)sizeof path && same_executable(actual, path))
                    return 1;
            }
        }
        dir += len;
        if (*dir == ':')
            dir++;
    }
    return 0;
}

static int shutdown_desktop(void) {
    pid_t pid = desktop_pid();
    if (!pid) {
        snprintf(status, sizeof status, "The ChatGPT desktop app is closed.");
        return 1;
    }
    if (pid < 0) {
        snprintf(status, sizeof status, "Cannot identify desktop process.");
        return 0;
    }
    char proc[80];
    snprintf(proc, sizeof proc, "/proc/%d", (int)pid);
    struct stat st;
    if (stat(proc, &st) || st.st_uid != getuid()) {
        snprintf(status, sizeof status, "Cannot verify desktop process owner.");
        return 0;
    }
    snprintf(proc, sizeof proc, "/proc/%d/exe", (int)pid);
    struct stat actual;
    const char *override = getenv("CODEX_DESKTOP_EXE");
    if (stat(proc, &actual) ||
        (override && *override ? override[0] != '/' || !same_executable(&actual, override)
                              : !matches_desktop_on_path(&actual))) {
        snprintf(status, sizeof status,
                 "Cannot verify desktop executable. Set CODEX_DESKTOP_EXE or quit manually.");
        return 0;
    }
    if (kill(pid, SIGTERM)) {
        snprintf(status, sizeof status, "Could not quit desktop: %.200s", strerror(errno));
        return 0;
    }
    for (int i = 0; i < 50 && desktop_running(); i++)
        usleep(100000);
    snprintf(status, sizeof status,
             desktop_running() ? "Quit requested; desktop is still running."
                               : "Desktop quit; deletion is now available.");
    return !desktop_running();
}

static void quit_desktop(void) {
    if (!desktop_running()) {
        snprintf(status, sizeof status, "The ChatGPT desktop app is closed.");
        return;
    }
    int h, w;
    getmaxyx(stdscr, h, w);
    mvaddnstr(h - 2, 0, "Running chats may stop.", w - 1);
    clrtoeol();
    char answer[32] = "";
    prompt("Quit the ChatGPT desktop app? Type QUIT: ", answer, sizeof answer);
    if (strcmp(answer, "QUIT")) {
        snprintf(status, sizeof status, "Desktop quit cancelled.");
        return;
    }
    shutdown_desktop();
}

static void associate_projects(void) {
    if (!projects)
        return;
    for (size_t i = 0; i < count; i++) {
        Chat *c = &chats[i];
        if (*c->project)
            continue;
        size_t best = 0;
        const char *id = NULL;
        for (size_t j = 0; j < json_object_array_length(projects); j++) {
            json_object *p = json_object_array_get_idx(projects, j), *roots = field(p, "roots");
            for (size_t k = 0; roots && k < json_object_array_length(roots); k++) {
                const char *path = str(json_object_array_get_idx(roots, k), "path");
                size_t len = strlen(path);
                while (len > 1 && path[len - 1] == '/')
                    len--;
                if (len > best && !strncmp(c->cwd, path, len) &&
                    (len == 1 || c->cwd[len] == '/' || c->cwd[len] == '\0')) {
                    best = len;
                    id = str(p, "id");
                }
            }
        }
        if (id && *id) {
            free(c->project);
            c->project = strdup(id);
        }
    }
}

static int choose_project(char **chosen, char **label) {
    json_object *all = json_object_new_array();
    char *cursor = NULL;
    do {
        json_object *p = json_object_new_object();
        json_object_object_add(p, "limit", json_object_new_int(100));
        if (cursor)
            json_object_object_add(p, "cursor", json_object_new_string(cursor));
        free(cursor);
        cursor = NULL;
        json_object *r = rpc("project/list", p);
        if (!r) {
            json_object_put(all);
            message();
            return 0;
        }
        json_object *d = field(r, "data");
        if (!d || !json_object_is_type(d, json_type_array)) {
            json_object_put(r);
            json_object_put(all);
            snprintf(error, sizeof error, "Invalid project list");
            message();
            return 0;
        }
        for (size_t i = 0; i < json_object_array_length(d); i++)
            json_object_array_add(all, json_object_get(json_object_array_get_idx(d, i)));
        const char *next = str(r, "nextCursor");
        if (*next)
            cursor = strdup(next);
        json_object_put(r);
    } while (cursor);
    if (projects)
        json_object_put(projects);
    projects = json_object_get(all);
    associate_projects();
    int pos = 0, top = 0, n = (int)json_object_array_length(all) + 2, changed = 0;
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        erase();
        mvaddnstr(0, 0, "Filter by project | j/k or arrows: choose  Enter: apply  q: cancel",
                  w - 1);
        if (pos < top)
            top = pos;
        if (pos >= top + h - 3)
            top = pos - (h - 4);
        for (int j = top; j < n && j < top + h - 3; j++) {
            const char *name = j == 0   ? "All projects"
                               : j == 1 ? "No project"
                                        : str(json_object_array_get_idx(all, j - 2), "name");
            if (j == pos)
                attron(A_REVERSE);
            mvaddnstr(j - top + 2, 0, name, w - 1);
            if (j == pos)
                attroff(A_REVERSE);
        }
        refresh();
        int k = getch();
        if (k == 'q' || k == 27)
            break;
        else if ((k == KEY_UP || k == 'k') && pos > 0)
            pos--;
        else if ((k == KEY_DOWN || k == 'j') && pos < n - 1)
            pos++;
        else if (k == 'G')
            pos = n - 1;
        else if (k == 'g') {
            if (getch() == 'g')
                pos = 0;
        } else if (k == '\n' || k == KEY_ENTER) {
            free(*chosen);
            free(*label);
            *chosen =
                pos == 0
                    ? NULL
                    : strdup(pos == 1 ? "" : str(json_object_array_get_idx(all, pos - 2), "id"));
            *label = strdup(pos == 0   ? "All projects"
                            : pos == 1 ? "No project"
                                       : str(json_object_array_get_idx(all, pos - 2), "name"));
            changed = 1;
            break;
        }
    }
    json_object_put(all);
    return changed;
}

static void preview_line(int row, int top, int height, int width, int user) {
    if (row < top || row >= top + height || !user)
        return;
    int screen_row = row - top + 2;
    attrset(A_REVERSE);
    mvhline(screen_row, 0, ' ', width);
    attrset(A_REVERSE | A_BOLD);
    mvaddch(screen_row, 0, ACS_VLINE);
    attrset(A_NORMAL);
}

static int preview_rows(const char *text, json_object *blocks, int top, int height, int width) {
    int row = 0;
    for (size_t i = 0; i < json_object_array_length(blocks); i++) {
        json_object *block = json_object_array_get_idx(blocks, i);
        const char *p = text + json_object_get_int64(json_object_array_get_idx(block, 0));
        const char *end = text + json_object_get_int64(json_object_array_get_idx(block, 1));
        int user = json_object_get_boolean(json_object_array_get_idx(block, 2));
        int column = 0, content_width = width - 3;
        mbstate_t state = {0};
        preview_line(row, top, height, width, user);
        while (p < end) {
            wchar_t ch;
            size_t length = mbrtowc(
                &ch, p, (size_t)(end - p) < MB_CUR_MAX ? (size_t)(end - p) : MB_CUR_MAX, &state);
            if (length == (size_t)-1 || length == (size_t)-2) {
                memset(&state, 0, sizeof state);
                ch = L'?';
                length = 1;
            }
            if (!length)
                break;
            p += length;
            if (ch == L'\n') {
                row++;
                column = 0;
                if (p < end)
                    preview_line(row, top, height, width, user);
                continue;
            }
            int cells = wcwidth(ch);
            if (cells < 0) {
                ch = L' ';
                cells = 1;
            }
            if (column + cells > content_width) {
                row++;
                column = 0;
                preview_line(row, top, height, width, user);
            }
            if (row >= top && row < top + height) {
                attrset(user ? A_REVERSE : A_NORMAL);
                move(row - top + 2, column + 2);
                addnwstr(&ch, 1);
                attrset(A_NORMAL);
            }
            column += cells;
        }
        row++;
    }
    return row;
}

static void write_preview_item(FILE *buffer, json_object *blocks, json_object *item) {
    const char *type = str(item, "type");
    int user = !strcmp(type, "userMessage");
    if (!user && strcmp(type, "agentMessage"))
        return;
    long start = ftell(buffer);
    if (user) {
        fprintf(buffer, "You\n");
        json_object *content = field(item, "content");
        for (size_t j = 0; content && json_object_is_type(content, json_type_array) &&
                           j < json_object_array_length(content);
             j++) {
            json_object *part = json_object_array_get_idx(content, j);
            const char *part_type = str(part, "type");
            if (!strcmp(part_type, "text"))
                fprintf(buffer, "%s\n", str(part, "text"));
            else if (!strcmp(part_type, "image") || !strcmp(part_type, "localImage"))
                fprintf(buffer, "[Image]\n");
            else if (!strcmp(part_type, "audio") || !strcmp(part_type, "localAudio"))
                fprintf(buffer, "[Audio]\n");
        }
    } else {
        fprintf(buffer, "Codex\n%s\n", str(item, "text"));
    }
    json_object *block = json_object_new_array();
    json_object_array_add(block, json_object_new_int64(start));
    json_object_array_add(block, json_object_new_int64(ftell(buffer)));
    json_object_array_add(block, json_object_new_boolean(user));
    json_object_array_add(blocks, block);
}

static void preview_chat(const Chat *chat) {
    char *text = NULL, *cursor = NULL;
    size_t length = 0;
    FILE *buffer = open_memstream(&text, &length);
    if (!buffer) {
        snprintf(error, sizeof error, "Could not open preview");
        message();
        return;
    }
    json_object *blocks = json_object_new_array();
    int list_items = 1, ok = 1;
    error[0] = 0;
    for (;;) {
        json_object *params = json_object_new_object();
        json_object_object_add(params, "threadId", json_object_new_string(chat->id));
        json_object_object_add(params, "limit", json_object_new_int(100));
        json_object_object_add(params, "sortDirection", json_object_new_string("asc"));
        int first_page = !cursor;
        if (cursor)
            json_object_object_add(params, "cursor", json_object_new_string(cursor));
        if (!list_items)
            json_object_object_add(params, "itemsView", json_object_new_string("full"));
        free(cursor);
        cursor = NULL;
        json_object *response = rpc(list_items ? "thread/items/list" : "thread/turns/list", params);
        if (!response && list_items && first_page && rpc_error_code == -32601) {
            list_items = 0;
            error[0] = 0;
            continue;
        }
        if (!response) {
            ok = 0;
            break;
        }
        json_object *items = field(response, "data");
        if (!items || !json_object_is_type(items, json_type_array)) {
            snprintf(error, sizeof error, "Invalid chat preview");
            json_object_put(response);
            ok = 0;
            break;
        }
        for (size_t i = 0; i < json_object_array_length(items); i++) {
            json_object *entry = json_object_array_get_idx(items, i);
            if (list_items) {
                write_preview_item(buffer, blocks, field(entry, "item"));
            } else {
                json_object *turn_items = field(entry, "items");
                if (turn_items && json_object_is_type(turn_items, json_type_array))
                    for (size_t j = 0; j < json_object_array_length(turn_items); j++)
                        write_preview_item(buffer, blocks, json_object_array_get_idx(turn_items, j));
            }
        }
        const char *next = str(response, "nextCursor");
        if (*next) {
            cursor = strdup(next);
            if (!cursor) {
                snprintf(error, sizeof error, "Out of memory");
                ok = 0;
            }
        }
        json_object_put(response);
        if (!cursor)
            break;
    }
    free(cursor);
    if (fclose(buffer)) {
        snprintf(error, sizeof error, "Could not prepare preview");
        ok = 0;
    }
    if (!ok) {
        json_object_put(blocks);
        free(text);
        message();
        return;
    }
    if (!json_object_array_length(blocks)) {
        free(text);
        text = strdup("No text messages available.\n");
        if (!text) {
            json_object_put(blocks);
            snprintf(error, sizeof error, "Out of memory");
            message();
            return;
        }
        json_object *block = json_object_new_array();
        json_object_array_add(block, json_object_new_int64(0));
        json_object_array_add(block, json_object_new_int64(strlen(text)));
        json_object_array_add(block, json_object_new_boolean(0));
        json_object_array_add(blocks, block);
    }
    int top = 0;
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        erase();
        if (h < 5 || w < 10) {
            mvaddnstr(0, 0, "Enlarge terminal or press q", w > 0 ? w - 1 : 0);
            refresh();
            int key = getch();
            if (key == 'q' || key == 27)
                break;
            continue;
        }
        attron(style(HEADER));
        mvaddnstr(0, 0, chat->title, w - 1);
        attroff(style(HEADER));
        mvaddnstr(1, 0, "j/k scroll  PgUp/PgDn page  gg/G start/end  q/Esc back", w - 1);
        int rows = preview_rows(text, blocks, top, h - 2, w - 1);
        int max_top = rows > h - 2 ? rows - (h - 2) : 0;
        if (top > max_top) {
            top = max_top;
            continue;
        }
        refresh();
        int key = getch();
        if (key == 'q' || key == 27)
            break;
        if ((key == 'j' || key == KEY_DOWN) && top < max_top)
            top++;
        else if ((key == 'k' || key == KEY_UP) && top > 0)
            top--;
        else if (key == KEY_NPAGE || key == 4)
            top = top + h - 2 < max_top ? top + h - 2 : max_top;
        else if (key == KEY_PPAGE || key == 21)
            top = top > h - 2 ? top - (h - 2) : 0;
        else if (key == 'G')
            top = max_top;
        else if (key == 'g' && getch() == 'g')
            top = 0;
    }
    json_object_put(blocks);
    free(text);
}

int main(void) {
    setlocale(LC_ALL, "");
    if (!start_server()) {
        fprintf(stderr, "Could not start Codex app-server: %s\n", error);
        return 1;
    }
    if (!load()) {
        fprintf(stderr, "Could not load chats: %s\n", error);
        return 1;
    }
    if (!desktop_running()) {
        int fd = pending_open();
        if (fd >= 0) {
            if (!pending_flush(fd))
                snprintf(status, sizeof status, "Sidebar cleanup pending: %.350s", error);
            close(fd);
        }
    }
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    colors();
    char filter[256] = "";
    char *project = NULL, *project_label = strdup("All projects");
    int pos = 0, top = 0, visual = -1;
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        if (h < 10 || w < 45) {
            erase();
            mvaddstr(0, 0, "Please enlarge the terminal (45x10). q quits.");
            refresh();
            if (getch() == 'q')
                break;
            continue;
        }
        int *visible = malloc((count ? count : 1) * sizeof(int)), n = 0;
        for (size_t i = 0; i < count; i++)
            if (match(&chats[i], filter) && (!project || !strcmp(chats[i].project, project)))
                visible[n++] = (int)i;
        if (pos >= n)
            pos = n ? n - 1 : 0;
        if (pos < top)
            top = pos;
        if (pos >= top + h - 7)
            top = pos - (h - 8);
        if (top < 0)
            top = 0;
        erase();
        char heading[512];
        snprintf(heading, sizeof heading, " CODEX CHAT MANAGER  /  %s  /  %d selected",
                 archived ? "ARCHIVED" : "ACTIVE", selected_count());
        attron(style(HEADER));
        mvaddnstr(0, 0, heading, w - 1);
        attroff(style(HEADER));
        snprintf(heading, sizeof heading,
                 " Project: %.100s | Search: %.100s | %d shown / %zu loaded", project_label, filter,
                 n, count);
        attron(style(INFO));
        mvaddnstr(1, 0, heading, w - 1);
        attroff(style(INFO));
        if (visual >= 0) {
            attron(style(VISUAL));
            mvaddnstr(
                2, 0,
                " VISUAL: j/k, gg/G or page to extend | v/Space/Enter select range | Esc cancel",
                w - 1);
            attroff(style(VISUAL));
        } else
            mvaddnstr(2, 0,
                      w < 85 ? " j/k move  v range  Space toggle  a/A all"
                             : " j/k move  gg/G first/last  v visual select  Space toggle  a shown "
                               " A all  u clear",
                      w - 1);
        mvaddnstr(3, 0,
                  w < 85 ? " / search  p project  Enter preview  d delete  Q close"
                         : " / search  p project  Enter preview  d delete  Q quit desktop  t/r/q",
                  w - 1);
        mvhline(4, 0, ACS_HLINE, w);
        mvhline(h - 2, 0, ACS_HLINE, w);
        for (int j = top; j < n && j < top + h - 7; j++) {
            Chat *c = &chats[visible[j]];
            int in_range = visual >= 0 && j >= (visual < pos ? visual : pos) &&
                           j <= (visual > pos ? visual : pos);
            char date[16] = "";
            struct tm tm;
            if (localtime_r(&c->updated, &tm))
                strftime(date, sizeof date, "%Y-%m-%d", &tm);
            char line[2048];
            snprintf(line, sizeof line, " %c [%c] %s  %s", j == pos ? '>' : ' ',
                     in_range      ? '+'
                     : c->selected ? 'x'
                                   : ' ',
                     date, c->title);
            for (char *s = line; *s; s++)
                if ((unsigned char)*s < 32)
                    *s = ' ';
            int row_style = in_range ? style(VISUAL) : c->selected ? style(SELECTED) : 0;
            if (row_style)
                attron(row_style);
            if (j == pos)
                attron(A_REVERSE);
            mvaddnstr(j - top + 5, 0, line, w - 1);
            if (j == pos)
                attroff(A_REVERSE);
            if (row_style)
                attroff(row_style);
        }
        if (!n)
            mvaddnstr(5, 2, "No chats match the current filters.", w - 3);
        move(h - 1, 0);
        clrtoeol();
        if (*status)
            printw("%.*s", w - 1, status);
        else if (n) {
            Chat *c = &chats[visible[pos]];
            printw("%.*s", w - 1, c->cwd);
        }
        refresh();
        int k = getch();
        if (visual >= 0) {
            if (k == 27) {
                visual = -1;
                free(visible);
                continue;
            }
            if (k == 'v' || k == ' ' || k == '\n' || k == KEY_ENTER) {
                int lo = visual < pos ? visual : pos, hi = visual > pos ? visual : pos;
                for (int i = lo; i <= hi; i++)
                    chats[visible[i]].selected = 1;
                visual = -1;
                free(visible);
                continue;
            }
            if (k != 'j' && k != 'k' && k != 'g' && k != 'G' && k != KEY_UP && k != KEY_DOWN &&
                k != KEY_PPAGE && k != KEY_NPAGE && k != 21 && k != 4) {
                free(visible);
                continue;
            }
        }
        if (k == 'q') {
            free(visible);
            break;
        }
        if ((k == KEY_UP || k == 'k') && pos > 0)
            pos--;
        else if ((k == KEY_DOWN || k == 'j') && pos < n - 1)
            pos++;
        else if (k == 'g') {
            if (getch() == 'g')
                pos = 0;
        } else if (k == 'G')
            pos = n ? n - 1 : 0;
        else if (k == KEY_PPAGE || k == 21) {
            pos -= h - 7;
            if (pos < 0)
                pos = 0;
        } else if (k == KEY_NPAGE || k == 4) {
            pos += h - 7;
            if (pos >= n)
                pos = n ? n - 1 : 0;
        } else if (k == 'v' && n)
            visual = pos;
        else if (k == ' ' && n)
            chats[visible[pos]].selected ^= 1;
        else if (k == 'a') {
            int all = 1;
            for (int j = 0; j < n; j++)
                if (!chats[visible[j]].selected)
                    all = 0;
            for (int j = 0; j < n; j++)
                chats[visible[j]].selected = !all;
        } else if (k == 'A') {
            for (size_t i = 0; i < count; i++)
                chats[i].selected = 1;
        } else if (k == 'u') {
            for (size_t i = 0; i < count; i++)
                chats[i].selected = 0;
        } else if (k == '/') {
            filter[0] = 0;
            prompt("Search title or folder (empty clears): ", filter, sizeof filter);
            pos = top = 0;
        } else if (k == 'p') {
            if (choose_project(&project, &project_label))
                pos = top = 0;
        } else if (k == 'r' || k == 't') {
            if (k == 't')
                archived = !archived;
            error[0] = 0;
            status[0] = 0;
            if (!load())
                message();
            pos = top = 0;
        } else if ((k == '\n' || k == KEY_ENTER) && n)
            preview_chat(&chats[visible[pos]]);
        else if (k == 'd' && selected_count())
            delete_selected();
        else if (k == 'Q')
            quit_desktop();
        free(visible);
    }
    endwin();
    free(project);
    free(project_label);
    if (projects)
        json_object_put(projects);
    clear_chats();
    fclose(out);
    fclose(in);
    waitpid(child, NULL, 0);
    return 0;
}
