/* Live DevTools schema discovery, event listening, and thin protocol wrappers. */
#include "protocol.h"
#include "hold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void text_flags(ctx_t *c, jv v) {
    jv x;
    if (json_get(v, "experimental", &x) && json_bool(x)) out_t(c, " [experimental]");
    if (json_get(v, "deprecated", &x) && json_bool(x)) out_t(c, " [deprecated]");
}

static char *member_name(jv v, const char *key) {
    char *s = json_get_str(v, key);
    return s ? s : xstrdup("");
}

static void text_type(ctx_t *c, jv v) {
    jv x;
    if (json_get(v, "$ref", &x)) { sb_jv_text(&c->ot, x); return; }
    if (!json_get(v, "type", &x)) { out_t(c, "any"); return; }
    sb_jv_text(&c->ot, x);
    if (json_str_eq(x, "array") && json_get(v, "items", &x)) {
        out_t(c, "<");
        text_type(c, x);
        out_t(c, ">");
    }
}

static void text_params(ctx_t *c, jv v, const char *key) {
    jv a, p;
    size_t pos = 0;
    int n = 0;
    if (!json_get(v, key, &a) || a.t != J_ARR) return;
    out_t(c, "%s", !strcmp(key, "parameters") ? " (" : " returns (");
    while (json_next(a, &pos, NULL, &p)) {
        char *name = member_name(p, "name");
        jv opt;
        int optional = json_get(p, "optional", &opt) && json_bool(opt);
        out_t(c, "%s%s%s:", n++ ? ", " : "", name, optional ? "?" : "!");
        text_type(c, p);
        free(name);
    }
    out_t(c, ")");
}

static int require_domain(const slim_schema *s, const char *name, jv *d) {
    *d = slim_schema_domain(s, name);
    if (d->t == J_NONE) return slim_seterr("unknown DevTools domain '%s'", name);
    return 0;
}

static int protocol_domains(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "protocol domains takes no arguments");
    slim_schema s;
    if (slim_schema_load(c, &s)) return -1;
    sb_puts(&c->oj, ",\"domains\":[");
    size_t pos = 0, n = 0;
    jv d;
    while (json_next(s.domains, &pos, NULL, &d)) {
        if (n++) sb_putc(&c->oj, ',');
        sb_add(&c->oj, d.p, d.n);
    }
    sb_putc(&c->oj, ']');
    pos = 0; n = 0;
    while (json_next(s.domains, &pos, NULL, &d)) {
        char *name = member_name(d, "domain");
        jv a;
        size_t nm = json_get(d, "commands", &a) ? json_len(a) : 0;
        size_t ne = json_get(d, "events", &a) ? json_len(a) : 0;
        out_t(c, "%s%s\tmethods=%zu\tevents=%zu", n++ ? "\n" : "", name, nm, ne);
        text_flags(c, d);
        free(name);
    }
    out_i(c, "count", (long long)n);
    slim_schema_clear(&s);
    return 0;
}

static int protocol_list(ctx_t *c, int argc, char **argv, int events) {
    const char *collection = events ? "events" : "commands";
    if (opt_done(c, &argc, argv)) return -1;
    if (argc > 1) return usage_err(c, "protocol %s [DOMAIN]", events ? "events" : "methods");
    slim_schema s;
    if (slim_schema_load(c, &s)) return -1;
    if (argc) {
        jv d;
        if (require_domain(&s, argv[0], &d)) { slim_schema_clear(&s); return -1; }
    }
    sb_printf(&c->oj, ",\"%s\":[", events ? "events" : "methods");
    size_t dp = 0, n = 0;
    jv d;
    while (json_next(s.domains, &dp, NULL, &d)) {
        char *dn = member_name(d, "domain");
        if (argc && strcmp(dn, argv[0])) { free(dn); continue; }
        jv a, v;
        size_t p = 0;
        if (json_get(d, collection, &a) && a.t == J_ARR) while (json_next(a, &p, NULL, &v)) {
            char *mn = member_name(v, "name");
            if (n++) sb_putc(&c->oj, ',');
            /* Include the domain alongside the unmodified live member schema. */
            sb_puts(&c->oj, "{\"domain\":"); sb_jstrz(&c->oj, dn);
            sb_puts(&c->oj, ",\"schema\":"); sb_add(&c->oj, v.p, v.n); sb_putc(&c->oj, '}');
            out_t(c, "%s%s.%s", n > 1 ? "\n" : "", dn, mn);
            text_params(c, v, "parameters");
            text_flags(c, v);
            free(mn);
        }
        free(dn);
    }
    sb_putc(&c->oj, ']');
    out_i(c, "count", (long long)n);
    slim_schema_clear(&s);
    return 0;
}

