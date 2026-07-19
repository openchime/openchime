/* Chat-render smoke (ARCH-80): links the Win32 chat module itself (build_msg_text
 * and the model helpers are pure model->text; only chat_build/layout touch HWNDs,
 * and those aren't called here) and prints the rendered message pane for a real
 * authenticated session. Proves the chat surface renders real channels/messages,
 * not just that it compiles.  win_chat_smoke.exe <workspace> <user:pass>
 */
#define main chat_main_unused        /* chat.c has no main; belt-and-suspenders */
#include "chat.c"
#undef main
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s workspace user:pass\n", argv[0]); return 2; }
    oc_endpoint ep;
    if (oc_resolve(argv[1], NULL, &ep) != OC_RESOLVE_OK) { printf("resolve FAILED\n"); return 1; }
    oc_client *c = oc_client_start_secure(ep.host, ep.port, argv[2], NULL, NULL);
    if (!c) { printf("start FAILED\n"); return 1; }
    CL = c;
    /* auth */
    const oc_model *m = NULL;
    for (int i = 0; i < 400; i++) { oc_client_tick(c); m = oc_client_model(c); if (m->authed) break; Sleep(20); }
    if (!m->authed) { printf("auth FAILED\n"); return 1; }
    /* pick first joined channel, backfill, let history stream in */
    uint64_t cid = 0;
    for (size_t i = 0; i < m->n_channels; i++) if (m->channels[i].joined) { cid = m->channels[i].channel_id; break; }
    if (!cid && m->n_channels) cid = m->channels[0].channel_id;
    g_focus_cid = cid;
    oc_client_backfill(c, cid);
    oc_client_list_users(c);
    for (int i = 0; i < 150; i++) { oc_client_tick(c); Sleep(20); }
    m = oc_client_model(c);

    printf("=== CHANNELS (%zu) ===\n", m->n_channels);
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *ch = &m->channels[i];
        printf("  %s%s  unread=%d joined=%d msgs=%zu\n",
               ch->kind == OC_CHANNEL_KIND_DM ? "@dm" : "#",
               ch->name ? ch->name : "", ch->unread, ch->joined, ch->n_msgs);
    }
    printf("=== MEMBERS (%zu) ===\n", m->n_users);
    for (size_t i = 0; i < m->n_users; i++)
        printf("  %-8s %s\n", presence_tag(oc_model_presence_of(m, m->users[i].user_id)),
               m->users[i].name);
    oc_client_send(c, cid, "hello from the win32 gui \xF0\x9F\x91\x8B");
    for (int i = 0; i < 120; i++) { oc_client_tick(c); Sleep(20); }
    m = oc_client_model(c);
    char *pane = build_msg_text(m);
    printf("=== MESSAGE PANE (focus cid=%llu) ===\n%s\n", (unsigned long long)cid, pane);
    free(pane);
    oc_client_stop(c);
    return 0;
}
