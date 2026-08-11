#ifndef PROTOCOL_H
#define PROTOCOL_H

// Dimensioni labirinto
#define MAZE_ROWS            20
#define MAZE_COLS            20
#define VIEW_RADIUS          1  // Finestra locale: (2*VIEW_RADIUS + 1)^2

// Celle del labirinto
#define CELL_FREE           '.'
#define CELL_WALL           '#'
#define CELL_EXIT           'E'
#define CELL_OBJECT         '@'
#define CELL_UNKNOWN        '?'
#define CELL_PLAYER         'P'

// Parametri di gioco
#define GLOBAL_MAP_INTERVAL 10  // Secondi tra un invio della mappa globale e il prossimo
#define GAME_TIMEOUT        90  // Durata massima partita
#define MAX_PLAYERS         10
#define MAX_NICK_LEN        32
#define MAX_PASS_LEN        32
#define MAX_PASS_DIGITS     20
#define MAX_MSG_LEN         1024
#define USERS_FILE          "users.txt"
#define LOG_FILE            "server.log"

// Messaggi stato del player TODO no use
#define CMD_READY           "READY"
#define MSG_WAITING         "WAITING"
#define MSG_START           "START"

// Codici di ritorno usati internamente
#define PROTO_OK            0
#define PROTO_ERR          -1

#endif