/* Tests for video messages on the wire and in the daemon (REQ-162–165,
 * ARCH-110): the attachment entry's media fields and the ATTACH_MEDIA_SET/OK
 * frames round-trip; every ATTACH_MEDIA_SET refusal; the media row rides SEND,
 * backfill and the Files listing; `has:video`; and the poster's lifetime in the
 * storage sweep — kept while its video is live, collected once it is not, and
 * reclaimed together with a video the age tier takes. */
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "dbwriter.h"
#include "protocol.h"
#include "searchq.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DBPATH "build/test_video_media.db"
#define DAY (24ull * 60 * 60 * 1000)

static oc_dbres *wait_result(oc_dbwriter *w) {
    for (int i = 0; i < 500; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) return r;
        usleep(2000);
    }
    return NULL;
}

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)(tv.tv_usec / 1000);
}

/* ---- wire ---------------------------------------------------------------------- */

static void test_wire(void) {
    uint8_t buf[4096];
    oc_wbuf w;

    /* A BROADCAST with an ordinary file and a video message side by side: the
     * media fields ride only the video's entry, and the entry after it still
     * decodes in place. */
    oc_broadcast in = { 5, 3, 42, 999, 0, oc_slice_str("clip"), 0, {{0}}, {0} };
    in.n_attach = 3;
    in.attach[0].id = 70; in.attach[0].filename = oc_slice_str("a.txt");
    in.attach[0].mime = oc_slice_str("text/plain"); in.attach[0].size = 10;
    in.attach[1].id = 71; in.attach[1].filename = oc_slice_str("Video message.mp4");
    in.attach[1].mime = oc_slice_str("video/mp4"); in.attach[1].size = 123456;
    in.attach[1].media_kind = OC_MEDIA_VIDEO_MESSAGE; in.attach[1].duration_ms = 10033;
    in.attach[1].width = 1280; in.attach[1].height = 720; in.attach[1].poster_id = 69;
    in.attach[2].id = 72; in.attach[2].filename = oc_slice_str("b.png");
    in.attach[2].mime = oc_slice_str("image/png"); in.attach[2].size = 20; in.attach[2].reclaimed = 1;
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &in) == OC_OK);
    oc_header h;
    oc_rbuf p;
    CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK && h.msg_type == OC_MSG_BROADCAST);
    oc_broadcast out;
    CHECK(oc_decode_broadcast(&p, &out) == OC_OK);
    CHECK(out.n_attach == 3);
    CHECK(out.attach[0].media_kind == OC_MEDIA_NONE && out.attach[0].poster_id == 0);
    CHECK(out.attach[1].media_kind == OC_MEDIA_VIDEO_MESSAGE && out.attach[1].duration_ms == 10033);
    CHECK(out.attach[1].width == 1280 && out.attach[1].height == 720 && out.attach[1].poster_id == 69);
    CHECK(out.attach[2].id == 72 && out.attach[2].size == 20 && out.attach[2].reclaimed == 1);

    /* An unknown media kind is malformed, not guessed at. */
    {
        oc_broadcast one = { 5, 3, 42, 999, 0, oc_slice_str("x"), 0, {{0}}, {0} };
        one.n_attach = 1; one.attach[0].id = 1; one.attach[0].filename = oc_slice_str("f");
        one.attach[0].mime = oc_slice_str("m"); one.attach[0].size = 1;
        oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &one) == OC_OK);
        buf[w.len - 1] = 7;                                    /* the media_kind byte, last */
        CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK);
        CHECK(oc_decode_broadcast(&p, &out) != OC_OK);
    }

    /* THREAD_REPLY carries the same entries. */
    {
        oc_thread_reply tr = {0};
        tr.channel_id = 3; tr.parent_id = 5; tr.body = oc_slice_str("r");
        tr.n_attach = 1; tr.attach[0] = in.attach[1];
        oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_thread_reply(&w, OC_PROTOCOL_VERSION, &tr) == OC_OK);
        CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK);
        oc_thread_reply tro;
        CHECK(oc_decode_thread_reply(&p, &tro) == OC_OK);
        CHECK(tro.n_attach == 1 && tro.attach[0].media_kind == 1 && tro.attach[0].poster_id == 69);
    }

    /* ATTACH_MEDIA_SET / OK and FILE_ENTRY. */
    oc_attach_media_set ms = { 71, OC_MEDIA_VIDEO_MESSAGE, 299999, 640, 360, 69 };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_attach_media_set(&w, OC_PROTOCOL_VERSION, &ms) == OC_OK);
    CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK && h.msg_type == OC_MSG_ATTACH_MEDIA_SET);
    oc_attach_media_set mso;
    CHECK(oc_decode_attach_media_set(&p, &mso) == OC_OK);
    CHECK(mso.attachment_id == 71 && mso.media_kind == 1 && mso.duration_ms == 299999 &&
          mso.width == 640 && mso.height == 360 && mso.poster_id == 69);

    oc_attach_media_ok mo = { 71 };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_attach_media_ok(&w, OC_PROTOCOL_VERSION, &mo) == OC_OK);
    CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK && h.msg_type == OC_MSG_ATTACH_MEDIA_OK);
    oc_attach_media_ok moo;
    CHECK(oc_decode_attach_media_ok(&p, &moo) == OC_OK && moo.attachment_id == 71);

    oc_file_entry fe = { 71, 3, 5, 42, 123456, 1000, 0, oc_slice_str("v.mp4"),
                         oc_slice_str("video/mp4"), OC_MEDIA_VIDEO_MESSAGE, 10033 };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_file_entry(&w, OC_PROTOCOL_VERSION, &fe) == OC_OK);
    CHECK(oc_parse_frame(buf, w.len, &h, &p) == OC_OK);
    oc_file_entry feo;
    CHECK(oc_decode_file_entry(&p, &feo) == OC_OK && feo.media_kind == 1 && feo.duration_ms == 10033);

    /* has:video parses, and describes itself back. */
    oc_searchq q;
    oc_searchq_parse("clip has:video", &q);
    CHECK((q.has & OC_SQ_HAS_VIDEO) != 0);
    char desc[128];
    oc_searchq_describe(&q, desc, sizeof desc);
    CHECK(strstr(desc, "has:video") != NULL);
}