static int protocol_show(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1 || !strchr(argv[0], '.')) return usage_err(c, "protocol show Domain.method");
    const char *dot = strchr(argv[0], '.');
    char *dn = xstrndup(argv[0], (size_t)(dot - argv[0]));
    const char *mn = dot + 1;
    slim_schema s;
    if (slim_schema_load(c, &s)) { free(dn); return -1; }
    jv d;
    if (require_domain(&s, dn, &d)) { free(dn); slim_schema_clear(&s); return -1; }
    jv v = slim_schema_member(d, "commands", mn);
    const char *kind = "method";
    if (v.t == J_NONE) { v = slim_schema_member(d, "events", mn); kind = "event"; }
    if (v.t == J_NONE) {
        slim_seterr("'%s' is not advertised by this browser's DevTools schema", argv[0]);
        free(dn); slim_schema_clear(&s); return -1;
    }
    out_s(c, "domain", dn); out_s(c, "name", mn); out_s(c, "kind", kind);
    out_raw(c, "schema", v.p, v.n);
    out_t(c, "%s.%s%s", dn, mn, !strcmp(kind, "event") ? " [event]" : "");
    text_flags(c, v);
    jv x;
    if (json_get(v, "description", &x)) { out_t(c, "\n"); sb_jv_text(&c->ot, x); }
    if (json_get(v, "parameters", &x)) {
        out_t(c, "\nparameters:");
        size_t p = 0; jv q;
        while (json_next(x, &p, NULL, &q)) {
            char *name = member_name(q, "name");
            jv opt;
            int optional = json_get(q, "optional", &opt) && json_bool(opt);
            out_t(c, "\n  %s (%s, ", name, optional ? "optional" : "required");
            text_type(c, q); out_t(c, ")");
            text_flags(c, q);
            if (json_get(q, "description", &opt)) { out_t(c, " — "); sb_jv_text(&c->ot, opt); }
            free(name);
        }
    }
    if (json_get(v, "returns", &x)) {
        out_t(c, "\nreturns:");
        size_t p = 0; jv q;
        while (json_next(x, &p, NULL, &q)) {
            char *name = member_name(q, "name");
            jv opt;
            int optional = json_get(q, "optional", &opt) && json_bool(opt);
            out_t(c, "\n  %s (%s, ", name, optional ? "optional" : "required");
            text_type(c, q); out_t(c, ")");
            text_flags(c, q);
            free(name);
        }
    }
    free(dn); slim_schema_clear(&s);
    return 0;
}

static int protocol_version(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "protocol version takes no arguments");
    if (ctx_state(c)) return -1;
    int port = c->port ? c->port : c->st.port;
    char *body = NULL; size_t len = 0; jv doc;
    int st = http_get("127.0.0.1", port, "/json/version", c->timeout, &body, &len);
    if (st < 0) return -1;
    if (st == 200) len = slim_json_compact(body, len);
    if (st != 200 || json_parse(body, len, &doc) || doc.t != J_OBJ) {
        free(body); return slim_seterr("bad /json/version response on port %d", port);
    }
    out_raw(c, "version", doc.p, doc.n);
    static const char *const keys[] = {"Browser", "Protocol-Version", "V8-Version", "WebKit-Version", "User-Agent", NULL};
    for (int i = 0; keys[i]; i++) {
        char *v = json_get_str(doc, keys[i]);
        if (v) { out_t(c, "%s%s\t%s", i ? "\n" : "", keys[i], v); free(v); }
    }
    free(body);
    return 0;
}

static int protocol_coverage(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "protocol coverage takes no arguments");
    slim_schema s;
    if (slim_schema_load(c, &s)) return -1;
    size_t dp = 0, nd = 0, nm = 0, ne = 0;
    jv d;
    while (json_next(s.domains, &dp, NULL, &d)) {
        jv a; nd++;
        if (json_get(d, "commands", &a)) nm += json_len(a);
        if (json_get(d, "events", &a)) ne += json_len(a);
    }
    out_i(c, "domains", (long long)nd); out_i(c, "methods", (long long)nm);
    out_i(c, "events", (long long)ne); out_i(c, "callable_methods", (long long)nm);
    out_i(c, "omitted_methods", 0); out_i(c, "coverage_percent", 100);
    out_t(c, "domains %zu\nmethods %zu\nevents %zu\ncallable %zu/%zu (100%%)\nomitted 0\n"
             "coverage means invocation through cdp; engine/target support is not emulated", nd, nm, ne, nm, nm);
    slim_schema_clear(&s);
    return 0;
}

