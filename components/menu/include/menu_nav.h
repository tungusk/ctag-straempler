#pragma once
#include <stdbool.h>

typedef struct {
    const char **labels;
    const int   *sids;
    int          n;
    int          parent;
    void       (*change)(int sid, int vid, int delta, void *ctx);
    void        *ctx;
} menu_nav_t;

int menu_nav_step(menu_nav_t *nav, int *pos, int *sel, int vid, int event);
