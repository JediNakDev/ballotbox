#include <stdio.h>
#include "ballotd/tetrisd.h"
#include "ballotd/listener.h"
#include "ballotd/session.h"
#include "ballotd/worker.h"
#include "ballotd/game.h"

int main(void) {
    printf("tetrisd\n");
    game_init();
    listener_init();
    session_init();
    worker_init();
    return 0;
}
