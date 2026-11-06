/* Stdio Model Context Protocol server. */
#ifndef SLIM_MCP_H
#define SLIM_MCP_H
#include "cmd.h"

/* Serve newline-delimited JSON-RPC until stdin closes. memory_profile is the
 * optional off|balanced|max default for browser_start (not a user profile). */
int slim_mcp_serve(ctx_t *c, const char *memory_profile);

#endif
