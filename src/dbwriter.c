/*
 * OpenChime DB-writer thread. See dbwriter.h and migrate.h.
 */

#include "dbwriter.h"
#include "migrate.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>

struct oc_dbwriter {
    sqlite3        *db;
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             stop;
    int             started;
};

/* Owns the write connection for the process lifetime. For now it only waits
 * for shutdown; write jobs from the network thread land here later. */
static void *writer_loop(void *arg) {
    oc_dbwriter *w = (oc_dbwriter *)arg;
    pthread_mutex_lock(&w->mu);
    while (!w->stop)
        pthread_cond_wait(&w->cv, &w->mu);
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

oc_dbwriter *oc_dbwriter_start(const char *path) {
    oc_dbwriter *w = calloc(1, sizeof *w);
    if (!w) return NULL;

    if (sqlite3_open(path, &w->db) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: open %s failed: %s\n", path, sqlite3_errmsg(w->db));
        sqlite3_close(w->db);
        free(w);
        return NULL;
    }

    char *err = NULL;
    if (sqlite3_exec(w->db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: pragma failed: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(w->db);
        free(w);
        return NULL;
    }

    if (oc_migrate_default(w->db, &err) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: migration failed: %s\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(w->db);
        free(w);
        return NULL;
    }

    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (pthread_create(&w->thread, NULL, writer_loop, w) != 0) {
        fprintf(stderr, "dbwriter: thread create failed\n");
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        sqlite3_close(w->db);
        free(w);
        return NULL;
    }
    w->started = 1;
    return w;
}

void oc_dbwriter_stop(oc_dbwriter *w) {
    if (!w) return;
    if (w->started) {
        pthread_mutex_lock(&w->mu);
        w->stop = 1;
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
    }
    sqlite3_close(w->db);
    free(w);
}
