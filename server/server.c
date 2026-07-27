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

// test Stato globale condiviso tra i thread

static Maze        g_maze;
static PlayerTable g_pt;
static pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_auth_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_notify_pipe[2] = {-1, -1};
static int g_ready_count    = 0;
static int g_game_started   = 0;
static int g_max_players_ever = 0;
static volatile int g_game_over = 0;

/* fd di ogni client in lobby, per broadcast */
static int g_client_fds[MAX_PLAYERS];

static int g_listen_fd = -1;

// Utility
static void send_line(int fd, const char *msg) {
    char buf[MAX_MSG_LEN + 2];
    int  n = snprintf(buf, sizeof(buf), "%s\n", msg);
    write(fd, buf, n);
}

/* Invia WAITING X/Y a tutti i client in lobby.
   Chiamare tenendo g_mutex. */
static void broadcast_lobby_update(void) {
    char wmsg[64];
    snprintf(wmsg, sizeof(wmsg), "WAITING %d/%d",
             g_ready_count, g_pt.count);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g_client_fds[i] >= 0)
            send_line(g_client_fds[i], wmsg);
}

/* 
  Autenticazione
 */
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
                return -1;
            }
        }
        fclose(f);
    }
    f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&g_auth_mutex); return -1; }
    fprintf(f, "%s %s\n", nick, pass);
    fclose(f);
    pthread_mutex_unlock(&g_auth_mutex);
    return 0;
}

static int auth_login(const char *nick, const char *pass) {
    pthread_mutex_lock(&g_auth_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&g_auth_mutex); return -1; }
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

static int read_line(int fd, char *buf, int maxlen) {
    int i = 0; char c;
    while (i < maxlen - 1) {
        int n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* 
  Mappe (chiamare tenendo g_mutex)
  */
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

typedef struct {
    int fd;
    struct sockaddr_in addr;
} ClientArgs;

/* 
  Thread per ogni client
  */
static void *handle_client(void *arg) {
    ClientArgs args = *(ClientArgs *)arg;
    free(arg);

    int fd = args.fd;
    struct sockaddr_in addr = args.addr;

    char line[MAX_MSG_LEN];
    char nick[MAX_NICK_LEN] = {0};
    int  player_idx  = -1;
    int  player_ready = 0;

    pthread_detach(pthread_self());

    /* autenticazione */
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

    /* blocca se partita già iniziata */
    pthread_mutex_lock(&g_mutex);
    if (g_game_started) {
        pthread_mutex_unlock(&g_mutex);
        send_line(fd, "ERR game already in progress");
        goto cleanup;
    }
    pthread_mutex_unlock(&g_mutex);

    /* posiziona il giocatore */
    pthread_mutex_lock(&g_mutex);
    int pr, pc;
    if (!maze_random_free_cell(&g_maze, &pr, &pc)) {
        pthread_mutex_unlock(&g_mutex);
        send_line(fd, "ERR maze full");
        goto cleanup;
    }
    player_idx = player_add(&g_pt, pthread_self(), nick, pr, pc);
    if (player_idx >= 0) {
        g_client_fds[player_idx] = fd;
        if (g_pt.count > g_max_players_ever)
            g_max_players_ever = g_pt.count;
        /* notifica tutti della nuova connessione in lobby */
        broadcast_lobby_update();
    }
    pthread_mutex_unlock(&g_mutex);

    if (player_idx < 0) {
        send_line(fd, "ERR server full");
        goto cleanup;
    }

    /* fase lobby: attendi READY */
    /* invia stato iniziale lobby */
    pthread_mutex_lock(&g_mutex);
    char init_wmsg[64];
    snprintf(init_wmsg, sizeof(init_wmsg), "WAITING %d/%d",
             g_ready_count, g_pt.count);
    pthread_mutex_unlock(&g_mutex);
    send_line(fd, init_wmsg);

    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0) goto cleanup;
        char cmd[16];
        sscanf(line, "%15s", cmd);
        if (strcmp(cmd, "READY") == 0) {
            pthread_mutex_lock(&g_mutex);
            g_ready_count++;
            player_ready = 1;
            /* broadcast aggiornamento a tutti */
            broadcast_lobby_update();
            /* se tutti pronti, avvia la partita */
            if (g_ready_count >= g_pt.count && g_pt.count >= 2)
                g_game_started = 1;
            pthread_mutex_unlock(&g_mutex);
            break;
        }
        if (strcmp(cmd, "QUIT") == 0) { send_line(fd, "OK"); goto cleanup; }
        send_line(fd, "ERR send READY to start");
    }

    /* attendi che g_game_started == 1 con select() */
    while (1) {
        pthread_mutex_lock(&g_mutex);
        int started = g_game_started;
        pthread_mutex_unlock(&g_mutex);
        if (started) break;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (r > 0 && FD_ISSET(fd, &rfds)) {
            if (read_line(fd, line, sizeof(line)) < 0) goto cleanup;
            char cmd2[16];
            sscanf(line, "%15s", cmd2);
            if (strcmp(cmd2, "QUIT") == 0) { send_line(fd, "OK"); goto cleanup; }
        }
    }

    /* partita iniziata: manda START poi subito LOCAL */
    send_line(fd, "START");

    pthread_mutex_lock(&g_mutex);
    /* rimuovi fd dalla lobby broadcast */
    g_client_fds[player_idx] = -1;
    send_local_map(fd, &g_pt.slots[player_idx]);
    pthread_mutex_unlock(&g_mutex);

    /* loop di gioco */
    time_t last_global = time(NULL);

    #define CHECK_GAME_END()                                                 \
    do {                                                                     \
        char   _wn[MAX_NICK_LEN]; int _ws, _draw;                           \
        pthread_mutex_lock(&g_mutex);                                        \
        GameStatus _gs = game_check_end(&g_maze, &g_pt, _wn, &_ws, &_draw, \
                                        g_max_players_ever);                 \
        if (_gs != GAME_RUNNING) g_game_over = 1;                           \
        pthread_mutex_unlock(&g_mutex);                                      \
        if (_gs != GAME_RUNNING) {                                           \
            char _end[MAX_MSG_LEN];                                          \
            game_build_end_msg(_gs, _wn, _ws, _draw, _end);                 \
            send_line(fd, _end);                                             \
            log_write("GAME_END nick=%s msg=%s", nick, _end);               \
            goto cleanup;                                                    \
        }                                                                    \
    } while(0)

    while (1) {
        if (g_game_over) {
            char _wn[MAX_NICK_LEN]; int _ws, _draw;
            pthread_mutex_lock(&g_mutex);
            GameStatus _gs = game_check_end(&g_maze, &g_pt, _wn, &_ws, &_draw,
                                            g_max_players_ever);
            pthread_mutex_unlock(&g_mutex);
            char _end[MAX_MSG_LEN];
            game_build_end_msg(_gs, _wn, _ws, _draw, _end);
            send_line(fd, _end);
            goto cleanup;
        }

        CHECK_GAME_END();

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
        FD_SET(g_notify_pipe[0], &rfds);
        int maxfd = (fd > g_notify_pipe[0]) ? fd : g_notify_pipe[0];

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; break; }

        if (ready == 0) {
            pthread_mutex_lock(&g_mutex);
            send_global_map(fd, &g_pt.slots[player_idx]);
            pthread_mutex_unlock(&g_mutex);
            last_global = time(NULL);
            CHECK_GAME_END();
            continue;
        }

        if (FD_ISSET(g_notify_pipe[0], &rfds)) {
            char _buf[64];
            int _n = read(g_notify_pipe[0], _buf, sizeof(_buf));
            for (int _i = 0; _i < _n; _i++)
                if (_buf[_i] == 0x01)
                    CHECK_GAME_END();
        }

        if (FD_ISSET(fd, &rfds)) {
            if (read_line(fd, line, sizeof(line)) <= 0) goto cleanup;

            char cmd[16];
            sscanf(line, "%15s", cmd);

            if (strcmp(cmd, "QUIT") == 0) { send_line(fd, "OK"); goto cleanup; }

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
                if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS ||
                        g_maze.grid[nr][nc].cell == CELL_WALL) {
                    pthread_mutex_unlock(&g_mutex);
                    send_line(fd, "ERR wall");
                    continue;
                }
                p->row = nr; p->col = nc;
                player_reveal(p, nr, nc);
                if (maze_collect_object(&g_maze, nr, nc)) {
                    p->score++;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "COLLECT %d", p->score);
                    send_line(fd, msg);
                    log_write("COLLECT nick=%s score=%d pos=(%d,%d)", nick, p->score, nr, nc);
                }
                if (g_maze.grid[nr][nc].cell == CELL_EXIT) {
                    p->exited = 1;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "EXIT_OK %d", p->score);
                    send_line(fd, msg);
                    log_write("EXIT nick=%s score=%d", nick, p->score);
                    pthread_mutex_unlock(&g_mutex);
                    CHECK_GAME_END();
                    break;
                }
                send_local_map(fd, p);
                pthread_mutex_unlock(&g_mutex);
                continue;
            }

            send_line(fd, "ERR unknown command");
        }
    }

    #undef CHECK_GAME_END

