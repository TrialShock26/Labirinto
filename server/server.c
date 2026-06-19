/*
 * server.c  –  Labyrinth Game Server  (versione multithread)
 *
 * Architettura:
 *   - pthread_create() per ogni client connesso (un thread per client)
 *   - Maze e PlayerTable sono variabili globali, accessibili da tutti i thread
 *   - pthread_mutex_t per mutua esclusione sulla struttura condivisa
 *   - Ogni thread usa select() con timeout per inviare
 *     periodicamente la mappa globale al proprio client senza bloccarsi
 *   - La logica di fine partita (timeout, vittoria) è verificata da
 *     ogni thread autonomamente tramite game_check_end()
 *   - auth_mutex protegge l'accesso concorrente a users.txt
 *
 * Uso: ./server <porta>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>

#include "../common/protocol.h"
#include "maze.h"
#include "player.h"
#include "logger.h"
#include "game.h"

/* ============================================================
 *  Stato globale condiviso tra i thread
 * ============================================================ */
static Maze        g_maze;
static PlayerTable g_pt;
static pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_auth_mutex = PTHREAD_MUTEX_INITIALIZER;

/* socket di ascolto (per cleanup nel signal handler) */
static int g_listen_fd = -1;

/* ============================================================
 *  Utility: invia una stringa al client
 * ============================================================ */
static void send_line(int fd, const char *msg) {
    char buf[MAX_MSG_LEN + 2];
    int  n = snprintf(buf, sizeof(buf), "%s\n", msg);
    write(fd, buf, n);
}

/* ============================================================
 *  Autenticazione: users.txt  →  nick:password (plain per ora)
 *
 *  REGISTER nick pass  → aggiunge riga se nick non esiste
 *  LOGIN    nick pass  → verifica
 * ============================================================ */
static int auth_register(const char *nick, const char *pass) {
    pthread_mutex_lock(&g_auth_mutex);

    FILE *f = fopen(USERS_FILE, "r");
    if (f) {
        char line[MAX_NICK_LEN + MAX_PASS_LEN + 4];
        while (fgets(line, sizeof(line), f)) {
            char n[MAX_NICK_LEN], p[MAX_PASS_LEN];
            if (sscanf(line, "%s %s", n, p) == 2 && strcmp(n, nick) == 0) {
                fclose(f);
                pthread_mutex_unlock(&g_auth_mutex);
                return -1;   /* nick già esistente */
            }
        }
        fclose(f);
    }

    f = fopen(USERS_FILE, "a");
    if (!f) {
        pthread_mutex_unlock(&g_auth_mutex);
        return -1;
    }
    fprintf(f, "%s %s\n", nick, pass);
    fclose(f);

    pthread_mutex_unlock(&g_auth_mutex);
    return 0;
}

static int auth_login(const char *nick, const char *pass) {
    pthread_mutex_lock(&g_auth_mutex);

    FILE *f = fopen(USERS_FILE, "r");
    if (!f) {
        pthread_mutex_unlock(&g_auth_mutex);
        return -1;
    }

    char line[MAX_NICK_LEN + MAX_PASS_LEN + 4];
    while (fgets(line, sizeof(line), f)) {
        char n[MAX_NICK_LEN], p[MAX_PASS_LEN];
        if (sscanf(line, "%s %s", n, p) == 2
                && strcmp(n, nick) == 0
                && strcmp(p, pass) == 0) {
            fclose(f);
            pthread_mutex_unlock(&g_auth_mutex);
            return 0;
        }
    }
    fclose(f);

    pthread_mutex_unlock(&g_auth_mutex);
    return -1;
}

/* ============================================================
 *  Legge una riga dal socket (terminata da '\n').
 *  Restituisce il numero di byte letti (senza '\n'), -1 su errore/EOF.
 * ============================================================ */
