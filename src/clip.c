#include "ktuyome.h"
/* System clipboard: one shared text buffer that every app copies to and pastes from.
 * Text is normalised on the way in (CR/CRLF -> LF, tabs -> 4 spaces, anything that is not
 * printable ASCII is dropped) so pasted text is always something the fonts and the
 * input widgets can show. */

#define CLIP_MAX 32768
static char clip_buf[CLIP_MAX + 1];
static int clip_n, clip_cut;

u32 clip_static_bytes(void) { return (u32)sizeof clip_buf; }

static void put(char c) { if (clip_n >= CLIP_MAX) { clip_cut = 1; return; } clip_buf[clip_n++] = c; clip_buf[clip_n] = 0; }

void clip_begin(void) { clip_n = 0; clip_cut = 0; clip_buf[0] = 0; }
void clip_add(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        if (c == '\r') { if (i + 1 < n && s[i + 1] == '\n') continue; put('\n'); }
        else if (c == '\n') put('\n');
        else if (c == '\t') { for (int k = 0; k < 4; k++) put(' '); }
        else if (c >= 32 && c < 127) put((char)c);
    }
}
int clip_set(const char *s, int n) { clip_begin(); clip_add(s, n); return clip_n; }
int clip_len(void) { return clip_n; }
const char *clip_text(void) { return clip_buf; }
int clip_truncated(void) { return clip_cut; }
void clip_clear(void) { clip_begin(); }

/* First non-blank line of the clipboard, trimmed, for single-line fields. *more is set when more text was left behind. */
int clip_line(char *out, int cap, int *more) {
    int i = 0, n = 0;
    while (i < clip_n && (clip_buf[i] == '\n' || clip_buf[i] == ' ')) i++;
    while (i < clip_n && clip_buf[i] != '\n') { if (n < cap - 1) out[n++] = clip_buf[i]; i++; }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (more) { *more = 0; for (; i < clip_n; i++) if (clip_buf[i] != '\n' && clip_buf[i] != ' ') { *more = 1; break; } }
    return n;
}

/* Maps a key event to a clipboard action: Ctrl+C/X/V/A plus the classic Ctrl+Insert, Shift+Insert and Shift+Delete. */
int clip_key(const Event *e) {
    if (e->type != EV_KEY || (e->mods & MOD_ALT)) return CK_NONE;
    if (e->mods & MOD_CTRL) {
        switch (e->key) {
            case 'c': case K_INS: return CK_COPY;
            case 'x': return CK_CUT;
            case 'v': return CK_PASTE;
            case 'a': return CK_ALL;
        }
        return CK_NONE;
    }
    if (e->mods & MOD_SHIFT) {
        if (e->key == K_INS) return CK_PASTE;
        if (e->key == K_DEL) return CK_CUT;
    }
    return CK_NONE;
}

/* Toast such as "Copied 12 characters". */
void clip_notify(const char *verb, int n) {
    char t[64], num[16];
    kstrcpy(t, verb, sizeof t); kstrcat(t, " ", sizeof t);
    kitoa(num, n); kstrcat(t, num, sizeof t); kstrcat(t, n == 1 ? " character" : " characters", sizeof t);
    if (clip_cut) kstrcat(t, " (clipboard full)", sizeof t);
    ui_flash(t);
}
