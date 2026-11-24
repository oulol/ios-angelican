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

#define MENU_ITEM(name) \
    con_str(selected == index ? selected_marker : "", name); \
    index++;

#define ANDROID "Android/Linux"
#define XNU "iOS (XNU)"
#define AUTOBOOT_TIMER 8

int menu_option_count(bool android, bool xnu)
{
    return (android ? 1 : 0) +
           (xnu ? 1 : 0) + 3;
}

static void panic(const char *reason) {
    con_skip_amt(7);
    con_str("BOOTMGR", reason);
    wait_loop();
}

void
drawmenu(int selected, bool android, bool xnu, int timer)
{
    int index = 0;
    char selected_marker[8] = "      > ";

    if (timer > 0) {
        int n = timer;
        int i = 5;

        while (n > 0 && i >= 0) {
            selected_marker[i--] = '0' + (n % 10);
            n /= 10;
        }

        int start = i + 1;
        int len = 6 - start;

        for (int j = 0; j < len; j++)
            selected_marker[j] = selected_marker[start + j];

        selected_marker[len] = 's';

        while (++len < 6)
            selected_marker[len] = ' ';
    }

    if (android) {
        MENU_ITEM(ANDROID)
    }
    if (xnu) {
        MENU_ITEM(XNU)
    }

    con_str("", "----------");
    MENU_ITEM("Restart");
    MENU_ITEM("Fastboot");
    MENU_ITEM("Power off");

    if (timer > 0) {
        con_skip();
        con_str("", "Selected option will boot automatically");
        con_skip_amt(-2);
    }
    con_skip_amt(-(index + 1));
}

static void select(bool android, bool xnu, int selected, struct payload *payload) {
    int index = 0;

    if (android) {
        if (selected == index) {
            loader_android_boot(payload);
            return;
        }
        index++;
    }

    if (xnu) {
        if (selected == index) {
            loader_main();
            return;
        }
        index++;
    }

    if (selected == index) {
        power_reset();
        return;
    }
    index++;

    if (selected == index) {
        power_reboot_fastboot();
        return;
    }
    index++;

    if (selected == index) {
        return;
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

    if (!android && !xnu) {
        con_str("", "No compatible payload found.");
        con_str("", "Use mkpayload.py to build one with XNU and optionally Android.");
    }

    int option_count = menu_option_count(android, xnu);
    int timer_left = AUTOBOOT_TIMER;
    int timer_cycle = 0;
    drawmenu(selected, android, xnu, timer_left);
    for (;;) {
        watchdog_service();

        uint64_t start = watchdog_read_counter();
        uint64_t delay = watchdog_read_frequency() / 10;
        while (watchdog_read_counter() - start < delay) {
            watchdog_service();
            __asm__ volatile("yield");
        }

        if (timer_left > 0) {
            timer_cycle++;
            if (timer_cycle == 10) {
                timer_left--;
                timer_cycle = 0;
                drawmenu(selected, android, xnu, timer_left);
            }
        } else if (timer_left == 0) {
            select(android, xnu, selected, payload);
            timer_left = -1;
        }

        int downC = volume_down_button_pressed();
        int upC = volume_up_button_pressed();

        if (upC != upP && upC == 1) {
            timer_left = -1;
            selected = selected - 1;
            if (selected < 0) {
                selected = option_count-1;
            }
            drawmenu(selected, android, xnu, timer_left);
        } else if (downC != downP && downC == 1) {
            timer_left = -1;
            selected = (selected + 1) % option_count;
            drawmenu(selected, android, xnu, timer_left);
        } else if (power_button_pressed()) {
            select(android, xnu, selected, payload);
        }

        downP = volume_down_button_pressed();
        upP = volume_up_button_pressed();
    }
}

#endif