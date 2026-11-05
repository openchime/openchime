/* Every payload decoder, each over its own reader (tests/fuzz/fuzz.h). Shared
 * by the deterministic sweep in tests/test_fuzz.c and the libFuzzer harnesses
 * under tests/fuzz/, so a new message type is added here once. */

#include "fuzz.h"

void oc_fuzz_decode_all(const uint8_t *buf, size_t len, oc_fuzz_bad_fn bad) {
    oc_rbuf p;
    oc_result rc;
    (void)rc;

    #define D(call) do { oc_rbuf_init(&p, buf, len); rc = (call); \
        if (rc != OC_OK && rc != OC_E_MALFORMED) bad(#call); } while (0)

    oc_hello hello; D(oc_decode_hello(&p, &hello));
    oc_welcome wel; D(oc_decode_welcome(&p, &wel));
    oc_reject rej; D(oc_decode_reject(&p, &rej));
    oc_auth_challenge ac; D(oc_decode_auth_challenge(&p, &ac));
    oc_auth au; D(oc_decode_auth(&p, &au));
    oc_auth_ok aok; D(oc_decode_auth_ok(&p, &aok));
    oc_logout lo; D(oc_decode_logout(&p, &lo));
    oc_send s = {0}; D(oc_decode_send(&p, &s));
    oc_send_ack sa; D(oc_decode_send_ack(&p, &sa));
    oc_broadcast b; D(oc_decode_broadcast(&p, &b));
    oc_client_ack ca; D(oc_decode_client_ack(&p, &ca));
    oc_edit ed; D(oc_decode_edit(&p, &ed));
    oc_delete de; D(oc_decode_delete(&p, &de));
    oc_msg_edited me; D(oc_decode_msg_edited(&p, &me));
    oc_msg_deleted md; D(oc_decode_msg_deleted(&p, &md));
    oc_react rc2; D(oc_decode_react(&p, &rc2));
    oc_reaction_updated ru; D(oc_decode_reaction_updated(&p, &ru));
    oc_list_reactions lr; D(oc_decode_list_reactions(&p, &lr));
    oc_send_reply sr = {0}; D(oc_decode_send_reply(&p, &sr));
    oc_thread_reply tr = {0}; D(oc_decode_thread_reply(&p, &tr));
    oc_list_thread lt; D(oc_decode_list_thread(&p, &lt));
    oc_thread th; D(oc_decode_thread(&p, &th));
    oc_thread_meta tm; D(oc_decode_thread_meta(&p, &tm));
    oc_create_channel cc; D(oc_decode_create_channel(&p, &cc));
    oc_channel_info ci; D(oc_decode_channel_info(&p, &ci));
    oc_channel_ref cr; D(oc_decode_join_channel(&p, &cr));
    oc_channel_member_op cm; D(oc_decode_invite_to_channel(&p, &cm));
    oc_set_role srr; D(oc_decode_set_role(&p, &srr));
    oc_invite_user iu; D(oc_decode_invite_user(&p, &iu));
    oc_remove_user rmu; D(oc_decode_remove_user(&p, &rmu));
    oc_user_updated uu; D(oc_decode_user_updated(&p, &uu));
    oc_invite_created ic; D(oc_decode_invite_created(&p, &ic));
    oc_backfill_done bd; D(oc_decode_backfill_done(&p, &bd));
    oc_error er; D(oc_decode_error(&p, &er));
    oc_open_dm odm; D(oc_decode_open_dm(&p, &odm));
    oc_set_presence sp; D(oc_decode_set_presence(&p, &sp));
    oc_presence_update pu; D(oc_decode_presence_update(&p, &pu));
    oc_typing ty; D(oc_decode_typing(&p, &ty));
    oc_typing_update tu2; D(oc_decode_typing_update(&p, &tu2));
    oc_set_notify_pref snp; D(oc_decode_set_notify_pref(&p, &snp));
    D(oc_decode_list_notify_prefs(&p));
    { oc_notify_pref_entry ne[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_notify_prefs(&p, ne, 4, &nc, NULL);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_notify_prefs"); }
    { oc_schedule sc; oc_schedule_day days[OC_SCHEDULE_DAYS];
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_schedule(&p, &sc, days); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_schedule"); }
    { oc_slice terms[8]; uint8_t nt = 0; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_set_keywords(&p, terms, 8, &nt); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_set_keywords"); }
    { oc_post_poll pp; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_post_poll(&p, &pp); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_post_poll"); }
    { oc_vote vo; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_vote(&p, &vo); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_vote"); }
    { oc_poll_msg po; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_poll(&p, &po); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_poll"); }
    { oc_edit_poll ep; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_edit_poll(&p, &ep); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_edit_poll"); }
    { oc_get_profile gp; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_get_profile(&p, &gp); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_get_profile"); }
    { oc_set_collapsed sc; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_set_collapsed(&p, &sc); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_set_collapsed"); }
    { oc_list_poll_voters lv; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_list_poll_voters(&p, &lv); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_list_poll_voters"); }
    { oc_poll_voter pv[4]; uint16_t npv = 0; uint64_t pmid = 0; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_poll_voters(&p, pv, 4, &npv, &pmid); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_poll_voters"); }
    { uint64_t pe[8]; uint8_t np = 0; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_set_priority(&p, pe, 8, &np); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_set_priority"); }
    { oc_slice terms[8]; uint8_t nt = 0; uint64_t pe[8]; uint8_t np = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_alert_prefs(&p, terms, 8, &nt, pe, 8, &np);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_alert_prefs"); }
    { oc_snooze sn; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_snooze(&p, &sn); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_snooze"); }
    { oc_call_join cj; uint64_t inv[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_join(&p, &cj, inv, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_join"); }
    oc_call_leave cle; D(oc_decode_call_leave(&p, &cle));
    { oc_call_joined jn; oc_call_part pr[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_joined(&p, &jn, pr, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_joined"); }
    { oc_call_roster ros; oc_call_part pr[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_roster(&p, &ros, pr, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_roster"); }
    { oc_call_invite ci; uint64_t us[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_invite(&p, &ci, us, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_invite"); }
    oc_call_decline cdc; D(oc_decode_call_decline(&p, &cdc));
    oc_call_end cen; D(oc_decode_call_end(&p, &cen));
    { oc_call_state cs; uint64_t pp[4], ii[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_state(&p, &cs, pp, 4, ii, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_state"); }
    { oc_call_key ck; oc_call_key_entry ke[4]; oc_rbuf_init(&p, buf, len);
      rc = oc_decode_call_key(&p, &ck, ke, 4); if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_call_key"); }
    oc_call_key_for ckf; D(oc_decode_call_key_for(&p, &ckf));
    oc_pdm_open po; D(oc_decode_pdm_open(&p, &po));
    oc_pdm_opened pod; D(oc_decode_pdm_opened(&p, &pod));
    oc_pdm_invite pin; D(oc_decode_pdm_invite(&p, &pin));
    oc_pdm_hs phs; D(oc_decode_pdm_hs(&p, &phs));
    oc_pdm_msg pmsg; D(oc_decode_pdm_msg(&p, &pmsg));
    oc_pdm_end pend; D(oc_decode_pdm_end(&p, &pend));
    oc_call_share csh; D(oc_decode_call_share(&p, &csh));
    oc_call_media_pkt cmu; D(oc_decode_call_media_up(&p, &cmu));
    oc_call_media_pkt cmd; D(oc_decode_call_media_down(&p, &cmd));
    oc_list_files lfz; D(oc_decode_list_files(&p, &lfz));
    oc_file_entry fez; D(oc_decode_file_entry(&p, &fez));
    oc_files fsz;      D(oc_decode_files(&p, &fsz));
    oc_upload_begin ub; D(oc_decode_upload_begin(&p, &ub));
    oc_upload_ready urd; D(oc_decode_upload_ready(&p, &urd));
    oc_upload_chunk uc; D(oc_decode_upload_chunk(&p, &uc));
    oc_upload_ack uak; D(oc_decode_upload_ack(&p, &uak));
    oc_upload_end ue2; D(oc_decode_upload_end(&p, &ue2));
    oc_upload_ok uok; D(oc_decode_upload_ok(&p, &uok));
    oc_download_begin db; D(oc_decode_download_begin(&p, &db));
    oc_download_info di; D(oc_decode_download_info(&p, &di));
    oc_download_chunk dc; D(oc_decode_download_chunk(&p, &dc));
    oc_download_end den; D(oc_decode_download_end(&p, &den));
    oc_transfer_cancel tc; D(oc_decode_transfer_cancel(&p, &tc));
    oc_create_webhook cwh; D(oc_decode_create_webhook(&p, &cwh));
    D(oc_decode_list_groups(&p));
    oc_create_group cgr; D(oc_decode_create_group(&p, &cgr));
    oc_update_group ugr; D(oc_decode_update_group(&p, &ugr));
    oc_group_ref grf; D(oc_decode_group_ref(&p, &grf));
    static oc_group_members_op gmo; D(oc_decode_group_members_op(&p, &gmo));
    static uint64_t gmem[OC_MAX_GROUP_MEMBERS]; oc_group_info gin;
    D(oc_decode_group_info(&p, &gin, gmem, OC_MAX_GROUP_MEMBERS));
    oc_groups_end gend; D(oc_decode_groups_end(&p, &gend));
    oc_channel_group_op cgo; D(oc_decode_channel_group_op(&p, &cgo));
    uint64_t vids[OC_MAX_MEMBER_LIST]; oc_channel_via_group cvg;
    D(oc_decode_channel_via_group(&p, &cvg, vids, OC_MAX_MEMBER_LIST));
    oc_mention_unresolved_more umo; D(oc_decode_mention_unresolved_more(&p, &umo));
    uint64_t cgids[OC_MAX_CHANNEL_GROUPS]; oc_channel_groups cgs;
    D(oc_decode_channel_groups(&p, &cgs, cgids, OC_MAX_CHANNEL_GROUPS));
    oc_webhook_info whi; D(oc_decode_webhook_info(&p, &whi));
    oc_list_webhooks lwh; D(oc_decode_list_webhooks(&p, &lwh));
    oc_delete_webhook dwh; D(oc_decode_delete_webhook(&p, &dwh));
    oc_webhook_deleted wdh; D(oc_decode_webhook_deleted(&p, &wdh));
    { oc_webhook_list_entry we[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_webhook_list(&p, we, 4, &nc);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_webhook_list"); }

    /* Array/paging decoders with small caller buffers. */
    { oc_cursor cur[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_backfill_request(&p, cur, 4, &nc);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_backfill_request"); }
    { oc_channel_list_entry ce[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_channel_list(&p, ce, 4, &nc);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_channel_list"); }
    { oc_user_list_entry ue[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_user_list(&p, ue, 4, &nc);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_user_list"); }
    { oc_reaction_entry re[4]; uint16_t nc = 0; uint64_t mid = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_reactions(&p, re, 4, &nc, &mid);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_reactions"); }
    { oc_search_result_entry se[4]; uint16_t nc = 0; uint8_t tr2 = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_search_results(&p, se, 4, &nc, &tr2);
      if (rc != OC_OK && rc != OC_E_MALFORMED) bad("oc_decode_search_results"); }

    #undef D
}
