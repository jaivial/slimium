/* slimium: CLI entry, global options, dispatch, batch mode. */
#include "cmd.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cmd_help(ctx_t *c, int argc, char **argv);
static int cmd_version(ctx_t *c, int argc, char **argv);
static int cmd_batch(ctx_t *c, int argc, char **argv);
int cmd_gov(ctx_t *c, int argc, char **argv); /* src/cmd_gov.c */

static const cmd_def CMDS[] = {
    {"start", cmd_start, CF_NOBATCH, "[--profile off|balanced|max] [--headful] [--user-data-dir D] [--engine PATH] [--window WxH] [--no-gov] [-- ENGINE_ARGS]  launch a browser session (+ memory governor)"},
    {"stop", cmd_stop, CF_NOBATCH, "stop the session's browser (attached sessions are only forgotten)"},
    {"status", cmd_status, 0, "alive, pid, port, tabs, memory"},
    {"attach", cmd_attach, CF_NOBATCH, "--port N  adopt an existing CDP browser as a session"},
    {"sessions", cmd_sessions, CF_NOBATCH, "list saved sessions"},
    {"tabs", cmd_tabs, 0, "list tabs (* = current)"},
    {"open", cmd_open, 0, "[URL] [--context ID] [--wait load|dom|none] [--background]  new tab, becomes current"},
    {"close", cmd_close, 0, "[ID]  close a tab (default current)"},
    {"switch", cmd_switch, 0, "ID  make tab current (id prefix ok)"},
    {"nav", cmd_nav, 0, "URL [--wait load|dom|none]  navigate current tab"},
    {"back", cmd_back, 0, "history back"},
    {"forward", cmd_forward, 0, "history forward"},
    {"reload", cmd_reload, 0, "[--hard] reload"},
    {"url", cmd_url, 0, "print current URL"},
    {"title", cmd_title, 0, "print title"},
    {"eval", cmd_eval, 0, "JS|-  evaluate (awaits promises), print value"},
    {"text", cmd_text, 0, "[SELECTOR]  innerText of body/element"},
    {"html", cmd_html, 0, "[SELECTOR]  outerHTML of document/element"},
    {"shot", cmd_shot, 0, "FILE [--full] [--selector S] [--format png|jpeg|webp] [--quality N]  screenshot"},
    {"pdf", cmd_pdf, 0, "FILE [--landscape] [--background]  print to PDF (headless)"},
    {"click", cmd_click, 0, "SELECTOR  real mouse click at element center"},
    {"dblclick", cmd_dblclick, 0, "SELECTOR  double click"},
    {"type", cmd_type, 0, "SELECTOR TEXT  focus and type key by key"},
    {"fill", cmd_fill, 0, "SELECTOR TEXT  clear and insert text"},
    {"press", cmd_press, 0, "KEY  e.g. Enter Tab Escape ArrowDown a Control+a"},
    {"hover", cmd_hover, 0, "SELECTOR  move mouse over element"},
    {"scroll", cmd_scroll, 0, "DX DY  mouse wheel scroll"},
    {"scroll-to", cmd_scroll_to, 0, "SELECTOR  scroll element into view"},
    {"select", cmd_select, 0, "SELECTOR VALUE  choose <select> option by value or label"},
    {"check", cmd_check, 0, "SELECTOR [--off]  check (or uncheck) a checkbox/radio"},
    {"wait", cmd_wait, 0, "SELECTOR [--visible] [--gone]  wait for element (--timeout MS)"},
    {"wait-load", cmd_wait_load, 0, "wait for document.readyState == complete"},
    {"wait-idle", cmd_wait_idle, 0, "[--idle MS]  wait for network idle"},
    {"cookies", cmd_cookies, 0, "list cookies"},
    {"cookie-set", cmd_cookie_set, 0, "NAME VALUE [--domain D] [--path P] [--url U] [--secure] [--httponly]"},
    {"cookies-clear", cmd_cookies_clear, 0, "delete all cookies"},
    {"storage-clear", cmd_storage_clear, 0, "clear storage of the current origin"},
    {"viewport", cmd_viewport, 0, "W H [--scale F] [--mobile] | reset"},
    {"ua", cmd_ua, 0, "STRING | --reset  user agent override"},
    {"offline", cmd_offline, 0, "on|off"},
    {"block", cmd_block, 0, "[URLPATTERN...]  block URLs (none = unblock)"},
    {"download-dir", cmd_download_dir, 0, "DIR | off  allow downloads into DIR"},
    {"frames", cmd_frames, 0, "frame tree"},
    {"console", cmd_console, 0, "collect console messages for --timeout MS (default 1000)"},
    {"mem", cmd_mem, 0, "browser process-tree memory + per-tab JS heap"},
    {"purge", cmd_purge, 0, "GC every tab + critical memory pressure"},
    {"gov", cmd_gov, CF_NOBATCH, "[--once] [--pid P] [--reclaim-s N] [--idle-ms N] [--idle-cpu PCT] [--hibernate K] [--no-gc]  memory governor (foreground)"},
    {"freeze", cmd_freeze, 0, "[ID]  freeze a tab"},
    {"thaw", cmd_thaw, 0, "[ID]  unfreeze a tab"},
    {"context-new", cmd_context_new, 0, "[--url URL]  new isolated browser context"},
    {"context-close", cmd_context_close, 0, "ID  dispose browser context"},
    {"contexts", cmd_contexts, 0, "list browser contexts"},
    {"cdp", cmd_cdp, 0, "METHOD [PARAMS_JSON] [--target browser|page]  raw DevTools call"},
    {"batch", cmd_batch, CF_NOBATCH, "run commands from stdin (one per line) over one connection"},
    {"chrome", NULL, CF_NOBATCH, "[ARGS...]  exec the engine with low-memory flags (= slimium-chrome)"},
    {"help", cmd_help, 0, "this help"},
    {"version", cmd_version, 0, "print version"},
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

static int cmd_help(ctx_t *c, int argc, char **argv) {
    (void)argc; (void)argv;
    out_t(c, "slimium %s - low-memory Chromium for agents, driven over DevTools\n\n"
             "usage: slimium [global options] COMMAND [ARGS]\n"
             "global options: --session NAME (default: $SLIM_SESSION or 'default'), --port N (any CDP browser),\n"
             "  --tab ID, --json (one JSON object per line), --timeout MS (default 30000)\n\ncommands:\n", SLIM_VERSION);
    for (size_t i = 0; i < NCMDS; i++) out_t(c, "  %-14s %s\n", CMDS[i].name, CMDS[i].help);
    slim_help_modules(c);
    out_t(c, "\nwrapper: invoked as 'slimium-chrome' (or 'slimium chrome ...') it execs the engine\n"
             "  (env SLIM_CHROMIUM, SLIM_PROFILE=off|balanced|max, SLIM_WRAPPER_LOG=FILE) in its own systemd\n"
             "  scope (SLIM_SCOPE=0: no scope) with a detached memory governor (SLIM_GOV=0: none; SLIM_GOV_LOG=FILE,\n"
             "  SLIM_GOV_IDLE_MS, SLIM_GOV_IDLE_CPU, SLIM_GOV_RECLAIM_S, SLIM_GOV_GC, SLIM_GOV_HIBERNATE=K, SLIM_GOV_FADVISE).\n"
             "exit codes: 0 ok, 1 error, 2 usage");
    if (c->json) { sb_reset(&c->oj); out_s(c, "version", SLIM_VERSION); }
    return 0;
}
static int cmd_version(ctx_t *c, int argc, char **argv) {
    (void)argc; (void)argv;
    out_t(c, "slimium %s", SLIM_VERSION);
    out_s(c, "version", SLIM_VERSION);
    return 0;
}

static const cmd_def *find_cmd(const char *n) {
    for (size_t i = 0; i < NCMDS; i++) if (!strcmp(CMDS[i].name, n)) return &CMDS[i];
    return slim_find_module(n);
}

/* Extract global options anywhere before "--". Returns 0, or -1 on usage error. */
static int parse_globals(ctx_t *c, int *argc, char **argv, int allow_conn) {
    int o = 0;
    int i;
    for (i = 0; i < *argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) break;
        const char *v = NULL; int has = 0;
        const char *names[] = {"--session", "--port", "--tab", "--timeout"};
        int which = -1;
        for (int k = 0; k < 4; k++) {
            size_t l = strlen(names[k]);
            if (!strncmp(a, names[k], l) && (a[l] == 0 || a[l] == '=')) {
                which = k;
                if (a[l] == '=') { v = a + l + 1; has = 1; }
                break;
            }
        }
        if (which >= 0) {
            if (!has) { if (i + 1 >= *argc) return usage_err(c, "option %s needs a value", names[which]); v = argv[++i]; }
            long n;
            if (which <= 1 && !allow_conn) return usage_err(c, "%s is not allowed inside batch", names[which]);
            switch (which) {
            case 0:
                if (!state_valid_name(v)) return usage_err(c, "invalid session name '%s'", v);
                c->session = v; break;
            case 1:
                if (parse_int(v, &n) || n <= 0 || n > 65535) return usage_err(c, "bad port '%s'", v);
                c->port = (int)n; break;
            case 2: c->tab = v; break;
            case 3:
                if (parse_int(v, &n) || n <= 0 || n > 86400000) return usage_err(c, "bad timeout '%s'", v);
                c->timeout = (int)n; c->timeout_set = 1; break;
            }
            continue;
        }
        if (!strcmp(a, "--json")) { c->json = 1; continue; }
        argv[o++] = a;
    }
    for (; i < *argc; i++) argv[o++] = argv[i];
    argv[o] = NULL;
    *argc = o;
    return 0;
}

