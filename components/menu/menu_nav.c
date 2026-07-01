#include "menu_nav.h"
#include "menutft.h"
#include "ui_events.h"

int menu_nav_step(menu_nav_t *nav, int *pos, int *sel, int vid, int event)
{
    switch (event) {
        case EV_FWD:
            if (!*sel) {
                if (++*pos >= nav->n) *pos = 0;
                menuTFTSelectMenuItem(pos, *sel, nav->labels, &nav->n);
            } else {
                nav->change(nav->sids[*pos], vid, +1, nav->ctx);
            }
            break;
        case EV_BWD:
            if (!*sel) {
                if (--*pos < 0) *pos = nav->n - 1;
                menuTFTSelectMenuItem(pos, *sel, nav->labels, &nav->n);
            } else {
                nav->change(nav->sids[*pos], vid, -1, nav->ctx);
            }
            break;
        case EV_SHORT_PRESS:
            *sel = !*sel;
            menuTFTSelectMenuItem(pos, *sel, nav->labels, &nav->n);
            break;
        case EV_LONG_PRESS:
            *sel = 0;
            *pos = 0;
            return nav->parent;
        default:
            break;
    }
    return 0;
}
