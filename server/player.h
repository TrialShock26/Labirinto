#include <pthread.h>
#ifndef PLAYER_H
#define PLAYER_H

#include "../common/protocol.h"

/* ============================================================
 *  Stato di un giocatore (in memoria condivisa tra thread)
 * ============================================================ */
typedef struct {
    int  active;                        /* 1 = slot occupato            */
    pthread_t tid;                      /* TID del thread               */
    char nick[MAX_NICK_LEN];
    int  row, col;                      /* posizione corrente           */
    int  score;                         /* oggetti raccolti             */
    int  exited;                        /* 1 = ha raggiunto l'uscita    */

    /* mappa delle celle scoperte: discovered[r][c] = 1 se vista */
    char discovered[MAZE_ROWS][MAZE_COLS];
} Player;

/* Tabella dei giocatori condivisa tra tutti i thread */
typedef struct {
    Player slots[MAX_PLAYERS];
    int    count;   /* slot attivi */
} PlayerTable;

/* ---- API ---- */

/* Cerca il primo slot libero e lo inizializza.
 * Restituisce l'indice oppure -1 se la tabella è piena. */
int  player_add(PlayerTable *pt, pthread_t tid, const char *nick, int row, int col);

/* Libera lo slot dell'indice dato. */
void player_remove(PlayerTable *pt, int idx);

/* Trova l'indice del giocatore con quel TID (-1 se non trovato). */
int  player_find_by_tid(const PlayerTable *pt, pthread_t tid);

/* Rivela le celle attorno a (row,col) nel raggio VIEW_RADIUS. */
void player_reveal(Player *p, int row, int col);

/* Costruisce la stringa della mappa locale (finestra centrata sul giocatore).
 * buf deve essere almeno (2*VIEW_RADIUS+1)^2 + 1 byte.
 * Restituisce righe e colonne effettive. */
void player_local_map(const Player *p, const char maze_grid[][MAZE_COLS],
                      char *buf, int *out_rows, int *out_cols);

/* Costruisce la stringa della mappa globale mascherata.
 * buf deve essere almeno MAZE_ROWS*MAZE_COLS + 1 byte. */
void player_global_map(const Player *p, const char maze_grid[][MAZE_COLS],
                       char *buf);

#endif /* PLAYER_H */
