/* Model Context Protocol stdio server, C11 + libc/POSIX only.
 * One ctx_t (and therefore one browser WebSocket) is reused by every tool.
 * Tool arguments and results are never logged. stdout carries JSON-RPC only. */
#include "mcp.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MCP_MAX_MESSAGE (16u << 20)
#define MCP_MAX_ARGS 128
#define S_STR "{\"type\":\"string\"}"
#define S_BOOL "{\"type\":\"boolean\"}"
#define S_ARGS "{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":128}"
#define P(n,s,r) {n,s,r}

typedef struct { const char *name, *schema; int required; } prop_def;
typedef enum {
    T_STATUS, T_START, T_STOP, T_TABS, T_OPEN, T_CLOSE, T_SWITCH, T_NAV,
    T_FORWARD, T_CLICK, T_FILL, T_TYPE, T_PRESS, T_WAIT, T_EVAL, T_READ,
    T_SHOT, T_VIEWPORT, T_CLI, T_BATCH, T_CDP
} tool_kind;
typedef struct {
    const char *name, *description, *command;
    const prop_def *props; size_t nprops;
    tool_kind kind;
} tool_def;

static const prop_def P_START[] = {
    P("port","{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535}",0),
    P("profile","{\"type\":\"string\",\"enum\":[\"off\",\"balanced\",\"max\"]}",0),
    P("userDataDir",S_STR,0), P("headful",S_BOOL,0),
    P("window","{\"type\":\"string\",\"pattern\":\"^[0-9]+x[0-9]+$\"}",0),
    P("noGovernor",S_BOOL,0),
};
static const prop_def P_OPEN[] = {
    P("url",S_STR,0), P("context",S_STR,0),
    P("wait","{\"type\":\"string\",\"enum\":[\"load\",\"dom\",\"none\"]}",0),
    P("background",S_BOOL,0),
};
static const prop_def P_CLOSE[] = { P("id",S_STR,0) };
static const prop_def P_SWITCH[] = { P("id",S_STR,1) };
static const prop_def P_NAV[] = {
    P("url",S_STR,1), P("wait","{\"type\":\"string\",\"enum\":[\"load\",\"dom\",\"none\"]}",0), P("tab",S_STR,0),
};
static const prop_def P_ARGS[] = { P("args",S_ARGS,0) };
static const prop_def P_CLICK[] = { P("selector",S_STR,1), P("tab",S_STR,0) };
static const prop_def P_FILL[] = { P("selector",S_STR,1), P("text",S_STR,1), P("tab",S_STR,0) };
static const prop_def P_PRESS[] = { P("key",S_STR,1), P("tab",S_STR,0) };
static const prop_def P_WAIT[] = {
    P("selector",S_STR,1), P("visible",S_BOOL,0), P("gone",S_BOOL,0),
    P("timeoutMs","{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400000}",0), P("tab",S_STR,0),
};
static const prop_def P_EVAL[] = { P("expression",S_STR,1), P("tab",S_STR,0) };
static const prop_def P_READ[] = { P("selector",S_STR,0), P("tab",S_STR,0) };
static const prop_def P_SHOT[] = {
    P("path",S_STR,1), P("full",S_BOOL,0), P("selector",S_STR,0),
    P("format","{\"type\":\"string\",\"enum\":[\"png\",\"jpeg\",\"webp\"]}",0),
    P("quality","{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}",0), P("tab",S_STR,0),
};
static const prop_def P_VIEWPORT[] = {
    P("width","{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000000}",0),
    P("height","{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000000}",0),
    P("scale","{\"type\":\"number\",\"exclusiveMinimum\":0,\"maximum\":10}",0),
    P("mobile",S_BOOL,0), P("reset",S_BOOL,0), P("tab",S_STR,0),
};
static const prop_def P_CLI[] = {
    P("argv","{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1,\"maxItems\":128}",1),
};
static const prop_def P_BATCH[] = {
    P("commands","{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1,\"maxItems\":128},\"minItems\":1,\"maxItems\":128}",1),
    P("stopOnError",S_BOOL,0),
};
static const prop_def P_CDP[] = {
    P("method",S_STR,1), P("params","{\"type\":\"object\"}",0),
    P("target","{\"type\":\"string\",\"enum\":[\"auto\",\"browser\",\"page\"]}",0), P("tab",S_STR,0),
};

