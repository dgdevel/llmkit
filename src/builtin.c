/* builtin.c - llmkit builtin-mcp: a stdio mcp server offering generic-use
   tools (web search/fetch, file list/search/read/create/edit/copy/move/
   delete/analyze, process exec/status, skills search/read). No config: `llmkit builtin-mcp`
   serves json-rpc on stdin/stdout. Tool and argument descriptions live
   in src/prompts/mcp/<tool>/description.txt and .../arguments/<arg>.txt,
   bundled into the binary at build time by tools/gen-prompts.sh (an
   empty txt is sent as an empty description) - edit the txt files and
   rebuild. Everything is stateless except the process tools: their
   spawned-process table (pids, exit codes and one temp output file each)
   lives until the server exits. The implementations live in src/builtin/
   (one file per tool family, shared helpers in src/builtin/util.c); this
   file is the tool table, the dispatcher and the json-rpc handler. */
#include "llmkit.h"
#include "builtin/internal.h"
#include "prompts.gen.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* process_send_signal: this build's tool texts are the windows variants */
#define prompt_mcp_process_send_signal_description \
    prompt_mcp_process_send_signal_description_windows
#define prompt_mcp_process_send_signal_arguments_signal \
    prompt_mcp_process_send_signal_arguments_signal_windows
#endif

/* ================= tool table ================= */

typedef struct arg_def {
    const char *name;
    const char *type;   /* "string" | "integer" | "boolean" */
    const char *desc;
    bool required;
} arg_def_t;

typedef struct tool_def {
    const char *name;
    const char *desc;
    const arg_def_t *args;
    size_t nargs;
} tool_def_t;

static const arg_def_t ARGS_web_search[] = {
    {"keywords", "string", prompt_mcp_web_search_arguments_keywords, true},
};
static const arg_def_t ARGS_web_fetch[] = {
    {"url", "string", prompt_mcp_web_fetch_arguments_url, true},
};
static const arg_def_t ARGS_files_list[] = {
    {"path", "string", prompt_mcp_files_list_arguments_path, true},
    {"regex", "string", prompt_mcp_files_list_arguments_regex, true},
    {"recurse_subdirectories", "boolean",
     prompt_mcp_files_list_arguments_recurse_subdirectories, false},
    {"show_hidden_files", "boolean",
     prompt_mcp_files_list_arguments_show_hidden_files, false},
};
static const arg_def_t ARGS_files_search[] = {
    {"path", "string", prompt_mcp_files_search_arguments_path, true},
    {"regex", "string", prompt_mcp_files_search_arguments_regex, true},
    {"show_hidden_files", "boolean",
     prompt_mcp_files_search_arguments_show_hidden_files, false},
};
static const arg_def_t ARGS_file_read[] = {
    {"path", "string", prompt_mcp_file_read_arguments_path, true},
    {"lines_offset", "integer", prompt_mcp_file_read_arguments_lines_offset,
     true},
    {"lines_length", "integer", prompt_mcp_file_read_arguments_lines_length,
     true},
};
static const arg_def_t ARGS_file_create[] = {
    {"path", "string", prompt_mcp_file_create_arguments_path, true},
    {"content", "string", prompt_mcp_file_create_arguments_content, true},
    {"overwrite", "boolean", prompt_mcp_file_create_arguments_overwrite,
     false},
};
static const arg_def_t ARGS_file_edit[] = {
    {"path", "string", prompt_mcp_file_edit_arguments_path, true},
    {"oldString", "string", prompt_mcp_file_edit_arguments_oldString, true},
    {"newString", "string", prompt_mcp_file_edit_arguments_newString, true},
    {"line_number", "integer", prompt_mcp_file_edit_arguments_line_number,
     true},
};
static const arg_def_t ARGS_file_delete[] = {
    {"path", "string", prompt_mcp_file_delete_arguments_path, true},
};
static const arg_def_t ARGS_file_copy[] = {
    {"sourcepath", "string", prompt_mcp_file_copy_arguments_sourcepath,
     true},
    {"destpath", "string", prompt_mcp_file_copy_arguments_destpath, true},
    {"overwrite", "boolean", prompt_mcp_file_copy_arguments_overwrite,
     false},
};
static const arg_def_t ARGS_file_move[] = {
    {"sourcepath", "string", prompt_mcp_file_move_arguments_sourcepath,
     true},
    {"destpath", "string", prompt_mcp_file_move_arguments_destpath, true},
    {"overwrite", "boolean", prompt_mcp_file_move_arguments_overwrite,
     false},
};
static const arg_def_t ARGS_file_analyze[] = {
    {"path", "string", prompt_mcp_file_analyze_arguments_path, true},
};
static const arg_def_t ARGS_process_exec[] = {
    {"cmdline", "string", prompt_mcp_process_exec_arguments_cmdline, true},
};
static const arg_def_t ARGS_process_status[] = {
    {"pid", "integer", prompt_mcp_process_status_arguments_pid, true},
};
static const arg_def_t ARGS_process_wait[] = {
    {"pid", "integer", prompt_mcp_process_wait_arguments_pid, true},
    {"timeout", "integer", prompt_mcp_process_wait_arguments_timeout, true},
};
static const arg_def_t ARGS_process_send_signal[] = {
    {"pid", "integer", prompt_mcp_process_send_signal_arguments_pid, true},
    {"signal", "string", prompt_mcp_process_send_signal_arguments_signal,
     true},
};
static const arg_def_t ARGS_skills_search[] = {
    {"keywords", "string", prompt_mcp_skills_search_arguments_keywords, true},
};
static const arg_def_t ARGS_skills_read[] = {
    {"name", "string", prompt_mcp_skills_read_arguments_name, true},
};

