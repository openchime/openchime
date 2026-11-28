/* The UI harness's own tests (docs/details/TESTING.md, "The UI harness"): the
 * real desktop client driven against a real daemon, every step ending on a
 * command's answer or the daemon's barrier, never on a time. Run by
 * `make test-ui`, never by the fast or full suite: it needs the desktop build
 * and a desktop to run it on. */
#include "check.h"
#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0 1767268800000ull   /* 2026-01-01 12:00 UTC, every client's held clock */

/* A wrong token is refused, the client goes; the right one is taken. */
static void ui_harness_port(void) {
    ui_client *c = ui_client_start_tokens(&UI_DEFAULT, T0, "presented", "expected");
    CHECK(c != NULL && ui_client_gone(c));
    ui_client_stop(c);
    c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    ui_client_stop(c);
}

/* One command, one frame: a sync too. */
static void ui_harness_frames(void) {
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (!c) return;
    CHECK(ui_cmd(c, "{\"cmd\":\"frame\"}", NULL, 0) == 0);
    uint64_t f = ui_frame(c);
    for (int i = 1; i <= 3; i++) {
        CHECK(ui_cmd(c, "{\"cmd\":\"frame\"}", NULL, 0) == 0);
        CHECK(ui_frame(c) == f + (uint64_t)i);
    }
    char *t = ui_tree(c);                                  /* a tree draws no frame */
    CHECK(t != NULL);
    free(t);
    CHECK(ui_cmd(c, "{\"cmd\":\"frame\"}", NULL, 0) == 0);
    CHECK(ui_frame(c) == f + 4);
    ui_client_stop(c);

    /* A sync is one command, one frame, whatever its rounds took in. */
    ui_daemon *d = ui_daemon_start("ui-frames.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        uint64_t g = ui_frame(c);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_frame(c) == g + 1);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

static int same_shot(ui_client *c, const uint8_t *px, int w, int h) {
    int w2 = 0, h2 = 0;
    uint8_t *p2 = ui_shot(c, &w2, &h2);
    int same = p2 && w2 == w && h2 == h && !memcmp(p2, px, (size_t)w * (size_t)h * 4u);
    free(p2);
    return same;
}

/* Time moves only when the harness says: the caret's two phases (530 ms
 * each), and a notice that is there until its time is up. */
static void ui_harness_clock(void) {
    ui_daemon *d = ui_daemon_start("ui-clock.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_click_id(c, "composer") == 0);
        char *t = ui_tree(c);
        CHECK(t && strstr(t, "\"focus\":\"composer\"") != NULL);
        free(t);
        int w = 0, h = 0;
        uint8_t *lit = ui_shot(c, &w, &h);
        CHECK(lit != NULL);
        CHECK(ui_cmd(c, "{\"cmd\":\"frame\"}", NULL, 0) == 0);
        CHECK(lit && same_shot(c, lit, w, h));                /* no time, no change */
        CHECK(ui_clock_advance(c, 530) == 0);
        CHECK(lit && !same_shot(c, lit, w, h));               /* the dark phase */
        CHECK(ui_clock_advance(c, 530) == 0);
        CHECK(lit && same_shot(c, lit, w, h));                /* lit again */
        /* A notice: "Nothing unread.", from the palette's Mark all as read. */
        CHECK(ui_key(c, "K", 1, 0) == 0);
        CHECK(ui_text(c, "Mark all as read") == 0);
        CHECK(ui_key(c, "Return", 0, 0) == 0);
        CHECK(ui_click_id(c, "composer") == 0);
        CHECK(lit && !same_shot(c, lit, w, h));               /* the notice is up */
        CHECK(ui_clock_advance(c, 8480) == 0);                /* past it, caret lit (8 phases) */
        CHECK(ui_click_id(c, "composer") == 0);
        CHECK(lit && same_shot(c, lit, w, h));
        free(lit);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

/* The same screen at two DPIs is two references. */
static void ui_harness_settings(void) {
    ui_daemon *d = ui_daemon_start("ui-settings.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_cmd(c, "{\"cmd\":\"frame\"}", NULL, 0) == 0);
        int w = 0, h = 0;
        uint8_t *at96 = ui_shot(c, &w, &h);
        CHECK(ui_check(c, "settings", "home", &UI_DEFAULT) == 0);
        ui_settings hi = UI_DEFAULT;
        hi.dpi = 144;
        CHECK(ui_settings_apply(c, &hi) == 0);
        CHECK(ui_check(c, "settings", "home", &hi) == 0);
        CHECK(at96 && !same_shot(c, at96, w, h));
        free(at96);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

/* Input by id and by position, through the paths a person's input takes. */
static void ui_harness_input(void) {
    ui_daemon *d = ui_daemon_start("ui-input.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_click_id(c, "composer") == 0);
        CHECK(ui_text(c, "typed through the harness") == 0);
        CHECK(ui_key(c, "Return", 0, 0) == 0);
        CHECK(ui_sync(c) == 0);
        char *t = ui_tree(c);
        CHECK(t && strstr(t, "typed through the harness") != NULL);
        free(t);
        /* By position: the top bar's field, then Escape takes it back out. */
        CHECK(ui_click_xy(c, UI_DEFAULT.w / 2, 22) == 0);
        t = ui_tree(c);
        CHECK(t && strstr(t, "\"focus\":\"topbar.search\"") != NULL);
        free(t);
        CHECK(ui_check(c, "input", "search-focused", &UI_DEFAULT) == 0);
        /* The wheel, over the conversation list: answered, and the field
         * keeps the keyboard. */
        CHECK(ui_cmd(c, "{\"cmd\":\"wheel\",\"x\":200,\"y\":300,\"dy\":-3}", NULL, 0) == 0);
        t = ui_tree(c);
        CHECK(t && strstr(t, "\"focus\":\"topbar.search\"") != NULL);
        free(t);
        CHECK(ui_key(c, "Escape", 0, 0) == 0);
        t = ui_tree(c);
        CHECK(t && strstr(t, "\"focus\":\"topbar.search\"") == NULL);
        free(t);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

/* Two clients, one system: alice sends, both sync, bob sees it, with nothing
 * between but the barrier. */
static void ui_harness_two_clients(void) {
    ui_daemon *d = ui_daemon_start("ui-two.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *a = ui_client_start(&UI_DEFAULT, T0);
    ui_client *b = ui_client_start(&UI_DEFAULT, T0);
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        CHECK(ui_signin(a, d, "alice", "pw-alice") == 0);
        CHECK(ui_signin(b, d, "bob", "pw-bob") == 0);
        CHECK(ui_sync(a) == 0);
        CHECK(ui_sync(b) == 0);
        CHECK(ui_click_id(a, "composer") == 0);
        CHECK(ui_text(a, "hello from alice") == 0);
        CHECK(ui_key(a, "Return", 0, 0) == 0);
        CHECK(ui_sync(a) == 0);
        CHECK(ui_sync(b) == 0);
        char *t = ui_tree(b);
        CHECK(t && strstr(t, "hello from alice") != NULL);
        free(t);
        CHECK(ui_check(b, "two-clients", "bob-sees-alice", &UI_DEFAULT) == 0);
    }
    ui_client_stop(a);
    ui_client_stop(b);
    ui_daemon_stop(d);
}

/* The daemon's timed work, stepped: a 30-minute mute, the daemon's clock 31
 * minutes on and its sweep run, and the conversation unmuted. */
static void ui_harness_daemon_clock(void) {
    ui_daemon *d = ui_daemon_start("ui-dclock.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_cmd(c, "{\"cmd\":\"click\",\"id\":\"conv.1\",\"button\":\"right\"}", NULL, 0) == 0);
        char name[128] = "";
        CHECK(ui_tree_has(c, "menu.31", name, sizeof name) && !strncmp(name, "Mute", 4));
        CHECK(ui_click_id(c, "menu.31") == 0);                 /* Mute... */
        CHECK(ui_click_id(c, "menu.71") == 0);                 /* For 30 minutes */
        CHECK(ui_sync(c) == 0);
        CHECK(ui_cmd(c, "{\"cmd\":\"click\",\"id\":\"conv.1\",\"button\":\"right\"}", NULL, 0) == 0);
        CHECK(ui_tree_has(c, "menu.31", name, sizeof name) && !strncmp(name, "Unmute", 6));
        CHECK(ui_key(c, "Escape", 0, 0) == 0);
        ui_daemon_advance(d, 31u * 60000u);
        ui_daemon_sweep(d);
        CHECK(ui_sync(c) == 0);
        CHECK(ui_cmd(c, "{\"cmd\":\"click\",\"id\":\"conv.1\",\"button\":\"right\"}", NULL, 0) == 0);
        CHECK(ui_tree_has(c, "menu.31", name, sizeof name) && !strncmp(name, "Mute", 4));
        CHECK(ui_key(c, "Escape", 0, 0) == 0);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

/* An item's rect from the tree, in the frame's pixels; 0 when not there. */
static int tree_rect(ui_client *c, const char *id, int *l, int *t, int *r, int *b) {
    char *tr = ui_tree(c);
    if (!tr) return 0;
    char key[160];
    snprintf(key, sizeof key, "\"id\":\"%s\",", id);
    char *p = strstr(tr, key);
    char *pl = p ? strstr(p, "\"l\":") : NULL;
    int ok = pl && sscanf(pl, "\"l\":%d,\"t\":%d,\"r\":%d,\"b\":%d", l, t, r, b) == 4;
    free(tr);
    return ok;
}

static int tree_focus_is(ui_client *c, const char *focus) {
    char *tr = ui_tree(c), key[96];
    snprintf(key, sizeof key, "\"focus\":\"%s\"", focus);
    int is = tr && strstr(tr, key) != NULL;
    free(tr);
    return is;
}

/* The top bar as light chrome: 40 px (60 at 144 DPI) with the field inside
 * it, the field 46 % of the window and centred, a clear button only while the
 * field holds text, and help's menu of three existing actions. */
static void ui_topbar(void) {
    ui_daemon *d = ui_daemon_start("ui-topbar.db", T0);
    CHECK(d != NULL);
    if (!d) return;
    ui_client *c = ui_client_start(&UI_DEFAULT, T0);
    CHECK(c != NULL);
    if (c) {
        CHECK(ui_signin(c, d, "alice", "pw-alice") == 0);
        CHECK(ui_sync(c) == 0);
        int l, t, r, b;
        CHECK(tree_rect(c, "topbar.search", &l, &t, &r, &b));
        CHECK(t >= 0 && b <= 40);                               /* inside the 40 px bar */
        CHECK(r - l >= 514 && r - l <= 516);                    /* 46 % of 1120 */
        CHECK(l + r >= 1118 && l + r <= 1122);                  /* centred */
        CHECK(tree_rect(c, "topbar.help", &l, &t, &r, &b) && r <= 1120 && r >= 1100 && b <= 40);
        CHECK(!tree_rect(c, "topbar.clear", &l, &t, &r, &b));   /* no text, no clear */
        CHECK(ui_check(c, "topbar", "home", &UI_DEFAULT) == 0);

        ui_settings light = UI_DEFAULT;
        light.theme = "light";
        CHECK(ui_settings_apply(c, &light) == 0);
        CHECK(ui_check(c, "topbar", "home", &light) == 0);

        ui_settings hi = UI_DEFAULT;
        hi.dpi = 144;
        CHECK(ui_settings_apply(c, &hi) == 0);
        CHECK(tree_rect(c, "topbar.search", &l, &t, &r, &b));
        CHECK(t >= 0 && b <= 60);                               /* the bar at 144 DPI */
        CHECK(r - l >= 514 && r - l <= 516);
        CHECK(ui_check(c, "topbar", "home", &hi) == 0);
        hi.theme = "light";
        CHECK(ui_settings_apply(c, &hi) == 0);
        CHECK(ui_check(c, "topbar", "home", &hi) == 0);

        ui_settings wide = UI_DEFAULT;
        wide.w = 1600;
        CHECK(ui_settings_apply(c, &wide) == 0);
        CHECK(tree_rect(c, "topbar.search", &l, &t, &r, &b));
        CHECK(r - l >= 735 && r - l <= 737);                    /* 46 % of 1600 */
        CHECK(l + r >= 1598 && l + r <= 1602);
        CHECK(ui_settings_apply(c, &UI_DEFAULT) == 0);

        /* Clear: shown with text; a click empties the field and keeps the keyboard. */
        CHECK(ui_click_id(c, "topbar.search") == 0);
        CHECK(ui_text(c, "hello") == 0);
        CHECK(tree_rect(c, "topbar.clear", &l, &t, &r, &b));
        CHECK(ui_check(c, "topbar", "search-text", &UI_DEFAULT) == 0);
        CHECK(ui_click_id(c, "topbar.clear") == 0);
        CHECK(!tree_rect(c, "topbar.clear", &l, &t, &r, &b));
        CHECK(tree_focus_is(c, "topbar.search"));
        CHECK(ui_key(c, "Escape", 0, 0) == 0);

        /* Help: its menu, and one of its actions. */
        CHECK(ui_click_id(c, "topbar.help") == 0);
        /* Its three items, by command: Keyboard shortcuts, What's new, About. */
        CHECK(ui_tree_has(c, "menu.72", NULL, 0));
        CHECK(ui_tree_has(c, "menu.2704", NULL, 0));
        CHECK(ui_tree_has(c, "menu.89", NULL, 0));
        CHECK(ui_check(c, "topbar", "help-menu", &UI_DEFAULT) == 0);
        CHECK(ui_click_id(c, "menu.72") == 0);
        CHECK(!ui_tree_has(c, "menu.72", NULL, 0));
        CHECK(ui_tree_has(c, "modal.button.close", NULL, 0));      /* the shortcuts sheet is up */
        CHECK(ui_check(c, "topbar", "shortcuts", &UI_DEFAULT) == 0);
        ui_client_stop(c);
    }
    ui_daemon_stop(d);
}

int run_ui_harness_tests(void) {
    printf("ui_harness: the token, one frame per command, the held clock, settings per run, "
           "input by id and position, two clients through the barrier, the daemon's clock stepped, the top bar\n");
    ui_harness_port();
    ui_harness_frames();
    ui_harness_clock();
    ui_harness_settings();
    ui_harness_input();
    ui_harness_two_clients();
    ui_harness_daemon_clock();
    ui_topbar();
    return failures;
}
