#pragma once


#if defined(BOOTMENU)
struct payload;
void loader_android_boot(const struct payload *payload);
#endif