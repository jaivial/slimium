/* Session lifecycle (start/stop/status/attach/sessions) and the slimium-chrome wrapper. */
#include "cmd.h"
#include "gov.h"
#include "mem.h"
#include "profile.h"
#include "userprof.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORT_MIN 9230
#define PORT_MAX 9361 /* 9362-9386 reserved for the user's cu daemon */

/* ---- engine discovery ---- */
static int same_file(const char *a, const char *b) {
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

/* Distro launch scripts (Fedora chromium-browser.sh) add flags and two 'cat' pipes that live as
 * long as the browser; exec the sibling ELF directly unless SLIM_ENGINE_DIRECT=0. */
static char *direct_engine(char *path) {
    const char *d = getenv("SLIM_ENGINE_DIRECT");
    if (d && !strcmp(d, "0")) return path;
    char rp[PATH_MAX];
    if (!realpath(path, rp)) return path;
    size_t l = strlen(rp);
    if (l < 3 || strcmp(rp + l - 3, ".sh")) return path;
    rp[l - 3] = 0;
    int fd = open(rp, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return path;
    unsigned char mg[4] = {0};
    ssize_t r = read(fd, mg, 4);
    close(fd);
    if (r != 4 || memcmp(mg, "\177ELF", 4) || access(rp, X_OK)) return path;
    free(path);
    return xstrdup(rp);
}

char *slim_find_engine(const char *explicit_path, int direct);
char *slim_find_engine(const char *explicit_path, int direct) {
    static const char *const names[] = {"chromium-browser", "chromium", "google-chrome-stable", "google-chrome", NULL};
    const char *env = explicit_path && *explicit_path ? explicit_path : getenv("SLIM_CHROMIUM");
    char *p = NULL;
    if (env && *env) {
        p = find_in_path(env);
        if (!p) { slim_seterr("engine not found or not executable: %s", env); return NULL; }
        if (same_file(p, "/proc/self/exe")) { free(p); slim_seterr("engine %s is slimium itself", env); return NULL; }
    } else {
        for (int i = 0; names[i] && !p; i++) {
            p = find_in_path(names[i]);
            if (p && same_file(p, "/proc/self/exe")) { free(p); p = NULL; }
        }
        if (!p) { slim_seterr("no Chromium found (set SLIM_CHROMIUM or install chromium-browser)"); return NULL; }
    }
    return direct ? direct_engine(p) : p;
}

static void log_argv(char **av) {
    const char *f = getenv("SLIM_WRAPPER_LOG");
    if (!f || !*f) return;
    int fd = open(f, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    sbuf b = {0};
    for (int i = 0; av[i]; i++) {
        if (i) sb_putc(&b, ' ');
        if (!av[i][0] || av[i][strcspn(av[i], " \t\n'\"\\$\x60;&|<>()*?")]) {
            sb_putc(&b, '\'');
            for (const char *s = av[i]; *s; s++) { if (*s == '\'') sb_puts(&b, "'\\''"); else sb_putc(&b, *s); }
            sb_putc(&b, '\'');
        } else sb_puts(&b, av[i]);
    }
    sb_putc(&b, '\n');
    ssize_t w = write(fd, b.p, b.n);
    (void)w;
    close(fd);
    sb_free(&b);
}

int wrapper_main(int argc, char **argv) {
    int ver = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-version") || !strcmp(argv[i], "--product-version")) ver = 1;
    char *engine = slim_find_engine(NULL, !ver);
    if (!engine) { fprintf(stderr, "slimium-chrome: %s\n", slim_errmsg()); return 127; }
    const char *gv = getenv("SLIM_GOV");
    int gov = !ver && !(gv && !strcmp(gv, "0"));
    if (gov) { char *pe = slim_engine_private(engine); if (pe) { free(engine); engine = pe; slim_engine_libs(engine); } }
    int wargc = argc;
    char **wargv = argv;
    char user_dir[PATH_MAX], user_arg[PATH_MAX + 20];
    const char *user = getenv("SLIM_USER_PROFILE");
    if (!ver && user && *user) {
        if (userprof_data_dir(user, user_dir, sizeof user_dir) || userprof_check_idle(user)) {
            fprintf(stderr, "slimium-chrome: %s\n", slim_errmsg()); return 127;
        }
        snprintf(user_arg, sizeof user_arg, "--user-data-dir=%s", user_dir);
        wargv = xmalloc(sizeof *wargv * (size_t)(argc + 2));
        wargv[0] = argv[0];
        int k = 1;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--user-data-dir")) { if (i + 1 < argc) i++; continue; }
            if (!strncmp(argv[i], "--user-data-dir=", 16)) continue;
            wargv[k++] = argv[i];
        }
        wargv[k++] = user_arg;
        wargv[k] = NULL;
        wargc = k;
    }
    char **av;
    if (ver) {
        av = xmalloc(sizeof *av * (size_t)(argc + 1));
        av[0] = engine;
        for (int i = 1; i < argc; i++) av[i] = argv[i];
        av[argc] = NULL;
    } else {
        const char *ps = getenv("SLIM_PROFILE");
        int level = SLIM_PROFILE_BALANCED;
        if (ps && *ps) {
            level = slim_profile_parse(ps);
            if (level < 0) { fprintf(stderr, "slimium-chrome: bad SLIM_PROFILE '%s', using balanced\n", ps); level = SLIM_PROFILE_BALANCED; }
        }
        av = slim_profile_merge_argv(level, engine, wargc - 1, wargv + 1);
        if (!av) { fprintf(stderr, "slimium-chrome: out of memory\n"); return 127; }
    }
    log_argv(av);
    if (gov) {
        /* own the page cache, fork the detached governor, then exec in a scope keeping this pid */
        const char *udd = NULL;
        int port = slim_gov_argv_port(wargc - 1, wargv + 1, &udd);
        slim_engine_fadvise(engine);
        slim_gov_spawn(getpid(), port, udd, 1);
        char **sv = slim_scope_argv(av, getpid());
        if (sv) { execv(sv[0], sv); free(sv); }   /* systemd-run gone? run unscoped */
    }
    execv(engine, av);
    fprintf(stderr, "slimium-chrome: exec %s: %s\n", engine, strerror(errno));
    return 127;
}

/* ---- process helpers ---- */
static int proc_alive(int pid) {
    if (pid <= 0) return 0;
    char p[64], *s; size_t n;
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    if (read_file(p, &s, &n, 4096)) return 0;
    char *rp = strrchr(s, ')');
    int alive = rp && rp[1] == ' ' && rp[2] != 'Z' && rp[2] != 'X';
    free(s);
    return alive;
}
/* pid is alive and is the browser we launched on this port */
static int proc_is_browser(int pid, int port) {
    if (!proc_alive(pid)) return 0;
    char want[48];
    snprintf(want, sizeof want, "--remote-debugging-port=%d", port);
    return proc_has_arg(pid, want, NULL);
}
static int port_free(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}
static int wait_gone(int pid, int ms) {
    long long dl = now_ms() + ms;
    while (proc_alive(pid)) { if (now_ms() > dl) return 0; sleep_ms(50); }
    return 1;
}

static int devtools_up(int port, int ms) {
    char *u = NULL;
    if (cdp_browser_url(port, ms, &u, NULL)) return 0;
    free(u);
    return 1;
}

static void tail_log(const char *path, char *out, size_t n) {
    out[0] = 0;
    char *d; size_t l;
    if (!path || read_file(path, &d, &l, 1 << 24)) return;
    while (l && (d[l - 1] == '\n' || d[l - 1] == ' ')) d[--l] = 0;
    char *s = strrchr(d, '\n');
    snprintf(out, n, "%s", s ? s + 1 : d);
    free(d);
}

/* ---- commands ---- */
int cmd_start(ctx_t *c, int argc, char **argv) {
    const char *prof = opt_val(c, &argc, argv, "--profile");
    const char *user = opt_val(c, &argc, argv, "--user");
    int headful = opt_flag(&argc, argv, "--headful");
    const char *udd = opt_val(c, &argc, argv, "--user-data-dir");
    const char *eng = opt_val(c, &argc, argv, "--engine");
    const char *win = opt_val(c, &argc, argv, "--window");
    int gov_on = opt_flag(&argc, argv, "--gov");
    int gov_off = opt_flag(&argc, argv, "--no-gov");
    const char *genv = getenv("SLIM_GOV");
    gov_on = !gov_off && (gov_on || !(genv && !strcmp(genv, "0")));
    if (opt_done(c, &argc, argv)) return -1;
    if (!prof) prof = getenv("SLIM_PROFILE");
    if (!prof || !*prof) prof = "balanced";
    int level = slim_profile_parse(prof);
    if (level < 0) return usage_err(c, "bad --profile '%s' (off|balanced|max)", prof);
    int ww = 0, wh = 0;
    if (win && (sscanf(win, "%dx%d", &ww, &wh) != 2 || ww < 100 || wh < 100 || ww > 16384 || wh > 16384))
        return usage_err(c, "bad --window '%s' (WxH)", win);
    const char *user_env = getenv("SLIM_USER_PROFILE");
    if (!user && user_env && *user_env) user = user_env;
    char default_user[65];
    if (!user && !udd) {
        char *selected = NULL;
        int dr = userprof_default(&selected);
        if (dr < 0) return -1;
        if (dr == 0) {
            snprintf(default_user, sizeof default_user, "%s", selected);
            user = default_user;
        }
        free(selected);
    }

    slim_state old;
    if (state_load(c->session, &old) == 0) {
        int up = old.attached ? devtools_up(old.port, 1000) : proc_is_browser(old.pid, old.port);
        if (up) {
            int pid = old.pid, port = old.port;
            state_free(&old);
            return slim_seterr("session '%s' already running (pid %d, port %d)", c->session, pid, port);
        }
        state_free(&old);
        state_remove(c->session);
    }

    int port = c->port;
    if (port) {
        if (!port_free(port)) return slim_seterr("port %d is already in use", port);
    } else {
        for (int p = PORT_MIN; p <= PORT_MAX && !port; p++)
            if (p != 9222 && !state_port_taken(p, c->session) && port_free(p)) port = p;
        if (!port) return slim_seterr("no free port in %d-%d", PORT_MIN, PORT_MAX);
    }

    char *engine = slim_find_engine(eng, 1);
    if (!engine) return -1;
    if (gov_on) { char *pe = slim_engine_private(engine); if (pe) { free(engine); engine = pe; slim_engine_libs(engine); } }

    sbuf ud = {0};
    if (user) {
        char named[PATH_MAX];
        if (userprof_data_dir(user, named, sizeof named)) { free(engine); return -1; }
        sb_puts(&ud, named);
    } else if (udd) sb_puts(&ud, udd);
    else {
        const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        if (x && *x == '/') sb_printf(&ud, "%s/slimium/%s", x, c->session);
        else if (h && *h) sb_printf(&ud, "%s/.local/share/slimium/%s", h, c->session);
        else sb_printf(&ud, "/tmp/slimium-%u-data/%s", (unsigned)getuid(), c->session);
    }
    if (mkdir_p(ud.p, 0700)) { free(engine); sb_free(&ud); return -1; }
    char rud[PATH_MAX];
    if (!realpath(ud.p, rud)) snprintf(rud, sizeof rud, "%s", ud.p);
    sb_free(&ud);
    if (user && userprof_check_idle(user)) { free(engine); return -1; }

    char ua_arg[2080], lang_arg[160];
    int have_ua = 0, have_lang = 0;
    if (user) {
        userprof_t up;
        if (userprof_load(user, &up)) { free(engine); return -1; }
        if (up.ua && *up.ua) {
            snprintf(ua_arg, sizeof ua_arg, "--user-agent=%s", up.ua); have_ua = 1;
        }
        if (up.locale && *up.locale) {
            snprintf(lang_arg, sizeof lang_arg, "--lang=%s", up.locale); have_lang = 1;
        }
        userprof_free(&up);
    }

    char logp[4096];
    if (state_path(c->session, ".log", logp, sizeof logp)) { free(engine); return -1; }

    /* argv: our switches + extra args, merged with the profile */
    char a_port[40], a_udd[PATH_MAX + 20], a_win[48];
    snprintf(a_port, sizeof a_port, "--remote-debugging-port=%d", port);
    snprintf(a_udd, sizeof a_udd, "--user-data-dir=%s", rud);
    snprintf(a_win, sizeof a_win, "--window-size=%d,%d", ww, wh);
    char **args = xmalloc(sizeof *args * (size_t)(argc + 12));
    int n = 0;
    args[n++] = a_port; args[n++] = a_udd;
    /* chrome-headless-shell is always headless and has no --headless=new mode */
    if (!headful && !strstr(path_base(engine), "headless")) args[n++] = "--headless=new";
    if (win) args[n++] = a_win;
    args[n++] = "--no-first-run";
    args[n++] = "--no-default-browser-check";
    if (have_ua) args[n++] = ua_arg;
    if (have_lang) args[n++] = lang_arg;
    for (int i = 0; i < argc; i++) args[n++] = argv[i];
    args[n++] = "about:blank";
    args[n] = NULL;
    char **av = slim_profile_merge_argv(level, engine, n, args);
    if (!av) { free(args); free(engine); return slim_seterr("out of memory"); }

    if (gov_on) slim_engine_fadvise(engine);
    pid_t pid = fork();
    if (pid < 0) { free(av); free(args); free(engine); return slim_seterr("fork: %s", strerror(errno)); }
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        int lf = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (nul >= 0) dup2(nul, 0);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        else if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        closefrom(3);
        char **sv = gov_on ? slim_scope_argv(av, getpid()) : NULL;
        if (sv) execv(sv[0], sv);
        execv(engine, av);
        _exit(127);
    }
    free(av); free(args);

    long long dl = now_ms() + c->timeout;
    char *wsurl = NULL;
    for (;;) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            char t[300]; tail_log(logp, t, sizeof t);
            free(engine);
            return slim_seterr("browser exited during startup (log %s)%s%s", logp, *t ? ": " : "", t);
        }
        if (cdp_browser_url(port, 500, &wsurl, NULL) == 0) break;
        if (now_ms() > dl) {
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(engine);
            return slim_seterr("browser DevTools did not come up on port %d within %d ms (log %s)", port, c->timeout, logp);
        }
        sleep_ms(50);
    }

    slim_state s; memset(&s, 0, sizeof s);
    s.name = xstrdup(c->session);
    s.pid = (int)pid; s.port = port; s.headless = !headful;
    s.ws = wsurl;
    s.profile = xstrdup(prof);
    if (user) s.user = xstrdup(user);
    s.udd = xstrdup(rud);
    s.engine = engine;
    s.log = xstrdup(logp);
    int r = state_save(&s);
    if (r) { kill(-pid, SIGTERM); state_free(&s); return -1; }
    pid_t gpid = gov_on ? slim_gov_spawn(pid, port, rud, 0) : 0;
    out_t(c, "started session %s: pid %d port %d profile %s%s", s.name, s.pid, port, prof, headful ? " (headful)" : "");
    if (gpid > 0) out_t(c, ", governor pid %d", (int)gpid);
    if (user) out_t(c, " user %s", user);
    out_i(c, "governor", gpid > 0 ? gpid : 0);
    out_s(c, "session", s.name); out_i(c, "pid", s.pid); out_i(c, "port", port);
    out_s(c, "ws", s.ws); out_s(c, "profile", prof);
    if (user) out_s(c, "user", user);
    out_s(c, "user_data_dir", rud);
    out_b(c, "headless", !headful); out_s(c, "engine", engine); out_s(c, "log", logp);
    state_free(&s);
    return 0;
}

