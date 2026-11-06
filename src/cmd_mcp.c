/* MCP command registration (weak-hook module in cmd_modules.c). */
#include "mcp.h"
#include "profile.h"

static int cmd_mcp(ctx_t *c, int argc, char **argv) {
    const char *profile = opt_val(c, &argc, argv, "--profile");
    if (opt_done(c, &argc, argv)) return -1;
    if (argc) return usage_err(c, "mcp takes no positional arguments");
    if (profile && slim_profile_parse(profile) < 0)
        return usage_err(c, "bad --profile '%s' (off|balanced|max)", profile);
    return slim_mcp_serve(c, profile);
}

static const cmd_def MCP_COMMANDS[] = {
    {"mcp", cmd_mcp, CF_NOBATCH, "[--profile off|balanced|max]  stdio JSON-RPC 2.0 MCP server"},
};

const cmd_def *slim_mcp_commands(size_t *count) {
    *count = sizeof MCP_COMMANDS / sizeof MCP_COMMANDS[0];
    return MCP_COMMANDS;
}