static int cmd_protocol(ctx_t *c, int argc, char **argv) {
    if (!argc) return usage_err(c, "protocol domains|methods|events|show|version|coverage ...");
    if (!strcmp(argv[0], "domains")) return protocol_domains(c, argc - 1, argv + 1);
    if (!strcmp(argv[0], "methods")) return protocol_list(c, argc - 1, argv + 1, 0);
    if (!strcmp(argv[0], "events")) return protocol_list(c, argc - 1, argv + 1, 1);
    if (!strcmp(argv[0], "show")) return protocol_show(c, argc - 1, argv + 1);
    if (!strcmp(argv[0], "version")) return protocol_version(c, argc - 1, argv + 1);
    if (!strcmp(argv[0], "coverage")) return protocol_coverage(c, argc - 1, argv + 1);
    return usage_err(c, "unknown protocol subcommand '%s'", argv[0]);
}

static int cmd_cdp_listen(ctx_t *c, int argc, char **argv) {
    const char *target = opt_val(c, &argc, argv, "--target");
    const char *target_id = opt_val(c, &argc, argv, "--target-id");
    const char *session_id = opt_val(c, &argc, argv, "--cdp-session");
    const char *cs = opt_val(c, &argc, argv, "--count");
    int timeout = opt_ms(c, &argc, argv, "--timeout", c->timeout);
    int no_enable = opt_flag(&argc, argv, "--no-enable");
    if (opt_done(c, &argc, argv)) return -1;
    long count = 1;
    if (cs && (parse_int(cs, &count) || count < 0 || count > 1000000))
        return usage_err(c, "bad --count '%s' (0 = until timeout)", cs);
    if (argc != 1 || !strchr(argv[0], '.'))
        return usage_err(c, "cdp-listen Domain.event [--timeout MS] [--count N] [--target browser|page]");
    const char *dot = strchr(argv[0], '.');
    char *dn = xstrndup(argv[0], (size_t)(dot - argv[0]));
    const char *event_name = dot + 1;
    const char *sid; char *owned = NULL;
    if (slim_cdp_select(c, target, target_id, session_id,
                        !slim_cdp_browser_domain(argv[0]), &sid, &owned)) {
        free(dn); return -1;
    }
    if (!no_enable) {
        slim_schema s;
        if (slim_schema_load(c, &s)) { slim_cdp_unselect(c, owned); free(dn); return -1; }
        jv d = slim_schema_domain(&s, dn);
        jv event = d.t == J_NONE ? (jv){NULL, 0, J_NONE} : slim_schema_member(d, "events", event_name);
        jv enable = d.t == J_NONE ? (jv){NULL, 0, J_NONE} : slim_schema_member(d, "commands", "enable");
        if (event.t == J_NONE) {
            slim_seterr("'%s' is not advertised by this browser's DevTools schema", argv[0]);
            slim_schema_clear(&s); slim_cdp_unselect(c, owned); free(dn); return -1;
        }
        if (enable.t != J_NONE) {
            sbuf m = {0}; sb_printf(&m, "%s.enable", dn);
            int r = cdp_call(&c->cdp, sid, m.p, NULL, c->timeout, NULL, NULL);
            sb_free(&m);
            if (r) { slim_schema_clear(&s); slim_cdp_unselect(c, owned); free(dn); return -1; }
        }
        slim_schema_clear(&s);
    }
    sb_puts(&c->oj, ",\"events\":[");
    long n = 0;
    long long deadline = now_ms() + timeout;
    while (!count || n < count) {
        long long left = deadline - now_ms();
        if (left <= 0) break;
        char *msg; jv params, method;
        int r = cdp_event(&c->cdp, sid, argv[0], (int)left, &msg, &params, &method);
        if (r < 0) { slim_cdp_unselect(c, owned); free(dn); return -1; }
        if (!r) break;
        if (n++) sb_putc(&c->oj, ',');
        sb_puts(&c->oj, "{\"method\":"); sb_add(&c->oj, method.p, method.n);
        jv doc, ses;
        if (json_parse(msg, strlen(msg), &doc) == 0 && json_get(doc, "sessionId", &ses)) {
            sb_puts(&c->oj, ",\"sessionId\":"); sb_add(&c->oj, ses.p, ses.n);
        }
        sb_puts(&c->oj, ",\"params\":"); sb_add(&c->oj, params.p, params.n); sb_putc(&c->oj, '}');
        out_t(c, "%s", n > 1 ? "\n" : ""); sb_jv_text(&c->ot, method);
        sb_putc(&c->ot, ' '); sb_add(&c->ot, params.p, params.n);
        free(msg);
    }
    sb_putc(&c->oj, ']'); out_i(c, "count", n);
    slim_cdp_unselect(c, owned); free(dn);
    return 0;
}

