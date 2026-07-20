/*
 * tuikit demo — a standalone harness that exercises every widget with no daemon
 * and no app-core, so the toolbox is testable on its own (`make tuikit-demo`).
 * Tab cycles focus across a filterable list, two inputs (one masked), a textarea,
 * and a scrolling viewport; Ctrl-O opens a modal menu; Ctrl-Q quits. The footer
 * is generated from a tk_binding table.
 */

#include "tuikit.h"

#include <stdio.h>
#include <string.h>

static const char *CHANNELS[] = {
    "general", "random", "dev", "devops", "design", "leads", "payments",
    "support", "pcos", "kudos", "announcements", "watercooler",
};
static int chan_count(void *ud) { (void)ud; return (int)(sizeof CHANNELS / sizeof *CHANNELS); }
static void chan_text(int i, void *ud, char *buf, size_t cap, uintattr_t *fg) {
    (void)ud; *fg = TB_DEFAULT; snprintf(buf, cap, "# %s", CHANNELS[i]);
}
static void chan_filter(int i, void *ud, char *buf, size_t cap) {
    (void)ud; snprintf(buf, cap, "%s", CHANNELS[i]);
}

static const char *ACTIONS[] = { "Reply in thread", "Add reaction", "Edit", "Delete", "Copy", "Who reacted" };
static int act_count(void *ud) { (void)ud; return (int)(sizeof ACTIONS / sizeof *ACTIONS); }
static void act_text(int i, void *ud, char *buf, size_t cap, uintattr_t *fg) {
    (void)ud; *fg = TB_DEFAULT; snprintf(buf, cap, "%s", ACTIONS[i]);
}

int main(void) {
    if (tb_init() != TB_OK) { fprintf(stderr, "need a tty\n"); return 1; }
    tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);

    tk_list chans; tk_list_opts co = { NULL, chan_count, chan_text, chan_filter, 1 };
    tk_list_init(&chans, &co);
    tk_list menu; tk_list_opts mo = { NULL, act_count, act_text, NULL, 0 };
    tk_list_init(&menu, &mo);
    tk_input user, pass; tk_input_init(&user, 0, "username"); tk_input_init(&pass, 1, "password");
    tk_textarea composer; tk_textarea_init(&composer, "Message #general");

    static const char *LOGLINES[24];
    static char logbuf[24][80];
    static uintattr_t logfg[24];
    for (int i = 0; i < 24; i++) {
        snprintf(logbuf[i], sizeof logbuf[i], "  line %2d — scrollable viewport content", i + 1);
        LOGLINES[i] = logbuf[i]; logfg[i] = (i % 3 == 0) ? TB_CYAN : TB_DEFAULT;
    }
    tk_viewport vp; tk_viewport_init(&vp, 1);
    tk_viewport_set(&vp, LOGLINES, logfg, 24);

    static const tk_binding FOOTER[] = {
        { TB_KEY_TAB, 0, "Tab", "focus", 1 }, { 0, 0, "type", "edit", 1 },
        { TB_KEY_CTRL_O, 0, "^O", "menu", 1 }, { TB_KEY_CTRL_Q, 0, "^Q", "quit", 1 },
    };

    int focus = 0, modal = 0, running = 1;
    while (running) {
        int W = tb_width(), H = tb_height();
        tb_clear();
        /* left: channel list */
        int lw = 24;
        tk_panel(0, 0, lw, H - 1, focus == 0 ? "Channels (/ filter)*" : "Channels (/ filter)", focus == 0);
        tk_rect lr = { 1, 1, lw - 2, H - 3 }; tk_list_draw(&chans, lr);
        /* right column */
        int rx = lw + 1, rw = W - rx - 1;
        tk_text(rx, 1, W, "Username", TB_YELLOW | TB_BOLD, TB_DEFAULT);
        tk_input_draw(&user, (tk_rect){ rx, 2, rw, 1 }, focus == 1);
        tk_text(rx, 4, W, "Password", TB_YELLOW | TB_BOLD, TB_DEFAULT);
        tk_input_draw(&pass, (tk_rect){ rx, 5, rw, 1 }, focus == 2);
        tk_text(rx, 7, W, "Composer (textarea)", TB_YELLOW | TB_BOLD, TB_DEFAULT);
        tk_textarea_draw(&composer, (tk_rect){ rx, 8, rw, 3 }, focus == 3);
        tk_panel(rx - 1, 12, rw + 2, H - 14, focus == 4 ? "Viewport*" : "Viewport", focus == 4);
        tk_viewport_draw(&vp, (tk_rect){ rx, 13, rw, H - 16 });
        /* footer */
        tk_help_footer(H - 1, 0, W, FOOTER, (int)(sizeof FOOTER / sizeof *FOOTER),
                       TB_WHITE, TB_BLACK | TB_BOLD);
        if (modal) {
            tk_rect in = tk_modal_begin(W, H, 32, 10, "Message actions");
            tk_list_draw(&menu, in);
        }
        tb_present();

        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) continue;
        if (ev.key == TB_KEY_CTRL_Q) break;

        if (modal) {
            tk_result r = tk_list_handle(&menu, &ev);
            if (r == TK_SELECT || r == TK_CANCEL) modal = 0;
            continue;
        }
        if (ev.key == TB_KEY_CTRL_O) { modal = 1; continue; }
        if (ev.key == TB_KEY_TAB)    { focus = (focus + 1) % 5; continue; }
        switch (focus) {
            case 0: tk_list_handle(&chans, &ev); break;
            case 1: tk_input_handle(&user, &ev); break;
            case 2: tk_input_handle(&pass, &ev); break;
            case 3: tk_textarea_handle(&composer, &ev); break;
            case 4: tk_viewport_handle(&vp, &ev, H - 16); break;
        }
    }
    tk_list_free(&chans); tk_list_free(&menu);
    tb_shutdown();
    return 0;
}
