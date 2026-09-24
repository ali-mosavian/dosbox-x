/* The macOS host functions menu_macos.mm provides, for a headless build: no
 * window, dock or touch bar, and nobody to answer a dialog, so every prompt
 * is declined. Links no framework. */

#include "config.h"
#include "sdlmain.h"

#include <string>

bool has_touch_bar_support = false;

bool macosx_detect_nstouchbar(void) { return has_touch_bar_support = false; }
void macosx_init_touchbar(void) {}
void macosx_reload_touchbar(void) {}
void macosx_init_dock_menu(void) {}
void qz_set_match_monitor_cb(void) {}
void sdl1_hax_set_topmost(unsigned char) {}
void MacOSEnableWindowCapture(unsigned int) {}

void macosx_GetWindowDPI(ScreenSizeInfo &info) { info.clear(); }

std::string macosx_prompt_folder(const char *) { return ""; }
int macosx_yesno(const char *, const char *) { return 0; }
int macosx_yesnocancel(const char *, const char *) { return -1; }

bool IME_GetEnable() { return false; }
void IME_SetEnable(int) {}
