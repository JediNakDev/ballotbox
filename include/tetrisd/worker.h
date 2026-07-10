#ifndef TETRISD_WORKER_H
#define TETRISD_WORKER_H

/*
 * worker.h - the worker thread routine.
 *
 * The thread (or thread pool) that services accepted sessions. This is the
 * one place where the listen socket and the per-connection server sockets
 * meet the game logic.
 */

void worker_init(void);

#endif /* TETRISD_WORKER_H */
