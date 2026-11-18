/* sg-compositor: each monitor's mode as the person last set it (mode_memory.h) */
#include "mode_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_MODE_MEMORY_MAX 16

static struct {
	char key[256];
	struct sg_mode mode;
} memory[SG_MODE_MEMORY_MAX];
static int used;

void
sg_mode_key(char *key, int size, const char *make, const char *model, const char *serial, const char *name)
{
	make = make ? make : "";
	model = model ? model : "";
	serial = serial ? serial : "";
	if (*serial) {
		snprintf(key, size, "%s/%s/%s", make, model, serial);
	} else if (*make || *model) {
		/* the port too: two monitors of one model with no serial are two */
		snprintf(key, size, "%s/%s/@%s", make, model, name ? name : "");
	} else {
		snprintf(key, size, "output:%s", name ? name : "");
	}
}

void
sg_mode_remember(const char *key, struct sg_mode mode)
{
	int i;
	for (i = 0; i < used; i++) {
		if (!strcmp(memory[i].key, key)) {
			memory[i].mode = mode;
			return;
		}
	}
	if (used == SG_MODE_MEMORY_MAX) {
		/* the oldest goes */
		memmove(&memory[0], &memory[1], sizeof(memory[0]) * (SG_MODE_MEMORY_MAX - 1));
		used--;
	}
	snprintf(memory[used].key, sizeof(memory[used].key), "%s", key);
	memory[used].mode = mode;
	used++;
}

bool
sg_mode_recall(const char *key, struct sg_mode *mode)
{
	if (getenv("SG_MUTANT_MODE_NO_RECALL")) {
		return false;
	}
	for (int i = 0; i < used; i++) {
		if (!strcmp(memory[i].key, key)) {
			*mode = memory[i].mode;
			return true;
		}
	}
	return false;
}

void
sg_mode_forget_all(void)
{
	used = 0;
}

int
sg_mode_pick(const struct sg_mode *modes, int count, struct sg_mode want)
{
	int best = -1, best_diff = 0;
	for (int i = 0; i < count; i++) {
		if (modes[i].width != want.width || modes[i].height != want.height) {
			continue;
		}
		int diff = want.refresh ? abs(modes[i].refresh - want.refresh) : 0;
		if (want.refresh && diff > 500) {
			continue;
		}
		if (best < 0 || diff < best_diff) {
			best = i;
			best_diff = diff;
		}
	}
	return best;
}
