#ifndef SG_MODE_MEMORY_H
#define SG_MODE_MEMORY_H

/* sg-compositor: each monitor's mode as the person last set it, kept for the
 * session and put back the moment that monitor returns -- as Windows keeps a
 * monitor's mode and applies it on reconnect. A monitor that powers down
 * (DPMS, its own power button) comes back as a new output; it used to start
 * at its fastest mode and only later went back to the kept one, so a monitor
 * whose fastest mode does not hold on its link (David's 165 Hz Optix AG32C
 * on NVIDIA, 2026-11-18) flickered every morning. */

#include <stdbool.h>

struct sg_mode {
	int width, height;
	int refresh; /* mHz; 0 = any */
};

/* the monitor, not its port: make/model/serial, else the output's name */
void sg_mode_key(char *key, int size, const char *make, const char *model, const char *serial, const char *name);

void sg_mode_remember(const char *key, struct sg_mode mode);
bool sg_mode_recall(const char *key, struct sg_mode *mode);
void sg_mode_forget_all(void);

/* index in modes[] of the one that matches want (same size, refresh within
 * 0.5 Hz, or any refresh when want.refresh is 0), the closest refresh
 * winning; -1 when none does */
int sg_mode_pick(const struct sg_mode *modes, int count, struct sg_mode want);

#endif
