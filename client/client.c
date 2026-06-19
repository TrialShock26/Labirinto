/*
 * client.c  –  Labyrinth Game Client
 *
 * Uso: ./client <host> <porta>
 *
 * Comandi interattivi:
 *   w/a/s/d  → MOVE N/W/S/E
 *   l        → LIST utenti connessi
 *   q        → QUIT (disconnessione)
 *
 * Il client usa select() per gestire simultaneamente:
 *   - input da tastiera (stdin)
 *   - messaggi dal server (socket), incluse mappe globali asincrone
 *     e notifiche di fine partita
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>

#include "../common/protocol.h"

/* ============================================================
 *  Codici colore ANSI
 * ============================================================ */
#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"

/* colori testo */
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_GRAY       "\033[90m"

/* sfondo */
#define ANSI_BG_BLACK   "\033[40m"
#define ANSI_BG_WHITE   "\033[47m"
#define ANSI_BG_BLUE    "\033[44m"
#define ANSI_BG_GREEN   "\033[42m"
#define ANSI_BG_YELLOW  "\033[43m"
#define ANSI_BG_RED     "\033[41m"

/* cursore / schermo */
#define ANSI_CLEAR      "\033[2J\033[H"
#define ANSI_CLEAR_LINE "\033[2K\r"

static int g_sock   = -1;
static int g_score  = 0;    /* punteggio corrente, aggiornato da COLLECT */
static int g_exited = 0;    /* 1 dopo EXIT_OK: aspettiamo GAME_END       */

/* ============================================================
 *  Utility: invia una riga al server (aggiunge '\n')
 * ============================================================ */
static void send_line(const char *msg) {
    char buf[MAX_MSG_LEN + 2];
    int  n = snprintf(buf, sizeof(buf), "%s\n", msg);
    write(g_sock, buf, n);
}

/* ============================================================
 *  Utility: legge una riga dal socket (terminata da '\n')
 *  Restituisce i byte letti (senza '\n'), -1 su EOF/errore.
 * ============================================================ */
