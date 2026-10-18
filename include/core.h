// ============================================
// File Pilot Clone - Core Header
// ============================================

#ifndef CORE_H
#define CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ---- UTF-8 String ----
typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} UTF8String;

UTF8String* utf8_create(const char* str);
void utf8_destroy(UTF8String* str);
bool utf8_append(UTF8String* str, const char* text);
int utf8_char_count(const char* str);

// ---- Dynamic Array ----
typedef struct {
    void* items;
    size_t item_count;
    size_t item_size;
    size_t capacity;
} DynamicArray;

void* array_get(DynamicArray* arr, size_t index);
bool array_push(DynamicArray* arr, const void* item);
void array_resize(DynamicArray* arr, size_t new_capacity);
void array_free(DynamicArray* arr);

// ---- Hash Map ----
typedef struct {
    char** keys;
    void** values;
    size_t count;
    size_t capacity;
} HashMap;

void* hm_get(HashMap* map, const char* key);
void hm_insert(HashMap* map, const char* key, void* value);
void hm_free(HashMap* map);

// ---- Memory ----
void* mem_alloc(size_t size);
void* mem_realloc(void* ptr, size_t old_size, size_t new_size);
void mem_free(void* ptr);
void* mem_zero(size_t size);

// ---- Indexer / search helpers (src/core/indexer.c) ----
bool fuzzy_match(const char* text, const char* pattern);
// Ranked name match for the search box (higher = stronger). See indexer.c.
#define SRCH_NONE  0
#define SRCH_WORD  1   // query starts a word inside the name (opt-in)
#define SRCH_SUB   2   // contiguous substring (broad match)
#define SRCH_PREF  3   // name starts with the query
#define SRCH_EXACT 4   // name equals the query
int  search_rank(const char* name, const char* query, bool case_sensitive,
                 bool want_substr, bool want_word);
bool str_eq_n(const char* a, const char* b, size_t n, bool case_sensitive);
void format_size(uint64_t bytes, char* buffer, size_t buf_size);
const char* get_file_extension(const char* filename);

#endif
