#include <string.h>
#include <stdio.h>
#include <time.h>

#include "game.h"
#include "../common/protocol.h"

GameStatus game_check_end(const Maze *maze, const PlayerTable *pt,
                          char *winner_nick, int *winner_score, int *draw) {
    *winner_nick = '\0';
    *winner_score = 0;
    *draw = 0;

    if (pt->count == 1) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (pt->slots[i].active) {
                strncpy(winner_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                *winner_score = pt->slots[i].score;
                *draw = 0;
                return GAME_OVER_EXIT;
            }
        }
    }

    int timed_out = (time(NULL) - maze->start_time) >= GAME_TIMEOUT;

    int  exited_count = 0;
    int  ingame_count = 0;
    int  best_exited_score = -1;
    char best_exited_nick[MAX_NICK_LEN] = {0};
    int  exited_tied = 0;
    int  best_any_score = -1;
    char best_any_nick[MAX_NICK_LEN] = {0};
    int  any_tied = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!pt->slots[i].active) continue;

        /* migliore tra TUTTI i giocatori (caso 3) */
        if (pt->slots[i].score > best_any_score) {
            best_any_score = pt->slots[i].score;
            strncpy(best_any_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
            any_tied = 0;
        } else if (pt->slots[i].score == best_any_score) {
            any_tied = 1;
        }

        /* classifica tra gli USCITI (casi 1 e 2) */
        if (pt->slots[i].exited) {
            exited_count++;
            if (pt->slots[i].score > best_exited_score) {
                best_exited_score = pt->slots[i].score;
                strncpy(best_exited_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                exited_tied = 0;
            } else if (pt->slots[i].score == best_exited_score) {
                exited_tied = 1;
            }
        } else {
            ingame_count++;
        }
    }

    /* --- condizione: tutti hanno quittato --- */
    if (exited_count == 0 && ingame_count == 0) {
        *winner_score = -1;
        return GAME_OVER_TIMEOUT;
    }

    /* --- condizione: tutti i giocatori attivi sono usciti --- */
    if (ingame_count == 0 && exited_count >= 1) {
        if (exited_count == 1) {
            /* regola 1: un solo uscito → vince lui */
            strncpy(winner_nick, best_exited_nick, MAX_NICK_LEN - 1);
            *winner_score = best_exited_score;
            *draw = 0;
        } else {
            /* regola 2: più usciti → vince chi ha più oggetti */
            strncpy(winner_nick, best_exited_nick, MAX_NICK_LEN - 1);
            *winner_score = best_exited_score;
            *draw = exited_tied;
        }
        return GAME_OVER_EXIT;
    }

    /* --- condizione: timeout scaduto --- */
    if (timed_out) {
        if (exited_count == 1) {
            /* regola 1: un solo uscito → vince lui */
            strncpy(winner_nick, best_exited_nick, MAX_NICK_LEN - 1);
            *winner_score = best_exited_score;
            *draw = 0;
        } else if (exited_count >= 2) {
            /* regola 2: più usciti → vince chi ha più oggetti */
            strncpy(winner_nick, best_exited_nick, MAX_NICK_LEN - 1);
            *winner_score = best_exited_score;
            *draw = exited_tied;
        } else {
            /* regola 3: nessuno uscito → vince chi ha più oggetti */
            strncpy(winner_nick, best_any_nick, MAX_NICK_LEN - 1);
            *winner_score = best_any_score;
            *draw = any_tied;
        }
        return GAME_OVER_TIMEOUT;
    }

    /* --- la partita continua --- */
    return GAME_RUNNING;
}

void game_build_end_msg(GameStatus status, const char *winner_nick, int winner_score, int draw, char *buf) {
    if (status == GAME_OVER_TIMEOUT && winner_score < 0) {
        snprintf(buf, MAX_MSG_LEN, "GAME_END TIMEOUT");
        return;
    }
    if (draw)
        snprintf(buf, MAX_MSG_LEN, "GAME_END DRAW %d", winner_score);
    else
        snprintf(buf, MAX_MSG_LEN, "GAME_END WIN %s %d", winner_nick, winner_score);
}