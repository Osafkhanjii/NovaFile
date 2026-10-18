// ============================================
// File Pilot Clone - Search helpers
// ============================================
// Only the three helpers the app actually calls live here: fuzzy_match
// (the in-memory name filter behind the search box), format_size and
// get_file_extension. The old indexer (index_directory / search_files /
// fuzzy_score / is_image_file) was never called from anywhere and has
// been removed with its File-Info types.
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Fuzzy string matching: every character of `pattern` must appear in
// `text` in order; comparison is case-insensitive. An empty pattern
// matches everything (the search box starts unfiltered).
bool fuzzy_match(const char* text, const char* pattern) {
    if (!pattern || !*pattern) return true;
    if (!text) return false;

    while (*text && *pattern) {
        if (tolower((unsigned char)*text) == tolower((unsigned char)*pattern)) {
            text++;
            pattern++;
        } else {
            text++;
        }
    }
    return *pattern == '\0';
}

// ---- Ranked search matching (the search box) ----------------------------
// The old `fuzzy_match` above is a SUBSEQUENCE test: "pho" matched any name
// containing p, then h, then o anywhere in order, so unrelated names slipped
// in. The search box instead needs a strict, predictable, ranked match:
// exact > prefix > contiguous substring > (opt-in) word-start. These
// helpers implement that. Case-insensitive by default; non-ASCII bytes are
// compared exactly (never passed through a locale tolower, so UTF-8 names
// are never corrupted). An empty query matches nothing here - callers treat
// an empty search box as "no filter" before ever calling.

static char ci_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
}

// Compare the first n bytes of a and b. Case-insensitive unless
// case_sensitive. Both buffers must hold at least n bytes.
bool str_eq_n(const char* a, const char* b, size_t n, bool case_sensitive) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (case_sensitive) { if (x != y) return false; }
        else if (ci_lower(x) != ci_lower(y)) return false;
    }
    return true;
}

// Word boundary: the character before a position that makes the text at
// that position start a new "word" (space, dash, dot, underscore, bracket).
static bool is_word_start(const char* name, size_t i) {
    if (i == 0) return true;
    unsigned char p = (unsigned char)name[i - 1];
    return p == ' ' || p == '-' || p == '_' || p == '.' || p == '(' ||
           p == '[' || p == '+' || p == ',';
}

// Rank `name` against `query`. Returns one of the SRCH_* levels, or
// SRCH_NONE when the name should not be shown for this query. The two
// flags gate the weaker tiers: want_substr admits a contiguous substring
// anywhere; want_word (only consulted when want_substr is false) admits a
// match that starts on a word boundary without allowing mid-word substrings.
int search_rank(const char* name, const char* query, bool case_sensitive,
                bool want_substr, bool want_word) {
    if (!query || !*query || !name) return SRCH_NONE;
    size_t nl = strlen(name), ql = strlen(query);
    if (ql > nl) return SRCH_NONE;

    if (nl == ql) return str_eq_n(name, query, ql, case_sensitive)
                          ? SRCH_EXACT : SRCH_NONE;
    if (str_eq_n(name, query, ql, case_sensitive)) return SRCH_PREF;

    if (want_substr) {
        for (size_t i = 1; i + ql <= nl; i++)
            if (str_eq_n(name + i, query, ql, case_sensitive)) return SRCH_SUB;
        return SRCH_NONE;
    }
    if (want_word) {
        for (size_t i = 1; i + ql <= nl; i++)
            if (is_word_start(name, i) &&
                str_eq_n(name + i, query, ql, case_sensitive))
                return SRCH_WORD;
    }
    return SRCH_NONE;
}

// Format file size
void format_size(uint64_t bytes, char* buffer, size_t buf_size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(buffer, buf_size, "%llu %s", (unsigned long long)bytes, units[unit]);
    else
        snprintf(buffer, buf_size, "%.1f %s", size, units[unit]);
}

// Get file extension
const char* get_file_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    return dot ? dot + 1 : "";
}
