#ifndef TETRISD_LISTENER_H
#define TETRISD_LISTENER_H

/*
 * listener.h - the listen socket.
 *
 * Owns the single long-lived accepting socket: socket()/bind()/listen() and
 * the accept loop. Hands each accepted connection fd off to a worker.
 */

void listener_init(void);

#endif /* TETRISD_LISTENER_H */
