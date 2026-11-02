#include <string.h>

#include "mcp.h"
#include "miniapp.h"
#include "miniapp_actions.h"
#include "mcp_miniapp_result.h"

static cJSON *miniapp_control_list(const char *name, const char *description)
{
    cJSON *tool = mcp_tool_list_info_create_default(name, description);
    if (!tool) {
        return NULL;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(tool, "name")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(tool, "description")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(schema, "type")) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(schema, "properties")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(schema, "required"))) {
        cJSON_Delete(tool);
        return NULL;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(schema, "additionalProperties");
    if (!cJSON_AddBoolToObject(schema, "additionalProperties", false)) {
        cJSON_Delete(tool);
        return NULL;
    }
    return tool;
}

static cJSON *miniapp_describe_list(const char *name)
{
    return miniapp_control_list(name,
                                "查询当前前台小应用的身份、最新状态和支持的操作。无参数。"
                                "没有前台小应用或闹钟抢占时返回 running=false；操作前先查询，不用于安装或修改脚本。");
}

static cJSON *miniapp_describe_call(const char *id, const char *name, cJSON *args)
{
    (void)id;
    if (args && (!cJSON_IsObject(args) || args->child)) {
        return miniapp_mcp_result(name, "ls.built_in.miniapp_describe takes no arguments", true);
    }
    char error[128];
    cJSON *description = miniapp_describe(error, sizeof(error));
    if (!description) {
        return miniapp_mcp_result(name, error[0] ? error : "not enough memory for description", true);
    }
    char *text = cJSON_PrintUnformatted(description);
    cJSON_Delete(description);
    cJSON *result = miniapp_mcp_result(name, text ? text : "not enough memory for description", !text);
    cJSON_free(text);
    return result;
}

static cJSON *miniapp_action_list(const char *name)
{
    cJSON *tool = miniapp_control_list(
        name, "执行前台小应用已声明的操作。先调用 ls.built_in.miniapp_describe 获取 actions，按声明填写 action 和 arguments。"
              "仅在小应用前台运行时有效；闹钟抢占、未知操作及非法参数会被拒绝，不用于修改、安装或退出脚本。");
    if (!tool) {
        return NULL;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    cJSON *action = properties ? cJSON_AddObjectToObject(properties, "action") : NULL;
    cJSON *arguments = properties ? cJSON_AddObjectToObject(properties, "arguments") : NULL;
    if (!schema || !required || !action || !arguments || !cJSON_AddStringToObject(action, "type", "string") ||
        !cJSON_AddNumberToObject(action, "minLength", 1) ||
        !cJSON_AddNumberToObject(action, "maxLength", MINIAPP_ACTION_NAME_MAX) ||
        !cJSON_AddStringToObject(arguments, "type", "object")) {
        goto failed;
    }
    cJSON *entry = cJSON_CreateString("action");
    if (!entry || !cJSON_AddItemToArray(required, entry)) {
        cJSON_Delete(entry);
        goto failed;
    }
    entry = cJSON_CreateString("arguments");
    if (!entry || !cJSON_AddItemToArray(required, entry)) {
        cJSON_Delete(entry);
        goto failed;
    }
    return tool;
failed:
    cJSON_Delete(tool);
    return NULL;
}

static cJSON *miniapp_action_call(const char *id, const char *name, cJSON *args)
{
    (void)id;
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(args, "action");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(args, "arguments");
    if (!cJSON_IsObject(args) || cJSON_GetArraySize(args) != 2 || !cJSON_IsString(action) || !action->valuestring[0] ||
        strlen(action->valuestring) > MINIAPP_ACTION_NAME_MAX || !cJSON_IsObject(arguments)) {
        return miniapp_mcp_result(name, "expected action string and arguments object", true);
    }
    /* Bound transport data before handing it to the Lua task. Parameters are
     * scalar; reject nested values before invoking recursive JSON functions. */
    size_t bytes = 0;
    for (const cJSON *item = arguments->child; item; item = item->next) {
        if (!item->string || strlen(item->string) > MINIAPP_ACTION_NAME_MAX ||
            !(cJSON_IsString(item) || cJSON_IsBool(item) || cJSON_IsNumber(item))) {
            return miniapp_mcp_result(name, "action parameters must be strings, booleans or numbers", true);
        }
        bytes += strlen(item->string) + 8 + (cJSON_IsString(item) ? strlen(item->valuestring) : 0);
        if (bytes > MINIAPP_ACTION_ARGS_BYTES_MAX) {
            return miniapp_mcp_result(name, "action arguments too large", true);
        }
    }
    char error[128];
    cJSON *response = miniapp_action(action->valuestring, arguments, error, sizeof(error));
    if (!response) {
        return miniapp_mcp_result(name, error[0] ? error : "not enough memory for action result", true);
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "message");
    cJSON *result = miniapp_mcp_result(name, message->valuestring, !cJSON_IsTrue(ok));
    cJSON_Delete(response);
    return result;
}

MCP_TOOL_DEFINE(ls.built_in.miniapp_describe, miniapp_describe_list, miniapp_describe_call);
MCP_TOOL_DEFINE(ls.built_in.miniapp_action, miniapp_action_list, miniapp_action_call);
