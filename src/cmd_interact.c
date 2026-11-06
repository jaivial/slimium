/* Interaction command registration. Implementation lives in interact.c. */
#include "interact.h"

const cmd_def *slim_interact_commands(size_t *count) {
    static const cmd_def defs[] = {
        {"snapshot", interact_snapshot, 0, "[--interactive] [--all] [--selector S] [--max N] compact accessibility tree"},
        {"find", interact_find, 0, "LOCATOR [--max N] count matches and assign stable @eN refs"},
        {"inspect", interact_inspect, 0, "LOCATOR unique selector, attributes, box and computed styles"},
        {"upload", interact_upload, 0, "LOCATOR FILE... set files on an input[type=file]"},
        {"drag", interact_drag, 0, "FROM TO drag between element centers"},
        {"focus", interact_focus, 0, "LOCATOR scroll and focus an element"},
        {"submit", interact_submit, 0, "LOCATOR submit the element's form"},
        {"form", interact_form, 0, "fill JSON|@file fill named locators in one connection"},
        {"act", interact_act, 0, "JSON|@file|- [--stop-on-error] [--snapshot] execute an action array"},
        {"pipe", interact_pipe, CF_NOBATCH, "[--stop-on-error] NDJSON commands over one persistent connection"},
    };
    *count = sizeof defs / sizeof defs[0];
    return defs;
}
