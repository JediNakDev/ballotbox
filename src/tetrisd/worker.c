#include <stdio.h>
#include "tetrisd/worker.h"

/* Worker thread routine: services accepted sessions against the game logic. */
void worker_init(void) {
    printf("tetrisd: worker\n");
}