#define NP(a) (sizeof(a) / sizeof((a)[0]))
static const tool_def TOOLS[] = {
    {"browser_status","Browser/session liveness, profile, tabs, and memory.","status",NULL,0,T_STATUS},
    {"browser_start","Start the selected session's Chromium browser.","start",P_START,NP(P_START),T_START},
    {"browser_stop","Stop the selected session's own browser (or detach an attached session).","stop",NULL,0,T_STOP},
    {"tabs","List page tabs and mark the session's current tab.","tabs",NULL,0,T_TABS},
    {"open","Open a tab and make it current.","open",P_OPEN,NP(P_OPEN),T_OPEN},
    {"close","Close a tab by id, or the current tab when id is omitted.","close",P_CLOSE,NP(P_CLOSE),T_CLOSE},
    {"switch","Switch to a tab by id or unique prefix.","switch",P_SWITCH,NP(P_SWITCH),T_SWITCH},
    {"navigate","Navigate the current tab and wait for load, DOM, or no event.","nav",P_NAV,NP(P_NAV),T_NAV},
    {"snapshot","Forward optional arguments to the merged snapshot command (CSS/ref snapshots are module-provided).","snapshot",P_ARGS,NP(P_ARGS),T_FORWARD},
    {"click","Click the center of a CSS or XPath selector.","click",P_CLICK,NP(P_CLICK),T_CLICK},
    {"fill","Replace a field's contents using a selector; input text is not logged.","fill",P_FILL,NP(P_FILL),T_FILL},
    {"type","Focus a selector and type text with key events; input text is not logged.","type",P_FILL,NP(P_FILL),T_TYPE},
    {"press","Press a named key or modifier combination.","press",P_PRESS,NP(P_PRESS),T_PRESS},
    {"wait","Wait for a selector, optionally visible or gone.","wait",P_WAIT,NP(P_WAIT),T_WAIT},
    {"eval","Evaluate JavaScript with promises awaited and values returned as JSON.","eval",P_EVAL,NP(P_EVAL),T_EVAL},
    {"text","Read visible text, optionally from one selector.","text",P_READ,NP(P_READ),T_READ},
    {"html","Read HTML, optionally from one selector.","html",P_READ,NP(P_READ),T_READ},
    {"screenshot","Write a screenshot to a requested file and return file/byte/dimension metadata, not inline data.","shot",P_SHOT,NP(P_SHOT),T_SHOT},
    {"viewport","Set width and height, or reset the saved viewport override.","viewport",P_VIEWPORT,NP(P_VIEWPORT),T_VIEWPORT},
    {"record","Forward arguments to the merged media module's record command.","record",P_ARGS,NP(P_ARGS),T_FORWARD},
    {"profile","Forward arguments to a merged profile module; MCP adds no named user-profile behavior.","profile",P_ARGS,NP(P_ARGS),T_FORWARD},
    {"slim_cli","Run one slimium argv array in-process. Process-entry and recursive commands are rejected.","",P_CLI,NP(P_CLI),T_CLI},
    {"slim_batch","Run argv arrays sequentially over this same context and connection.","",P_BATCH,NP(P_BATCH),T_BATCH},
    {"slim_cdp","Call a raw DevTools method with an object of parameters on the browser or page target.","cdp",P_CDP,NP(P_CDP),T_CDP},
};

typedef struct { char **v; int n, cap, overflow; } argv_t;
typedef struct {
    ctx_t *c;
    const char *session, *tab, *profile;
    int port, timeout, timeout_set;
    FILE *log; int close_log;
    sbuf tools;
    char *cancelled[16]; size_t ncancel;
} server_t;
typedef struct { int rc; char error[512]; } invoke_result;

static void av_add_owned(argv_t *a, char *s) {
    if (a->n >= MCP_MAX_ARGS) { free(s); a->overflow = 1; return; }
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = xrealloc(a->v, sizeof *a->v * (size_t)a->cap);
    }
    a->v[a->n++] = s;
}
static void av_add(argv_t *a, const char *s) { av_add_owned(a, xstrdup(s)); }
static void av_free(argv_t *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v); memset(a, 0, sizeof *a);
}
static void av_num(argv_t *a, jv v) { av_add_owned(a, xstrndup(v.p, v.n)); }

