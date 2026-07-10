#include <stdio.h>
#include "tetrisd/tetrisd.h"
#include "tetrisd/listener.h"
#include "tetrisd/session.h"
#include "tetrisd/worker.h"
#include "tetrisd/game.h"

int main(void) {
    printf("tetrisd\n");
    game_init();
    listener_init();
    session_init();
    worker_init();
    return 0;
}