static void emit(ctx_t *c, const char *name, int rc, int force_json) {
    if (rc == 0) {
        if (c->json || force_json) {
            sbuf b = {0};
            sb_puts(&b, "{\"ok\":true");
            if (force_json) { sb_puts(&b, ",\"cmd\":"); sb_jstrz(&b, name); }
            sb_add(&b, c->oj.p ? c->oj.p : "", c->oj.n);
            sb_puts(&b, "}\n");
            fwrite(b.p, 1, b.n, stdout);
            sb_free(&b);
        } else if (c->ot.n) {
            fwrite(c->ot.p, 1, c->ot.n, stdout);
            if (c->ot.p[c->ot.n - 1] != '\n') fputc('\n', stdout);
        }
    } else {
        if (c->json || force_json) {
            sbuf b = {0};
            sb_puts(&b, "{\"ok\":false");
            if (force_json) { sb_puts(&b, ",\"cmd\":"); sb_jstrz(&b, name); }
            sb_puts(&b, ",\"error\":"); sb_jstrz(&b, slim_errmsg());
            sb_puts(&b, "}\n");
            fwrite(b.p, 1, b.n, stdout);
            sb_free(&b);
        }
        fprintf(stderr, "slimium: %s%s\n", c->usage ? "usage: " : "", slim_errmsg());
    }
    fflush(stdout);
}

