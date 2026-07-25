#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "llmkit.h"

static void print_usage(void) {
    fprintf(stderr,
            "llmkit v" LLMKIT_VERSION "\n"
            "\n"
            "Usage:\n"
            "  llmkit agent -c <config.yml> -o <conversation.jsonl> -p <prompt|prompt_file>\n"
            "  llmkit proxy -c <config.yml> [-l <host:port>]\n"
            "\n"
            "Commands:\n"
            "  agent   Run LLM conversation agent with MCP tool support\n"
            "  proxy   Run MCP proxy server (stdio or HTTP)\n"
            "\n"
            "Run 'llmkit <command> --help' for detailed options.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return EXIT_ARGS_ERR;
    }

    if (strcmp(argv[1], "agent") == 0) {
        fprintf(stderr, "[init] LLMKIT agent mode (stub)\n");
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "proxy") == 0) {
        fprintf(stderr, "[init] LLMKIT proxy mode (stub)\n");
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    print_usage();
    return EXIT_ARGS_ERR;
}
