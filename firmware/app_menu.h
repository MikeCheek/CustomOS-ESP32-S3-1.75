#pragma once
#include "ui.h"

extern Screen menu_screen;

// An app's icon as the menu draws it (the same disc), centred on (cx, cy)
// with radius r - for other screens that show apps (the Smart Stack).
void menu_draw_app_icon(const Screen *screen, int cx, int cy, int r);

// Opens a folder of the app menu (0 = Games), as tapping it would.
void menu_open_folder(int category);