/* Invoke without emitting: output remains in c->oj/c->ot for MCP and embedders. */
int cli_invoke(ctx_t *c, int argc, char **argv) {
    sb_reset(&c->oj); sb_reset(&c->ot); c->usage = 0;
    if (argc < 1) return usage_err(c, "no command (try: slimium help)");
    const cmd_def *d = find_cmd(argv[0]);
    if (!d) return usage_err(c, "unknown command '%s' (try: slimium help)", argv[0]);
    if (c->in_batch && (d->flags & CF_NOBATCH)) return usage_err(c, "'%s' is not allowed in batch", d->name);
    if (!d->fn) return usage_err(c, "'%s' must be the first argument", d->name);
    return d->fn(c, argc - 1, argv + 1) ? -1 : 0;
}

/* run one command; returns process exit code */
static int run(ctx_t *c, int argc, char **argv, int force_json) {
    int rc = cli_invoke(c, argc, argv);
    emit(c, argc > 0 ? argv[0] : "", rc, force_json);
    return rc == 0 ? 0 : c->usage ? 2 : 1;
}

/* shell-like split: whitespace, '...', "..." with \ escapes. */
static int split_line(char *s, char ***out, int *n) {
    int cap = 8, k = 0;
    char **v = xmalloc(sizeof *v * (size_t)cap);
    char *w = s;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) break;
        if (*s == '#' && k == 0) break;
        char *start = w; int q = 0;
        while (*s) {
            if (!q && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) break;
            if (!q && (*s == '\'' || *s == '"')) { q = *s++; continue; }
            if (q && *s == q) { q = 0; s++; continue; }
            if (*s == '\\' && q != '\'' && s[1]) {
                s++;
                if (q == '"' && *s == 'n') { *w++ = '\n'; s++; continue; }
                if (q == '"' && *s == 't') { *w++ = '\t'; s++; continue; }
            }
            *w++ = *s++;
        }
        if (q) { free(v); return slim_seterr("unterminated quote"); }
        int end_here = *s != 0;
        *w++ = 0;
        if (end_here) s++;
        if (k + 2 > cap) { cap *= 2; v = xrealloc(v, sizeof *v * (size_t)cap); }
        v[k++] = start;
        if (w > s) { /* cannot happen: w never overtakes s */ }
    }
    v[k] = NULL;
    *out = v; *n = k;
    return 0;
}