int cmd_stop(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "stop takes no arguments");
    if (c->port) return usage_err(c, "stop works on sessions (--session), not --port");
    slim_state s;
    if (state_load(c->session, &s)) return -1;
    if (s.attached) {
        state_remove(c->session);
        out_t(c, "detached session %s (browser on port %d left running)", c->session, s.port);
        out_s(c, "session", c->session); out_b(c, "detached", 1);
        state_free(&s);
        return 0;
    }
    int how = 0; /* 0 already dead, 1 graceful, 2 SIGTERM, 3 SIGKILL */
    if (proc_is_browser(s.pid, s.port)) {
        cdp_t cd;
        if (s.ws && cdp_connect(&cd, s.ws, 2000) == 0) {
            cdp_call(&cd, NULL, "Browser.close", NULL, 3000, NULL, NULL);
            cdp_close(&cd);
        }
        how = 1;
        if (!wait_gone(s.pid, 5000)) {
            how = 2;
            if (getpgid(s.pid) == s.pid) kill(-s.pid, SIGTERM); else kill(s.pid, SIGTERM);
            if (!wait_gone(s.pid, 3000)) {
                how = 3;
                if (getpgid(s.pid) == s.pid) kill(-s.pid, SIGKILL); else kill(s.pid, SIGKILL);
                wait_gone(s.pid, 2000);
            }
        }
        /* leftovers of the browser's own process group (only processes it spawned) */
        if (kill(-s.pid, 0) == 0) {
            long long dl = now_ms() + 2000;
            while (kill(-s.pid, 0) == 0 && now_ms() < dl) sleep_ms(50);
            if (kill(-s.pid, 0) == 0) kill(-s.pid, SIGKILL);
        }
    }
    state_remove(c->session);
    static const char *const hw[] = {"was not running", "closed", "terminated", "killed"};
    out_t(c, "stopped session %s (%s)", c->session, hw[how]);
    out_s(c, "session", c->session); out_s(c, "result", hw[how]);
    state_free(&s);
    return 0;
}

