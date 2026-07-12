/*
 * OpenChime DB-writer thread (ARCH-5).
 *
 * The single SQLite write connection is owned by one dedicated thread; the
 * network event loop never writes the database directly. On startup the writer
 * opens the database in WAL mode, applies migrations (ARCH-27), and then idles
 * waiting for write jobs. (The job queue that the network thread hands work to
 * arrives with the message-handling milestone; this skeleton establishes the
 * thread, the connection ownership, and migrate-on-boot.)
 */

#ifndef OPENCHIME_DBWRITER_H
#define OPENCHIME_DBWRITER_H

typedef struct oc_dbwriter oc_dbwriter;

/* Open `path`, set WAL + foreign_keys, run migrations, and start the writer
 * thread. Returns NULL if the database can't be opened or migrations fail
 * (the daemon must not serve traffic in that case). */
oc_dbwriter *oc_dbwriter_start(const char *path);

/* Stop the writer thread, join it, and close the database. */
void oc_dbwriter_stop(oc_dbwriter *w);

#endif /* OPENCHIME_DBWRITER_H */
