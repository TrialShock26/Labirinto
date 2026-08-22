#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "player.h"
#include "maze.h"

int player_add(PlayerTable *pt, pthread_t tid, const char *nick, int row, int col) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!pt->slots[i].active) {
            Player *p = &pt->slots[i];
            memset(p, 0, sizeof(Player));
            p->active = 1;
            p->tid    = tid;
            p->row    = row;
            p->col    = col;
            p->score  = 0;
            p->exited = 0;
            strncpy(p->nick, nick, MAX_NICK_LEN - 1);
            memset(p->discovered, 0, sizeof(p->discovered));
            player_reveal(p, row, col);
            pt->count++;
            return i;
        }
    }
    return -1;
}

void player_remove(PlayerTable *pt, int idx) {
    if (idx < 0 || idx >= MAX_PLAYERS) return;
    pt->slots[idx].active = 0;
    pt->count--;
}

void player_reveal(Player *p, int row, int col) {
    for (int r_to_see = -VIEW_RADIUS; r_to_see <= VIEW_RADIUS; r_to_see++) {
        for (int c_to_see = -VIEW_RADIUS; c_to_see <= VIEW_RADIUS; c_to_see++) {
            int r = row + r_to_see;
            int c = col + c_to_see;
            if (r >= 0 && r < MAZE_ROWS && c >= 0 && c < MAZE_COLS)
                p->discovered[r][c] = 1;
        }
    }
}

/* Mappa locale: finestra (2*VIEW_RADIUS+1) x (2*VIEW_RADIUS+1) centrata sul giocatore
   Celle non scoperte = CELL_UNKNOWN
   Posizione giocatore = CELL_PLAYER */
void player_local_map(const Player *p, const Maze* maze, char *buf) {
    int idx = 0;
    for (int r_to_see = -VIEW_RADIUS; r_to_see <= VIEW_RADIUS; r_to_see++) {
        for (int c_to_see = -VIEW_RADIUS; c_to_see <= VIEW_RADIUS; c_to_see++) {
            int r = p->row + r_to_see;
            int c = p->col + c_to_see;
            char ch;
            if (r < 0 || r >= MAZE_ROWS || c < 0 || c >= MAZE_COLS) {
                ch = CELL_WALL;
            } else if (r_to_see == 0 && c_to_see == 0) {
                ch = CELL_PLAYER;
            } else if (!p->discovered[r][c]) {
                ch = CELL_UNKNOWN;
            } else {
                ch = maze->grid[r][c].cell;
            }
            buf[idx++] = ch;
        }
    }
    buf[idx] = '\0';
}

/* Mappa globale: intera matrice
   Celle non scoperte = CELL_UNKNOWN
   Posizione giocatore = CELL_PLAYER */
void player_global_map(const Player *p, const Maze* maze, char *buf) {
    int idx = 0;
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int c = 0; c < MAZE_COLS; c++) {
            if (r == p->row && c == p->col)
                buf[idx++] = CELL_PLAYER;
            else if (!p->discovered[r][c])
                buf[idx++] = CELL_UNKNOWN;
            else
                buf[idx++] = maze->grid[r][c].cell;
        }
    }
    buf[idx] = '\0';
}