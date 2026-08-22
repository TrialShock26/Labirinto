#ifndef GAME_H
#define GAME_H

#include "player.h"
#include "maze.h"

// Stato fine partita
typedef enum {
    GAME_RUNNING,
    GAME_OVER_EXIT,
    GAME_OVER_TIMEOUT
} GameStatus;

// Controlla se la partita è terminata
GameStatus game_check_end(const Maze *maze, const PlayerTable *pt,
                          char *winner_nick, int *winner_score, int *draw);

// Costruisce il messaggio GAME_END da inviare al client
void game_build_end_msg(const char *winner_nick, int winner_score, int draw, char *buf);

#endif