static int member(jv obj, const char *name, jtype type, jv *out) {
    if (!json_get(obj, name, out)) { memset(out, 0, sizeof *out); return 0; }
    if (out->t != type) return slim_seterr("'%s' has the wrong JSON type", name);
    return 0;
}
static int validate_args(const tool_def *t, jv args) {
    if (args.t == J_NONE || args.t == J_NULL) { args.p = "{}"; args.n = 2; args.t = J_OBJ; }
    if (args.t != J_OBJ) return slim_seterr("tool arguments must be an object");
    size_t pos = 0; jv key, val;
    while (json_next(args, &pos, &key, &val)) {
        size_t i;
        for (i = 0; i < t->nprops; i++) if (json_str_eq(key, t->props[i].name)) break;
        if (i == t->nprops) {
            char *k = json_str(key);
            int r = slim_seterr("unknown argument '%s' for %s", k, t->name);
            free(k); return r;
        }
    }
    for (size_t i = 0; i < t->nprops; i++)
        if (t->props[i].required && !json_get(args, t->props[i].name, NULL))
            return slim_seterr("%s requires '%s'", t->name, t->props[i].name);
    return 0;
}
static int put_str(argv_t *av, jv args, const char *name, const char *flag) {
    jv v; if (member(args, name, J_STR, &v)) return -1;
    if (v.t == J_NONE) return 0;
    if (flag) av_add(av, flag);
    av_add_owned(av, json_str(v));
    return 0;
}
static int put_num(argv_t *av, jv args, const char *name, const char *flag) {
    jv v; if (member(args, name, J_NUM, &v)) return -1;
    if (v.t == J_NONE) return 0;
    if (flag) av_add(av, flag);
    av_num(av, v);
    return 0;
}
static int put_flag(argv_t *av, jv args, const char *name, const char *flag) {
    jv v; if (member(args, name, J_BOOL, &v)) return -1;
    if (v.t != J_NONE && json_bool(v)) av_add(av, flag);
    return 0;
}
static int put_array(argv_t *av, jv value) {
    if (value.t != J_ARR) return slim_seterr("command arguments must be an array of strings");
    size_t pos = 0; jv s;
    while (json_next(value, &pos, NULL, &s)) {
        if (s.t != J_STR) return slim_seterr("every command argument must be a string");
        av_add_owned(av, json_str(s));
    }
    return av->overflow ? slim_seterr("too many command arguments") : 0;
}
static int put_named_array(argv_t *av, jv args, const char *name) {
    jv v; if (member(args, name, J_ARR, &v)) return -1;
    return v.t == J_NONE ? 0 : put_array(av, v);
}
static int allowed(jv args, const char *name, const char *const values[]) {
    jv v;
    if (member(args, name, J_STR, &v)) return -1;
    if (v.t == J_NONE) return 0;
    for (int i = 0; values[i]; i++) if (json_str_eq(v, values[i])) return 0;
    return slim_seterr("invalid value for '%s'", name);
}
static int put_common(argv_t *av, jv args) {
    if (put_str(av, args, "tab", "--tab")) return -1;
    return put_num(av, args, "timeoutMs", "--timeout");
}

static int build_args(server_t *s, const tool_def *t, jv args, argv_t *av) {
    static const char *const profiles[] = {"off","balanced","max",NULL};
    static const char *const waits[] = {"load","dom","none",NULL};
    static const char *const formats[] = {"png","jpeg","webp",NULL};
    static const char *const targets[] = {"auto","browser","page",NULL};
    if (validate_args(t, args)) return -1;
    if (t->kind != T_CLI) av_add(av, t->command);
    switch (t->kind) {
    case T_STATUS: case T_STOP: case T_TABS: break;
    case T_START:
        if (allowed(args, "profile", profiles)) return -1;
        if (json_get(args, "profile", NULL)) { if (put_str(av, args, "profile", "--profile")) return -1; }
        else if (s->profile) { av_add(av, "--profile"); av_add(av, s->profile); }
        if (put_num(av, args, "port", "--port") || put_str(av, args, "userDataDir", "--user-data-dir") ||
            put_flag(av, args, "headful", "--headful") || put_str(av, args, "window", "--window") ||
            put_flag(av, args, "noGovernor", "--no-gov")) return -1;
        break;
    case T_OPEN:
        if (allowed(args, "wait", waits) || put_str(av, args, "url", NULL) ||
            put_str(av, args, "context", "--context") || put_str(av, args, "wait", "--wait") ||
            put_flag(av, args, "background", "--background")) return -1;
        break;
    case T_CLOSE: case T_SWITCH:
        if (put_str(av, args, "id", NULL)) return -1;
        break;
    case T_NAV:
        if (allowed(args, "wait", waits) || put_str(av, args, "url", NULL) ||
            put_str(av, args, "wait", "--wait") || put_common(av, args)) return -1;
        break;
    case T_FORWARD:
        if (put_named_array(av, args, "args")) return -1;
        break;
    case T_CLICK:
        if (put_str(av, args, "selector", NULL) || put_common(av, args)) return -1;
        break;
    case T_FILL: case T_TYPE:
        if (put_str(av, args, "selector", NULL) || put_str(av, args, "text", NULL) || put_common(av, args)) return -1;
        break;
    case T_PRESS:
        if (put_str(av, args, "key", NULL) || put_common(av, args)) return -1;
        break;
    case T_WAIT:
        if (put_str(av, args, "selector", NULL) || put_flag(av, args, "visible", "--visible") ||
            put_flag(av, args, "gone", "--gone") || put_common(av, args)) return -1;
        break;
    case T_EVAL:
        if (put_str(av, args, "expression", NULL) || put_common(av, args)) return -1;
        break;
    case T_READ:
        if (put_str(av, args, "selector", NULL) || put_common(av, args)) return -1;
        break;
    case T_SHOT:
        if (allowed(args, "format", formats) || put_str(av, args, "path", NULL) ||
            put_flag(av, args, "full", "--full") || put_str(av, args, "selector", "--selector") ||
            put_str(av, args, "format", "--format") || put_num(av, args, "quality", "--quality") ||
            put_common(av, args)) return -1;
        break;
    case T_VIEWPORT: {
        jv reset, width, height;
        if (member(args, "reset", J_BOOL, &reset) || member(args, "width", J_NUM, &width) ||
            member(args, "height", J_NUM, &height)) return -1;
        if (reset.t != J_NONE && json_bool(reset)) {
            if (width.t != J_NONE || height.t != J_NONE || json_get(args, "scale", NULL) || json_get(args, "mobile", NULL))
                return slim_seterr("viewport reset cannot be combined with size, scale, or mobile");
            av_add(av, "reset");
        } else {
            if (width.t == J_NONE || height.t == J_NONE) return slim_seterr("viewport requires width and height, or reset:true");
            av_num(av, width); av_num(av, height);
            if (put_num(av, args, "scale", "--scale") || put_flag(av, args, "mobile", "--mobile")) return -1;
        }
        if (put_common(av, args)) return -1;
        break;
    }
    case T_CLI:
        if (put_named_array(av, args, "argv")) return -1;
        if (!av->n) return slim_seterr("argv must not be empty");
        break;
    case T_CDP: {
        if (allowed(args, "target", targets) || put_str(av, args, "method", NULL)) return -1;
        jv params;
        if (member(args, "params", J_OBJ, &params)) return -1;
        if (params.t != J_NONE) av_add_owned(av, xstrndup(params.p, params.n));
        if (json_get(args, "target", NULL)) {
            jv target; json_get(args, "target", &target);
            if (!json_str_eq(target, "auto") && put_str(av, args, "target", "--target")) return -1;
        }
        if (put_common(av, args)) return -1;
        break;
    }
    case T_BATCH: break;
    }
    return av->overflow ? slim_seterr("too many command arguments") : 0;
}

