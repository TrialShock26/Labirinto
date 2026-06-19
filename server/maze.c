#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "maze.h"

/* ---- parametri generazione ---- */
#define NUM_OBJECTS  10
#define NUM_EXITS     2

/* ============================================================
 *  Generazione con algoritmo di Prim randomizzato
 *
 *  Struttura della griglia: celle "logiche" su indici pari (0,2,4,...),
 *  muri tra celle adiacenti su indici dispari.
 *  Prim parte da una cella seme, mantiene una lista di muri candidati
 *  ("frontiera") e ad ogni passo sceglie un muro a caso che separa
 *  una cella già nel labirinto da una cella non ancora visitata.
 *  Risultato: labirinto perfetto con struttura più "aperta" e ramificata
 *  rispetto al DFS (corridoi più brevi, più incroci).
 * ============================================================ */

/* direzioni: passo 2 (celle logiche) e passo 1 (muro intermedio) */
static const int DR2[] = {-2,  2,  0,  0};
static const int DC2[] = { 0,  0, -2,  2};
static const int DR1[] = {-1,  1,  0,  0};
static const int DC1[] = { 0,  0, -1,  1};

/* ---- lista dinamica di muri di frontiera ---- */
typedef struct { int r, c, dir; } Wall;

static Wall  *frontier     = NULL;
static int    frontier_sz  = 0;
static int    frontier_cap = 0;

static void frontier_push(int r, int c, int dir) {
    if (frontier_sz == frontier_cap) {
        frontier_cap = frontier_cap ? frontier_cap * 2 : 64;
        frontier = realloc(frontier, frontier_cap * sizeof(Wall));
    }
    frontier[frontier_sz++] = (Wall){r, c, dir};
}

static Wall frontier_pop_random(void) {
    int idx = rand() % frontier_sz;
    Wall w  = frontier[idx];
    frontier[idx] = frontier[--frontier_sz];
    return w;
}

/* aggiunge alla frontiera tutti i muri non ancora visitati attorno a (r,c) */
static void add_frontier(Maze *maze, int r, int c) {
    for (int d = 0; d < 4; d++) {
        int nr = r + DR2[d];
        int nc = c + DC2[d];
        if (nr >= 0 && nr < MAZE_ROWS && nc >= 0 && nc < MAZE_COLS
                && maze->grid[nr][nc].cell == CELL_WALL)
            frontier_push(r, c, d);
    }
}

void maze_generate(Maze *maze) {
    /* inizializza tutto a muro */
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            maze->grid[r][c].cell = CELL_WALL;

    /* cella seme: primo indice pari disponibile */
    int sr = 0, sc = 0;
    maze->grid[sr][sc].cell = CELL_FREE;

    frontier_sz  = 0;
    frontier_cap = 0;
    frontier     = NULL;
    add_frontier(maze, sr, sc);

    while (frontier_sz > 0) {
        Wall w  = frontier_pop_random();
        int  nr = w.r + DR2[w.dir];
        int  nc = w.c + DC2[w.dir];

        /* la cella di destinazione deve essere ancora muro */
        if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS) continue;
        if (maze->grid[nr][nc].cell != CELL_WALL) continue;

        /* abbatti il muro intermedio e segna la cella come libera */
        maze->grid[w.r + DR1[w.dir]][w.c + DC1[w.dir]].cell = CELL_FREE;
        maze->grid[nr][nc].cell = CELL_FREE;

        add_frontier(maze, nr, nc);
    }

    free(frontier);
    frontier = NULL;

    /* posiziona uscite */
    int placed = 0;
    while (placed < NUM_EXITS) {
        int r = rand() % MAZE_ROWS;
        int c = rand() % MAZE_COLS;
        if (maze->grid[r][c].cell == CELL_FREE) {
            maze->grid[r][c].cell = CELL_EXIT;
            placed++;
        }
    }

    /* posiziona oggetti */
    placed = 0;
    while (placed < NUM_OBJECTS) {
        int r = rand() % MAZE_ROWS;
        int c = rand() % MAZE_COLS;
        if (maze->grid[r][c].cell == CELL_FREE) {
            maze->grid[r][c].cell = CELL_OBJECT;
            placed++;
        }
    }

    maze->num_objects    = NUM_OBJECTS;
    maze->num_exits      = NUM_EXITS;
    maze->game_over      = 0;
    maze->winner_pid     = -1;
    maze->winner_score   = 0;
    maze->winner_nick[0] = '\0';
    maze->start_time     = time(NULL);
}

int maze_random_free_cell(const Maze *maze, int *row, int *col) {
    for (int attempt = 0; attempt < MAZE_ROWS * MAZE_COLS * 2; attempt++) {
        int r = rand() % MAZE_ROWS;
        int c = rand() % MAZE_COLS;
        if (maze->grid[r][c].cell == CELL_FREE) {
            *row = r; *col = c;
            return 1;
        }
    }
    return 0;
}

int maze_collect_object(Maze *maze, int row, int col) {
    if (maze->grid[row][col].cell == CELL_OBJECT) {
        maze->grid[row][col].cell = CELL_FREE;
        maze->num_objects--;
        return 1;
    }
    return 0;
}

void maze_dump(const Maze *maze) {
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int c = 0; c < MAZE_COLS; c++)
            fputc(maze->grid[r][c].cell, stderr);
        fputc('\n', stderr);
    }
}
