/* OpenChime GUI -- the keys the application handles, named once.
 *
 * Letters and digits are their ASCII (upper-case) code, as the key is
 * labelled; every other key the application handles has an OCK_ name. SDL's
 * keycodes are mapped here and nowhere else, so a platform whose keyboard
 * differs (the Mac's Command key) changes one function. */
#ifndef OC_KEYS_H
#define OC_KEYS_H

#include <SDL3/SDL.h>

enum {
    OCK_NONE = 0,
    OCK_SPACE = ' ',
    OCK_BACK = 0x100, OCK_TAB, OCK_RETURN, OCK_ESCAPE,
    OCK_SHIFT, OCK_CONTROL, OCK_ALT,
    OCK_PRIOR, OCK_NEXT, OCK_END, OCK_HOME,
    OCK_LEFT, OCK_UP, OCK_RIGHT, OCK_DOWN,
    OCK_DELETE, OCK_APPS,
    OCK_F1, OCK_F2, OCK_F3, OCK_F4, OCK_F5, OCK_F6, OCK_F7, OCK_F8, OCK_F9, OCK_F10, OCK_F11, OCK_F12,
    OCK_SLASH, OCK_COMMA, OCK_MINUS, OCK_PLUS
};

static inline int oc_key_from_sdl(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= '0' && k <= '9') return (int)k;
    switch (k) {
    case SDLK_SPACE:     return OCK_SPACE;
    case SDLK_BACKSPACE: return OCK_BACK;
    case SDLK_TAB:       return OCK_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return OCK_RETURN;
    case SDLK_ESCAPE:    return OCK_ESCAPE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return OCK_SHIFT;
    case SDLK_LCTRL:  case SDLK_RCTRL:  return OCK_CONTROL;
    case SDLK_LALT:   case SDLK_RALT:   return OCK_ALT;
    case SDLK_PAGEUP:    return OCK_PRIOR;
    case SDLK_PAGEDOWN:  return OCK_NEXT;
    case SDLK_END:       return OCK_END;
    case SDLK_HOME:      return OCK_HOME;
    case SDLK_LEFT:      return OCK_LEFT;
    case SDLK_UP:        return OCK_UP;
    case SDLK_RIGHT:     return OCK_RIGHT;
    case SDLK_DOWN:      return OCK_DOWN;
    case SDLK_DELETE:    return OCK_DELETE;
    case SDLK_APPLICATION: case SDLK_MENU: return OCK_APPS;
    case SDLK_F1: return OCK_F1;   case SDLK_F2: return OCK_F2;   case SDLK_F3: return OCK_F3;
    case SDLK_F4: return OCK_F4;   case SDLK_F5: return OCK_F5;   case SDLK_F6: return OCK_F6;
    case SDLK_F7: return OCK_F7;   case SDLK_F8: return OCK_F8;   case SDLK_F9: return OCK_F9;
    case SDLK_F10: return OCK_F10; case SDLK_F11: return OCK_F11; case SDLK_F12: return OCK_F12;
    case SDLK_SLASH: case SDLK_QUESTION: return OCK_SLASH;
    case SDLK_COMMA:     return OCK_COMMA;
    case SDLK_MINUS: case SDLK_KP_MINUS: return OCK_MINUS;
    case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: return OCK_PLUS;
    default:             return OCK_NONE;
    }
}

/* The harness names keys; the same names here and in scripts/gui_drive.sh. */
static inline int oc_key_from_name(const char *k) {
    if (!k || !k[0]) return OCK_NONE;
    if (!k[1]) {
        if (k[0] >= '0' && k[0] <= '9') return k[0];
        if (k[0] >= 'a' && k[0] <= 'z') return k[0] - 32;
        if (k[0] >= 'A' && k[0] <= 'Z') return k[0];
        if (k[0] == '=') return OCK_PLUS;
        if (k[0] == '-') return OCK_MINUS;
        if (k[0] == ',') return OCK_COMMA;
    }
    static const struct { const char *n; int key; } T[] = {
        { "enter", OCK_RETURN }, { "return", OCK_RETURN }, { "esc", OCK_ESCAPE }, { "escape", OCK_ESCAPE },
        { "tab", OCK_TAB }, { "up", OCK_UP }, { "down", OCK_DOWN }, { "left", OCK_LEFT }, { "right", OCK_RIGHT },
        { "home", OCK_HOME }, { "end", OCK_END }, { "pgup", OCK_PRIOR }, { "pageup", OCK_PRIOR },
        { "pgdn", OCK_NEXT }, { "pagedown", OCK_NEXT }, { "space", OCK_SPACE }, { "slash", OCK_SLASH },
        { "back", OCK_BACK }, { "backspace", OCK_BACK }, { "del", OCK_DELETE }, { "delete", OCK_DELETE },
        { "menu", OCK_APPS }, { "apps", OCK_APPS }, { "plus", OCK_PLUS }, { "equals", OCK_PLUS },
        { "minus", OCK_MINUS }, { "comma", OCK_COMMA },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!SDL_strcmp(T[i].n, k)) return T[i].key;
    if (k[0] == 'f' && k[1] >= '1' && k[1] <= '9') {
        int n = SDL_atoi(k + 1);
        if (n >= 1 && n <= 12) return OCK_F1 + n - 1;
    }
    return OCK_NONE;
}

#endif /* OC_KEYS_H */