static void clear_context(ctx_t *c) {
    ctx_disconnect(c);
    if (c->have_st) { state_free(&c->st); c->have_st = 0; }
}
static int forbidden_command(const char *cmd) {
    static const char *const names[] = {"mcp","pipe","batch","chrome","gov",NULL};
    for (int i = 0; names[i]; i++) if (!strcmp(cmd, names[i])) return 1;
    return 0;
}
static int same_str(const char *a, const char *b) {
    return a == b || (a && b && !strcmp(a, b));
}
static void invocation_error(invoke_result *ir, const char *message) {
    ir->rc = -1;
    snprintf(ir->error, sizeof ir->error, "%s", message);
}

/* Global options follow main.c's placement rules. Session/port changes are
 * scoped to this call; tab overrides never become the session's current tab. */
static void invoke_argv(server_t *s, char **args, int argc, int in_batch, invoke_result *ir) {
    ctx_t *c = s->c;
    const char *session = s->session, *tab = s->tab;
    int port = s->port, timeout = s->timeout, tab_given = 0;
    ir->rc = -1; ir->error[0] = 0;
    sb_reset(&c->oj); sb_reset(&c->ot); c->usage = 0;
    char **v = xmalloc(sizeof *v * (size_t)(argc + 1));
    int n = 0;
    for (int i = 0; i < argc; i++) {
        char *a = args[i];
        if (!strcmp(a, "--")) { for (; i < argc; i++) v[n++] = args[i]; break; }
        if (!strcmp(a, "--json")) continue;
        static const char *const names[] = {"--session","--port","--tab","--timeout"};
        int which = -1, has = 0;
        const char *val = NULL;
        for (int k = 0; k < 4; k++) {
            size_t l = strlen(names[k]);
            if (!strncmp(a, names[k], l) && (a[l] == 0 || a[l] == '=')) {
                which = k; has = a[l] == '='; val = has ? a + l + 1 : NULL; break;
            }
        }
        if (which < 0) { v[n++] = a; continue; }
        if (!has) {
            if (i + 1 >= argc) { invocation_error(ir, "global option needs a value"); goto out; }
            val = args[++i];
        }
        long num;
        switch (which) {
        case 0:
            if (!state_valid_name(val)) { invocation_error(ir, "invalid session name"); goto out; }
            session = val; break;
        case 1:
            if (parse_int(val, &num) || num <= 0 || num > 65535) { invocation_error(ir, "bad port"); goto out; }
            port = (int)num; break;
        case 2: tab = val; tab_given = 1; break;
        case 3:
            if (parse_int(val, &num) || num <= 0 || num > 86400000) { invocation_error(ir, "bad timeout"); goto out; }
            timeout = (int)num; break;
        }
    }
    v[n] = NULL;
    if (!n) { invocation_error(ir, "no command"); goto out; }
    if (!strcmp(v[0], "--version") || !strcmp(v[0], "-V")) v[0] = (char *)"version";
    if (!strcmp(v[0], "--help") || !strcmp(v[0], "-h")) v[0] = (char *)"help";
    if (forbidden_command(v[0])) {
        snprintf(ir->error, sizeof ir->error, "command '%s' is not available through MCP", v[0]);
        goto out;
    }
    if (!strcmp(v[0], "eval") || !strcmp(v[0], "shot") || !strcmp(v[0], "pdf")) {
        for (int i = 1; i < n; i++) if (!strcmp(v[i], "-")) {
            snprintf(ir->error, sizeof ir->error, "'-' is not allowed for %s through MCP (stdin/stdout belong to JSON-RPC)", v[0]);
            goto out;
        }
    }

    int mode_change = !same_str(session, s->session) || port != s->port;
    if (mode_change && !tab_given) tab = NULL;
    int tab_change = !same_str(tab, c->tab);
    if (mode_change) clear_context(c);
    else if (tab_change && c->tid) ctx_reset_page(c);
    c->session = session; c->port = port; c->tab = tab;
    c->timeout = timeout; c->timeout_set = timeout != s->timeout || s->timeout_set;
    c->json = 1; c->in_batch = in_batch;
    ir->rc = cli_invoke(c, n, v);
    if (ir->rc) snprintf(ir->error, sizeof ir->error, "%s", slim_errmsg());

    int lifecycle = !strcmp(v[0], "start") || !strcmp(v[0], "stop") || !strcmp(v[0], "attach");
    if ((!ir->rc && lifecycle) || (c->connected && !c->cdp.open)) clear_context(c);
    if (mode_change) {
        clear_context(c);
        c->session = s->session; c->port = s->port; c->tab = s->tab;
    } else {
        if ((tab_change || c->tab != tab) && c->tid) ctx_reset_page(c);
        c->tab = s->tab;
    }
    c->timeout = s->timeout; c->timeout_set = s->timeout_set; c->in_batch = 0;
out:
    free(v);
}

