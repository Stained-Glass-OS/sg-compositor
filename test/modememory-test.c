/* make test-modememory: a monitor that comes back gets the mode the person
 * set for it, not its fastest (mode_memory.c). Mutant SG_MUTANT_MODE_NO_RECALL
 * must fail it. */
#include <stdio.h>
#include <string.h>

#include "../mode_memory.h"

static int fails;
#define CHECK(c, what)                                                                                                 \
	do {                                                                                                           \
		printf("%s  %s\n", (c) ? "PASS" : "FAIL", what);                                                       \
		if (!(c))                                                                                              \
			fails++;                                                                                       \
	} while (0)

int
main(void)
{
	/* David's Optix AG32C: 165 Hz is its fastest, 144 is what he keeps */
	struct sg_mode modes[] = {
		{1920, 1080, 164970}, {1920, 1080, 143990}, {1920, 1080, 119880}, {1920, 1080, 60000}, {1280, 720, 60000},
	};
	int n = sizeof(modes) / sizeof(modes[0]);
	char k1[256], k2[256], k3[256], k4[256];
	struct sg_mode m;

	sg_mode_key(k1, sizeof(k1), "Microstep", "Optix AG32C", "0x00000001", "DP-1");
	sg_mode_key(k2, sizeof(k2), "Microstep", "Optix AG32C", "0x00000001", "DP-2");
	CHECK(!strcmp(k1, k2), "a monitor with a serial is the same on another port");
	sg_mode_key(k3, sizeof(k3), "Acme", "Panel", "", "DP-1");
	sg_mode_key(k4, sizeof(k4), "Acme", "Panel", "", "DP-2");
	CHECK(strcmp(k3, k4) != 0, "two alike monitors with no serial are told apart by port");

	CHECK(!sg_mode_recall(k1, &m), "nothing is kept before the person sets a mode");

	sg_mode_remember(k1, (struct sg_mode){1920, 1080, 143990});
	CHECK(sg_mode_recall(k1, &m) && m.refresh == 143990, "the mode set is kept for that monitor");
	int i = m.refresh ? sg_mode_pick(modes, n, m) : -1;
	CHECK(i == 1, "on return it gets 144 Hz, not its fastest 165");

	CHECK(sg_mode_pick(modes, n, (struct sg_mode){1920, 1080, 144000}) == 1, "a refresh within 0.5 Hz matches");
	CHECK(sg_mode_pick(modes, n, (struct sg_mode){2560, 1440, 144000}) == -1, "a mode it no longer offers: none");
	CHECK(sg_mode_pick(modes, n, (struct sg_mode){1920, 1080, 100000}) == -1, "a refresh it does not offer: none");
	CHECK(sg_mode_pick(modes, n, (struct sg_mode){1280, 720, 0}) == 4, "a size without a refresh matches any");

	sg_mode_remember(k1, (struct sg_mode){1920, 1080, 60000});
	CHECK(sg_mode_recall(k1, &m) && m.refresh == 60000, "a later choice replaces the earlier one");
	CHECK(!sg_mode_recall(k3, &m), "another monitor keeps nothing of it");

	for (int j = 0; j < 40; j++) {
		char k[64];
		snprintf(k, sizeof(k), "output:X-%d", j);
		sg_mode_remember(k, (struct sg_mode){800, 600, 60000});
	}
	CHECK(sg_mode_recall("output:X-39", &m), "the newest are kept when many monitors came and went");

	printf("%s\n", fails ? "[modememory] GATE FAIL" : "[modememory] GATE PASS");
	return fails ? 1 : 0;
}
