#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <termios.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>

#include "../common/protocol.h"

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_GRAY       "\033[90m"
#define ANSI_BG_BLACK   "\033[40m"
#define ANSI_BG_WHITE   "\033[47m"
#define ANSI_BG_BLUE    "\033[44m"
#define ANSI_BG_GREEN   "\033[42m"
#define ANSI_BG_YELLOW  "\033[43m"
#define ANSI_BG_RED     "\033[41m"
#define ANSI_CLEAR      "\033[2J\033[H"
#define ANSI_CLEAR_LINE "\033[2K\r"

// Alternate screen buffer
#define ANSI_ALT_SCREEN_ON  "\033[?1049h"
#define ANSI_ALT_SCREEN_OFF "\033[?1049l"
#define ANSI_CURSOR_HIDE    "\033[?25l"
#define ANSI_CURSOR_SHOW    "\033[?25h"

#define SYM_PLAYER   "\xe1\x8c\xb8"
#define SYM_WALL     "\xf0\x91\x80\xa9"
#define SYM_OBJECT   "@"
#define SYM_UNKNOWN  "?"

#define MAP_START_ROW  5

int sock = -1;
char nick[MAX_NICK_LEN] = {0};

int score    = 0;
int exited   = 0;
int in_lobby = 1;
int g_ready  = 0;

pthread_t tid_timer;
int timer_exists   = 0;
int time_remaining = GAME_TIMEOUT;

char persist_msg[MAX_MSG_LEN]    = {0};
char game_end_msg[MAX_MSG_LEN*2] = {0};

// Per memorizzare la mappa locale per il posizionamento a sinistra
char local_data[MAX_MSG_LEN] = {0};
int  local_rows = 0, local_cols = 0;
char global_data[MAZE_ROWS*MAZE_COLS + 4] = {0};
int  global_rows = 0, global_cols = 0;

struct termios orig_termios;

void send_line(const char* msg) {
    char buf[MAX_MSG_LEN + 2];
    int ret = snprintf(buf, sizeof(buf), "%s\n", msg);
    if (ret < 0) { perror("parsing"); return; }

    size_t sent = 0, n = ret;
    while (sent < n) {
        ssize_t w = send(sock, buf+sent, (size_t)n-sent, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) perror("server disconnesso");
            else perror("send");
            return;
        }
        sent = sent + (size_t)w;
    }
}