static void format_invoke(ctx_t *c, const invoke_result *ir, sbuf *out) {
    if (ir->rc) {
        sb_puts(out, "{\"ok\":false,\"error\":");
        sb_jstrz(out, ir->error[0] ? ir->error : "command failed");
        sb_putc(out, '}');
    } else {
        sb_puts(out, "{\"ok\":true");
        if (c->oj.n) sb_add(out, c->oj.p, c->oj.n);
        else if (c->ot.n) { sb_puts(out, ",\"text\":"); sb_jstr(out, c->ot.p, c->ot.n); }
        sb_putc(out, '}');
    }
    if (c->oj.cap > (1u << 20)) sb_free(&c->oj);
    if (c->ot.cap > (1u << 20)) sb_free(&c->ot);
}

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned le16(const unsigned char *p) { return ((unsigned)p[1] << 8) | p[0]; }
static unsigned le24(const unsigned char *p) { return ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0]; }
static unsigned be32(const unsigned char *p) { return ((unsigned)be16(p) << 16) | be16(p + 2); }
static void image_dimensions(const char *path, unsigned *width, unsigned *height) {
    *width = *height = 0;
    char *data = NULL; size_t n = 0;
    if (read_file(path, &data, &n, 1u << 20)) return;
    unsigned char *d = (unsigned char *)data;
    if (n >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        *width = be32(d + 16); *height = be32(d + 20);
    } else if (n >= 30 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
        if (!memcmp(d + 12, "VP8X", 4)) {
            *width = le24(d + 24) + 1; *height = le24(d + 27) + 1;
        } else if (!memcmp(d + 12, "VP8L", 4) && d[20] == 0x2f) {
            *width = 1 + d[21] + ((d[22] & 0x3f) << 8);
            *height = 1 + (d[22] >> 6) + (d[23] << 2) + ((d[24] & 0x0f) << 10);
        } else if (!memcmp(d + 12, "VP8 ", 4) && d[23] == 0x9d && d[24] == 0x01 && d[25] == 0x2a) {
            *width = le16(d + 26) & 0x3fff; *height = le16(d + 28) & 0x3fff;
        }
    } else if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xff) { i++; continue; }
            while (i < n && d[i] == 0xff) i++;
            if (i >= n) break;
            unsigned marker = d[i++];
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9)) continue;
            if (i + 2 > n) break;
            unsigned len = be16(d + i);
            if (len < 2 || i + len > n) break;
            int sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                      (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
            if (sof && len >= 7) {
                *height = be16(d + i + 3); *width = be16(d + i + 5); break;
            }
            i += len;
        }
    }
    free(data);
}

