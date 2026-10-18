// ============================================
// File Pilot Clone - Core Memory & String
// ============================================
#include "core.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void* mem_alloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void* mem_realloc(void* ptr, size_t old_size, size_t new_size) {
    (void)old_size; // unused, just forward to realloc
    return realloc(ptr, new_size);
}

void mem_free(void* ptr) {
    free(ptr);
}

void* mem_zero(size_t size) {
    return calloc(1, size);
}

// ---- UTF-8 String ----
UTF8String* utf8_create(const char* str) {
    if (!str) return NULL;
    UTF8String* s = (UTF8String*)mem_zero(sizeof(UTF8String));
    size_t len = strlen(str);
    s->data = (char*)mem_alloc(len + 1);
    if (s->data) {
        memcpy(s->data, str, len);
        s->length = len;
        s->capacity = len + 1;
    }
    return s;
}

void utf8_destroy(UTF8String* str) {
    if (str) {
        if (str->data) mem_free(str->data);
        mem_free(str);
    }
}

bool utf8_append(UTF8String* str, const char* text) {
    if (!str || !text) return false;
    size_t text_len = strlen(text);
    size_t needed = str->length + text_len + 1;
    
    if (needed > str->capacity) {
        size_t new_cap = needed * 2;
        char* new_data = (char*)mem_realloc(str->data, str->capacity, new_cap);
        if (!new_data) return false;
        str->data = new_data;
        str->capacity = new_cap;
    }
    
    memcpy(str->data + str->length, text, text_len + 1);
    str->length += text_len;
    return true;
}

int utf8_char_count(const char* str) {
    int count = 0;
    while (*str) {
        unsigned char c = (unsigned char)*str;
        if (c < 0x80) str++;
        else if ((c & 0xE0) == 0xC0) str += 2;
        else if ((c & 0xF0) == 0xE0) str += 3;
        else str += 4;
        count++;
    }
    return count;
}

// ---- Dynamic Array ----
void* array_get(DynamicArray* arr, size_t index) {
    if (!arr || index >= arr->item_count) return NULL;
    return (char*)arr->items + (index * arr->item_size);
}

bool array_push(DynamicArray* arr, const void* item) {
    if (!arr || !item) return false;
    if (arr->item_count >= arr->capacity) {
        size_t nc = (arr->capacity == 0) ? 16 : arr->capacity * 2;
        array_resize(arr, nc);
        // A failed resize leaves capacity UNCHANGED - writing the item
        // anyway would store it straight past the end of the block, which
        // is exactly how a heap overflow starts. Refuse instead of lying.
        if (arr->capacity <= arr->item_count) return false;
    }
    char* dest = (char*)arr->items + (arr->item_count * arr->item_size);
    memcpy(dest, item, arr->item_size);
    arr->item_count++;
    return true;
}

void array_resize(DynamicArray* arr, size_t new_capacity) {
    if (new_capacity == 0) return;
    size_t new_size = new_capacity * arr->item_size;
    void* new_items = realloc(arr->items, new_size);
    if (new_items) {
        arr->items = new_items;
        arr->capacity = new_capacity;
    }
}

void array_free(DynamicArray* arr) {
    if (arr) {
        free(arr->items);
        arr->items = NULL;
        arr->item_count = 0;
        arr->capacity = 0;
    }
}

// ---- Hash Map ----
static unsigned long hash_str(const char* key) {
    unsigned long h = 5381;
    int c;
    while ((c = *key++))
        h = ((h << 5) + h) + c;
    return h;
}

void* hm_get(HashMap* map, const char* key) {
    if (!map || !key || !map->capacity || !map->keys) return NULL;
    unsigned long idx = hash_str(key) % map->capacity;
    for (size_t i = 0; i < map->capacity; i++) {
        size_t ci = (idx + i) % map->capacity;
        if (map->keys[ci] && strcmp(map->keys[ci], key) == 0)
            return map->values[ci];
        if (!map->keys[ci]) break;
    }
    return NULL;
}

void hm_insert(HashMap* map, const char* key, void* value) {
    if (!map || !key) return;
    // % 0 is a hard crash (STATUS_INTEGER_DIVIDE_BY_ZERO), and an
    // allocation failure in the caller really does leave capacity at 0.
    if (!map->capacity || !map->keys || !map->values) return;
    if (map->count >= map->capacity * 0.7) {
        // Resize
        size_t new_cap = map->capacity * 2;
        char** nk = (char**)mem_zero(new_cap * sizeof(char*));
        void** nv = (void**)mem_zero(new_cap * sizeof(void*));
        if (!nk || !nv) {
            // Out of memory: keep the OLD table intact. The original code
            // dereferenced nk unconditionally here - a NULL that turned a
            // low-memory condition straight into a crash.
            mem_free(nk);
            mem_free(nv);
        } else {
            for (size_t i = 0; i < map->capacity; i++) {
                if (map->keys[i]) {
                    unsigned long ni = hash_str(map->keys[i]) % new_cap;
                    size_t j = 0;
                    while (nk[(ni + j) % new_cap]) j++;
                    nk[(ni + j) % new_cap] = map->keys[i];
                    nv[(ni + j) % new_cap] = map->values[i];
                }
            }
            // The key strings were MOVED into nk, not copied: nk[i] IS
            // map->keys[i]. The old loop freed every one of them and then
            // installed nk anyway, so after the first resize the map held
            // freed pointers - hm_get compared against freed memory, and
            // hm_free freed them AGAIN. That second free is the heap
            // corruption (STATUS_HEAP_CORRUPTION_DETECTED, 0xc0000374) the
            // app was dying with. Only the two ARRAYS are released; they
            // own no strings.
            mem_free(map->keys);
            mem_free(map->values);
            map->keys = nk;
            map->values = nv;
            map->capacity = new_cap;
        }
    }
    unsigned long idx = hash_str(key) % map->capacity;
    size_t i = 0;
    while (map->keys[(idx + i) % map->capacity] && strcmp(map->keys[(idx + i) % map->capacity], key) != 0)
        i++;
    if (!map->keys[(idx + i) % map->capacity]) {
        char* dup = strdup(key);
        if (!dup) return;        // never count a key we failed to store
        map->keys[(idx + i) % map->capacity] = dup;
        map->count++;
    }
    map->values[(idx + i) % map->capacity] = value;
}

void hm_free(HashMap* map) {
    if (!map || !map->keys) return;
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->keys[i]) free(map->keys[i]);
    }
    free(map->keys);
    free(map->values);
    map->keys = NULL;
    map->values = NULL;
    map->count = 0;
    map->capacity = 0;
}
