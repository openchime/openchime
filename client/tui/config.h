/*
 * OpenChime TUI — machine-local preferences (theme/mouse/layout live on the
 * terminal, so they belong here, not in the daemon). Loaded from
 * $XDG_CONFIG_HOME/openchime/config (or ~/.config/openchime/config), which is
 * created with commented defaults on first run. Portable, cross-device settings
 * live in the daemon's per-(user, client_type) settings bucket instead.
 */

#ifndef OC_TUI_CONFIG_H
#define OC_TUI_CONFIG_H

typedef struct {
    int  mouse;              /* 1 = enable mouse (click to focus, wheel to scroll) */
    int  members_panel;      /* 0 off · 1 always · 2 auto (hide when < ~74 cols) */
    int  channels_width;     /* Channels panel width, columns */
    int  members_width;      /* Members panel width, columns */
    int  time_24h;           /* 1 = 24-hour timestamps, 0 = 12-hour */
    char instance[192];      /* default instance to connect to (optional) */
} oc_config;

/* Built-in defaults. */
void oc_config_defaults(oc_config *c);

/* Load the config into *c (defaults applied first). Creates the file with
 * commented defaults if it doesn't exist. Best-effort: any parse/IO error leaves
 * the defaults in place. */
void oc_config_load(oc_config *c);

#endif /* OC_TUI_CONFIG_H */