static int run_tool(server_t *s, const tool_def *t, jv args, sbuf *out, int *is_error) {
    *is_error = 0;
    if (t->kind == T_BATCH) {
        if (validate_args(t, args)) return -1;
        jv commands, stop;
        if (member(args, "commands", J_ARR, &commands) || member(args, "stopOnError", J_BOOL, &stop)) return -1;
        if (commands.t == J_NONE || !json_len(commands)) return slim_seterr("commands must not be empty");
        sb_puts(out, "{\"results\":[");
        size_t pos = 0, index = 0; jv item; int failures = 0, stopped = 0;
        while (json_next(commands, &pos, NULL, &item)) {
            argv_t av = {0};
            if (put_array(&av, item)) { av_free(&av); sb_free(out); return -1; }
            if (!av.n) { av_free(&av); sb_free(out); return slim_seterr("batch argv must not be empty"); }
            invoke_result ir;
            invoke_argv(s, av.v, av.n, 1, &ir);
            sbuf one = {0}; format_invoke(s->c, &ir, &one);
            if (index++) sb_putc(out, ',');
            sb_printf(out, "{\"index\":%zu,\"result\":", index - 1);
            sb_add(out, one.p, one.n); sb_putc(out, '}');
            sb_free(&one);
            if (ir.rc) {
                failures++;
                if (stop.t != J_NONE && json_bool(stop)) { stopped = 1; av_free(&av); break; }
            }
            av_free(&av);
        }
        sb_putc(out, ']');
        if (stopped) sb_puts(out, ",\"stopped\":true");
        if (failures) *is_error = 1;
        sb_puts(out, failures ? ",\"ok\":false}" : ",\"ok\":true}");
        return 0;
    }

    argv_t av = {0};
    if (build_args(s, t, args, &av)) { av_free(&av); return -1; }
    invoke_result ir;
    invoke_argv(s, av.v, av.n, 0, &ir);
    if (t->kind == T_SHOT && !ir.rc) {
        char *path = json_get_str(args, "path");
        unsigned w, h;
        if (path) image_dimensions(path, &w, &h);
        else w = h = 0;
        if (w && h) { out_i(s->c, "width", w); out_i(s->c, "height", h); }
        free(path);
    }
    format_invoke(s->c, &ir, out);
    *is_error = ir.rc != 0;
    av_free(&av);
    return 0;
}

static void log_event(server_t *s, const char *event, const char *detail, int rc) {
    if (!s->log) return;
    fprintf(s->log, "%lld %s", now_ms(), event);
    if (detail) fprintf(s->log, " %s", detail);
    if (rc >= 0) fprintf(s->log, " rc=%d", rc);
    fputc('\n', s->log); fflush(s->log);
}

static int write_response(server_t *s, const sbuf *b) {
    (void)s;
    return fwrite(b->p, 1, b->n, stdout) == b->n && fputc('\n', stdout) != EOF && fflush(stdout) == 0 ? 0 : -1;
}
static void begin_response(sbuf *b, jv id) {
    sb_puts(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id.p) sb_add(b, id.p, id.n); else sb_puts(b, "null");
}
static void send_result(server_t *s, jv *id, int notification, const char *result, size_t n) {
    if (notification) return;
    sbuf b = {0}; begin_response(&b, id ? *id : (jv){0});
    sb_puts(&b, ",\"result\":"); sb_add(&b, result, n); sb_putc(&b, '}');
    if (write_response(s, &b)) log_event(s, "stdout-write-failed", NULL, -1);
    sb_free(&b);
}
static void send_error(server_t *s, jv *id, int notification, int code, const char *message) {
    if (notification) return;
    sbuf b = {0}; begin_response(&b, id ? *id : (jv){0});
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_jstrz(&b, message); sb_puts(&b, "}}");
    if (write_response(s, &b)) log_event(s, "stdout-write-failed", NULL, -1);
    sb_free(&b);
}
static void send_tool(server_t *s, jv *id, int notification, const sbuf *result, int is_error) {
    sbuf b = {0};
    sb_puts(&b, "{\"content\":[{\"type\":\"text\",\"text\":");
    sb_jstr(&b, result->p, result->n);
    sb_printf(&b, "}],\"isError\":%s}", is_error ? "true" : "false");
    send_result(s, id, notification, b.p, b.n);
    sb_free(&b);
}

static void build_tools(server_t *s) {
    if (s->tools.n) return;
    sb_puts(&s->tools, "{\"tools\":[");
    for (size_t i = 0; i < sizeof TOOLS / sizeof TOOLS[0]; i++) {
        const tool_def *t = &TOOLS[i];
        if (i) sb_putc(&s->tools, ',');
        sb_puts(&s->tools, "{\"name\":"); sb_jstrz(&s->tools, t->name);
        sb_puts(&s->tools, ",\"description\":"); sb_jstrz(&s->tools, t->description);
        sb_puts(&s->tools, ",\"inputSchema\":{\"type\":\"object\",\"properties\":{");
        int required = 0;
        for (size_t k = 0; k < t->nprops; k++) {
            if (k) sb_putc(&s->tools, ',');
            sb_jstrz(&s->tools, t->props[k].name); sb_putc(&s->tools, ':');
            sb_puts(&s->tools, t->props[k].schema);
            if (t->props[k].required) required++;
        }
        sb_puts(&s->tools, "}");
        if (required) {
            sb_puts(&s->tools, ",\"required\":[");
            int n = 0;
            for (size_t k = 0; k < t->nprops; k++) if (t->props[k].required) {
                if (n++) sb_putc(&s->tools, ',');
                sb_jstrz(&s->tools, t->props[k].name);
            }
            sb_putc(&s->tools, ']');
        }
        sb_puts(&s->tools, ",\"additionalProperties\":false}}");
    }
    sb_puts(&s->tools, "]}");
}

