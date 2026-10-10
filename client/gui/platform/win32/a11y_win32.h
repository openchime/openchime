/* The UIA provider's Win32 face, for the platform's window procedure only. */
#ifndef OC_A11Y_WIN32_H
#define OC_A11Y_WIN32_H
#include <windows.h>
LRESULT oc_a11y_get_object(HWND hwnd, WPARAM wp, LPARAM lp, int *handled);
#endif