/* ---- daemon ---------------------------------------------------------------------- */

/* An attachment row, straight into the table: `uploader`, `channel`, `mime`,
 * `size`, and whether it is finalized; `age_ms` old. */
static void put_attachment(sqlite3 *db, uint64_t id, uint64_t uploader, uint64_t channel,
                           const char *mime, uint64_t size, int final, uint64_t age_ms) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO attachments(id, channel_id, message_id, uploader_id, storage_key, "
        " filename, mime, size, sha256, created_at_ms) "
        "VALUES(?1, ?2, NULL, ?3, printf('%016x', ?1), 'f', ?4, ?5, ?6, ?7);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)channel);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)uploader);
    sqlite3_bind_text (st, 4, mime, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)size);
    if (final) sqlite3_bind_blob(st, 6, "x", 1, SQLITE_STATIC); else sqlite3_bind_null(st, 6);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)(now_ms() - age_ms));
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

static int reclaimed(sqlite3 *db, uint64_t id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT reclaimed_at_ms FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) != 0 : -1;
    sqlite3_finalize(st);
    return v;
}

static void exec(sqlite3 *db, const char *sql) {
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static int media_set(oc_dbwriter *w, uint64_t user, uint64_t aid, uint8_t kind, uint32_t dur,
                     uint16_t wd, uint16_t ht, uint64_t poster, uint64_t cap) {
    oc_job *j = oc_job_new(OC_JOB_ATTACH_MEDIA_SET, 9);
    j->user_id = user; j->attachment_id = aid; j->att_size = cap;
    j->media_kind = kind; j->media_duration_ms = dur;
    j->media_width = wd; j->media_height = ht; j->media_poster_id = poster;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    int out = !r ? -9999 : r->type == OC_RES_MEDIA_OK ? 0 : (int)r->err_code;
    if (r) oc_dbres_free(r);
    return out;
}

static oc_dbres *send_with(oc_dbwriter *w, uint64_t user, uint8_t tag, const char *body, uint64_t aid) {
    oc_job *j = oc_job_new(OC_JOB_SEND, 10);
    j->user_id = user; j->channel_id = OC_DEFAULT_CHANNEL;
    memset(j->idem, tag, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    if (aid) { j->attach_ids[0] = aid; j->n_attach = 1; }
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void run_maint(oc_dbwriter *w, uint64_t grace_ms, uint64_t max_age_ms, int evict) {
    oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
    j->maint_grace_ms = grace_ms; j->maint_batch = 64;
    j->maint_max_age_ms = max_age_ms; j->maint_evict = evict;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_STORAGE_MAINT);
    if (r) oc_dbres_free(r);
}

static void test_daemon(void) {
    unlink(DBPATH); unlink(DBPATH "-wal"); unlink(DBPATH "-shm");
    oc_dbwriter *w = oc_dbwriter_start(DBPATH);
    CHECK(w != NULL);
    if (!w) return;
    uint64_t alice = oc_dbwriter_register_local(w, "vm-alice", "pw", OC_ROLE_OWNER, 1000);
    uint64_t bob   = oc_dbwriter_register_local(w, "vm-bob", "pw", OC_ROLE_MEMBER, 1000);
    CHECK(alice && bob);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(DBPATH, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    exec(db, "INSERT INTO channels(id, kind, name, is_public, created_at_ms) VALUES(77, 'channel', 'elsewhere', 1, 1);");

    const uint64_t CAP = 64ull * 1024 * 1024;
    /* 100 video, 101 poster: alice's, channel 1, finalized. The rest are the
     * refusals' raw material. */
    put_attachment(db, 100, alice, OC_DEFAULT_CHANNEL, "video/mp4", 5000000, 1, 0);
    put_attachment(db, 101, alice, OC_DEFAULT_CHANNEL, "image/jpeg", 40000, 1, 0);
    put_attachment(db, 102, alice, OC_DEFAULT_CHANNEL, "video/mp4", CAP + 1, 1, 0);    /* too big */
    put_attachment(db, 103, alice, OC_DEFAULT_CHANNEL, "video/webm", 1000, 1, 0);      /* not MP4 */
    put_attachment(db, 104, alice, OC_DEFAULT_CHANNEL, "video/mp4", 1000, 0, 0);       /* unfinished */
    put_attachment(db, 105, bob,   OC_DEFAULT_CHANNEL, "image/jpeg", 1000, 1, 0);      /* bob's poster */
    put_attachment(db, 106, alice, 77,                 "image/jpeg", 1000, 1, 0);      /* other channel */
    put_attachment(db, 107, alice, OC_DEFAULT_CHANNEL, "image/png", 1000, 1, 0);       /* not JPEG */
    put_attachment(db, 108, alice, OC_DEFAULT_CHANNEL, "image/jpeg", 2 * 1024 * 1024, 1, 0); /* big poster */

    int V = OC_MEDIA_VIDEO_MESSAGE;
    /* Bounds. */
    CHECK(media_set(w, alice, 100, 2, 10000, 1280, 720, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 0, 1280, 720, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 300001, 1280, 720, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1281, 720, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 3840, 2160, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 0, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 100, CAP) == OC_ERR_MEDIA_INVALID);
    /* The video. */
    CHECK(media_set(w, bob,   100, V, 10000, 1280, 720, 101, CAP) == OC_ERR_UNKNOWN_ATTACHMENT);
    CHECK(media_set(w, alice, 999, V, 10000, 1280, 720, 101, CAP) == OC_ERR_UNKNOWN_ATTACHMENT);
    CHECK(media_set(w, alice, 104, V, 10000, 1280, 720, 101, CAP) == OC_ERR_UNKNOWN_ATTACHMENT);
    CHECK(media_set(w, alice, 103, V, 10000, 1280, 720, 101, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 102, V, 10000, 1280, 720, 101, CAP) == OC_ERR_MEDIA_TOO_LARGE);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 101, 4999999) == OC_ERR_MEDIA_TOO_LARGE);
    /* The poster. */
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 105, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 106, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 107, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 108, CAP) == OC_ERR_MEDIA_INVALID);
    CHECK(media_set(w, alice, 100, V, 10000, 1280, 720, 9999, CAP) == OC_ERR_MEDIA_INVALID);
    /* Accepted, and a second report replaces the first while unsent. */
    CHECK(media_set(w, alice, 100, V, 9000, 640, 360, 101, CAP) == 0);
    CHECK(media_set(w, alice, 100, V, 10033, 1280, 720, 101, CAP) == 0);
    /* A video cannot serve as another video's poster. */
    put_attachment(db, 109, alice, OC_DEFAULT_CHANNEL, "video/mp4", 1000, 1, 0);
    exec(db, "UPDATE attachments SET mime='image/jpeg' WHERE id=100;");
    CHECK(media_set(w, alice, 109, V, 1000, 640, 360, 100, CAP) == OC_ERR_MEDIA_INVALID);
    exec(db, "UPDATE attachments SET mime='video/mp4' WHERE id=100;");

    /* SEND carries the media row; so does backfill, and the Files view. */
    oc_dbres *r = send_with(w, alice, 0x31, "clip", 100);
    CHECK(r && r->type == OC_RES_SEND_OK && r->n_attach == 1);
    uint64_t mid = r ? r->message_id : 0;
    if (r && r->n_attach == 1) {
        CHECK(r->attach[0].id == 100 && r->attach[0].media_kind == V);
        CHECK(r->attach[0].duration_ms == 10033 && r->attach[0].width == 1280 &&
              r->attach[0].height == 720 && r->attach[0].poster_id == 101);
    }
    if (r) oc_dbres_free(r);
    /* Once sent, its facts are fixed. */
    CHECK(media_set(w, alice, 100, V, 1, 640, 360, 101, CAP) == OC_ERR_MEDIA_INVALID);

    oc_job *j = oc_job_new(OC_JOB_BACKFILL, 11);
    j->user_id = bob;
    j->cursors = calloc(1, sizeof *j->cursors);
    j->cursors[0].channel_id = OC_DEFAULT_CHANNEL; j->cursors[0].after_message_id = mid - 1;
    j->n_cursors = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    int saw = 0;
    for (size_t i = 0; r && i < r->n_replay; i++)
        if (r->replay[i].message_id == mid && r->replay[i].n_attach == 1 &&
            r->replay[i].attach[0].media_kind == V && r->replay[i].attach[0].poster_id == 101) saw = 1;
    CHECK(saw);
    if (r) oc_dbres_free(r);

    j = oc_job_new(OC_JOB_LIST_FILES, 12);
    j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    saw = 0;
    for (size_t i = 0; r && i < r->n_flist; i++)
        if (r->flist[i].id == 100 && r->flist[i].media_kind == V && r->flist[i].duration_ms == 10033) saw = 1;
    CHECK(saw);
    if (r) oc_dbres_free(r);

    /* has:video finds the video message and not a message with a picture. */
    put_attachment(db, 110, alice, OC_DEFAULT_CHANNEL, "image/png", 100, 1, 0);
    r = send_with(w, alice, 0x32, "clip", 110);
    if (r) oc_dbres_free(r);
    j = oc_job_new(OC_JOB_SEARCH, 13);
    j->user_id = bob; j->search_limit = 50; j->sq_has = OC_SQ_HAS_VIDEO;
    oc_job_set_body(j, "clip", 4);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 1 && r->search[0].message_id == mid);
    if (r) oc_dbres_free(r);

    /* The poster's lifetime. Everything below is old enough to be swept. */
    exec(db, "UPDATE attachments SET created_at_ms = created_at_ms - 400 * 86400000;");

    /* Live video: the poster is not an orphan, though no message names it. */
    run_maint(w, DAY, 0, 0);
    CHECK(reclaimed(db, 100) == 0);
    CHECK(reclaimed(db, 101) == 0);

    /* A second video message whose message is deleted: the video leaves the
     * message, and the next sweep collects both it and its poster. */
    put_attachment(db, 120, alice, OC_DEFAULT_CHANNEL, "video/mp4", 1000, 1, 0);
    put_attachment(db, 121, alice, OC_DEFAULT_CHANNEL, "image/jpeg", 1000, 1, 0);
    CHECK(media_set(w, alice, 120, V, 1000, 640, 360, 121, CAP) == 0);
    r = send_with(w, alice, 0x33, "second", 120);
    uint64_t mid2 = r ? r->message_id : 0;
    if (r) oc_dbres_free(r);
    exec(db, "UPDATE attachments SET created_at_ms = created_at_ms - 400 * 86400000 WHERE id IN (120, 121);");
    run_maint(w, DAY, 0, 0);
    CHECK(reclaimed(db, 121) == 0);                           /* still live */
    j = oc_job_new(OC_JOB_DELETE, 14);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    if (r) oc_dbres_free(r);
    run_maint(w, DAY, 0, 0);
    CHECK(reclaimed(db, 120) == 1);
    CHECK(reclaimed(db, 121) == 1);

    /* A video that was never sent: after the grace window both go. */
    put_attachment(db, 130, alice, OC_DEFAULT_CHANNEL, "video/mp4", 1000, 1, 3 * DAY);
    put_attachment(db, 131, alice, OC_DEFAULT_CHANNEL, "image/jpeg", 1000, 1, 3 * DAY);
    CHECK(media_set(w, alice, 130, V, 1000, 640, 360, 131, CAP) == 0);
    run_maint(w, DAY, 0, 0);
    CHECK(reclaimed(db, 130) == 1 && reclaimed(db, 131) == 1);

    /* The age tier takes the sent video of the first message — and its poster in
     * the same pass, never the poster on its own account. */
    exec(db, "UPDATE attachments SET created_at_ms = created_at_ms + 399 * 86400000 WHERE id = 101;");
    run_maint(w, DAY, 300 * DAY, 0);
    CHECK(reclaimed(db, 100) == 1);
    CHECK(reclaimed(db, 101) == 1);

    sqlite3_close(db);
    oc_dbwriter_stop(w);
    unlink(DBPATH); unlink(DBPATH "-wal"); unlink(DBPATH "-shm");
}

int run_video_media_tests(void) {
    printf("test_video_media: attachment media fields and ATTACH_MEDIA_SET on the wire, every refusal, "
           "the media row on SEND/backfill/files, has:video, the poster's lifetime in the storage sweep\n");
    test_wire();
    test_daemon();
    return failures;
}