static void remember_cancel(server_t *s, jv params) {
    jv id;
    if (params.t != J_OBJ || !json_get(params, "requestId", &id) || (id.t != J_STR && id.t != J_NUM)) return;
    for (size_t i = 0; i < s->ncancel; i++)
        if (strlen(s->cancelled[i]) == id.n && !memcmp(s->cancelled[i], id.p, id.n)) return;
    if (s->ncancel == sizeof s->cancelled / sizeof s->cancelled[0]) {
        free(s->cancelled[0]);
        memmove(s->cancelled, s->cancelled + 1, sizeof s->cancelled - sizeof s->cancelled[0]);
        s->ncancel--;
    }
    s->cancelled[s->ncancel++] = xstrndup(id.p, id.n);
}
static int take_cancel(server_t *s, jv id) {
    for (size_t i = 0; i < s->ncancel; i++) if (strlen(s->cancelled[i]) == id.n && !memcmp(s->cancelled[i], id.p, id.n)) {
        free(s->cancelled[i]);
        for (size_t k = i + 1; k < s->ncancel; k++) s->cancelled[k - 1] = s->cancelled[k];
        s->ncancel--;
        return 1;
    }
    return 0;
}

static void handle_document(server_t *s, jv doc) {
    jv id = {0}, method = {0}, params = {0};
    int has_id = 0, notification = 0;
    if (doc.t != J_OBJ) { send_error(s, NULL, 0, -32600, "expected one JSON-RPC object (batches are not supported)"); return; }
    if (json_get(doc, "id", &id)) {
        has_id = 1;
        if (id.t != J_STR && id.t != J_NUM && id.t != J_NULL) {
            send_error(s, NULL, 0, -32600, "id must be a string, number, or null"); return;
        }
    }
    if (!json_get(doc, "jsonrpc", &method) || !json_str_eq(method, "2.0")) {
        send_error(s, has_id ? &id : NULL, 0, -32600, "jsonrpc must be \"2.0\""); return;
    }
    if (!json_get(doc, "method", &method) || method.t != J_STR) {
        send_error(s, has_id ? &id : NULL, 0, -32600, "method must be a string"); return;
    }
    notification = !has_id;
    char *name = json_str(method);
    json_get(doc, "params", &params);

    if (!strcmp(name, "notifications/cancelled")) { remember_cancel(s, params); free(name); return; }
    if (!strcmp(name, "notifications/initialized")) { log_event(s, "initialized", NULL, -1); free(name); return; }
    if (!strcmp(name, "initialize")) {
        if (params.t != J_NONE && params.t != J_NULL && params.t != J_OBJ) {
            send_error(s, &id, notification, -32602, "initialize params must be an object"); free(name); return;
        }
        const char *version = "2025-03-26";
        jv asked;
        if (params.t == J_OBJ && json_get(params, "protocolVersion", &asked)) {
            if (asked.t != J_STR) { send_error(s, &id, notification, -32602, "protocolVersion must be a string"); free(name); return; }
            if (json_str_eq(asked, "2024-11-05") || json_str_eq(asked, "2025-03-26")) version = json_str_eq(asked, "2024-11-05") ? "2024-11-05" : "2025-03-26";
        }
        sbuf b = {0};
        sb_puts(&b, "{\"protocolVersion\":"); sb_jstrz(&b, version);
        sb_puts(&b, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"slimium\",\"title\":\"slimium low-memory Chromium\",\"version\":"); sb_jstrz(&b, SLIM_VERSION);
        sb_puts(&b, "},\"instructions\":\"Drive one persistent slimium session. Prefer snapshot/text before screenshots. Screenshot writes a file and returns metadata. slim_cli, slim_batch and slim_cdp cover merged modules and raw DevTools.\"}");
        send_result(s, &id, notification, b.p, b.n); sb_free(&b);
        log_event(s, "initialize", version, -1);
    } else if (!strcmp(name, "ping")) {
        send_result(s, &id, notification, "{}", 2);
    } else if (!strcmp(name, "tools/list")) {
        build_tools(s); send_result(s, &id, notification, s->tools.p, s->tools.n);
    } else if (!strcmp(name, "tools/call")) {
        if (params.t != J_OBJ) { send_error(s, &id, notification, -32602, "tools/call params must be an object"); free(name); return; }
        jv tool_name;
        if (!json_get(params, "name", &tool_name) || tool_name.t != J_STR) {
            send_error(s, &id, notification, -32602, "tools/call needs a string name"); free(name); return;
        }
        char *tn = json_str(tool_name);
        size_t i;
        for (i = 0; i < sizeof TOOLS / sizeof TOOLS[0]; i++) if (!strcmp(tn, TOOLS[i].name)) break;
        if (i == sizeof TOOLS / sizeof TOOLS[0]) {
            send_error(s, &id, notification, -32602, "unknown tool"); free(tn); free(name); return;
        }
        if (has_id && take_cancel(s, id)) {
            send_error(s, &id, notification, -32800, "request cancelled"); free(tn); free(name); return;
        }
        jv args;
        if (!json_get(params, "arguments", &args)) { args.p = "{}"; args.n = 2; args.t = J_OBJ; }
        sbuf result = {0}; int is_error = 0;
        if (run_tool(s, &TOOLS[i], args, &result, &is_error)) {
            send_error(s, &id, notification, -32602, slim_errmsg());
            log_event(s, "tool-invalid", TOOLS[i].name, -1);
        } else {
            send_tool(s, &id, notification, &result, is_error);
            log_event(s, "tool", TOOLS[i].name, is_error);
        }
        sb_free(&result); free(tn);
    } else {
        send_error(s, &id, notification, -32601, "method not found");
    }
    free(name);
}

