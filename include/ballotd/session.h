#ifndef TETRISD_SESSION_H
#define TETRISD_SESSION_H

/*
 * session.h - the server socket (one accepted connection).
 *
 * Server side of a single accepted connection: read/parse requests, write
 * responses, and hold per-connection state. Drives the game logic in game.h.
 */

void session_init(void);

#endif /* TETRISD_SESSION_H */
