#include "jdprofile.h"

DEVICE_CLASS(0x3a3314ae, "JDF030 OLED v0");

void app_init_services() {
    oled_init();
}