static int blank_line(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!isspace((unsigned char)s[i])) return 0;
    return 1;
}
/* 1 = message (NULL means framing/size error), 0 = EOF, -1 = read error. */
static int read_message(char **msg, size_t *len) {
    char *line = NULL; size_t cap = 0; ssize_t n;
    *msg = NULL; *len = 0;
    for (;;) {
        n = getline(&line, &cap, stdin);
        if (n < 0) { free(line); return ferror(stdin) ? -1 : 0; }
        if (!blank_line(line, (size_t)n)) break;
    }
    if (n >= 15 && !strncasecmp(line, "Content-Length:", 15)) {
        char *end = NULL; errno = 0;
        const char *number = line + 15;
        while (*number == ' ' || *number == '\t') number++;
        if (*number == '-') { free(line); return 1; }
        unsigned long long want = strtoull(number, &end, 10);
        while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
        if (errno || !end || *end) { free(line); return 1; }
        while ((n = getline(&line, &cap, stdin)) >= 0 && !blank_line(line, (size_t)n)) { /* skip other headers */ }
        if (n < 0) { free(line); return ferror(stdin) ? -1 : 0; }
        if (want > MCP_MAX_MESSAGE) {
            unsigned char buf[8192]; unsigned long long left = want;
            while (left) {
                size_t take = left > sizeof buf ? sizeof buf : (size_t)left;
                size_t got = fread(buf, 1, take, stdin);
                if (!got) { free(line); return ferror(stdin) ? -1 : 0; }
                left -= got;
            }
            free(line); return 1;
        }
        char *body = xmalloc((size_t)want + 1);
        size_t got = 0;
        while (got < (size_t)want) {
            size_t r = fread(body + got, 1, (size_t)want - got, stdin);
            if (!r) { free(body); free(line); return ferror(stdin) ? -1 : 0; }
            got += r;
        }
        body[got] = 0; free(line); *msg = body; *len = got; return 1;
    }
    if ((size_t)n > MCP_MAX_MESSAGE) { free(line); return 1; }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    *msg = line; *len = (size_t)n; return 1;
}

int slim_mcp_serve(ctx_t *c, const char *memory_profile) {
    server_t s;
    memset(&s, 0, sizeof s);
    s.c = c; s.session = c->session; s.tab = c->tab; s.profile = memory_profile;
    s.port = c->port; s.timeout = c->timeout; s.timeout_set = c->timeout_set;
    /* CDP debug tracing includes input text and storage values; never enable it
     * inside MCP, whose logging contract excludes arguments and field data. */
    unsetenv("SLIM_DEBUG");
    c->json = 1;
    const char *lp = getenv("SLIM_MCP_LOG");
    if (lp && *lp && strcmp(lp, "0")) {
        if (!strcmp(lp, "1") || !strcmp(lp, "-") || !strcmp(lp, "stderr")) s.log = stderr;
        else {
            int fd = open(lp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd < 0 || !(s.log = fdopen(fd, "a"))) {
                int e = errno; if (fd >= 0) close(fd);
                return slim_seterr("SLIM_MCP_LOG %s: %s", lp, strerror(e));
            }
            s.close_log = 1;
        }
    }
    log_event(&s, "start", NULL, -1);
    int result = 0;
    for (;;) {
        char *msg; size_t len;
        int r = read_message(&msg, &len);
        if (r == 0) break;
        if (r < 0) { result = slim_seterr("MCP stdin: read failed"); break; }
        if (!msg) { send_error(&s, NULL, 0, -32700, "parse error or request too large"); continue; }
        jv doc;
        if (json_parse(msg, len, &doc)) send_error(&s, NULL, 0, -32700, "parse error");
        else handle_document(&s, doc);
        free(msg);
    }
    log_event(&s, "exit", NULL, result);
    for (size_t i = 0; i < s.ncancel; i++) free(s.cancelled[i]);
    sb_free(&s.tools);
    if (s.close_log) fclose(s.log);
    return result;
}