int recv_line(int fd, char* buf) {
    int i = 0;
    char c;
    while (i < MAX_MSG_LEN - 1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) { if (errno == EINTR) continue; if (n != 0) perror("recv"); return -1; }
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

void* timer(void* arg) {
    (void)arg;
    while (1) {
        sleep(1);
        time_remaining--;
    }
    return NULL;
}


void enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Visualizzazione lobby pre-partita
void display_lobby(int ready, int total) {
    printf("\033[1;1H");   /* cursore in cima */
    printf("\033[J");       /* cancella tutto sotto */

    printf(ANSI_BOLD ANSI_CYAN
           "  ╔══════════════════════════════════╗\n"
           "  ║          LABYRINTH GAME          ║\n"
           "  ║             — LOBBY —            ║\n"
           "  ╚══════════════════════════════════╝\n"
           ANSI_RESET "\n");

    printf("  Utente: " ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET "\n\n", nick);

    printf("  Giocatori pronti: " ANSI_BOLD ANSI_GREEN "%d" ANSI_RESET
           " / " ANSI_BOLD "%d" ANSI_RESET "\n\n", ready, total);

    int bar_len = 20;
    int filled  = (total > 0) ? (ready * bar_len / total) : 0;
    printf("  [");
    for (int i = 0; i < bar_len; i++)
        printf(i < filled ? ANSI_GREEN "█" ANSI_RESET : ANSI_GRAY "░" ANSI_RESET);
    printf("]\n\n");

    if (ready == total && total >= 2) {
        printf(ANSI_YELLOW ANSI_BOLD
               "  Tutti pronti! La partita sta per iniziare...\n"
               ANSI_RESET);
    } else if (!g_ready) {
        printf(ANSI_GRAY
               "  In attesa dei giocatori...\n"
               "  Premi " ANSI_RESET ANSI_BOLD "[r]" ANSI_RESET ANSI_GRAY
               " quando sei pronto.\n" ANSI_RESET);
    } else {
        printf(ANSI_GRAY "  In attesa degli altri giocatori...\n" ANSI_RESET);
    }

    printf("\n  " ANSI_BOLD "[r]" ANSI_RESET "eady   "
               ANSI_BOLD "[q]" ANSI_RESET "uit\n");
    fflush(stdout);
}

// Ridisegna entrambe le mappe affiancate, sempre alla stessa posizione
void redraw_maps(void) {
    if (local_rows == 0 && global_rows == 0) return;

    int local_width = local_cols * 2 + 4;


    printf("\033[5;1H");
    printf("\033[J");

    /* intestazioni */
    printf(ANSI_BOLD "  LOCAL MAP" ANSI_RESET);
    int min = time_remaining / 60;
    int sec = time_remaining % 60;
    if (time_remaining <= 30)
        printf(ANSI_RED ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    else if (time_remaining <= 60)
        printf(ANSI_YELLOW ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    else
        printf(ANSI_GREEN ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    int gap = local_width + 5 - 20;   /* 20 = "  LOCAL MAP" + " ⏱ MM:SS" */
    if (gap < 2) gap = 2;
    for (int i = 0; i < gap; i++) printf(" ");
    printf(ANSI_BOLD "GLOBAL MAP" ANSI_RESET
           "   " ANSI_CYAN "%s" ANSI_RESET
           "   punteggio: " ANSI_YELLOW ANSI_BOLD "%d" ANSI_RESET "\n", nick, score);

    printf("  +");
    for (int c = 0; c < local_cols; c++) printf("--");
    printf("+");
    if (global_cols > 0) {
        printf("     +");
        for (int c = 0; c < global_cols; c++) printf("--");
        printf("+\n");
    } else {
        printf("\n");
    }

    int max_rows = global_rows > local_rows ? global_rows : local_rows;
    int local_done  = 0;
    int global_done = 0;

    for (int r = 0; r < max_rows; r++) {

        /* --- lato sinistro: mappa locale --- */
        if (!local_done && r == local_rows) {
            /* chiudi il bordo locale */
            printf("  +");
            for (int c = 0; c < local_cols; c++) printf("--");
            printf("+");
            local_done = 1;
            printf("     ");
        } else if (local_done) {
            /* spazio per allinearsi alla globale */
            for (int i = 0; i < local_width + 5; i++) printf(" ");
        } else {
            printf("  |");
            for (int c = 0; c < local_cols; c++) {
                char ch = local_data[r * local_cols + c];
                switch (ch) {
                    case CELL_WALL:    printf(" " SYM_WALL);                                                    break;
                    case CELL_FREE:    printf("  ");                                                            break;
                    case CELL_OBJECT:  printf(ANSI_YELLOW ANSI_BOLD " " SYM_OBJECT ANSI_RESET);                 break;
                    case CELL_EXIT:    printf(ANSI_BG_GREEN "  " ANSI_RESET);                                   break;
                    case CELL_UNKNOWN: printf(" " SYM_UNKNOWN);                                                 break;
                    case CELL_PLAYER:  printf(" " SYM_PLAYER);                                                  break;
                    default:           printf(" %c", ch);                                                       break;
                }
            }
            printf("|     ");
        }

        if(global_cols > 0){
            /* --- lato destro: mappa globale --- */
            if (!global_done && r == global_rows) {
                /* chiudi il bordo globale */
                printf("+");
                for (int c = 0; c < global_cols; c++) printf("--");
                printf("+");
                global_done = 1;
            } else if (global_done) {
                /* spazio vuoto dove la globale è già finita */
                int global_width = global_cols * 2 + 2;
                for (int i = 0; i < global_width; i++) printf(" ");
            } else {
                printf("|");
                for (int c = 0; c < global_cols; c++) {
                    char ch = global_data[r * global_cols + c];
                    switch (ch) {
                        case CELL_WALL:    printf(" " SYM_WALL);                                                    break;
                        case CELL_FREE:    printf("  ");                                                            break;
                        case CELL_OBJECT:  printf(ANSI_YELLOW ANSI_BOLD " " SYM_OBJECT ANSI_RESET);                 break;
                        case CELL_EXIT:    printf(ANSI_BG_GREEN "  " ANSI_RESET);                                   break;
                        case CELL_UNKNOWN: printf(" " SYM_UNKNOWN);                                                 break;
                        case CELL_PLAYER:  printf(" " SYM_PLAYER);                                                  break;
                        default:           printf(" %c", ch);                                                       break;
                    }
                }
                printf("|");
            }
        }

        printf("\n");
    }

    /* bordi inferiori se non già stampati nel loop */
    if (!local_done) {
        printf("  +");
        for (int c = 0; c < local_cols; c++) printf("--");
        printf("+     ");
    } else {
        for (int i = 0; i < local_width + 5; i++) printf(" ");
    }

    if (global_cols > 0 && !global_done) {
        printf("+");
        for (int c = 0; c < global_cols; c++) printf("--");
        printf("+\n");
    } else {
        printf("\n");
    }

    printf(ANSI_GRAY
           "  legenda: " SYM_WALL ": muro  "
           SYM_OBJECT ": oggetto  "
           ANSI_RESET ANSI_BG_GREEN "  " ANSI_RESET ANSI_GRAY ": uscita  "
           SYM_PLAYER ": tu  "
           SYM_UNKNOWN ": inesplorato"
           ANSI_RESET "\n\n");

    printf(ANSI_BOLD "  [w/a/s/d]" ANSI_RESET " per muoverti - "
           ANSI_BOLD "[l]" ANSI_RESET "ista - "
           ANSI_BOLD "[q]" ANSI_RESET "uit\n");

    if (persist_msg[0] != '\0') {
        printf("\033[32;1H");
        printf(ANSI_CLEAR_LINE);
        printf("%s", persist_msg);
    }

    fflush(stdout);
}

/* stampa un messaggio nelle righe 32-33 fisse, senza disturbare la mappa,
   e lo dimentica: sara' cancellato al prossimo redraw della mappa */
void show_msg(const char *fmt, ...) {
    printf("\033[32;1H");
    printf(ANSI_CLEAR_LINE);
    printf("\033[33;1H");
    printf(ANSI_CLEAR_LINE);
    printf("\033[32;1H");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* come show_msg, ma il messaggio resta visibile anche dopo i successivi
   redraw della mappa (es. arrivo di una nuova mappa GLOBAL) */
void show_msg_persist(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(persist_msg, sizeof(persist_msg), fmt, ap);
    va_end(ap);

    printf("\033[32;1H");
    printf(ANSI_CLEAR_LINE);
    printf("\033[33;1H");
    printf(ANSI_CLEAR_LINE);
    printf("\033[32;1H");
    printf("%s", persist_msg);
    fflush(stdout);
}

/* stampa/aggiorna solo la zona timer (riga 5, subito dopo "  LOCAL MAP"),
   senza toccare il resto dello schermo. */
void print_timer(void) {
    int min = time_remaining / 60;
    int sec = time_remaining % 60;
    printf("\033[5;12H");
    if (time_remaining <= 30)
        printf(ANSI_RED ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    else if (time_remaining <= 60)
        printf(ANSI_YELLOW ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    else
        printf(ANSI_GREEN ANSI_BOLD " ⏱ %2d:%02d" ANSI_RESET, min, sec);
    fflush(stdout);
}


int handle_server_msg(const char *line) {
    char cmd[MAX_MSG_LEN];
    sscanf(line, "%s", cmd);


    /* lobby */
    if (strcmp(cmd, "WAITING") == 0) {
        int ready = 0, total = 0;
        sscanf(line + 8, "%d/%d", &ready, &total);
        display_lobby(ready, total);
        return 1;
    }

    if (strcmp(cmd, "START") == 0) {
        pthread_create(&tid_timer, NULL, timer, NULL);
        timer_exists = 1;
        in_lobby = 0;
        printf(ANSI_CLEAR);
        printf(ANSI_GREEN ANSI_BOLD
             "\n  ╔═════════════════════════════════╗\n"
               "  ║        PARTITA INIZIATA!        ║\n"
               "  ╚═════════════════════════════════╝\n"
               ANSI_RESET "\n");
        fflush(stdout);
        /* legge subito LOCAL che il server manda immediatamente dopo START */
        char next[MAX_MSG_LEN];
        if (recv_line(sock, next) > 0)
            handle_server_msg(next);
        return 1;
    }


    /* messaggi di gioco ignorati se ancora in lobby */
    if (in_lobby) {
        if (strcmp(cmd, "ERR") == 0) {
        show_msg(ANSI_RED "  [!] %s" ANSI_RESET, line + 4);
        return 1;
        }
    }

    /* mappa locale */
    if (strcmp(cmd, "LOCAL") == 0) {
        if (sscanf(line, "LOCAL %d %d %s", &local_rows, &local_cols, local_data) == 3)
            redraw_maps();
        return 1;
    }

    /*mappa globale */

    if (strcmp(cmd, "GLOBAL") == 0) {
        if (sscanf(line, "GLOBAL %d %d %s", &global_rows, &global_cols, global_data) == 3)
            redraw_maps();
        return 1;
    }

    /* raccolta oggetto */

    if (strcmp(cmd, "COLLECT") == 0) {
        sscanf(line, "COLLECT %d", &score);
        show_msg(ANSI_YELLOW ANSI_BOLD "  ★ Oggetto raccolto! Punteggio: %d" ANSI_RESET, score);
        return 1;
    }

    /* uscita */

    if (strcmp(cmd, "EXIT_OK") == 0) {
        sscanf(line, "EXIT_OK %d", &score);
        show_msg_persist(ANSI_GREEN ANSI_BOLD "  ✓ Uscita! Punteggio: %d — attendi risultato..." ANSI_RESET, score);
        exited = 1;
        return 1;
    }

    /* fine partita */
    if (strcmp(cmd, "GAME_END") == 0) {
        const char *payload = line + 9;
        char body[MAX_MSG_LEN] = {0};
        if (strncmp(payload, "WIN", 3) == 0) {
            char winner[MAX_NICK_LEN]; int score;
            if (sscanf(payload, "WIN %s %d", winner, &score) == 2)
                snprintf(body, sizeof(body),
                         ANSI_YELLOW ANSI_BOLD "  🏆 Vincitore: %s  (punteggio: %d)\n"
                         ANSI_RESET, winner, score);
        } else if (strncmp(payload, "DRAW", 4) == 0) {
            int score; sscanf(payload, "DRAW %d", &score);
            snprintf(body, sizeof(body),
                     ANSI_CYAN ANSI_BOLD "  🤝 Pareggio! Punteggio massimo: %d\n"
                     ANSI_RESET, score);
        } else {
            snprintf(body, sizeof(body), "  %s\n", payload);
        }
        /* il messaggio viene solo memorizzato: se lo stampassimo ora,
           saremmo ancora nello schermo alternativo e verrebbe cancellato
           non appena il programma torna al terminale normale. Il main lo
           stampera' DOPO l'uscita dallo schermo alternativo, cosi' resta
           visibile in modo permanente. */
        snprintf(game_end_msg, sizeof(game_end_msg),
                 ANSI_BOLD "\n  ╔════════════════════════════╗\n"
                             "  ║        FINE PARTITA        ║\n"
                             "  ╚════════════════════════════╝\n\n" ANSI_RESET
                             "%s", body);
        return 0;
    }

    /* lista giocatori */
    if (strcmp(cmd, "LIST") == 0) {
        int n = 0;
        const char *ptr = line + 5;
        sscanf(ptr, "%d", &n);
        while (*ptr && *ptr != ' ') ptr++;
        if (*ptr) ptr++;
        show_msg(ANSI_CYAN ANSI_BOLD "  Giocatori connessi (%d):" ANSI_RESET " %s", n, ptr);
        return 1;
    }

    if (strcmp(cmd, "ERR") == 0) {
        show_msg(ANSI_RED "  [!] %s" ANSI_RESET, line + 4);
        return 1;
    }

    if (strcmp(cmd, "OK") == 0) return 1;

    show_msg(ANSI_GRAY "  [server] %s" ANSI_RESET "\n", line);
    fflush(stdout);
    return 1;
}


int authenticate(void) {
    char choice[8], input_nick[MAX_NICK_LEN], pass[MAX_PASS_LEN];
    char buf[MAX_MSG_LEN], msg[MAX_MSG_LEN];

    printf(ANSI_BOLD "\n  ╔══════════════════════════╗\n" ANSI_RESET);
    printf(ANSI_BOLD   "  ║      LABYRINTH GAME      ║\n" ANSI_RESET);
    printf(ANSI_BOLD   "  ╚══════════════════════════╝\n\n" ANSI_RESET);
    fflush(stdout);

    while (1) {
        printf("  (1) Registrati   (2) Login   (0) Esci  > ");
        if (!fgets(choice, sizeof(choice), stdin)) return 0;
        choice[strcspn(choice, "\n")] = '\0';

        if (strcmp(choice, "0") == 0) return 0;
        if (strcmp(choice, "1") != 0 && strcmp(choice, "2") != 0) {
            printf(ANSI_RED "  Opzione non valida, inserisci 1, 2 o 0\n" ANSI_RESET);
            fflush(stdout);
            continue;
        }

        printf("\n  Nickname: "); fflush(stdout);
        if (!fgets(input_nick, sizeof(input_nick), stdin)) return 0;
        input_nick[strcspn(input_nick, "\n")] = '\0';

        printf("  Password: "); fflush(stdout);
        if (!fgets(pass, sizeof(pass), stdin)) return 0;
        pass[strcspn(pass, "\n")] = '\0';

        if (choice[0] == '1')
            snprintf(msg, sizeof(msg), "REGISTER %s %s", input_nick, pass);
        else
            snprintf(msg, sizeof(msg), "LOGIN %s %s", input_nick, pass);

        send_line(msg);
        if (recv_line(sock, buf) < 0) return 0;
        if (strncmp(buf, "OK", 2) != 0) {
            printf(ANSI_RED "  Errore: %s\n" ANSI_RESET, buf);
            continue;
        }

        if (choice[0] == '1') {
            snprintf(msg, sizeof(msg), "LOGIN %s %s", input_nick, pass);
            send_line(msg);
            if (recv_line(sock, buf) < 0) return 0;
            if (strncmp(buf, "OK", 2) != 0) {
                printf(ANSI_RED "  Login fallito: %s\n" ANSI_RESET, buf);
                continue;
            }
        }
        break;
    }

    printf(ANSI_GREEN ANSI_BOLD "\n  Benvenuto, %s!\n" ANSI_RESET, input_nick);
    snprintf(nick, sizeof(nick), "%s", input_nick);
    fflush(stdout);
    sleep(1);
    return 1;
}

void game_loop(void) {
    char line[MAX_MSG_LEN];
    int running = 1;

    display_lobby(0, 0);

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(sock, &fds);
        int maxfd = sock + 1;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(maxfd, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        if (ready == 0) {
            if (!in_lobby && time_remaining > 0) {
                print_timer();
            }
            continue;
        }

        if (FD_ISSET(sock, &fds)) {
            if (recv_line(sock, line) < 0) {
                disable_raw_mode();
                printf(ANSI_ALT_SCREEN_OFF ANSI_CURSOR_SHOW);
                printf(ANSI_RED "\n  Connessione chiusa dal server.\n\n" ANSI_RESET);
                close(sock);
                exit(-1);
            }
            running = handle_server_msg(line);
            if (!running) break;
        }

        if (running && FD_ISSET(STDIN_FILENO, &fds)) {
            char ch;
            int n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) break;

            /* ignora sequenze di escape (frecce, ecc.) */
            if (ch == '\x1b') {
                char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) > 0) {
                    if (read(STDIN_FILENO, &seq[1], 1) > 0) {
                        /* sequenza escape a 3 byte, ignora */
                    }
                }
                continue;
            }

            /* ignora Invio e altri caratteri di controllo */
            if (ch == '\n' || ch == '\r') continue;

            if (in_lobby) {
                switch (ch) {
                    case 'r':
                        if (!g_ready) {
                            send_line("READY");
                            g_ready = 1;
                            show_msg(ANSI_GREEN "  Sei pronto! In attesa degli altri..." ANSI_RESET);
                        } else
                            show_msg(ANSI_GRAY "  Hai già dichiarato di essere pronto." ANSI_RESET);
                        break;
                    case 'q':
                        send_line("QUIT"); running = 0; recv_line(sock, line); break;
                    default:
                        show_msg(ANSI_GRAY "  Sei in lobby. Premi [r]eady o [q]uit." ANSI_RESET);
                        break;
                }
            } else {
                if (exited && (ch == 'w' || ch == 's' ||
                                 ch == 'a' || ch == 'd')) {
                    show_msg(ANSI_GRAY "  Sei già uscito, attendi il risultato..." ANSI_RESET);
                    continue;
                }
                switch (ch) {
                    case 'w': send_line("MOVE N"); break;
                    case 'a': send_line("MOVE W"); break;
                    case 's': send_line("MOVE S"); break;
                    case 'd': send_line("MOVE E"); break;
                    case 'l': send_line("LIST");   break;
                    case 'q': send_line("QUIT"); running = 0; recv_line(sock, line); break;
                    default:
                        show_msg(ANSI_GRAY "  Comando non riconosciuto. Usa w/a/s/d, [l]ista, [q]uit" ANSI_RESET);
                        break;
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s <host> <porta>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int         port = atoi(argv[2]);

    setlocale(LC_ALL, "");

    struct hostent *he = gethostbyname(host);
    if (!he) { perror("gethostbyname"); return 1; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); return 1;
    }

    printf(ANSI_CYAN "\n  Connesso a %s:%d\n" ANSI_RESET, host, port);

    signal(SIGINT,  SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    if (!authenticate()) {
        printf(ANSI_BOLD ANSI_MAGENTA "\n  Arrivederci!\n\n" ANSI_RESET);
        close(sock);
        fflush(stdout);
        return 1;
    }

    while (1) {
        printf(ANSI_ALT_SCREEN_ON ANSI_CURSOR_HIDE);
        fflush(stdout);

        enable_raw_mode();

        game_loop();

        if (timer_exists) {
            pthread_cancel(tid_timer);
            pthread_join(tid_timer, NULL);
        }

        disable_raw_mode();
        printf(ANSI_ALT_SCREEN_OFF ANSI_CURSOR_SHOW);
        fflush(stdout);

        printf("%s\n", game_end_msg);
        fflush(stdout);

        char choice[8];
        while (1) {
            printf("  Vuoi rigiocare?  (1) Sì  (2) No  > ");
            if (!fgets(choice, sizeof(choice), stdin)) return 0;
            choice[strcspn(choice, "\n")] = '\0';

            if (strcmp(choice, "1") != 0 && strcmp(choice, "2") != 0) {
                printf(ANSI_RED "  Opzione non valida, inserisci 1 o 2\n" ANSI_RESET);
                fflush(stdout);
                continue;
            }
            break;
        }
        if (choice[0] == '2') {
            printf(ANSI_BOLD ANSI_MAGENTA "\n  Arrivederci!\n\n" ANSI_RESET);
            close(sock);
            return 0;
        }

        score    = 0;
        exited   = 0;
        in_lobby = 1;
        g_ready  = 0;
        timer_exists = 0;

        time_remaining = GAME_TIMEOUT;
        memset(persist_msg, 0, sizeof(persist_msg));
        memset(game_end_msg, 0, sizeof(game_end_msg));

        memset(local_data, 0, sizeof(local_data));
        local_rows = 0, local_cols = 0;
        memset(global_data, 0, sizeof(global_data));
        global_rows = 0, global_cols = 0;
        send_line("AGAIN");
    }

    return 0;
}