cleanup:
    if (player_idx >= 0) {
        pthread_mutex_lock(&g_mutex);
        if (player_ready) g_ready_count--;
        g_client_fds[player_idx] = -1;
        player_remove(&g_pt, player_idx);
        broadcast_lobby_update();
        if (g_pt.count == 0) {
            g_ready_count  = 0;
            g_game_started = 0;
            g_game_over    = 0;
        }
        pthread_mutex_unlock(&g_mutex);
        write(g_notify_pipe[1], "\x01", 1);
        log_write("DISCONNECT nick=%s", nick);
    }
    close(fd);
    return NULL;
}

static void sigterm_handler(int sig) {
    (void)sig;
    log_write("server terminated by signal");
    log_close();
    if (g_listen_fd >= 0) close(g_listen_fd);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "uso: %s <porta>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    srand((unsigned)time(NULL));

    log_init(LOG_FILE);

    memset(&g_pt, 0, sizeof(PlayerTable));
    for (int i = 0; i < MAX_PLAYERS; i++)
        g_client_fds[i] = -1;

    maze_generate(&g_maze);
    log_write("maze generated (%dx%d)", MAZE_ROWS, MAZE_COLS);

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

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);
    signal(SIGPIPE, SIG_IGN);

    if (pipe(g_notify_pipe) < 0) { perror("pipe"); return 1; }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        log_write("new connection from %s:%d",
                  inet_ntoa(client_addr.sin_addr),
                  ntohs(client_addr.sin_port));
        ClientArgs *cargs = malloc(sizeof(ClientArgs));
        if (!cargs) { perror("malloc"); close(client_fd); continue; }
        cargs->fd   = client_fd;
        cargs->addr = client_addr;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cargs) != 0) {
            perror("pthread_create");
            free(cargs);
            close(client_fd);
            continue;
        }
    }
    return 0;
}