static int wrapper_call(ctx_t *c, int page, const char *method, const char *params, const char *text) {
    char *resp; jv res;
    if (ctx_call(c, page, method, params, &resp, &res)) return -1;
    out_raw(c, "result", res.p, res.n);
    out_s(c, "method", method);
    out_t(c, "%s", text);
    free(resp);
    return 0;
}

static int cmd_permissions(ctx_t *c, int argc, char **argv) {
    const char *origin = opt_val(c, &argc, argv, "--origin");
    const char *context = opt_val(c, &argc, argv, "--context");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc < 1 || (strcmp(argv[0], "grant") && strcmp(argv[0], "reset")))
        return usage_err(c, "permissions grant PERMISSION... [--origin O] [--context ID] | reset [--origin O] [--context ID]");
    int reset = !strcmp(argv[0], "reset");
    if (reset && argc != 1) return usage_err(c, "permissions reset takes no permission names");
    if (reset && origin) return usage_err(c, "Browser.resetPermissions does not accept --origin");
    if (!reset && argc < 2) return usage_err(c, "permissions grant needs at least one permission");
    sbuf p = {0}; sb_putc(&p, '{');
    if (!reset) {
        sb_puts(&p, "\"permissions\":[");
        for (int i = 1; i < argc; i++) { if (i > 1) sb_putc(&p, ','); sb_jstrz(&p, argv[i]); }
        sb_putc(&p, ']');
    }
    if (origin) { if (p.n > 1) sb_putc(&p, ','); sb_puts(&p, "\"origin\":"); sb_jstrz(&p, origin); }
    if (context) { if (p.n > 1) sb_putc(&p, ','); sb_puts(&p, "\"browserContextId\":"); sb_jstrz(&p, context); }
    sb_putc(&p, '}');
    int r = wrapper_call(c, 0, reset ? "Browser.resetPermissions" : "Browser.grantPermissions", p.p,
                         reset ? "permissions reset" : "permissions granted");
    sb_free(&p); return r;
}

static int cmd_geolocation(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc == 1 && !strcmp(argv[0], "clear"))
        return wrapper_call(c, 1, "Emulation.clearGeolocationOverride", NULL, "geolocation cleared");
    if (argc < 3 || argc > 4 || strcmp(argv[0], "set"))
        return usage_err(c, "geolocation set LAT LON [ACCURACY_M] | clear");
    double lat, lon, acc = 1;
    if (parse_double(argv[1], &lat) || parse_double(argv[2], &lon) ||
        (argc == 4 && parse_double(argv[3], &acc)) ||
        lat != lat || lon != lon || acc != acc || lat < -90 || lat > 90 ||
        lon < -180 || lon > 180 || acc < 0 || acc > 1000000000)
        return usage_err(c, "bad geolocation (LAT -90..90, LON -180..180, ACCURACY_M >= 0)");
    sbuf p = {0}; sb_printf(&p, "{\"latitude\":%.17g,\"longitude\":%.17g,\"accuracy\":%.17g}", lat, lon, acc);
    int r = wrapper_call(c, 1, "Emulation.setGeolocationOverride", p.p, "geolocation set");
    sb_free(&p); return r;
}

