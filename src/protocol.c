#include "protocol.h"
#include <stdlib.h>
#include <string.h>

size_t slim_json_compact(char *text, size_t len) {
    size_t i = 0, o = 0;
    int string = 0, escape = 0;
    while (i < len) {
        char ch = text[i++];
        if (string) {
            text[o++] = ch;
            if (escape) escape = 0;
            else if (ch == '\\') escape = 1;
            else if (ch == '"') string = 0;
        } else if (ch == '"') {
            string = 1; text[o++] = ch;
        } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') text[o++] = ch;
    }
    text[o] = 0;
    return o;
}

int slim_schema_load(ctx_t *c, slim_schema *s) {
    memset(s, 0, sizeof *s);
    if (ctx_state(c)) return -1;
    int port = c->port ? c->port : c->st.port;
    size_t len = 0;
    int st = http_get("127.0.0.1", port, "/json/protocol", c->timeout, &s->body, &len);
    if (st < 0) return slim_seterr("could not read DevTools schema on port %d: %s", port, slim_errmsg());
    if (st == 200) len = slim_json_compact(s->body, len);
    if (st != 200 || json_parse(s->body, len, &s->root) || s->root.t != J_OBJ ||
        !json_get(s->root, "domains", &s->domains) || s->domains.t != J_ARR) {
        slim_schema_clear(s);
        return slim_seterr("bad /json/protocol response on port %d", port);
    }
    return 0;
}

void slim_schema_clear(slim_schema *s) {
    free(s->body);
    memset(s, 0, sizeof *s);
}

jv slim_schema_domain(const slim_schema *s, const char *name) {
    jv d, none = {NULL, 0, J_NONE};
    size_t pos = 0;
    while (json_next(s->domains, &pos, NULL, &d))
        if (json_streq_path(d, "domain", name)) return d;
    return none;
}

jv slim_schema_member(jv domain, const char *collection, const char *name) {
    jv a, v, none = {NULL, 0, J_NONE};
    size_t pos = 0;
    if (!json_get(domain, collection, &a) || a.t != J_ARR) return none;
    while (json_next(a, &pos, NULL, &v))
        if (json_streq_path(v, "name", name)) return v;
    return none;
}

int slim_cdp_browser_domain(const char *m) {
    static const char *const d[] = {
        "Target.", "Browser.", "SystemInfo.", "Tethering.", "Tracing.",
        "Memory.", "Storage.", "ServiceWorker.", NULL
    };
    for (int i = 0; d[i]; i++) if (!strncmp(m, d[i], strlen(d[i]))) return 1;
    return 0;
}

int slim_cdp_select(ctx_t *c, const char *target, const char *target_id,
                    const char *session_id, int default_page,
                    const char **sid, char **owned_sid) {
    int routes = (target_id != NULL) + (session_id != NULL) + (target != NULL);
    if (routes > 1) return usage_err(c, "use only one of --target, --target-id, --cdp-session");
    *sid = NULL;
    *owned_sid = NULL;
    if (session_id) {
        if (!*session_id) return usage_err(c, "--cdp-session needs a non-empty session id");
        if (ctx_browser(c)) return -1;
        *sid = session_id;
        return 0;
    }
    if (target_id) {
        char *tid = NULL;
        if (ctx_browser(c) || ctx_resolve_target(c, target_id, &tid)) return -1;
        int r = cdp_attach(&c->cdp, tid, c->timeout, owned_sid);
        free(tid);
        if (r) return -1;
        *sid = *owned_sid;
        return 0;
    }
    int page = default_page;
    if (target) {
        if (!strcmp(target, "browser")) page = 0;
        else if (!strcmp(target, "page")) page = 1;
        else return usage_err(c, "bad --target '%s' (browser|page)", target);
    }
    if (page) {
        if (ctx_page(c)) return -1;
        *sid = c->sid;
    } else if (ctx_browser(c)) return -1;
    return 0;
}

void slim_cdp_unselect(ctx_t *c, char *owned_sid) {
    if (owned_sid) cdp_detach(&c->cdp, owned_sid, 2000);
    free(owned_sid);
}
