#include <string.h>
#include <stdio.h>
#include <time.h>
#include "game.h"
#include "../common/protocol.h"

GameStatus game_check_end(const Maze *maze, const PlayerTable *pt,
                          char *winner_nick, int *winner_score, int *draw,
                          int max_players_ever)
{
    *winner_nick  = '\0';
    *winner_score = 0;
    *draw         = 0;

    int timed_out = (time(NULL) - maze->start_time) >= GAME_TIMEOUT;

    int  exited_count = 0;
    int  ingame_count = 0;
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
        } else {
            ingame_count++;
        }
    }

    /* caso -1: tutti hanno quittato, tabella vuota */
    if (ingame_count == 0 && exited_count == 0) {
        *winner_score = -1;
        return GAME_OVER_TIMEOUT;
    }

    /* caso 0: rimasto un solo giocatore in gioco */
    if (ingame_count == 1 && (exited_count > 0 || max_players_ever > 1)) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!pt->slots[i].active || pt->slots[i].exited)
                continue;

            if (exited_count == 0) {
                /* unico rimasto, vince lui */
                strncpy(winner_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                *winner_score = pt->slots[i].score;
                *draw = 0;
            } else {
                /* confronta con i già usciti */
                if (pt->slots[i].score > best_score) {
                    strncpy(winner_nick, pt->slots[i].nick, MAX_NICK_LEN - 1);
                    *winner_score = pt->slots[i].score;
                    *draw = 0;
                } else if (pt->slots[i].score == best_score) {
                    *winner_score = best_score;
                    *draw = 1;
                } else {
                    strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
                    *winner_score = best_score;
                    *draw = 0;
                }
            }
            return GAME_OVER_EXIT;
        }
    }
    /* caso 1: tutti i giocatori attivi sono usciti */
    if (exited_count >= 1 && ingame_count == 0) {
        strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
        *winner_score = best_score;
        *draw         = tied;
        return GAME_OVER_EXIT;
    }

    /* caso 2: timeout con almeno un uscito */
    if (timed_out && exited_count >= 1) {
        strncpy(winner_nick, best_nick, MAX_NICK_LEN - 1);
        *winner_score = best_score;
        *draw         = tied;
        return GAME_OVER_EXIT;
    }

    /* caso 3: timeout senza usciti */
    if (timed_out) {
        best_score = -1; tied = 0;
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
        return GAME_OVER_TIMEOUT;
    }

    return GAME_RUNNING;
}

void game_build_end_msg(GameStatus status,
                        const char *winner_nick, int winner_score, int draw,
                        char *buf)
{
    if (status == GAME_OVER_TIMEOUT && winner_score < 0) {
        snprintf(buf, MAX_MSG_LEN, "GAME_END TIMEOUT");
        return;
    }
    if (draw)
        snprintf(buf, MAX_MSG_LEN, "GAME_END DRAW %d", winner_score);
    else
        snprintf(buf, MAX_MSG_LEN, "GAME_END WIN %s %d", winner_nick, winner_score);
}