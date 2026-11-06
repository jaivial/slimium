/* Named persistent Chromium user profiles (not the off|balanced|max memory profiles). */
#ifndef SLIM_USERPROF_H
#define SLIM_USERPROF_H
#include <stddef.h>

typedef struct {
    char *name;
    char *ua, *locale;
    long long created_ms, updated_ms, saved_ms;
    char *pages;       /* JSON array of {url,scroll_x,scroll_y}; never storage values */
    int current_index;
    char *current_url;
} userprof_t;

int userprof_valid_name(const char *name);
void userprof_free(userprof_t *p);
int userprof_load(const char *name, userprof_t *p);
int userprof_create(const char *name, const char *ua, const char *locale);
int userprof_list(userprof_t **items, size_t *count);
void userprof_sort(userprof_t *items, size_t count);
int userprof_rename(const char *old_name, const char *new_name);
int userprof_delete(const char *name);

/* 0 = selected, 1 = no default, -1 = error. Does not create the registry. */
int userprof_default(char **name);
int userprof_set_default(const char *name);

/* Validate the profile, create <root>/<name>/data if needed, return its canonical path. */
int userprof_data_dir(const char *name, char *buf, size_t cap);
/* Refuse a live browser already using this named profile. */
int userprof_check_idle(const char *name);

/* Atomically replace only the saved tab snapshot, preserving preferences. */
int userprof_save_pages(const char *name, const char *pages_json,
                        int current_index, const char *current_url);

#endif
