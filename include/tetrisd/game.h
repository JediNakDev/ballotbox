#ifndef TETRISD_GAME_H
#define TETRISD_GAME_H

/*
 * game.h - pure game logic for tetrisd.
 *
 * Board/piece/rules/tick state. No sockets, no threads: this layer only
 * mutates game state and is driven by the session/worker layers.
 */

void game_init(void);

#endif /* TETRISD_GAME_H */
