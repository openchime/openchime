/*
 * OpenChime TUI — machine-local config. See config.h.
 */

#include "config.h"
#include "oc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void oc_config_defaults(oc_config *c) {
    c->mouse = 1;
    c->members_panel = 2;      /* auto */
    c->channels_width = 22;
    c->members_width = 22;
    c->time_24h = 1;
    c->workspace[0] = '\0';
}

/* Resolve the config file path into `out`; returns 0 on success. Ensures the
 * parent directory exists. */
static int config_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char dir[900];
    if (xdg && xdg[0]) {
        oc_mkdir(xdg);                              /* ensure the base exists */
        snprintf(dir, sizeof dir, "%s/openchime", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return -1;
        char base[700];
        snprintf(base, sizeof base, "%s/.config", home);   oc_mkdir(base);
        snprintf(dir, sizeof dir, "%s/.config/openchime", home);
    }
    oc_mkdir(dir);
    if ((size_t)snprintf(out, cap, "%s/config", dir) >= cap) return -1;
    return 0;
}

/* Trim leading/trailing ASCII whitespace in place, returning the start. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
    return s;
}

static void write_defaults(const char *path, const oc_config *c) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# OpenChime TUI configuration (machine-local).\n"
        "# Edit and restart. Lines are key = value; '#' starts a comment.\n"
        "\n"
        "# Mouse: click a channel/member to select, wheel to scroll.\n"
        "mouse = %s\n"
        "\n"
        "# Members panel: off | on | auto (auto hides on narrow terminals).\n"
        "members_panel = %s\n"
        "\n"
        "# Panel widths, in columns.\n"
        "channels_width = %d\n"
        "members_width = %d\n"
        "\n"
        "# Timestamps: 24h or 12h.\n"
        "time = %s\n"
        "\n"
        "# Default workspace to connect to when none is given on the command line.\n"
        "# workspace = chat.example.com\n",
        c->mouse ? "on" : "off",
        c->members_panel == 0 ? "off" : c->members_panel == 1 ? "on" : "auto",
        c->channels_width, c->members_width,
        c->time_24h ? "24h" : "12h");
    fclose(f);
}

static int truthy(const char *v) {
    return strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0;
}

void oc_config_load(oc_config *c) {
    oc_config_defaults(c);
    char path[1024];
    if (config_path(path, sizeof path) != 0) return;

    FILE *f = fopen(path, "r");
    if (!f) { write_defaults(path, c); return; }   /* first run */

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        char *hash = strchr(s, '#'); if (hash) *hash = '\0';
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (!*key) continue;
        if (strcmp(key, "mouse") == 0) c->mouse = truthy(val);
        else if (strcmp(key, "members_panel") == 0)
            c->members_panel = strcmp(val, "off") == 0 ? 0 : strcmp(val, "on") == 0 ? 1 : 2;
        else if (strcmp(key, "channels_width") == 0) { int n = atoi(val); if (n >= 10 && n <= 60) c->channels_width = n; }
        else if (strcmp(key, "members_width") == 0)  { int n = atoi(val); if (n >= 10 && n <= 60) c->members_width = n; }
        else if (strcmp(key, "time") == 0) c->time_24h = (strcmp(val, "12h") != 0);
        /* `instance` is the pre-rename spelling — still accepted so an existing
         * config file keeps working. */
        else if (strcmp(key, "workspace") == 0 || strcmp(key, "instance") == 0)
            snprintf(c->workspace, sizeof c->workspace, "%s", val);
    }
    fclose(f);
}