static int count_tabs(ctx_t *c) {
    char *resp; jv arr, t; size_t pos = 0; int n = 0;
    if (ctx_targets(c, &resp, &arr)) return -1;
    while (json_next(arr, &pos, NULL, &t)) if (json_streq_path(t, "type", "page")) n++;
    free(resp);
    return n;
}

int cmd_status(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "status takes no arguments");
    int pid, port; const char *prof = NULL;
    if (c->port) { port = c->port; pid = find_browser_pid(port); }
    else {
        if (ctx_state(c)) return -1;
        port = c->st.port; pid = c->st.pid; prof = c->st.profile;
        if (c->st.attached || !pid) pid = find_browser_pid(port);
    }
    char *url = NULL, *product = NULL;
    if (cdp_browser_url(port, c->timeout < 3000 ? c->timeout : 3000, &url, &product)) {
        if (c->port) return slim_seterr("no browser on port %d", port);
        return slim_seterr("session '%s' is not running (port %d)", c->session, port);
    }
    free(url);
    int tabs = count_tabs(c);
    slim_mem m; memset(&m, 0, sizeof m);
    int have_mem = pid > 0 && slim_mem_tree(pid, &m) == 0;
    if (!c->port) { out_t(c, "session  %s\n", c->session); out_s(c, "session", c->session); }
    out_t(c, "alive    yes\npid      %d\nport     %d\n", pid, port);
    out_b(c, "alive", 1); out_i(c, "pid", pid); out_i(c, "port", port);
    if (product) { out_t(c, "browser  %s\n", product); out_s(c, "browser", product); }
    if (prof) { out_t(c, "profile  %s\n", prof); out_s(c, "profile", prof); }
    if (c->have_st) {
        out_b(c, "attached", c->st.attached);
        if (c->st.attached) out_t(c, "attached yes\n");
        if (c->st.target) { out_t(c, "tab      %s\n", c->st.target); out_s(c, "tab", c->st.target); }
        if (c->st.user) { out_t(c, "user     %s\n", c->st.user); out_s(c, "user", c->st.user); }
    }
    out_t(c, "tabs     %d", tabs); out_i(c, "tabs", tabs);
    if (have_mem) {
        out_t(c, "\nprocs    %d\npss_kb   %ld\nrss_kb   %ld\nuss_kb   %ld", m.procs, m.pss_kb, m.rss_kb, m.uss_kb);
        out_i(c, "procs", m.procs); out_i(c, "pss_kb", m.pss_kb); out_i(c, "rss_kb", m.rss_kb); out_i(c, "uss_kb", m.uss_kb);
        out_t(c, "\niso_kb   %ld\nanon_kb  %ld\nshmem_kb %ld\nfile_union_kb %ld", m.iso_kb, m.anon_kb, m.shmem_kb, m.file_union_kb);
        out_i(c, "iso_kb", m.iso_kb); out_i(c, "anon_kb", m.anon_kb); out_i(c, "shmem_kb", m.shmem_kb); out_i(c, "file_union_kb", m.file_union_kb);
        pid_t gp = slim_gov_find(pid);
        out_t(c, "\ngovernor %d", (int)gp); out_i(c, "governor", gp);
    }
    free(product);
    return 0;
}

