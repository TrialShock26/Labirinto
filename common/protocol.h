#ifndef PROTOCOL_H
#define PROTOCOL_H

/* ---------- dimensioni labirinto ---------- */
#define MAZE_ROWS       20
#define MAZE_COLS       20
#define VIEW_RADIUS     2          /* finestra locale: (2*VIEW_RADIUS+1)^2 */

/* ---------- celle del labirinto ---------- */
#define CELL_FREE       '.'
#define CELL_WALL       '#'
#define CELL_EXIT       'E'
#define CELL_OBJECT     '@'
#define CELL_UNKNOWN    '?'        /* celle non ancora scoperte (mappa globale) */
#define CELL_PLAYER     'P'        /* posizione del giocatore (vista locale)   */

/* ---------- parametri di gioco ---------- */
#define GLOBAL_MAP_INTERVAL  10   /* secondi tra un invio della mappa globale e il prossimo */
#define GAME_TIMEOUT         300  /* secondi, durata massima partita */
#define MAX_PLAYERS          32
#define MAX_NICK_LEN         32
#define MAX_PASS_LEN         64
#define MAX_MSG_LEN          1024
#define USERS_FILE           "users.txt"
#define LOG_FILE             "server.log"

/* 
 *  Comandi CLIENT → SERVER  
 *
 *  REGISTER <nick> <password>\n
 *  LOGIN    <nick> <password>\n
 *  MOVE     <N|S|E|W>\n
 *  LIST\n
 *  QUIT\n
 */

/* 
 *  Risposte SERVER → CLIENT  
 *
 *  OK\n                          – comando accettato (generico)
 *  ERR <messaggio>\n             – errore
 *
 *  LOCAL <rows> <cols> <data>\n
 *      data = stringa di (rows*cols) caratteri, riga per riga
 *      (celle: CELL_FREE / CELL_WALL / CELL_OBJECT / CELL_EXIT /
 *              CELL_UNKNOWN / CELL_PLAYER)
 *
 *  GLOBAL <rows> <cols> <data>\n
 *      stessa codifica di LOCAL, ma copre l'intera mappa
 *
 *  LIST <n> <nick1> <nick2> ... <nickN>\n
 *      lista degli utenti correntemente connessi
 *
 *  COLLECT <oggetti_totali>\n    – notifica raccolta oggetto
 *  EXIT_OK <punteggio>\n         – giocatore raggiunto uscita
 *
 *  GAME_END WIN   <nick> <score>\n   – fine partita, vincitore
 *  GAME_END DRAW  <score>\n          – pareggio
 *  GAME_END TIMEOUT\n                – timeout senza usciti
 */

/* Codici di ritorno usati internamente */
#define PROTO_OK        0
#define PROTO_ERR      -1

#endif /* PROTOCOL_H */
