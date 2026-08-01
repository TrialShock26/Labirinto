#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "maze.h"

#define NUM_OBJECTS  10
#define NUM_EXITS     2

int DR2[] = {-2,  2,  0,  0};
int DC2[] = { 0,  0, -2,  2};
int DR1[] = {-1,  1,  0,  0};
int DC1[] = { 0,  0, -1,  1};

typedef struct { int r, c, dir; } Wall;

Wall  *frontier     = NULL;
int    frontier_sz  = 0;
int    frontier_cap = 0;

void frontier_push(int r, int c, int dir) {
    if (frontier_sz == frontier_cap) {
        frontier_cap = frontier_cap ? frontier_cap * 2 : 64;
        frontier = realloc(frontier, frontier_cap * sizeof(Wall));
    }
    frontier[frontier_sz++] = (Wall){r, c, dir};
}

Wall frontier_pop_random() {
    int idx = rand() % frontier_sz;
    Wall w  = frontier[idx];
    frontier[idx] = frontier[--frontier_sz];
    return w;
}

void add_frontier(Maze *maze, int r, int c) {
    for (int d = 0; d < 4; d++) {
        int nr = r + DR2[d];
        int nc = c + DC2[d];
        if (nr >= 0 && nr < MAZE_ROWS && nc >= 0 && nc < MAZE_COLS
                && maze->grid[nr][nc].cell == CELL_WALL)
            frontier_push(r, c, d);
    }
}

void maze_generate(Maze *maze) {
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            maze->grid[r][c].cell = CELL_WALL;

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

        if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS) continue;
        if (maze->grid[nr][nc].cell != CELL_WALL) continue;

        maze->grid[w.r + DR1[w.dir]][w.c + DC1[w.dir]].cell = CELL_FREE;
        maze->grid[nr][nc].cell = CELL_FREE;

        add_frontier(maze, nr, nc);
    }

    free(frontier);
    frontier = NULL;

    int placed = 0;
    while (placed < NUM_EXITS) {
        int r = rand() % MAZE_ROWS;
        int c = rand() % MAZE_COLS;
        if (maze->grid[r][c].cell == CELL_FREE) {
            maze->grid[r][c].cell = CELL_EXIT;
            placed++;
        }
    }

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