int cmd_attach(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "attach takes no positional arguments");
    if (!c->port) return usage_err(c, "attach needs --port N");
    slim_state old;
    if (state_load(c->session, &old) == 0) {
        int running = !old.attached && proc_is_browser(old.pid, old.port);
        state_free(&old);
        if (running) return slim_seterr("session '%s' already runs its own browser; use another --session", c->session);
    }
    char *url, *product = NULL;
    if (cdp_browser_url(c->port, c->timeout < 5000 ? c->timeout : 5000, &url, &product)) return -1;
    slim_state s; memset(&s, 0, sizeof s);
    s.name = xstrdup(c->session);
    s.port = c->port; s.attached = 1; s.headless = 0;
    s.pid = find_browser_pid(c->port);
    s.ws = url;
    int r = state_save(&s);
    if (!r) {
        out_t(c, "attached session %s to port %d (pid %d)%s%s", s.name, s.port, s.pid, product ? ", " : "", product ? product : "");
        out_s(c, "session", s.name); out_i(c, "port", s.port); out_i(c, "pid", s.pid); out_s(c, "ws", url);
        if (product) out_s(c, "browser", product);
    }
    free(product);
    state_free(&s);
    return r;
}

int cmd_sessions(ctx_t *c, int argc, char **argv) {
    if (opt_done(c, &argc, argv)) return -1;
    (void)argv;
    char d[3072];
    if (state_dir(d, sizeof d)) return -1;
    DIR *dir = opendir(d);
    if (!dir) return slim_seterr("%s: %s", d, strerror(errno));
    struct dirent *e;
    sb_puts(&c->oj, ",\"sessions\":[");
    int n = 0;
    while ((e = readdir(dir))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") || l - 5 > 64) continue;
        char name[80]; memcpy(name, e->d_name, l - 5); name[l - 5] = 0;
        slim_state s;
        if (state_load(name, &s)) continue;
        int alive = s.attached ? devtools_up(s.port, 500) : proc_is_browser(s.pid, s.port);
        if (n++) sb_putc(&c->oj, ',');
        sb_puts(&c->oj, "{\"session\":"); sb_jstrz(&c->oj, name);
        sb_printf(&c->oj, ",\"pid\":%d,\"port\":%d,\"alive\":%s,\"attached\":%s}", s.pid, s.port, alive ? "true" : "false", s.attached ? "true" : "false");
        out_t(c, "%s%s\tport %d\tpid %d\t%s%s", n > 1 ? "\n" : "", name, s.port, s.pid, alive ? "alive" : "dead", s.attached ? "\tattached" : "");
        state_free(&s);
    }
    closedir(dir);
    sb_putc(&c->oj, ']');
    return 0;
}