static const tool_def_t TOOLS[] = {
    {"web_search", prompt_mcp_web_search_description, ARGS_web_search, 1},
    {"web_fetch", prompt_mcp_web_fetch_description, ARGS_web_fetch, 1},
    {"files_list", prompt_mcp_files_list_description, ARGS_files_list, 4},
    {"files_search", prompt_mcp_files_search_description, ARGS_files_search,
     3},
    {"file_read", prompt_mcp_file_read_description, ARGS_file_read, 3},
    {"file_create", prompt_mcp_file_create_description, ARGS_file_create, 3},
    {"file_edit", prompt_mcp_file_edit_description, ARGS_file_edit, 4},
    {"file_delete", prompt_mcp_file_delete_description, ARGS_file_delete, 1},
    {"file_copy", prompt_mcp_file_copy_description, ARGS_file_copy, 3},
    {"file_move", prompt_mcp_file_move_description, ARGS_file_move, 3},
    {"file_analyze", prompt_mcp_file_analyze_description, ARGS_file_analyze,
     1},
    {"process_exec", prompt_mcp_process_exec_description, ARGS_process_exec,
     1},
    {"process_status", prompt_mcp_process_status_description,
     ARGS_process_status, 1},
    {"process_wait", prompt_mcp_process_wait_description,
     ARGS_process_wait, 2},
    {"process_send_signal", prompt_mcp_process_send_signal_description,
     ARGS_process_send_signal, 2},
    {"skills_search", prompt_mcp_skills_search_description, ARGS_skills_search,
     1},
    {"skills_read", prompt_mcp_skills_read_description, ARGS_skills_read, 1},
};
enum { TOOLS_N = sizeof TOOLS / sizeof TOOLS[0] };

/* ================= small utilities ================= */

/* ---- tool entry points (the .c files under src/builtin) ---- */

