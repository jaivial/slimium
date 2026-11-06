/* CLI for named persistent user profiles. */
#include "cmd.h"
#include "userprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int no_args(ctx_t *c, int argc, char **argv, const char *usage) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "%s", usage);
    return 0;
}

static int profile_create(ctx_t *c, int argc, char **argv) {
    const char *ua = opt_val(c, &argc, argv, "--ua");
    const char *locale = opt_val(c, &argc, argv, "--locale");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "profile create NAME [--ua X] [--locale X]");
    if (userprof_create(argv[0], ua, locale)) return -1;
    char data[4096];
    if (userprof_data_dir(argv[0], data, sizeof data)) return -1;
    out_t(c, "created user profile %s", argv[0]);
    out_s(c, "user", argv[0]); out_s(c, "user_data_dir", data);
    if (ua) out_s(c, "ua", ua);
    if (locale) out_s(c, "locale", locale);
    return 0;
}

static int profile_list(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "profile list takes no arguments")) return -1;
    userprof_t *items; size_t count;
    if (userprof_list(&items, &count)) return -1;
    userprof_sort(items, count);
    char *selected = NULL;
    if (userprof_default(&selected) < 0) {
        for (size_t i = 0; i < count; i++) userprof_free(&items[i]);
        free(items); free(selected); return -1;
    }
    sb_puts(&c->oj, ",\"profiles\":[");
    for (size_t i = 0; i < count; i++) {
        userprof_t *p = &items[i];
        int current = selected && !strcmp(selected, p->name);
        if (i) sb_putc(&c->oj, ',');
        sb_puts(&c->oj, "{\"user\":"); sb_jstrz(&c->oj, p->name);
        sb_printf(&c->oj, ",\"current\":%s,\"saved_pages\":%zu", current ? "true" : "false", json_len((jv){p->pages, strlen(p->pages), J_ARR}));
        if (p->ua) { sb_puts(&c->oj, ",\"ua\":"); sb_jstrz(&c->oj, p->ua); }
        if (p->locale) { sb_puts(&c->oj, ",\"locale\":"); sb_jstrz(&c->oj, p->locale); }
        sb_putc(&c->oj, '}');
        out_t(c, "%s%s%s", i ? "\n" : "", current ? "* " : "  ", p->name);
        if (p->ua) out_t(c, "\tua=%s", p->ua);
        if (p->locale) out_t(c, "\tlocale=%s", p->locale);
        userprof_free(p);
    }
    sb_putc(&c->oj, ']');
    free(items); free(selected);
    return 0;
}

static int profile_use(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "profile use NAME");
    if (userprof_set_default(argv[0])) return -1;
    out_t(c, "default user profile: %s", argv[0]);
    out_s(c, "user", argv[0]);
    return 0;
}

static int profile_current(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "profile current takes no arguments")) return -1;
    char *name = NULL;
    int r = userprof_default(&name);
    if (r < 0) return -1;
    out_t(c, "%s", r == 1 ? "none" : name);
    out_s(c, "user", r == 1 ? NULL : name); out_b(c, "selected", r == 0);
    free(name);
    return 0;
}

static int profile_show(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "profile show NAME");
    userprof_t p;
    if (userprof_load(argv[0], &p)) return -1;
    char data[4096];
    if (userprof_data_dir(argv[0], data, sizeof data)) { userprof_free(&p); return -1; }
    size_t pages = json_len((jv){p.pages, strlen(p.pages), J_ARR});
    out_t(c, "user      %s\ndata      %s\ncreated_ms %lld\nupdated_ms %lld", p.name, data, p.created_ms, p.updated_ms);
    if (p.ua) out_t(c, "\nua        %s", p.ua);
    if (p.locale) out_t(c, "\nlocale    %s", p.locale);
    out_t(c, "\nsaved     %zu page%s", pages, pages == 1 ? "" : "s");
    if (p.saved_ms) out_t(c, "\ncurrent   %d\t%s", p.current_index, p.current_url ? p.current_url : "");
    out_s(c, "user", p.name); out_s(c, "user_data_dir", data);
    out_i(c, "created_ms", p.created_ms); out_i(c, "updated_ms", p.updated_ms);
    if (p.ua) out_s(c, "ua", p.ua);
    if (p.locale) out_s(c, "locale", p.locale);
    out_i(c, "saved_ms", p.saved_ms); out_i(c, "current_index", p.current_index);
    if (p.current_url) out_s(c, "current_url", p.current_url);
    out_raw(c, "pages", p.pages, strlen(p.pages));
    userprof_free(&p);
    return 0;
}