static int cmd_throttle(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "throttle off|offline|slow-3g|fast-3g|slow-4g|fast-4g");
    const char *p = argv[0];
    int offline = !strcmp(p, "offline");
    long latency = 0, down = -1, up = -1;
    if (!strcmp(p, "slow-3g")) { latency = 400; down = 51200; up = 51200; }
    else if (!strcmp(p, "fast-3g")) { latency = 150; down = 204800; up = 96000; }
    else if (!strcmp(p, "slow-4g")) { latency = 150; down = 409600; up = 409600; }
    else if (!strcmp(p, "fast-4g")) { latency = 60; down = 4194304; up = 3145728; }
    else if (!offline && strcmp(p, "off")) return usage_err(c, "unknown throttle preset '%s'", p);
    if (ctx_enable(c, "Network")) return -1;
    sbuf b = {0};
    sb_printf(&b, "{\"offline\":%s,\"latency\":%ld,\"downloadThroughput\":%ld,\"uploadThroughput\":%ld}",
              offline ? "true" : "false", latency, down, up);
    int r = wrapper_call(c, 1, "Network.emulateNetworkConditions", b.p,
                         offline ? "network offline" : !strcmp(p, "off") ? "network throttling off" : "network throttling set");
    sb_free(&b); return r;
}

static int cmd_cache(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off") &&
                      strcmp(argv[0], "enable") && strcmp(argv[0], "disable")))
        return usage_err(c, "cache on|off");
    int off = !strcmp(argv[0], "off") || !strcmp(argv[0], "disable");
    if (ctx_enable(c, "Network")) return -1;
    return wrapper_call(c, 1, "Network.setCacheDisabled", off ? "{\"cacheDisabled\":true}" : "{\"cacheDisabled\":false}",
                        off ? "cache disabled" : "cache enabled");
}

static int cmd_script(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off") &&
                      strcmp(argv[0], "enable") && strcmp(argv[0], "disable")))
        return usage_err(c, "script on|off");
    int off = !strcmp(argv[0], "off") || !strcmp(argv[0], "disable");
    return wrapper_call(c, 1, "Emulation.setScriptExecutionDisabled", off ? "{\"value\":true}" : "{\"value\":false}",
                        off ? "page scripts disabled" : "page scripts enabled");
}

/* save a new dialog policy and wait for the keeper to take it (0, or 1 when there is none) */
static int dialog_policy_set(ctx_t *c, const char *pol) {
    state_set(&c->st.dlg, pol);
    c->st.gen++;
    if (state_save(&c->st)) return -1;
    return hold_ready(c);
}

static int keeper_on(ctx_t *c) {
    if (c->port || ctx_state(c) || !hold_enabled()) return 0;
    return hold_ready(c) == 0;
}