static int read_line(int fd, char *buf, int maxlen) {
    int i = 0;
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
 *  Costruisce e invia la mappa locale al client
 *  (chiamare tenendo g_mutex)
 * ============================================================ */
static void send_local_map(int fd, Player *p) {
    char data[((2*VIEW_RADIUS+1)*(2*VIEW_RADIUS+1)) + 4];
    int rows, cols;
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = g_maze.grid[r][c].cell;

    player_local_map(p, flat, data, &rows, &cols);

    char msg[MAX_MSG_LEN];
    snprintf(msg, sizeof(msg), "LOCAL %d %d %s", rows, cols, data);
    send_line(fd, msg);
}

/* ============================================================
 *  Costruisce e invia la mappa globale al client
 *  (chiamare tenendo g_mutex)
 * ============================================================ */
static void send_global_map(int fd, Player *p) {
    char data[MAZE_ROWS * MAZE_COLS + 4];
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = g_maze.grid[r][c].cell;

    player_global_map(p, flat, data);

    char msg[MAX_MSG_LEN + MAZE_ROWS * MAZE_COLS];
    snprintf(msg, sizeof(msg), "GLOBAL %d %d %s", MAZE_ROWS, MAZE_COLS, data);
    send_line(fd, msg);
}

/* ============================================================
 *  Argomenti passati al thread per ogni client
 * ============================================================ */
typedef struct {
    int                fd;
    struct sockaddr_in addr;
} ClientArgs;

/* ============================================================
 *  Ciclo principale del THREAD (un client)
 * ============================================================ */
static void *handle_client(void *arg) {
    ClientArgs args = *(ClientArgs *)arg;
    free(arg);

    int fd = args.fd;
    struct sockaddr_in addr = args.addr;

    char line[MAX_MSG_LEN];
    char nick[MAX_NICK_LEN] = {0};
    int  player_idx = -1;

    /* Detach: il thread gestisce la propria memoria,
     * il main non deve fare join */
    pthread_detach(pthread_self());

    /* --- fase autenticazione --- */
    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0) goto cleanup;

        char cmd[16], arg1[MAX_NICK_LEN], arg2[MAX_PASS_LEN];
        int  n = sscanf(line, "%15s %31s %63s", cmd, arg1, arg2);

        if (strcmp(cmd, "REGISTER") == 0 && n == 3) {
            if (auth_register(arg1, arg2) == 0) {
                send_line(fd, "OK");
                log_write("REGISTER nick=%s from %s", arg1, inet_ntoa(addr.sin_addr));
            } else {
                send_line(fd, "ERR nick already exists");
            }
        } else if (strcmp(cmd, "LOGIN") == 0 && n == 3) {
            if (auth_login(arg1, arg2) == 0) {
                strncpy(nick, arg1, MAX_NICK_LEN - 1);
                send_line(fd, "OK");
                log_write("LOGIN nick=%s from %s", nick, inet_ntoa(addr.sin_addr));
                break;
            } else {
                send_line(fd, "ERR invalid credentials");
            }
        } else {
            send_line(fd, "ERR must REGISTER or LOGIN first");
        }
    }

    /* --- posiziona il giocatore sulla mappa --- */
    pthread_mutex_lock(&g_mutex);
    int pr, pc;
    if (!maze_random_free_cell(&g_maze, &pr, &pc)) {
        pthread_mutex_unlock(&g_mutex);
        send_line(fd, "ERR maze full");
        goto cleanup;
    }
    player_idx = player_add(&g_pt, pthread_self(), nick, pr, pc);
    pthread_mutex_unlock(&g_mutex);

    if (player_idx < 0) {
        send_line(fd, "ERR server full");
        goto cleanup;
    }

    /* manda subito la mappa locale iniziale */
    pthread_mutex_lock(&g_mutex);
    send_local_map(fd, &g_pt.slots[player_idx]);
    pthread_mutex_unlock(&g_mutex);

    /* --- loop comandi con select() per timer mappa globale --- */
    time_t last_global = time(NULL);

    /* ---- helper: controlla fine partita e notifica il client ----
     * Restituisce 1 se la partita è finita (il chiamante deve uscire
     * dal loop), 0 altrimenti.
     * Deve essere chiamata FUORI dal mutex.                          */
    #define CHECK_GAME_END()                                              \
    do {                                                                  \
        char   _wn[MAX_NICK_LEN]; int _ws, _draw;                        \
        pthread_mutex_lock(&g_mutex);                                     \
        GameStatus _gs = game_check_end(&g_maze, &g_pt, _wn, &_ws, &_draw);\
        pthread_mutex_unlock(&g_mutex);                                   \
        if (_gs != GAME_RUNNING) {                                        \
            char _end[MAX_MSG_LEN];                                       \
            game_build_end_msg(_gs, _wn, _ws, _draw, _end);              \
            send_line(fd, _end);                                          \
            log_write("GAME_END nick=%s msg=%s", nick, _end);            \
            goto cleanup;                                                 \
        }                                                                 \
    } while(0)

    while (1) {
        /* controlla fine partita a ogni iterazione */
        CHECK_GAME_END();

        /* calcola quanto manca al prossimo invio della mappa globale */
        time_t now       = time(NULL);
        long   elapsed   = (long)(now - last_global);
        long   remaining = (long)GLOBAL_MAP_INTERVAL - elapsed;
        if (remaining <= 0) remaining = 0;

        struct timeval tv;
        tv.tv_sec  = remaining;
        tv.tv_usec = 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int ready = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* ---- timeout: invia mappa globale e ricontrolla fine partita ---- */
        if (ready == 0) {
            pthread_mutex_lock(&g_mutex);
            send_global_map(fd, &g_pt.slots[player_idx]);
            pthread_mutex_unlock(&g_mutex);
            last_global = time(NULL);
            log_write("GLOBAL_MAP sent to nick=%s", nick);
            continue;
        }

        /* ---- dati disponibili sul socket ---- */
        if (read_line(fd, line, sizeof(line)) < 0) break;

        char cmd[16];
        sscanf(line, "%15s", cmd);

        /* QUIT */
        if (strcmp(cmd, "QUIT") == 0) {
            send_line(fd, "OK");
            break;
        }

        /* LIST */
        if (strcmp(cmd, "LIST") == 0) {
            pthread_mutex_lock(&g_mutex);
            char msg[MAX_MSG_LEN] = "LIST";
            int  cnt = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (g_pt.slots[i].active) {
                    strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
                    strncat(msg, g_pt.slots[i].nick, sizeof(msg) - strlen(msg) - 1);
                    cnt++;
                }
            }
            char final_msg[MAX_MSG_LEN + 16];
            snprintf(final_msg, sizeof(final_msg), "LIST %d%s", cnt, msg + 4);
            send_line(fd, final_msg);
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        /* MOVE */
        if (strcmp(cmd, "MOVE") == 0) {
            char dir;
            if (sscanf(line, "MOVE %c", &dir) != 1) {
                send_line(fd, "ERR invalid direction");
                continue;
            }

            pthread_mutex_lock(&g_mutex);
            Player *p = &g_pt.slots[player_idx];
            int nr = p->row, nc = p->col;

            switch (dir) {
                case 'N': nr--; break;
                case 'S': nr++; break;
                case 'W': nc--; break;
                case 'E': nc++; break;
                default:
                    pthread_mutex_unlock(&g_mutex);
                    send_line(fd, "ERR unknown direction");
                    continue;
            }

            /* controlla limiti e muri */
            if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS ||
                    g_maze.grid[nr][nc].cell == CELL_WALL) {
                pthread_mutex_unlock(&g_mutex);
                send_line(fd, "ERR wall");
                continue;
            }

            /* aggiorna posizione */
            p->row = nr; p->col = nc;
            player_reveal(p, nr, nc);

            /* raccoglie oggetto? */
            if (maze_collect_object(&g_maze, nr, nc)) {
                p->score++;
                char msg[64];
                snprintf(msg, sizeof(msg), "COLLECT %d", p->score);
                send_line(fd, msg);
                log_write("COLLECT nick=%s score=%d pos=(%d,%d)", nick, p->score, nr, nc);
            }

            /* uscita? */
            if (g_maze.grid[nr][nc].cell == CELL_EXIT) {
                p->exited = 1;
                char msg[64];
                snprintf(msg, sizeof(msg), "EXIT_OK %d", p->score);
                send_line(fd, msg);
                log_write("EXIT nick=%s score=%d", nick, p->score);
                pthread_mutex_unlock(&g_mutex);
                /* controlla subito se la partita è finita dopo l'uscita */
                CHECK_GAME_END();
                /* se altri giocatori sono ancora in gioco, aspetta il timeout */
                break;
            }

            /* invia mappa locale aggiornata */
            send_local_map(fd, p);
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        send_line(fd, "ERR unknown command");
    }

    #undef CHECK_GAME_END

