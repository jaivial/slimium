/* Detached Page.startScreencast recorder (runtime ffmpeg, no linked codecs). */
#ifndef SLIM_RECORD_H
#define SLIM_RECORD_H
#include "cmd.h"

int record_command(ctx_t *c, int argc, char **argv);
int record_worker_command(ctx_t *c, int argc, char **argv);

#endif
