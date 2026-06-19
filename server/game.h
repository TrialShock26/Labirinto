#ifndef GAME_H
#define GAME_H

#include "player.h"
#include "maze.h"

/* ============================================================
 *  Stato fine partita
 * ============================================================ */
typedef enum {
    GAME_RUNNING   = 0,
    GAME_OVER_EXIT,      /* almeno un giocatore è uscito prima del timeout */
    GAME_OVER_TIMEOUT    /* timeout scaduto, nessuno o più usciti           */
} GameStatus;

/* Controlla se la partita è terminata.
 *   - Restituisce GAME_RUNNING se la partita continua.
 *   - Scrive in winner_nick e winner_score il vincitore se trovato.
 *   - draw viene posto a 1 in caso di pareggio.
 *
 * Regole (da specifica):
 *   1. Se è uscito un solo giocatore → vince lui
 *   2. Se sono usciti più giocatori prima del timeout → vince chi ha più oggetti
 *   3. Se nessuno è uscito (timeout) → vince chi ha più oggetti tra tutti
 *
 * NON acquisisce il semaforo: deve essere chiamata dentro sem_p/sem_v.
 */
GameStatus game_check_end(const Maze *maze, const PlayerTable *pt,
                          char *winner_nick, int *winner_score, int *draw);

/* Costruisce il messaggio GAME_END da inviare al client.
 * buf deve essere almeno MAX_MSG_LEN byte. */
void game_build_end_msg(GameStatus status,
                        const char *winner_nick, int winner_score, int draw,
                        char *buf);

#endif /* GAME_H */