/* src/builtin/web.c */
void tool_web_search(const cJSON *args, buf_t *out, bool *is_error);
void tool_web_fetch(const cJSON *args, buf_t *out, bool *is_error);
/* src/builtin/files.c */
void tool_files_list(const cJSON *a, buf_t *o, bool *e);
void tool_files_search(const cJSON *a, buf_t *o, bool *e);
void tool_file_read(const cJSON *args, buf_t *out, bool *is_error);
void tool_file_create(const cJSON *args, buf_t *out, bool *is_error);
void tool_file_edit(const cJSON *args, buf_t *out, bool *is_error);
void tool_file_delete(const cJSON *args, buf_t *out, bool *is_error);
void tool_file_copy(const cJSON *args, buf_t *out, bool *is_error);
void tool_file_move(const cJSON *args, buf_t *out, bool *is_error);
/* src/builtin/analyze.c */
void tool_file_analyze(const cJSON *args, buf_t *out, bool *is_error);
/* src/builtin/process.c */
void tool_process_exec(const cJSON *args, buf_t *out, bool *is_error);
void tool_process_status(const cJSON *args, buf_t *out, bool *is_error);
void tool_process_wait(const cJSON *args, buf_t *out, bool *is_error);
void tool_process_send_signal(const cJSON *args, buf_t *out, bool *is_error);
/* src/builtin/skills.c */
void tool_skills_search(const cJSON *args, buf_t *out, bool *is_error);
void tool_skills_read(const cJSON *args, buf_t *out, bool *is_error);

static const tool_fn TOOL_FNS[TOOLS_N] = {
    tool_web_search, tool_web_fetch,  tool_files_list, tool_files_search,
    tool_file_read,  tool_file_create, tool_file_edit, tool_file_delete,
    tool_file_copy, tool_file_move,
    tool_file_analyze,
    tool_process_exec, tool_process_status, tool_process_wait,
    tool_process_send_signal,
    tool_skills_search, tool_skills_read,
};

/* ================= json-rpc handler ================= */

static cJSON *tools_list_result(void) {
    cJSON *tools = cJSON_CreateArray();
    for (size_t i = 0; i < TOOLS_N; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON *req = cJSON_CreateArray();
        for (size_t a = 0; a < TOOLS[i].nargs; a++) {
            const arg_def_t *d = &TOOLS[i].args[a];
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", d->type);
            cJSON_AddStringToObject(p, "description", d->desc);
            cJSON_AddItemToObject(props, d->name, p);
            if (d->required) cJSON_AddItemToArray(req, cJSON_CreateString(d->name));
        }
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "tools", tools);
    return res;
}

int builtin_handle(void *ctx, const char *method, cJSON *params, cJSON *id,
                   cJSON **result_out, char **errmsg_out) {
    (void)ctx;
    (void)id;
    int rc = rpc_common(method, params, "llmkit-builtin-mcp", result_out);
    if (rc >= 0) return rc;
    if (!strcmp(method, "tools/list")) {
        *result_out = tools_list_result();
        return 0;
    }
    if (!strcmp(method, "tools/call")) {
        const char *name = rec_str(params, "name");
        size_t ti = TOOLS_N;
        if (name)
            for (size_t i = 0; i < TOOLS_N; i++)
                if (!strcmp(TOOLS[i].name, name)) ti = i;
        if (ti == TOOLS_N) {
            *errmsg_out = strdup(name ? "unknown tool" : "missing tool name");
            return 1;
        }
        const cJSON *args =
            cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (args && !cJSON_IsObject(args)) {
            *errmsg_out = strdup("arguments must be an object");
            return 1;
        }
        cJSON *empty = NULL;
        if (!args) {
            empty = cJSON_CreateObject();
            args = empty;
        }
        buf_t out;
        buf_init(&out);
        bool is_error = false;
        TOOL_FNS[ti](args, &out, &is_error);
        cJSON_Delete(empty);
        cJSON *res = cJSON_CreateObject();
        cJSON *content = cJSON_CreateArray();
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        cJSON_AddStringToObject(blk, "text", out.data ? out.data : "");
        cJSON_AddItemToArray(content, blk);
        cJSON_AddItemToObject(res, "content", content);
        if (is_error) cJSON_AddBoolToObject(res, "isError", true);
        buf_free(&out);
        *result_out = res;
        return 0;
    }
    *errmsg_out = strdup("method not found");
    return 1;
}

int cmd_builtin(void) {
    signals_init();
    rpc_handler_t h = { builtin_handle, NULL };
    rpc_serve_stdio(&h);
    return 0;
}