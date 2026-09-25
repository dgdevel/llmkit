/* main.c - subcommand dispatch, help, version (design sec.13). */
#include "llmkit.h"

#include <stdio.h>
#include <string.h>

static int usage(FILE *out) {
    fputs("usage: llmkit <command> [args]\n"
          "\n"
          "commands:\n"
          "  runner                 run a conversation: jsonl records on\n"
          "                         stdin, jsonl records on stdout\n"
          "  agent-as-tool <seed>   expose one agent conversation as an mcp\n"
          "                         tool on stdio\n"
          "  mcp-proxy <config>     expose a curated view of upstream mcp\n"
          "                         servers on stdio\n"
          "  call <flags>           one prompt in, one answer out: plain\n"
          "                         text on stdout\n"
          "  repl <flags>           interactive chat: type the turns, see\n"
          "                         thinking and tool calls as they happen\n"
          "  help                   show this help\n"
          "  version                show the version\n",
          out);
    return 1;
}

int cmd_help(void) {
    usage(stdout);
    return 0;
}

int cmd_version(void) {
    printf("llmkit %s\n", LLMKIT_VERSION);
    return 0;
}

int main(int argc, char **argv) {
    http_global_init();
    if (argc < 2) return usage(stderr);
    const char *cmd = argv[1];
    if (!strcmp(cmd, "runner")) {
        if (argc != 2) return usage(stderr);
        return cmd_runner();
    }
    if (!strcmp(cmd, "agent-as-tool")) {
        if (argc != 3) return usage(stderr);
        return cmd_agent(argv[2]);
    }
    if (!strcmp(cmd, "mcp-proxy")) {
        if (argc != 3) return usage(stderr);
        return cmd_proxy(argv[2]);
    }
    if (!strcmp(cmd, "call")) return cmd_call(argc, argv);
    if (!strcmp(cmd, "repl")) return cmd_repl(argc, argv);
    if (!strcmp(cmd, "help")) return cmd_help();
    if (!strcmp(cmd, "version")) return cmd_version();
    fprintf(stderr, "llmkit: unknown command '%s'\n", cmd);
    return usage(stderr);
}
