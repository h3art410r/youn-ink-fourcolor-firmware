#include "memo_storage.h"

#include "settings.h"

#include <cJSON.h>

#include <cstring>
#include <string>

namespace {
constexpr char kNamespace[] = "memos";
constexpr char kItemsKey[] = "items";

bool CopyBounded(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) return false;
    const size_t len = strlen(src);
    if (len >= dst_size) return false;
    memcpy(dst, src, len + 1);
    return true;
}
}  // namespace

bool memo_storage_get_all(MemoItem* items, size_t capacity, size_t* count) {
    if (!count || (capacity > 0 && !items)) return false;
    *count = 0;

    Settings settings(kNamespace);
    const std::string json = settings.GetString(kItemsKey, "[]");
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    const int total = cJSON_GetArraySize(root);
    if (total < 0 || static_cast<size_t>(total) > capacity ||
        static_cast<size_t>(total) > MEMO_MAX_ITEMS) {
        cJSON_Delete(root);
        return false;
    }

    bool valid = true;
    for (int i = 0; i < total; ++i) {
        const cJSON* item = cJSON_GetArrayItem(root, i);
        const cJSON* title = cJSON_GetObjectItemCaseSensitive(item, "title");
        const cJSON* body = cJSON_GetObjectItemCaseSensitive(item, "body");
        if (!cJSON_IsString(title) || !cJSON_IsString(body) ||
            !CopyBounded(items[i].title, sizeof(items[i].title), title->valuestring) ||
            !CopyBounded(items[i].body, sizeof(items[i].body), body->valuestring)) {
            valid = false;
            break;
        }
    }
    if (valid) *count = static_cast<size_t>(total);
    cJSON_Delete(root);
    return valid;
}

bool memo_storage_replace(const MemoItem* items, size_t count) {
    if (count > MEMO_MAX_ITEMS || (count > 0 && !items)) return false;

    cJSON* root = cJSON_CreateArray();
    if (!root) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!items[i].title[0] || !items[i].body[0] ||
            strnlen(items[i].title, sizeof(items[i].title)) >= sizeof(items[i].title) ||
            strnlen(items[i].body, sizeof(items[i].body)) >= sizeof(items[i].body)) {
            cJSON_Delete(root);
            return false;
        }
        cJSON* item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "title", items[i].title) ||
            !cJSON_AddStringToObject(item, "body", items[i].body)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return false;
        }
        cJSON_AddItemToArray(root, item);
    }

    char* encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!encoded) return false;
    const size_t encoded_size = strlen(encoded);
    if (encoded_size >= 3800) {
        cJSON_free(encoded);
        return false;
    }

    Settings settings(kNamespace, true);
    settings.SetString(kItemsKey, encoded);
    cJSON_free(encoded);
    return true;
}
