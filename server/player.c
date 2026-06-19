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

int player_find_by_tid(const PlayerTable *pt, pthread_t tid) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (pt->slots[i].active && pthread_equal(pt->slots[i].tid, tid))
            return i;
    return -1;
}

void player_reveal(Player *p, int row, int col) {
    for (int dr = -VIEW_RADIUS; dr <= VIEW_RADIUS; dr++) {
        for (int dc = -VIEW_RADIUS; dc <= VIEW_RADIUS; dc++) {
            int r = row + dr;
            int c = col + dc;
            if (r >= 0 && r < MAZE_ROWS && c >= 0 && c < MAZE_COLS)
                p->discovered[r][c] = 1;
        }
    }
}

/* ============================================================
 *  Mappa locale: finestra (2*VIEW_RADIUS+1) x (2*VIEW_RADIUS+1)
 *  centrata sul giocatore. Celle non scoperte → CELL_UNKNOWN.
 *  Posizione giocatore → CELL_PLAYER.
 * ============================================================ */
void player_local_map(const Player *p, const char maze_grid[][MAZE_COLS],
                      char *buf, int *out_rows, int *out_cols)
{
    int dim = 2 * VIEW_RADIUS + 1;
    *out_rows = dim;
    *out_cols = dim;
    int idx = 0;

    for (int dr = -VIEW_RADIUS; dr <= VIEW_RADIUS; dr++) {
        for (int dc = -VIEW_RADIUS; dc <= VIEW_RADIUS; dc++) {
            int r = p->row + dr;
            int c = p->col + dc;
            char ch;
            if (r < 0 || r >= MAZE_ROWS || c < 0 || c >= MAZE_COLS) {
                ch = CELL_WALL;
            } else if (dr == 0 && dc == 0) {
                ch = CELL_PLAYER;
            } else if (!p->discovered[r][c]) {
                ch = CELL_UNKNOWN;
            } else {
                ch = maze_grid[r][c];
            }
            buf[idx++] = ch;
        }
    }
    buf[idx] = '\0';
}

/* ============================================================
 *  Mappa globale: intera matrice, celle non scoperte → CELL_UNKNOWN
 * ============================================================ */
void player_global_map(const Player *p, const char maze_grid[][MAZE_COLS],
                       char *buf)
{
    int idx = 0;
    for (int r = 0; r < MAZE_ROWS; r++) {
        for (int c = 0; c < MAZE_COLS; c++) {
            if (r == p->row && c == p->col)
                buf[idx++] = CELL_PLAYER;
            else if (!p->discovered[r][c])
                buf[idx++] = CELL_UNKNOWN;
            else
                buf[idx++] = maze_grid[r][c];
        }
    }
    buf[idx] = '\0';
}