static int profile_rename(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 2) return usage_err(c, "profile rename OLD NEW");
    if (userprof_rename(argv[0], argv[1])) return -1;
    out_t(c, "renamed user profile %s to %s", argv[0], argv[1]);
    out_s(c, "old_user", argv[0]); out_s(c, "user", argv[1]);
    return 0;
}

static int profile_delete(ctx_t *c, int argc, char **argv) {
    int yes = opt_flag(&argc, argv, "--yes");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc != 1) return usage_err(c, "profile delete NAME --yes");
    if (!yes) return usage_err(c, "profile delete requires --yes (deletes the named profile and its Chromium data)");
    if (userprof_delete(argv[0])) return -1;
    out_t(c, "deleted user profile %s", argv[0]);
    out_s(c, "user", argv[0]); out_b(c, "deleted", 1);
    return 0;
}

static int session_user(ctx_t *c) {
    if (c->port) return usage_err(c, "profile save/restore needs a session, not --port");
    if (ctx_state(c)) return -1;
    if (!c->st.user) return slim_seterr("session '%s' has no user profile (start with --user NAME)", c->session);
    return 0;
}

static int profile_save(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "profile save [--session S]")) return -1;
    if (session_user(c)) return -1;
    char *resp; jv targets, target; size_t pos = 0;
    if (ctx_targets(c, &resp, &targets)) return -1;
    sbuf pages = {0}; sb_putc(&pages, '[');
    int count = 0, current = -1;
    char *current_url = NULL;
    while (json_next(targets, &pos, NULL, &target)) {
        if (!json_streq_path(target, "type", "page")) continue;
        char *tid = json_get_str(target, "targetId");
        char *fallback_url = json_get_str(target, "url");
        if (!tid) { free(fallback_url); continue; }
        char *sid = NULL, *eval_resp = NULL;
        jv obj;
        if (cdp_attach(&c->cdp, tid, c->timeout, &sid)) { free(tid); free(fallback_url); continue; }
        int er = ctx_eval_in(c, sid, "[location.href,Math.round(window.scrollX||0),Math.round(window.scrollY||0)]", &eval_resp, &obj);
        char *url = NULL;
        long long sx = 0, sy = 0;
        if (!er) {
            url = json_get_str(obj, "value.0");
            sx = (long long)json_get_num(obj, "value.1", 0);
            sy = (long long)json_get_num(obj, "value.2", 0);
        }
        free(eval_resp);
        cdp_detach(&c->cdp, sid, 2000);
        free(sid);
        if (!url) url = fallback_url; else free(fallback_url);
        if (!url) { free(tid); continue; }
        if (count++) sb_putc(&pages, ',');
        sb_puts(&pages, "{\"url\":"); sb_jstrz(&pages, url);
        sb_printf(&pages, ",\"scroll_x\":%lld,\"scroll_y\":%lld}", sx, sy);
        if (c->st.target && !strcmp(c->st.target, tid)) {
            current = count - 1; free(current_url); current_url = xstrdup(url);
        }
        free(url); free(tid);
    }
    sb_putc(&pages, ']');
    free(resp);
    if (count && current < 0) {
        current = 0;
        jv parsed, first;
        if (!json_parse(pages.p, pages.n, &parsed) && json_get(parsed, "0.url", &first)) current_url = json_str(first);
    }
    int r = userprof_save_pages(c->st.user, pages.p ? pages.p : "[]", current, current_url);
    if (!r) {
        out_t(c, "saved %d page%s for user profile %s", count, count == 1 ? "" : "s", c->st.user);
        out_s(c, "user", c->st.user); out_i(c, "pages", count); out_i(c, "current_index", current);
        out_raw(c, "tabs", pages.p ? pages.p : "[]", pages.n);
    }
    sb_free(&pages); free(current_url);
    return r;
}

static char *invoked_id(ctx_t *c) {
    sbuf doc = {0}; sb_puts(&doc, "{\"ok\":true"); sb_add(&doc, c->oj.p ? c->oj.p : "", c->oj.n); sb_putc(&doc, '}');
    jv d;
    char *id = json_parse(doc.p, doc.n, &d) ? NULL : json_get_str(d, "id");
    sb_free(&doc);
    return id;
}

