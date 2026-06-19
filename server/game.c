#include <string.h>
#include <stdio.h>
#include <time.h>
#include "game.h"
#include "../common/protocol.h"

/* ============================================================
 *  game_check_end
 *
 *  Chiamata tenendo il semaforo. Legge lo stato corrente di
 *  Maze e PlayerTable e decide se la partita è finita.
 * ============================================================ */
GameStatus game_check_end(const Maze *maze, const PlayerTable *pt,
                          char *winner_nick, int *winner_score, int *draw)
{
    *winner_nick  = '\0';
    *winner_score = 0;
    *draw         = 0;

    /* ---- controlla timeout globale ---- */
    int timed_out = (time(NULL) - maze->start_time) >= GAME_TIMEOUT;

    /* ---- raccoglie statistiche sugli usciti ---- */
    int  exited_count = 0;
    int  best_score   = -1;
    char best_nick[MAX_NICK_LEN] = {0};
    int  tied         = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!pt->slots[i].active) continue;
        if (pt->slots[i].exited) {
            exited_count++;
            if (pt->slots[i].score > best_score) {
                best_score = pt->slots[i].score;
                strncpy(best_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                tied = 0;
            } else if (pt->slots[i].score == best_score) {
                tied = 1;
            }
        }
    }

    /* ---- caso 1: un solo giocatore uscito (prima del timeout) ---- */
    if (!timed_out && exited_count == 1) {
        strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
        *winner_score = best_score;
        *draw         = 0;
        return GAME_OVER_EXIT;
    }

    /* ---- caso 2: più giocatori usciti prima del timeout ---- */
    if (!timed_out && exited_count > 1) {
        strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
        *winner_score = best_score;
        *draw         = tied;
        return GAME_OVER_EXIT;
    }

    /* ---- caso 3: timeout scaduto ---- */
    if (timed_out) {
        if (exited_count > 0) {
            /* vince chi ha più oggetti tra gli usciti */
            strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
            *winner_score = best_score;
            *draw         = tied;
        } else {
            /* nessuno uscito: vince chi ha più oggetti in assoluto */
            best_score = -1;
            tied       = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!pt->slots[i].active) continue;
                if (pt->slots[i].score > best_score) {
                    best_score = pt->slots[i].score;
                    strncpy(best_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                    tied = 0;
                } else if (pt->slots[i].score == best_score) {
                    tied = 1;
                }
            }
            strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
            *winner_score = best_score;
            *draw         = tied;
        }
        return GAME_OVER_TIMEOUT;
    }

    return GAME_RUNNING;
}

/* ============================================================
 *  game_build_end_msg
 *
 *  Formati:
 *    GAME_END WIN  <nick> <score>
 *    GAME_END DRAW <score>
 *    GAME_END TIMEOUT
 * ============================================================ */
void game_build_end_msg(GameStatus status,
                        const char *winner_nick, int winner_score, int draw,
                        char *buf)
{
    if (status == GAME_OVER_TIMEOUT && winner_score < 0) {
        /* nessun giocatore attivo */
        snprintf(buf, MAX_MSG_LEN, "GAME_END TIMEOUT");
        return;
    }

    if (draw) {
        snprintf(buf, MAX_MSG_LEN, "GAME_END DRAW %d", winner_score);
    } else {
        snprintf(buf, MAX_MSG_LEN, "GAME_END WIN %s %d", winner_nick, winner_score);
    }
}
