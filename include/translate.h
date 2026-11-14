// translate.h - display-only translation of file and folder names.
//
// NovaFile never renames anything here: a name in another language is
// translated for DISPLAY only (default: English), the file on disk keeps
// its real name, and every file operation still uses the real name.
//
// Translations come from an online service on a background thread, are
// cached in memory and in %LOCALAPPDATA%\NovaFile\translations.txt, so a
// name is fetched once and then shows instantly, also offline.  Until a
// translation arrives (or when the network is unavailable) the original
// name is shown.
#ifndef NOVA_TRANSLATE_H
#define NOVA_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>

// Languages offered in the bottom bar's language menu.  Index 0 is
// "Original" (no translation).  code = the service's language code.
typedef struct {
    const char* code;    // "" for Original
    const char* tag;     // short chip text: "EN", "UR", ...
    const char* name;    // menu text: "English", "Urdu", ...
} TrLang;

#define TR_LANG_ORIGINAL 0
#define TR_LANG_ENGLISH  1
extern const TrLang TR_LANGS[];
extern const int TR_LANG_COUNT;

// Start the worker and load the on-disk cache.  cache_dir = the folder that
// holds translations.txt (created if missing); wake = called (from the
// worker thread) when new translations are ready, to repaint.
void tr_init(const char* cache_dir, void (*wake)(void));

// Does this text need translating into lang?  For English only names with
// non-ASCII letters do; for any other language every name with a letter.
bool tr_needed(const char* text, int lang);

// The translation of text into TR_LANGS[lang], or NULL when it is not known
// yet (the request is queued and the UI is woken when it lands) or the
// service failed.  The returned pointer stays valid for the whole run.
const char* tr_get(const char* text, int lang);

// Bumped every time a translation lands (lets callers drop derived state).
unsigned tr_generation(void);

#endif
