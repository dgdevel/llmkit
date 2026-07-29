#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "llmkit.h"
#include "config.h"
#include "agent.h"
#include "proxy.h"
#include "conversation.h"
#include "util.h"

static void print_usage(void) {
    fprintf(stderr, "llmkit v" LLMKIT_VERSION "\n"
                    "\n"
                    "Usage:\n"
                    "  llmkit agent -c <config.yml> --conversation <convo.jsonl> "
                    "-p <prompt|prompt_file> [--mode <type>]\n"
                    "  llmkit proxy -c <config.yml> [-l <host:port>]\n"
                    "  llmkit response --conversation <conversation.jsonl>\n"
                    "\n"
                    "Commands:\n"
                    "  agent     Run LLM conversation agent with MCP tool support\n"
                    "  proxy     Run MCP proxy server (stdio or HTTP)\n"
                    "  response  Print the last LLM response from a conversation\n"
                    "\n"
                    "Flags:\n"
                    "  -c, --config <file>        YAML configuration file (agent, proxy)\n"
                    "  --conversation <file>      Conversation JSONL file. The agent\n"
                    "                             appends to it and continues prior turns;\n"
                    "                             response reads it\n"
                    "  -p, --prompt <text|file>   Prompt text, or path to a file with it\n"
                    "                             (agent)\n"
                    "  --mode <type>              Stdout output mode (agent only)\n"
                    "                             quiet  (default) print only the final response\n"
                    "                             debug  timestamped event lines\n"
                    "                             stream JSONL event stream\n"
                    "  --steer                    Enable steering: read additional user messages\n"
                    "                             from stdin during the run and inject them into\n"
                    "                             the conversation at the next turn (agent only)\n"
                    "  -l, --listen <host:port>   Listen address; omit for stdio mode (proxy)\n"
                    "  -h, --help                 Print this help and exit\n"
                    "  -V, --version              Print version and exit\n");
}

/* Read a value from argv for a given flag, accepting both a short and a long
 * form (either may be NULL). Returns the value pointer or NULL if not found. */
static const char *get_flag(int argc, char **argv, const char *short_flag, const char *long_flag) {
    for (int i = 1; i < argc - 1; i++) {
        if ((short_flag != NULL && strcmp(argv[i], short_flag) == 0) ||
            (long_flag != NULL && strcmp(argv[i], long_flag) == 0)) {
            return argv[i + 1];
        }
    }
    return NULL;
}

/* Test for a boolean (valueless) flag, accepting both a short and a long
 * form (either may be NULL). Returns true if present. */
static bool has_flag(int argc, char **argv, const char *short_flag, const char *long_flag) {
    for (int i = 1; i < argc; i++) {
        if ((short_flag != NULL && strcmp(argv[i], short_flag) == 0) ||
            (long_flag != NULL && strcmp(argv[i], long_flag) == 0)) {
            return true;
        }
    }
    return false;
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

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("llmkit v" LLMKIT_VERSION "\n");
        return EXIT_SUCCESS;
    }

    /* ---- agent ---- */
    if (strcmp(argv[1], "agent") == 0) {
        const char *config_path = get_flag(argc, argv, "-c", "--config");
        const char *convo_path = get_flag(argc, argv, NULL, "--conversation");
        const char *prompt_arg = get_flag(argc, argv, "-p", "--prompt");
        const char *output_mode = get_flag(argc, argv, NULL, "--mode");
        bool steering = has_flag(argc, argv, NULL, "--steer");

        /* Default output mode. */
        if (output_mode == NULL) {
            output_mode = RUN_MODE_QUIET;
        } else {
            if (strcmp(output_mode, RUN_MODE_QUIET) != 0 &&
                strcmp(output_mode, RUN_MODE_DEBUG) != 0 &&
                strcmp(output_mode, RUN_MODE_STREAM) != 0) {
                fprintf(stderr, "error: invalid --mode '%s' (valid: quiet, debug, stream)\n",
                        output_mode);
                return EXIT_ARGS_ERR;
            }
        }

        if (config_path == NULL || convo_path == NULL || prompt_arg == NULL) {
            fprintf(stderr, "error: agent requires -c <config> --conversation <file> -p "
                            "<prompt>\n");
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

        rc = agent_run(&ctx, convo_path, prompt_arg, output_mode, steering);
        config_free(&ctx);
        return rc;
    }

    /* ---- proxy ---- */
    if (strcmp(argv[1], "proxy") == 0) {
        const char *config_path = get_flag(argc, argv, "-c", "--config");
        const char *listen_addr = get_flag(argc, argv, "-l", "--listen");

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

    /* ---- response ---- */
    if (strcmp(argv[1], "response") == 0) {
        const char *file_path = get_flag(argc, argv, NULL, "--conversation");

        if (file_path == NULL) {
            fprintf(stderr, "error: response requires --conversation <conversation.jsonl>\n");
            return EXIT_ARGS_ERR;
        }

        char *content = NULL;
        char *model = NULL;
        usage_info usage;
        int rc = conversation_read_last_assistant(file_path, &content, &model, &usage);
        if (rc != EXIT_SUCCESS) {
            free(content);
            free(model);
            return rc;
        }

        if (usage.total_tokens > 0) {
            fprintf(stderr, "[stats] %s | %d tokens (prompt=%d + completion=%d)\n",
                    model && model[0] ? model : "?", usage.total_tokens, usage.prompt_tokens,
                    usage.completion_tokens);
        }
        free(model);

        printf("%s", content);
        /* Print trailing newline only when content is non-empty. */
        if (content[0] != '\0') printf("\n");
        free(content);
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    print_usage();
    return EXIT_ARGS_ERR;
}