static int profile_restore(ctx_t *c, int argc, char **argv) {
    if (no_args(c, argc, argv, "profile restore [--session S]")) return -1;
    if (session_user(c)) return -1;
    userprof_t p;
    if (userprof_load(c->st.user, &p)) return -1;
    jv pages, page;
    if (json_parse(p.pages, strlen(p.pages), &pages)) { userprof_free(&p); return slim_seterr("invalid saved tab snapshot"); }
    char *initial_resp = NULL; jv targets, target; size_t pos = 0;
    char **blank = NULL; size_t blanks = 0, blank_cap = 0;
    if (ctx_targets(c, &initial_resp, &targets)) { userprof_free(&p); return -1; }
    while (json_next(targets, &pos, NULL, &target)) {
        if (!json_streq_path(target, "type", "page") || !json_streq_path(target, "url", "about:blank")) continue;
        char *id = json_get_str(target, "targetId");
        if (!id) continue;
        if (blanks == blank_cap) { blank_cap = blank_cap ? blank_cap * 2 : 4; blank = xrealloc(blank, blank_cap * sizeof *blank); }
        blank[blanks++] = id;
    }
    free(initial_resp);

    char **opened = NULL; size_t opened_count = 0, opened_cap = 0;
    int r = 0;
    pos = 0;
    while (!r && json_next(pages, &pos, NULL, &page)) {
        char *url = json_get_str(page, "url");
        if (!url || !*url) { free(url); r = slim_seterr("saved tab has no URL"); break; }
        char *args[] = {"open", url, "--wait", "load"};
        r = cli_invoke(c, 4, args);
        free(url);
        if (r) break;
        char *id = invoked_id(c);
        if (!id) { r = slim_seterr("restore could not determine opened tab"); break; }
        if (opened_count == opened_cap) { opened_cap = opened_cap ? opened_cap * 2 : 4; opened = xrealloc(opened, opened_cap * sizeof *opened); }
        opened[opened_count++] = id;
        long long sx = (long long)json_get_num(page, "scroll_x", 0), sy = (long long)json_get_num(page, "scroll_y", 0);
        if (sx || sy) {
            char expr[160];
            snprintf(expr, sizeof expr, "window.scrollTo(%lld,%lld);true", sx, sy);
            char *eval_resp = NULL; jv obj;
            r = ctx_eval(c, expr, &eval_resp, &obj);
            free(eval_resp);
        }
    }
    int current = p.current_index;
    if (current < 0 || (size_t)current >= opened_count) current = opened_count ? 0 : -1;
    if (!r && current >= 0) {
        char *args[] = {"switch", opened[current]};
        r = cli_invoke(c, 2, args);
    }
    for (size_t i = 0; !r && i < blanks; i++) {
        char *args[] = {"close", blank[i]};
        r = cli_invoke(c, 2, args);
    }
    for (size_t i = 0; i < blanks; i++) free(blank[i]);
    free(blank);
    if (!r) {
        sb_reset(&c->oj); sb_reset(&c->ot);
        out_t(c, "restored %zu page%s for user profile %s (current %d)", opened_count, opened_count == 1 ? "" : "s", c->st.user, current);
        out_s(c, "user", c->st.user); out_i(c, "pages", (long long)opened_count); out_i(c, "current_index", current);
        if (current >= 0) out_s(c, "tab", opened[current]);
    }
    for (size_t i = 0; i < opened_count; i++) free(opened[i]);
    free(opened); userprof_free(&p);
    return r;
}

static int cmd_profile(ctx_t *c, int argc, char **argv) {
    if (argc < 1) return usage_err(c, "profile create|list|use|current|show|rename|delete|save|restore ...");
    const char *sub = argv[0];
    argc--; argv++;
    if (!strcmp(sub, "create")) return profile_create(c, argc, argv);
    if (!strcmp(sub, "list")) return profile_list(c, argc, argv);
    if (!strcmp(sub, "use")) return profile_use(c, argc, argv);
    if (!strcmp(sub, "current")) return profile_current(c, argc, argv);
    if (!strcmp(sub, "show")) return profile_show(c, argc, argv);
    if (!strcmp(sub, "rename")) return profile_rename(c, argc, argv);
    if (!strcmp(sub, "delete")) return profile_delete(c, argc, argv);
    if (!strcmp(sub, "save")) return profile_save(c, argc, argv);
    if (!strcmp(sub, "restore")) return profile_restore(c, argc, argv);
    return usage_err(c, "unknown profile subcommand '%s'", sub);
}

static const cmd_def COMMANDS[] = {
    {"profile", cmd_profile, 0, "create|list|use|current|show|rename|delete|save|restore  named persistent login profiles"},
};

const cmd_def *slim_user_profile_commands(size_t *count) {
    *count = sizeof COMMANDS / sizeof COMMANDS[0];
    return COMMANDS;
}
