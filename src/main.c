#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "llmkit.h"
#include "config.h"
#include "agent.h"
#include "proxy.h"
#include "util.h"

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
            "  proxy   Run MCP proxy server (stdio or HTTP)\n");
}

/* Read a config value from argv for a given flag.
 * Returns the value pointer or NULL if not found. */
static const char *get_flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

/* Hash the contents of a file and store the hex digest in out[65]. */
static int hash_config_file(const char *path, char out[65]) {
    char *content = util_read_file(path);
    if (content == NULL) {
        log_activity("[error] Cannot read config file: %s", path);
        return EXIT_CONFIG_ERR;
    }
    util_sha256(content, strlen(content), out);
    free(content);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return EXIT_ARGS_ERR;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return EXIT_SUCCESS;
    }

    /* ---- agent ---- */
    if (strcmp(argv[1], "agent") == 0) {
        const char *config_path = get_flag(argc, argv, "-c");
        const char *output_path = get_flag(argc, argv, "-o");
        const char *prompt_arg = get_flag(argc, argv, "-p");

        if (config_path == NULL || output_path == NULL || prompt_arg == NULL) {
            fprintf(stderr, "error: agent requires -c <config> -o <output> -p <prompt>\n");
            return EXIT_ARGS_ERR;
        }

        runtime_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));

        /* UUID for this run. */
        util_uuid_v4(ctx.run_id);

        /* Hash the raw config YAML *before* parsing it. */
        int rc = hash_config_file(config_path, ctx.config_hash);
        if (rc != EXIT_SUCCESS) return rc;

        /* Load and validate config. */
        rc = config_load(config_path, &ctx);
        if (rc != EXIT_SUCCESS) {
            config_free(&ctx);
            return rc;
        }

        rc = agent_run(&ctx, output_path, prompt_arg);
        config_free(&ctx);
        return rc;
    }

    /* ---- proxy ---- */
    if (strcmp(argv[1], "proxy") == 0) {
        const char *config_path = get_flag(argc, argv, "-c");
        const char *listen_addr = get_flag(argc, argv, "-l");

        if (config_path == NULL) {
            fprintf(stderr, "error: proxy requires -c <config>\n");
            return EXIT_ARGS_ERR;
        }

        runtime_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));

        int rc = config_load(config_path, &ctx);
        if (rc != EXIT_SUCCESS) {
            config_free(&ctx);
            return rc;
        }

        rc = proxy_run(&ctx, listen_addr);
        config_free(&ctx);
        return rc;
    }

    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    print_usage();
    return EXIT_ARGS_ERR;
}