static int read_line(int fd, char *buf, int maxlen) {
    int  i = 0;
    char c;
    while (i < maxlen - 1) {
        int n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* ============================================================
 *  Simboli Unicode per la mappa
 *
 *  Giocatore : ጸ  (U+1338, Ethiopic Syllable Tze)
 *  Nemico    : ጼ  (U+133C, Ethiopic Syllable Tzi) — riservato per uso futuro
 *  Muro      : 𑀩  (U+11029, Brahmi Letter Tha)
 *  Oggetto   : @  (ASCII, ben visibile)
 *  Oggetto2  : ħ  (U+0127, Latin Small Letter H with Stroke)
 *
 *  Tutti i simboli sono stringhe UTF-8 costanti.
 *  Il protocollo trasporta byte ASCII ('P','#','O',...);
 *  la conversione avviene solo qui nel client.
 * ============================================================ */
#define SYM_PLAYER   "\xe1\x8c\xb8"        /* ጸ  U+1338  */
#define SYM_ENEMY    "\xe1\x8c\xbc"        /* ጼ  U+133C  */
#define SYM_WALL     "\xf0\x91\x80\xa9"    /* 𑀩  U+11029 */
#define SYM_OBJECT   "@"                   /* oggetto principale */
#define SYM_OBJECT2  "\xc4\xa7"            /* ħ   U+0127  — oggetto alternativo */
#define SYM_EXIT     "E"
#define SYM_FREE     " "
#define SYM_UNKNOWN  "?"

/* ============================================================
 *  Rendering mappa con colori ANSI e simboli Unicode
 *
 *  Legenda:
 *   𑀩  muro          – sfondo bianco
 *      cella libera  – sfondo nero
 *   @  oggetto       – giallo su nero
 *   E  uscita        – verde brillante su nero
 *   ?  non scoperta  – grigio scuro
 *   ጸ  giocatore     – ciano bold su blu
 * ============================================================ */
 
static void display_map(const char *type, int rows, int cols, const char *data) {
    printf("\n" ANSI_BOLD "=== %s MAP (%dx%d) ===" ANSI_RESET
           "   punteggio: " ANSI_YELLOW ANSI_BOLD "%d" ANSI_RESET "\n",
           type, rows, cols, g_score);

    /* riga superiore del bordo */
    printf(ANSI_BG_WHITE " ");
    for (int c = 0; c < cols; c++) printf("  ");
    printf("  " ANSI_RESET "\n");

    for (int r = 0; r < rows; r++) {
        printf(ANSI_BG_WHITE " " ANSI_RESET);   /* bordo sinistro */
        for (int c = 0; c < cols; c++) {
            char ch = data[r * cols + c];
            switch (ch) {
                case CELL_WALL:
                    /* 𑀩 è 4 byte, occupa visivamente 1 colonna → aggiungiamo uno spazio */
                    printf(ANSI_BG_WHITE " " SYM_WALL ANSI_RESET);
                    break;
                case CELL_FREE:
                    printf(ANSI_BG_BLACK "  " ANSI_RESET);
                    break;
                case CELL_OBJECT:
                    printf(ANSI_BG_BLACK ANSI_YELLOW ANSI_BOLD " " SYM_OBJECT ANSI_RESET);
                    break;
                case CELL_EXIT:
                    printf(ANSI_BG_GREEN ANSI_WHITE ANSI_BOLD " " SYM_EXIT ANSI_RESET);
                    break;
                case CELL_UNKNOWN:
                    printf(ANSI_BG_BLACK ANSI_GRAY " " SYM_UNKNOWN ANSI_RESET);
                    break;
                case CELL_PLAYER:
                    /* ጸ è 3 byte UTF-8, larghezza visiva 1 → spazio davanti */
                    printf(ANSI_BG_BLUE ANSI_CYAN ANSI_BOLD " " SYM_PLAYER ANSI_RESET);
                    break;
                default:
                    printf(ANSI_BG_BLACK " %c" ANSI_RESET, ch);
                    break;
            }
        }
        printf(ANSI_BG_WHITE " " ANSI_RESET "\n");  /* bordo destro */
    }

    /* riga inferiore del bordo */
    printf(ANSI_BG_WHITE " ");
    for (int c = 0; c < cols; c++) printf("  ");
    printf("  " ANSI_RESET "\n");

    /* legenda */
    printf(ANSI_GRAY
           "  legenda: "
           ANSI_BG_WHITE " " SYM_WALL " " ANSI_RESET ANSI_GRAY "=muro  "
           ANSI_YELLOW SYM_OBJECT ANSI_GRAY "=oggetto  "
           ANSI_GREEN  SYM_EXIT   ANSI_GRAY "=uscita  "
           ANSI_CYAN   SYM_PLAYER ANSI_GRAY "=tu  "
           SYM_UNKNOWN "=inesplorato"
           ANSI_RESET "\n\n");

    printf(ANSI_BOLD "[w/s/a/d]" ANSI_RESET "=muovi  "
           ANSI_BOLD "[l]" ANSI_RESET "=lista  "
           ANSI_BOLD "[q]" ANSI_RESET "=esci  > ");
    fflush(stdout);
}

/* ============================================================
 *  Gestisce un messaggio ricevuto dal server.
 *  Restituisce 1 se il gioco continua, 0 se deve terminare.
 * ============================================================ */
static int handle_server_msg(const char *line) {
    char cmd[16];
    sscanf(line, "%15s", cmd);

    /* ---- mappa locale ---- */
    if (strcmp(cmd, "LOCAL") == 0) {
        int  rows, cols;
        char data[MAX_MSG_LEN];
        if (sscanf(line, "LOCAL %d %d %s", &rows, &cols, data) == 3)
            display_map("LOCAL", rows, cols, data);
        return 1;
    }

    /* ---- mappa globale (inviata periodicamente dal server) ---- */
    if (strcmp(cmd, "GLOBAL") == 0) {
        int  rows, cols;
        char data[MAZE_ROWS * MAZE_COLS + 4];
        if (sscanf(line, "GLOBAL %d %d %s", &rows, &cols, data) == 3)
            display_map("GLOBAL", rows, cols, data);
        return 1;
    }

    /* ---- raccolta oggetto ---- */
    if (strcmp(cmd, "COLLECT") == 0) {
        sscanf(line, "COLLECT %d", &g_score);
        printf("\n" ANSI_YELLOW ANSI_BOLD
               "  ★ Oggetto raccolto! Punteggio: %d"
               ANSI_RESET "\n", g_score);
        fflush(stdout);
        return 1;
    }

    /* ---- giocatore raggiunge l'uscita ---- */
    if (strcmp(cmd, "EXIT_OK") == 0) {
        sscanf(line, "EXIT_OK %d", &g_score);
        printf("\n" ANSI_GREEN ANSI_BOLD
               "  ✓ Uscita raggiunta! Punteggio: %d"
               "\n  In attesa del risultato finale..."
               ANSI_RESET "\n", g_score);
        fflush(stdout);
        g_exited = 1;
        return 1;   /* continua: aspetta GAME_END dal server */
    }

    /* ---- fine partita ---- */
    if (strcmp(cmd, "GAME_END") == 0) {
        const char *payload = line + 9;   /* salta "GAME_END " */
        printf("\n");
        printf(ANSI_BOLD "╔══════════════════════════════╗\n" ANSI_RESET);
        printf(ANSI_BOLD "║       FINE PARTITA           ║\n" ANSI_RESET);
        printf(ANSI_BOLD "╚══════════════════════════════╝\n" ANSI_RESET);

        if (strncmp(payload, "WIN", 3) == 0) {
            char winner[MAX_NICK_LEN]; int score;
            if (sscanf(payload, "WIN %s %d", winner, &score) == 2) {
                printf(ANSI_YELLOW ANSI_BOLD
                       "  🏆 Vincitore: %s  (punteggio: %d)\n"
                       ANSI_RESET, winner, score);
            }
        } else if (strncmp(payload, "DRAW", 4) == 0) {
            int score;
            sscanf(payload, "DRAW %d", &score);
            printf(ANSI_CYAN ANSI_BOLD
                   "  🤝 Pareggio! Punteggio massimo: %d\n"
                   ANSI_RESET, score);
        } else if (strncmp(payload, "TIMEOUT", 7) == 0) {
            printf(ANSI_RED ANSI_BOLD
                   "  ⏰ Timeout! Nessun vincitore.\n"
                   ANSI_RESET);
        } else {
            printf("  %s\n", payload);
        }
        printf("\n");
        fflush(stdout);
        return 0;   /* termina il client */
    }

    /* ---- lista giocatori ---- */
    if (strcmp(cmd, "LIST") == 0) {
        /* formato: "LIST <n> <nick1> <nick2> ..." */
        int n = 0;
        const char *ptr = line + 5;
        sscanf(ptr, "%d", &n);
        printf("\n" ANSI_CYAN ANSI_BOLD "  Giocatori connessi (%d):" ANSI_RESET, n);
        /* avanza oltre il numero */
        while (*ptr && *ptr != ' ') ptr++;
        if (*ptr) ptr++;
        while (*ptr && *ptr != ' ') ptr++;
        if (*ptr) ptr++;
        printf(" %s\n\n", ptr);
        fflush(stdout);
        return 1;
    }

    /* ---- errore dal server ---- */
    if (strcmp(cmd, "ERR") == 0) {
        printf(ANSI_RED "  [!] %s" ANSI_RESET "\n", line + 4);
        fflush(stdout);
        return 1;
    }

    /* ---- ok generico (ignorato silenziosamente) ---- */
    if (strcmp(cmd, "OK") == 0)
        return 1;

    /* ---- messaggio sconosciuto ---- */
    printf(ANSI_GRAY "  [server] %s" ANSI_RESET "\n", line);
    fflush(stdout);
    return 1;
}

/* ============================================================
 *  Fase di autenticazione (interattiva, prima del gioco)
 * ============================================================ */
static int authenticate(void) {
    char choice[8], nick[MAX_NICK_LEN], pass[MAX_PASS_LEN];
    char buf[MAX_MSG_LEN], msg[MAX_MSG_LEN];

    printf(ANSI_BOLD "\n  ╔══════════════════════════╗\n" ANSI_RESET);
    printf(ANSI_BOLD   "  ║   LABYRINTH  GAME        ║\n" ANSI_RESET);
    printf(ANSI_BOLD   "  ╚══════════════════════════╝\n\n" ANSI_RESET);

    printf("  (1) Registrati   (2) Login  > ");
    fflush(stdout);
    if (!fgets(choice, sizeof(choice), stdin)) return 0;

    printf("  Nickname: ");
    fflush(stdout);
    if (!fgets(nick, sizeof(nick), stdin)) return 0;
    nick[strcspn(nick, "\n")] = '\0';

    printf("  Password: ");
    fflush(stdout);
    if (!fgets(pass, sizeof(pass), stdin)) return 0;
    pass[strcspn(pass, "\n")] = '\0';

    if (choice[0] == '1')
        snprintf(msg, sizeof(msg), "REGISTER %s %s", nick, pass);
    else
        snprintf(msg, sizeof(msg), "LOGIN %s %s", nick, pass);

    send_line(msg);

    if (read_line(g_sock, buf, sizeof(buf)) < 0) return 0;
    if (strncmp(buf, "OK", 2) != 0) {
        printf(ANSI_RED "  Errore: %s\n" ANSI_RESET, buf);
        return 0;
    }

    /* dopo REGISTER, effettua subito il LOGIN */
    if (choice[0] == '1') {
        snprintf(msg, sizeof(msg), "LOGIN %s %s", nick, pass);
        send_line(msg);
        if (read_line(g_sock, buf, sizeof(buf)) < 0) return 0;
        if (strncmp(buf, "OK", 2) != 0) {
            printf(ANSI_RED "  Login fallito: %s\n" ANSI_RESET, buf);
            return 0;
        }
    }

    printf(ANSI_GREEN ANSI_BOLD "\n  Benvenuto, %s!\n" ANSI_RESET, nick);
    printf("  " ANSI_BOLD "w" ANSI_RESET "=Nord  "
               ANSI_BOLD "s" ANSI_RESET "=Sud  "
               ANSI_BOLD "a" ANSI_RESET "=Ovest  "
               ANSI_BOLD "d" ANSI_RESET "=Est  "
               ANSI_BOLD "l" ANSI_RESET "=Lista  "
               ANSI_BOLD "q" ANSI_RESET "=Esci\n\n");
    fflush(stdout);
    return 1;
}

/* ============================================================
 *  Loop principale: select() su stdin e socket
 *
 *  Il select() è senza timeout: si sblocca solo quando arriva
 *  qualcosa da stdin (input utente) o dal socket (messaggio
 *  server, incluse mappe globali e GAME_END asincroni).
 * ============================================================ */
static void game_loop(void) {
    char line[MAX_MSG_LEN];
    int  running = 1;

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(g_sock, &fds);
        int maxfd = g_sock + 1;

        int ready = select(maxfd, &fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* ---- messaggio dal server (ha priorità sull'input) ---- */
        if (FD_ISSET(g_sock, &fds)) {
            if (read_line(g_sock, line, sizeof(line)) < 0) {
                printf(ANSI_RED "\n  Connessione chiusa dal server.\n" ANSI_RESET);
                break;
            }
            running = handle_server_msg(line);
            if (!running) break;
        }

        /* ---- input da tastiera ---- */
        if (running && FD_ISSET(STDIN_FILENO, &fds)) {
            char input[8];
            if (!fgets(input, sizeof(input), stdin)) break;

            /* se il giocatore è già uscito, ignora i comandi di movimento */
            if (g_exited && (input[0]=='w' || input[0]=='s' ||
                             input[0]=='a' || input[0]=='d')) {
                printf(ANSI_GRAY
                       "  Sei già uscito dal labirinto, attendi il risultato...\n"
                       ANSI_RESET);
                fflush(stdout);
                continue;
            }

            switch (input[0]) {
                case 'w': send_line("MOVE N"); break;
                case 's': send_line("MOVE S"); break;
                case 'a': send_line("MOVE W"); break;
                case 'd': send_line("MOVE E"); break;
                case 'l': send_line("LIST");   break;
                case 'q':
                    send_line("QUIT");
                    running = 0;
                    break;
                case '\n': break;   /* invio vuoto ignorato */
                default:
                    printf(ANSI_GRAY
                           "  Comando non riconosciuto."
                           " Usa w/s/a/d per muoverti, l=lista, q=esci\n"
                           ANSI_RESET);
                    fflush(stdout);
                    break;
            }
        }
    }
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s <host> <porta>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);

    setlocale(LC_ALL, "");   /* abilita UTF-8 nel terminale */

    struct hostent *he = gethostbyname(host);
    if (!he) {
        herror("gethostbyname");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(g_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        return 1;
    }

    printf(ANSI_CYAN "  Connesso a %s:%d\n" ANSI_RESET, host, port);

    if (!authenticate()) {
        close(g_sock);
        return 1;
    }

    game_loop();

    close(g_sock);
    return 0;
}
