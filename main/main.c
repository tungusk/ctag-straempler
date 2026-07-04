#include "ui.h"
#include "machine.h"
#include "menu.h"
#include "menu_config.h"

void app_main()
{
    initUI();
    // core is up and silent; boot into the persisted machine choice
    // ("machine" in CONFIG.JSN settings), falling back to the first registered
    const machine_t *m = NULL;
    char name[16];
    if (configGetStringSetting("machine", name, sizeof(name)))
        m = machine_by_name(name);
    if (!m) m = machine_registry[0];
    machine_activate(m);
    menuBindMachineUI();
}