static int cmd_batch(ctx_t *c, int argc, char **argv) {
    (void)argv;
    if (argc) return usage_err(c, "batch takes commands on stdin");
    int stop_on_err = 0;
    char *line = NULL; size_t cap = 0; ssize_t l;
    int failures = 0, count = 0;
    const char *tab0 = c->tab; int to0 = c->timeout, ts0 = c->timeout_set;
    c->in_batch = 1;
    while ((l = getline(&line, &cap, stdin)) >= 0) {
        char **v = NULL; int n = 0;
        if (split_line(line, &v, &n)) { emit(c, "", -1, 1); failures++; continue; }
        if (n == 0) { free(v); continue; }
        c->tab = tab0; c->timeout = to0; c->timeout_set = ts0;
        const char *prev_tab = c->tab;
        int json = c->json;
        c->usage = 0;
        int rc;
        if (parse_globals(c, &n, v, 0)) { emit(c, v[0], -1, 1); rc = 2; }
        else {
            if (c->tab != prev_tab) ctx_reset_page(c);
            rc = run(c, n, v, 1);
            if (c->tab != prev_tab) ctx_reset_page(c);
        }
        c->json = json;
        count++;
        if (rc) failures++;
        free(v);
        if (rc && stop_on_err) break;
    }
    free(line);
    c->in_batch = 0;
    sb_reset(&c->oj); sb_reset(&c->ot);
    c->json = 0; /* results already printed; summary error goes to stderr only */
    c->usage = 0;
    if (failures) return slim_seterr("batch: %d of %d commands failed", failures, count);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *self = path_base(argv[0]);
    if (!strcmp(self, "slimium-chrome")) return wrapper_main(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "chrome")) return wrapper_main(argc - 1, argv + 1);

    ctx_t c;
    memset(&c, 0, sizeof c);
    c.session = getenv("SLIM_SESSION");
    if (!c.session || !*c.session) c.session = "default";
    c.timeout = DEFAULT_TIMEOUT;
    int n = argc - 1;
    char **v = xmalloc(sizeof *v * (size_t)(argc + 1));
    for (int i = 0; i < n; i++) v[i] = argv[i + 1];
    v[n] = NULL;
    if (parse_globals(&c, &n, v, 1)) { emit(&c, "", -1, 0); free(v); return 2; }
    if (n >= 1 && (!strcmp(v[0], "--version") || !strcmp(v[0], "-V"))) v[0] = "version";
    if (n >= 1 && (!strcmp(v[0], "--help") || !strcmp(v[0], "-h"))) v[0] = "help";
    if (n == 0) { v[0] = "help"; v[1] = NULL; n = 1; }
    int is_batch = !strcmp(v[0], "batch");
    int rc = run(&c, n, v, 0);
    if (is_batch && rc == 0) { /* nothing extra */ }
    ctx_disconnect(&c);
    if (c.have_st) state_free(&c.st);
    sb_free(&c.oj); sb_free(&c.ot);
    free(v);
    return rc;
}
