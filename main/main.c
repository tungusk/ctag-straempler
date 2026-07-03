#include "ui.h"
#include "machine.h"
#include "menu.h"

void app_main()
{
    initUI();
    // core is up and silent; hand the audio path to the first machine
    machine_activate(machine_registry[0]);
    menuBindMachineUI();
}

