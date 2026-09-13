#ifndef MEMO_STORAGE_H
#define MEMO_STORAGE_H

#include <stddef.h>

constexpr size_t MEMO_MAX_ITEMS = 8;

struct MemoItem {
    char title[64];
    char body[160];
};

bool memo_storage_get_all(MemoItem* items, size_t capacity, size_t* count);
bool memo_storage_replace(const MemoItem* items, size_t count);

#endif  // MEMO_STORAGE_H
