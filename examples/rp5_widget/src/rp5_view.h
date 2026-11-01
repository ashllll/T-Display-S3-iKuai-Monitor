#pragma once
#include <stdbool.h>
#include <stdint.h>

void rp5_view_create(void);
void rp5_view_refresh(uint64_t now_ms, bool wifi_online, const char *local_ip);
void rp5_view_step(int direction);
void rp5_view_home(void);
int rp5_view_page(void);

void rp5_view_animate(uint64_t now_ms);
