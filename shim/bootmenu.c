#include <stdint.h>

#if defined(BOOTMENU)

#include "power.h"
#include "watchdog.h"
#include "loader.h"
#include "console.h"
#include "exception.h"
#include "payload.h"
#include "boot_state.h"
#include "fdt.h"
#include "loader_android.h"


#define ANDROID "Android"
#define XNU "iOS (XNU)"

int menu_option_count(bool android, bool xnu)
{
    return (android ? 1 : 0) +
           (xnu ? 1 : 0) + 2;
}

static void panic(const char *reason) {
    con_skip_amt(7);
    con_str("BOOTMGR", reason);
    wait_loop();
}

void
drawmenu(int selected, bool android, bool xnu)
{
    int index = 0;

    if (android) {
        con_str(selected == index ? "      >" : "", ANDROID);
        index++;
    }
    if (xnu) {
        con_str(selected == index ? "      >" : "", XNU);
        index++;
    }

    if (!android && !xnu) {
        con_str("", "No compatible payload found.");
        con_str("", "Use mkpayload.py to build one with XNU and optionally Android.");
        con_skip();
    }

    con_str(selected == index ? "      >" : "", "Restart");
    index++;
    con_str(selected == index ? "      >" : "", "Power Off");
    con_skip_amt(-(index + 1));
    
    if (!android && !xnu) {
        con_skip_amt(-3);
    }
}

void
bootmenu(void) {
    
    watchdog_init();
    con_skip_amt(19);
    con_str("Welcome", "to ios-angelican boot menu!");
    con_str("", "Select an option:");
    con_skip();
    
    static int selected = 0;

    static int downP = 0;
    static int upP = 0;

    bool android = true;
    bool xnu = true;

    uintptr_t fdt = (uintptr_t)boot_state.x0;
    uintptr_t initrd_start;
    uint64_t initrd_size;

    if (!fdt_get_initrd(fdt, &initrd_start, &initrd_size)) {
        panic("Failed to locate initrd");
        wait_loop();
    }

    struct payload *payload =
        payload_find(initrd_start, initrd_size);

    if (!payload) {
        xnu = false;
        android = false;
    } else {
        android = payload->has_android == 1;
    }

    int option_count = menu_option_count(android, xnu);
    drawmenu(selected, android, xnu);
    for (;;) {
        watchdog_service();

        uint64_t start = watchdog_read_counter();
        uint64_t delay = watchdog_read_frequency() / 10;

        while (watchdog_read_counter() - start < delay) {
            watchdog_service();
            __asm__ volatile("yield");
        }

        int downC = volume_down_button_pressed();
        int upC = volume_up_button_pressed();

        if (upC != upP && upC == 1) {
            selected = selected - 1;
            if (selected < 0) {
                selected = option_count-1;
            }
            drawmenu(selected, android, xnu);
        } else if (downC != downP && downC == 1) {
            selected = (selected + 1) % option_count;
            drawmenu(selected, android, xnu);
        } else if (power_button_pressed()) {
            int index = 0;

            if (android) {
                if (selected == index) {
                    con_str("BOOTMGR", "Android selected");
                    loader_android_boot(payload);
                    continue;
                }
                index++;
            }

            if (xnu) {
                if (selected == index) {
                    loader_main();
                    continue;
                }
                index++;
            }

            if (selected == index) {
                power_reset();
                continue;
            }

            index++;

            if (selected == index) {
                continue;
            }
        }

        downP = volume_down_button_pressed();
        upP = volume_up_button_pressed();
    }
}

#endif