static int cmd_dialog(ctx_t *c, int argc, char **argv) {
    const char *cs = opt_val(c, &argc, argv, "--count");
    int timeout = opt_ms(c, &argc, argv, "--timeout", c->timeout);
    if (opt_done(c, &argc, argv)) return -1;
    if (argc >= 1 && !strcmp(argv[0], "policy")) {
        if (argc > 2) return usage_err(c, "dialog policy [TYPE=accept|dismiss|manual,...|reset]  (TYPE: alert confirm prompt beforeunload all)");
        if (c->port) return usage_err(c, "dialog policy needs a session (--session), not --port");
        if (ctx_state_reload(c)) return -1;
        char *np = NULL;
        if (dlg_policy_merge(argc == 2 ? c->st.dlg : NULL, argc == 2 ? argv[1] : NULL, &np)) { c->usage = 1; return -1; }
        if (argc == 2) { int r = dialog_policy_set(c, np); if (r < 0) { free(np); return -1; } }
        static const char *const ty[] = {"beforeunload", "alert", "confirm", "prompt"};
        for (int i = 0; i < 4; i++) out_s(c, ty[i], dlg_action(np, ty[i]));
        out_s(c, "policy", np);
        out_t(c, "dialog policy %s", np);
        free(np);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[0], "auto")) {
        int accept;
        if (!strcmp(argv[1], "accept")) accept = 1;
        else if (!strcmp(argv[1], "dismiss")) accept = 0;
        else return usage_err(c, "dialog auto accept|dismiss [--timeout MS] [--count N]");
        long count = 1;
        if (cs && (parse_int(cs, &count) || count < 0 || count > 1000000)) return usage_err(c, "bad --count '%s'", cs);
        long n = 0; long long deadline = now_ms() + timeout;
        if (keeper_on(c)) {   /* the keeper is the one session that sees dialogs: lend it the policy */
            if (ctx_state_reload(c)) return -1;
            char *old = c->st.dlg ? xstrdup(c->st.dlg) : NULL, *np = NULL;
            dlg_policy_merge(NULL, accept ? "all=accept" : "all=dismiss", &np);
            long sz; long base = dlg_log_count(c->session, &sz);
            int r = dialog_policy_set(c, np);
            free(np);
            while (!r && (!count || n < count) && now_ms() < deadline) {
                long cur = dlg_log_count(c->session, &sz);
                if (cur < base) base = 0;   /* log was trimmed */
                n = cur - base;
                if (count && n >= count) break;
                sleep_ms(20);
            }
            if (count && n > count) n = count;
            if (ctx_state_reload(c) == 0) dialog_policy_set(c, old);
            free(old);
            if (r < 0) return -1;
        } else {
            if (ctx_enable(c, "Page")) return -1;
            while (!count || n < count) {
                long long left = deadline - now_ms(); if (left <= 0) break;
                char *msg;
                int r = cdp_event(&c->cdp, c->sid, "Page.javascriptDialogOpening", (int)left, &msg, NULL, NULL);
                if (r < 0) return -1;
                if (!r) break;
                free(msg);
                if (cdp_call(&c->cdp, c->sid, "Page.handleJavaScriptDialog",
                             accept ? "{\"accept\":true}" : "{\"accept\":false}", c->timeout, NULL, NULL)) return -1;
                n++;
            }
        }
        out_i(c, "handled", n); out_b(c, "accept", accept);
        out_t(c, "handled %ld dialog%s (%s)", n, n == 1 ? "" : "s", accept ? "accept" : "dismiss");
        return 0;
    }
    if (cs) return usage_err(c, "--count is only valid with dialog auto");
    if (argc < 1 || argc > 2 || (strcmp(argv[0], "accept") && strcmp(argv[0], "dismiss")))
        return usage_err(c, "dialog accept [PROMPT_TEXT] | dismiss | auto accept|dismiss [--timeout MS] | policy [SPEC]");
    int accept = !strcmp(argv[0], "accept");
    if (!accept && argc == 2) return usage_err(c, "dialog dismiss takes no prompt text");
    if (keeper_on(c)) {   /* only the session that saw the dialog open can answer it */
        char *tid = NULL;
        if (c->tab && ctx_resolve_target(c, c->tab, &tid)) return -1;
        if (!tid && c->st.target) tid = xstrdup(c->st.target);
        int r = hold_request(c, "dialog", tid, accept, argc == 2 ? argv[1] : NULL);
        if (r && tid && strstr(slim_errmsg(), "on this tab")) r = hold_request(c, "dialog", NULL, accept, argc == 2 ? argv[1] : NULL);
        free(tid);
        if (r) return -1;
        out_s(c, "method", "Page.handleJavaScriptDialog"); out_b(c, "keeper", 1);
        out_t(c, accept ? "dialog accepted" : "dialog dismissed");
        return 0;
    }
    sbuf p = {0}; sb_printf(&p, "{\"accept\":%s", accept ? "true" : "false");
    if (argc == 2) { sb_puts(&p, ",\"promptText\":"); sb_jstrz(&p, argv[1]); }
    sb_putc(&p, '}');
    int r = wrapper_call(c, 1, "Page.handleJavaScriptDialog", p.p, accept ? "dialog accepted" : "dialog dismissed");
    sb_free(&p); return r;
}

static const cmd_def PROTOCOL_COMMANDS[] = {
    {"protocol", cmd_protocol, 0, "domains|methods [DOMAIN]|events [DOMAIN]|show M|version|coverage  live DevTools schema"},
    {"cdp-listen", cmd_cdp_listen, 0, "EVENT [--timeout MS] [--count N] [--target browser|page] [--no-enable]  collect DevTools events"},
    {"permissions", cmd_permissions, 0, "grant PERMISSION... [--origin O]|reset [--context ID]  browser permissions"},
    {"geolocation", cmd_geolocation, 0, "set LAT LON [ACCURACY_M]|clear  geolocation override"},
    {"throttle", cmd_throttle, 0, "off|offline|slow-3g|fast-3g|slow-4g|fast-4g  network conditions"},
    {"cache", cmd_cache, 0, "on|off  HTTP cache for the current page"},
    {"dialog", cmd_dialog, 0, "accept [TEXT]|dismiss|auto accept|dismiss [--timeout MS]|policy [TYPE=accept|dismiss|manual,...]  JavaScript dialogs"},
    {"script", cmd_script, 0, "on|off  page script execution"},
};

const cmd_def *slim_protocol_commands(size_t *count) {
    *count = sizeof PROTOCOL_COMMANDS / sizeof PROTOCOL_COMMANDS[0];
    return PROTOCOL_COMMANDS;
}