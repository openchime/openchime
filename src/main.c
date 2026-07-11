/*
 * openchimed placeholder daemon.
 *
 * Scope: prove the build -> container -> replication -> restore pipeline
 * works end-to-end. No wire protocol, no chat logic yet (ARCH-6..ARCH-9 are
 * not implemented here). This binary only:
 *   - opens the local SQLite database (already created by entrypoint.sh
 *     before this process starts, so Litestream has a WAL file to watch)
 *   - writes a heartbeat row every few seconds so there is something for
 *     Litestream to actually ship on its sync interval (ARCH-23)
 *   - serves GET /healthz on a configurable port (ARCH-25)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *db_path(void) {
    const char *path = getenv("OPENCHIME_DB_PATH");
    return path ? path : "/data/openchime.db";
}

static int health_port(void) {
    const char *port = getenv("OPENCHIME_HEALTH_PORT");
    return port ? atoi(port) : 8080;
}

static sqlite3 *open_db(void) {
    sqlite3 *db;
    if (sqlite3_open(db_path(), &db) != SQLITE_OK) {
        fprintf(stderr, "openchimed: failed to open %s: %s\n", db_path(), sqlite3_errmsg(db));
        exit(1);
    }
    char *err = NULL;
    const char *schema =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS _placeholder ("
        "  id INTEGER PRIMARY KEY,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");";
    if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "openchimed: schema init failed: %s\n", err);
        sqlite3_free(err);
        exit(1);
    }
    return db;
}

static void *heartbeat_loop(void *arg) {
    sqlite3 *db = (sqlite3 *)arg;
    for (;;) {
        char *err = NULL;
        if (sqlite3_exec(db, "INSERT INTO _placeholder DEFAULT VALUES;", NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "openchimed: heartbeat insert failed: %s\n", err);
            sqlite3_free(err);
        }
        sleep(5);
    }
    return NULL;
}

static const char HEALTH_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

static void serve_health(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("openchimed: socket");
        exit(1);
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("openchimed: bind");
        exit(1);
    }
    if (listen(listen_fd, 16) < 0) {
        perror("openchimed: listen");
        exit(1);
    }

    fprintf(stderr, "openchimed: healthz listening on :%d\n", port);

    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            continue;
        }
        /* Placeholder: no real HTTP parsing yet (ARCH-32 covers the real
         * picohttpparser-based listener). Drain whatever the client sent
         * and always answer 200 OK. */
        char buf[512];
        ssize_t n = read(conn_fd, buf, sizeof(buf));
        (void)n;
        ssize_t written = write(conn_fd, HEALTH_RESPONSE, sizeof(HEALTH_RESPONSE) - 1);
        (void)written;
        close(conn_fd);
    }
}

int main(void) {
    sqlite3 *db = open_db();

    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_loop, db) != 0) {
        fprintf(stderr, "openchimed: failed to start heartbeat thread\n");
        exit(1);
    }

    serve_health(health_port());

    return 0;
}