cleanup:
    if (player_idx >= 0) {
        pthread_mutex_lock(&g_mutex);
        player_remove(&g_pt, player_idx);
        pthread_mutex_unlock(&g_mutex);
        log_write("DISCONNECT nick=%s", nick);
    }
    close(fd);
    return NULL;
}

/* ============================================================
 *  Signal handler: termina il server in modo pulito
 * ============================================================ */
static void sigterm_handler(int sig) {
    (void)sig;
    log_write("server terminated by signal");
    log_close();
    if (g_listen_fd >= 0) close(g_listen_fd);
    /* I thread detached termineranno quando il processo uscirà */
    exit(0);
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "uso: %s <porta>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    srand((unsigned)time(NULL));

    log_init(LOG_FILE);

    /* ---- inizializza strutture condivise (in-process, niente shm) ---- */
    memset(&g_pt, 0, sizeof(PlayerTable));

    /* ---- genera labirinto ---- */
    maze_generate(&g_maze);
    log_write("maze generated (%dx%d)", MAZE_ROWS, MAZE_COLS);

    /* ---- socket di ascolto ---- */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(g_listen_fd, 10) < 0) { perror("listen"); return 1; }

    log_write("listening on port %d", port);

    /* ---- segnali ---- */
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGPIPE, SIG_IGN);   /* ignora scritture su socket chiusi */

    /* ---- loop principale: accetta connessioni ---- */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;   /* interrotto da segnale */
            perror("accept");
            continue;
        }

        log_write("new connection from %s:%d",
                  inet_ntoa(client_addr.sin_addr),
                  ntohs(client_addr.sin_port));

        /* Alloca gli argomenti per il thread (liberati dal thread stesso) */
        ClientArgs *cargs = malloc(sizeof(ClientArgs));
        if (!cargs) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        cargs->fd   = client_fd;
        cargs->addr = client_addr;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cargs) != 0) {
            perror("pthread_create");
            free(cargs);
            close(client_fd);
            continue;
        }
        /* il thread si è già detachato da solo */
    }

    /* mai raggiunto normalmente */
    return 0;
}
