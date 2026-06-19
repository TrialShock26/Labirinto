#ifndef MAZE_H
#define MAZE_H

#include <time.h>
#include "../common/protocol.h"

/* ============================================================
 *  Struttura dati del labirinto (condivisa in shared memory)
 * ============================================================ */

typedef struct {
    char cell;          /* CELL_FREE / CELL_WALL / CELL_EXIT / CELL_OBJECT */
} MazeCell;

/* Struttura del labirinto in shared memory.
 * Accesso: maze->grid[r][c]
 */
typedef struct {
    MazeCell grid[MAZE_ROWS][MAZE_COLS];
    int      num_objects;       /* oggetti ancora presenti */
    int      num_exits;         /* numero di uscite        */
    int      game_over;         /* flag fine partita        */
    int      winner_pid;        /* PID del processo vincitore (o -1) */
    char     winner_nick[MAX_NICK_LEN];
    int      winner_score;
    time_t   start_time;
} Maze;

/* ============================================================
 *  API generazione labirinto
 * ============================================================ */

/* Genera il labirinto con DFS randomizzato.
 * Popola maze->grid, posiziona muri, uscite e oggetti. */
void maze_generate(Maze *maze);

/* Trova una cella libera casuale (non muro, non uscita).
 * Restituisce 1 se trovata, 0 altrimenti. */
int maze_random_free_cell(const Maze *maze, int *row, int *col);

/* Rimuove un oggetto dalla cella (r,c).
 * Restituisce 1 se c'era un oggetto, 0 altrimenti. */
int maze_collect_object(Maze *maze, int row, int col);

/* Stampa il labirinto su stderr (debug). */
void maze_dump(const Maze *maze);

#endif /* MAZE_H */
