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

// Stato globale condiviso tra i thread

Maze maze;
PlayerTable pt;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t auth_mutex = PTHREAD_MUTEX_INITIALIZER;
int notify_pipe[2] = {-1, -1};
int ready_count = 0;
int game_started = 0;
int max_players_ever = 0;
volatile int game_over = 0; //TODO se funziona togliendo volatile
int client_fds[MAX_PLAYERS];
int listen_sd = -1;

typedef struct {
    int fd;
    struct sockaddr_in addr;
} ClientArgs;

void send_line(int fd, const char *msg) {
    char buf[MAX_MSG_LEN + 2];
    int  n = snprintf(buf, sizeof(buf), "%s\n", msg);
    write(fd, buf, n);
}

void broadcast_lobby_update(void) {
    char wmsg[64];
    snprintf(wmsg, sizeof(wmsg), "WAITING %d/%d",
             ready_count, pt.count);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] >= 0)
            send_line(client_fds[i], wmsg);
}

int auth_register(const char *nick, const char *pass) {
    pthread_mutex_lock(&auth_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (f) {
        char line[MAX_NICK_LEN + MAX_PASS_LEN + 4];
        while (fgets(line, sizeof(line), f)) {
            char n[MAX_NICK_LEN], p[MAX_PASS_LEN];
            if (sscanf(line, "%s %s", n, p) == 2 && strcmp(n, nick) == 0) {
                fclose(f);
                pthread_mutex_unlock(&auth_mutex);
                return -1;
            }
        }
        fclose(f);
    }
    f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&auth_mutex); return -1; }
    fprintf(f, "%s %s\n", nick, pass);
    fclose(f);
    pthread_mutex_unlock(&auth_mutex);
    return 0;
}

int auth_login(const char *nick, const char *pass) {
    pthread_mutex_lock(&auth_mutex);
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&auth_mutex); return -1; }
    char line[MAX_NICK_LEN + MAX_PASS_LEN + 4];
    while (fgets(line, sizeof(line), f)) {
        char n[MAX_NICK_LEN], p[MAX_PASS_LEN];
        if (sscanf(line, "%s %s", n, p) == 2
                && strcmp(n, nick) == 0
                && strcmp(p, pass) == 0) {
            fclose(f);
            pthread_mutex_unlock(&auth_mutex);
            return 0;
        }
    }
    fclose(f);
    pthread_mutex_unlock(&auth_mutex);
    return -1;
}

int read_line(int fd, char *buf, int maxlen) {
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

void send_local_map(int fd, Player *p) {
    char data[((2*VIEW_RADIUS+1)*(2*VIEW_RADIUS+1)) + 4];
    int rows, cols;
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = maze.grid[r][c].cell;
    player_local_map(p, flat, data, &rows, &cols);
    char msg[MAX_MSG_LEN];
    snprintf(msg, sizeof(msg), "LOCAL %d %d %s", rows, cols, data);
    send_line(fd, msg);
}

void send_global_map(int fd, Player *p) {
    char data[MAZE_ROWS * MAZE_COLS + 4];
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = maze.grid[r][c].cell;
    player_global_map(p, flat, data);
    char msg[MAX_MSG_LEN + MAZE_ROWS * MAZE_COLS];
    snprintf(msg, sizeof(msg), "GLOBAL %d %d %s", MAZE_ROWS, MAZE_COLS, data);
    send_line(fd, msg);
}

/*void *handle_client(void *arg) {
    ClientArgs args = *(ClientArgs*)arg;
    free(arg);

    int fd = args.fd;
    struct sockaddr_in addr = args.addr;

    char line[MAX_MSG_LEN];
    char nick[MAX_NICK_LEN] = {0};
    int  player_idx  = -1;
    int  player_ready = 0;

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

    pthread_mutex_lock(&mutex);
    if (game_started) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR game already in progress");
        goto cleanup;
    }
    pthread_mutex_unlock(&mutex);

    pthread_mutex_lock(&mutex);
    int pr, pc;
    if (!maze_random_free_cell(&maze, &pr, &pc)) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR maze full");
        goto cleanup;
    }
    player_idx = player_add(&pt, pthread_self(), nick, pr, pc);
    if (player_idx >= 0) {
        client_fds[player_idx] = fd;
        if (pt.count > max_players_ever)
            max_players_ever = pt.count;
        broadcast_lobby_update();
    }
    pthread_mutex_unlock(&mutex);

    if (player_idx < 0) {
        send_line(fd, "ERR server full");
        goto cleanup;
    }

    pthread_mutex_lock(&mutex);
    char init_wmsg[64];
    snprintf(init_wmsg, sizeof(init_wmsg), "WAITING %d/%d",
             ready_count, pt.count);
    pthread_mutex_unlock(&mutex);
    send_line(fd, init_wmsg);

    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0) goto cleanup;
        char cmd[16];
        sscanf(line, "%15s", cmd);
        if (strcmp(cmd, "READY") == 0) {
            pthread_mutex_lock(&mutex);
            ready_count++;
            player_ready = 1;
            broadcast_lobby_update();
            if (ready_count >= pt.count && pt.count >= 2)
                game_started = 1;
            pthread_mutex_unlock(&mutex);
            break;
        }
        if (strcmp(cmd, "QUIT") == 0) { send_line(fd, "OK"); goto cleanup; }
        send_line(fd, "ERR send READY to start");
    }

    while (1) {
        pthread_mutex_lock(&mutex);
        int started = game_started;
        pthread_mutex_unlock(&mutex);
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

    send_line(fd, "START");

    pthread_mutex_lock(&mutex);
    client_fds[player_idx] = -1;
    send_local_map(fd, &pt.slots[player_idx]);
    pthread_mutex_unlock(&mutex);

    time_t last_global = time(NULL);
    time_t last_time   = time(NULL);

    #define CHECK_GAME_END()                                                 \
    do {                                                                     \
        char   _wn[MAX_NICK_LEN]; int _ws, _draw;                           \
        pthread_mutex_lock(&g_mutex);                                        \
        GameStatus _gs = game_check_end(&g_maze, &g_pt, _wn, &_ws, &_draw);                 \
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
        if (game_over) {
            char _wn[MAX_NICK_LEN]; int _ws, _draw;
            pthread_mutex_lock(&g_mutex);
            GameStatus _gs = game_check_end(&g_maze, &g_pt, _wn, &_ws, &_draw);
            pthread_mutex_unlock(&g_mutex);
            char _end[MAX_MSG_LEN];
            game_build_end_msg(_gs, _wn, _ws, _draw, _end);
            send_line(fd, _end);
            goto cleanup;
        }

        CHECK_GAME_END();

        time_t now = time(NULL);

        /* invia TIME ogni secondo */
        if (now > last_time) {
            last_time = now;
            long game_remaining = (long)GAME_TIMEOUT
                                - (long)(now - g_maze.start_time);
            if (game_remaining < 0) game_remaining = 0;
            char time_msg[32];
            snprintf(time_msg, sizeof(time_msg), "TIME %ld", game_remaining);
            send_line(fd, time_msg);
        }

        /* invia GLOBAL map ogni GLOBAL_MAP_INTERVAL secondi */
        if (now - last_global >= GLOBAL_MAP_INTERVAL) {
            pthread_mutex_lock(&g_mutex);
            send_global_map(fd, &g_pt.slots[player_idx]);
            pthread_mutex_unlock(&g_mutex);
            last_global = now;
            CHECK_GAME_END();
            continue;
        }

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;


        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(notify_pipe[0], &rfds);
        int maxfd = (fd > notify_pipe[0]) ? fd : notify_pipe[0];

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; break; }

        if (ready == 0) {
            pthread_mutex_lock(&mutex);
            send_global_map(fd, &pt.slots[player_idx]);
            pthread_mutex_unlock(&mutex);
            last_global = time(NULL);
            CHECK_GAME_END();
            continue;
        }

        if (FD_ISSET(notify_pipe[0], &rfds)) {
            char _buf[64];
            int _n = read(notify_pipe[0], _buf, sizeof(_buf));
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
                pthread_mutex_lock(&mutex);
                char msg[MAX_MSG_LEN] = "LIST";
                int  cnt = 0;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (pt.slots[i].active) {
                        strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
                        strncat(msg, pt.slots[i].nick, sizeof(msg) - strlen(msg) - 1);
                        cnt++;
                    }
                }
                char final_msg[MAX_MSG_LEN + 16];
                snprintf(final_msg, sizeof(final_msg), "LIST %d%s", cnt, msg + 4);
                send_line(fd, final_msg);
                pthread_mutex_unlock(&mutex);
                continue;
            }

            if (strcmp(cmd, "MOVE") == 0) {
                char dir;
                if (sscanf(line, "MOVE %c", &dir) != 1) {
                    send_line(fd, "ERR invalid direction");
                    continue;
                }
                pthread_mutex_lock(&mutex);
                Player *p = &pt.slots[player_idx];
                int nr = p->row, nc = p->col;
                switch (dir) {
                    case 'N': nr--; break;
                    case 'S': nr++; break;
                    case 'W': nc--; break;
                    case 'E': nc++; break;
                    default:
                        pthread_mutex_unlock(&mutex);
                        send_line(fd, "ERR unknown direction");
                        continue;
                }
                if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS ||
                        maze.grid[nr][nc].cell == CELL_WALL) {
                    pthread_mutex_unlock(&mutex);
                    send_line(fd, "ERR wall");
                    continue;
                }
                p->row = nr; p->col = nc;
                player_reveal(p, nr, nc);
                if (maze_collect_object(&maze, nr, nc)) {
                    p->score++;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "COLLECT %d", p->score);
                    send_line(fd, msg);
                    log_write("COLLECT nick=%s score=%d pos=(%d,%d)", nick, p->score, nr, nc);
                }
                if (maze.grid[nr][nc].cell == CELL_EXIT) {
                    p->exited = 1;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "EXIT_OK %d", p->score);
                    send_line(fd, msg);
                    log_write("EXIT nick=%s score=%d", nick, p->score);
                    pthread_mutex_unlock(&mutex);
                    CHECK_GAME_END();
                    break;
                }
                send_local_map(fd, p);
                pthread_mutex_unlock(&mutex);
                continue;
            }

            send_line(fd, "ERR unknown command");
        }
    }

    #undef CHECK_GAME_END

cleanup:
    if (player_idx >= 0) {
        pthread_mutex_lock(&mutex);
        if (player_ready) ready_count--;
        client_fds[player_idx] = -1;
        player_remove(&pt, player_idx);
        broadcast_lobby_update();
        if (pt.count == 0) {
            ready_count  = 0;
            game_started = 0;
            game_over    = 0;
        }
        pthread_mutex_unlock(&mutex);
        write(notify_pipe[1], "\x01", 1);
        log_write("DISCONNECT nick=%s", nick);
    }
    close(fd);
    return NULL;
}*/

//NEW
int  phase_auth(int fd, struct sockaddr_in addr,
                        char *nick, size_t nick_size);
int  phase_join(int fd, const char *nick);
int  phase_lobby(int fd, int *player_ready);
int  phase_wait_start(int fd);
void phase_play(int fd, int player_idx, const char *nick);

int  handle_play_command(int fd, int player_idx, const char *nick);
int  handle_move_command(int fd, const char *line,
                                 int player_idx, const char *nick);
void handle_list_command(int fd);

int  check_and_handle_game_end(int fd, const char *nick);

void client_cleanup(int fd, int player_idx,
                            int player_ready, const char *nick);

void *handle_client(void *arg) {
    ClientArgs args = *(ClientArgs *)arg;
    free(arg);

    int fd = args.fd;
    struct sockaddr_in addr = args.addr;

    char nick[MAX_NICK_LEN] = {0};
    int  player_idx  = -1;
    int  player_ready = 0;

    if (phase_auth(fd, addr, nick, sizeof(nick))) {
        player_idx = phase_join(fd, nick);

        if (player_idx >= 0) {
            if (phase_lobby(fd, &player_ready)) {
                if (phase_wait_start(fd)) {
                    phase_play(fd, player_idx, nick);
                }
            }
        }
    }

    client_cleanup(fd, player_idx, player_ready, nick);
    return NULL;
}

int phase_auth(int fd, struct sockaddr_in addr,
                       char *nick, size_t nick_size) {
    char line[MAX_MSG_LEN];

    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0) return 0;

        char cmd[16], arg1[MAX_NICK_LEN], arg2[MAX_PASS_LEN];
        int n = sscanf(line, "%15s %31s %63s", cmd, arg1, arg2);

        if (strcmp(cmd, "REGISTER") == 0 && n == 3) {
            if (auth_register(arg1, arg2) == 0) {
                send_line(fd, "OK");
                log_write("REGISTER nick=%s from %s", arg1,
                           inet_ntoa(addr.sin_addr));
            } else {
                send_line(fd, "ERR nick already exists");
            }
        } else if (strcmp(cmd, "LOGIN") == 0 && n == 3) {
            if (auth_login(arg1, arg2) == 0) {
                strncpy(nick, arg1, nick_size - 1);
                send_line(fd, "OK");
                log_write("LOGIN nick=%s from %s", nick,
                           inet_ntoa(addr.sin_addr));
                return 1;
            } else {
                send_line(fd, "ERR invalid credentials");
            }
        } else {
            send_line(fd, "ERR must REGISTER or LOGIN first");
        }
    }
}

int phase_join(int fd, const char *nick) {
    pthread_mutex_lock(&mutex);

    if (game_started) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR game already in progress");
        return -1;
    }

    int pr, pc;
    if (!maze_random_free_cell(&maze, &pr, &pc)) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR maze full");
        return -1;
    }

    int idx = player_add(&pt, pthread_self(), nick, pr, pc);
    if (idx >= 0) {
        client_fds[idx] = fd;
        if (pt.count > max_players_ever) max_players_ever = pt.count;
        broadcast_lobby_update();
    }

    pthread_mutex_unlock(&mutex);

    if (idx < 0) send_line(fd, "ERR server full");
    return idx;
}

int phase_lobby(int fd, int* player_ready) {
    pthread_mutex_lock(&mutex);
    char init_msg[64];
    snprintf(init_msg, sizeof(init_msg), "WAITING %d/%d",
             ready_count, pt.count);
    pthread_mutex_unlock(&mutex);
    send_line(fd, init_msg);

    char line[MAX_MSG_LEN];
    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0) return 0;

        char cmd[16];
        sscanf(line, "%15s", cmd);

        if (strcmp(cmd, "READY") == 0) {
            pthread_mutex_lock(&mutex);
            ready_count++;
            *player_ready = 1;
            broadcast_lobby_update();
            if (ready_count >= pt.count && pt.count >= 2)
                game_started = 1;
            pthread_mutex_unlock(&mutex);
            return 1;
        }

        if (strcmp(cmd, "QUIT") == 0) {
            send_line(fd, "OK");
            return 0;
        }

        send_line(fd, "ERR send READY to start");
    }
}

int phase_wait_start(int fd) {
    char line[MAX_MSG_LEN];

    while (1) {
        pthread_mutex_lock(&mutex);
        int started = game_started;
        pthread_mutex_unlock(&mutex);
        if (started) return 1;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            return 0;
        }

        if (r > 0 && FD_ISSET(fd, &rfds)) {
            if (read_line(fd, line, sizeof(line)) < 0) return 0;
            char cmd[16];
            sscanf(line, "%15s", cmd);
            if (strcmp(cmd, "QUIT") == 0) {
                send_line(fd, "OK");
                return 0;
            }
        }
    }
}

void phase_play(int fd, int player_idx, const char *nick) {
    send_line(fd, "START");

    pthread_mutex_lock(&mutex);
    client_fds[player_idx] = -1;  /* esce dai destinatari del broadcast lobby */
    send_local_map(fd, &pt.slots[player_idx]);
    pthread_mutex_unlock(&mutex);

    time_t last_global = time(NULL);

    while (1) {
        pthread_mutex_lock(&mutex);
        int over = game_over;
        pthread_mutex_unlock(&mutex);
        if (over) {
            check_and_handle_game_end(fd, nick);
            return;
        }

        if (check_and_handle_game_end(fd, nick)) return;

        time_t now = time(NULL);
        long elapsed = (long)(now - last_global);
        long remaining = (long)GLOBAL_MAP_INTERVAL - elapsed;
        if (remaining <= 0) remaining = 0;

        struct timeval tv = { .tv_sec = remaining, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(notify_pipe[0], &rfds);
        int maxfd = (fd > notify_pipe[0]) ? fd : notify_pipe[0];

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }

        if (ready == 0) {
            pthread_mutex_lock(&mutex);
            send_global_map(fd, &pt.slots[player_idx]);
            pthread_mutex_unlock(&mutex);
            last_global = time(NULL);
            if (check_and_handle_game_end(fd, nick)) return;
            continue;
        }

        if (FD_ISSET(notify_pipe[0], &rfds)) {
            char buf[64];
            int n = read(notify_pipe[0], buf, sizeof(buf));
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x01) {
                    if (check_and_handle_game_end(fd, nick)) return;
                }
            }
        }

        if (FD_ISSET(fd, &rfds)) {
            int rc = handle_play_command(fd, player_idx, nick);
            if (rc == 0) return;                 /* disconnesso o QUIT   */
            if (rc == 2) {                        /* uscito dal labirinto */
                check_and_handle_game_end(fd, nick);
                return;
            }
            /* rc == 1: comando gestito, si continua il ciclo */
        }
    }
}

/* Elabora un singolo comando di gioco letto dal socket.
 * Ritorna: 0 = disconnessione/QUIT, 1 = comando gestito (continua),
 *          2 = il giocatore e' appena uscito dal labirinto. */
int handle_play_command(int fd, int player_idx, const char *nick) {
    char line[MAX_MSG_LEN];
    if (read_line(fd, line, sizeof(line)) <= 0) return 0;

    char cmd[16];
    sscanf(line, "%15s", cmd);

    if (strcmp(cmd, "QUIT") == 0) {
        send_line(fd, "OK");
        return 0;
    }

    if (strcmp(cmd, "LIST") == 0) {
        handle_list_command(fd);
        return 1;
    }

    if (strcmp(cmd, "MOVE") == 0) {
        return handle_move_command(fd, line, player_idx, nick);
    }

    send_line(fd, "ERR unknown command");
    return 1;
}

void handle_list_command(int fd) {
    pthread_mutex_lock(&mutex);
    char names[MAX_MSG_LEN] = "";
    int  cnt = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (pt.slots[i].active) {
            strncat(names, " ", sizeof(names) - strlen(names) - 1);
            strncat(names, pt.slots[i].nick, sizeof(names) - strlen(names) - 1);
            cnt++;
        }
    }
    char msg[MAX_MSG_LEN + 16];
    snprintf(msg, sizeof(msg), "LIST %d%s", cnt, names);
    pthread_mutex_unlock(&mutex);

    send_line(fd, msg);
}

// Ritorna: 1 = comando gestito, continua; 2 = il giocatore e' uscito
int handle_move_command(int fd, const char *line,
                                int player_idx, const char *nick) {
    char dir;
    if (sscanf(line, "MOVE %c", &dir) != 1) {
        send_line(fd, "ERR invalid direction");
        return 1;
    }

    pthread_mutex_lock(&mutex);
    Player *p = &pt.slots[player_idx];
    int nr = p->row, nc = p->col;

    switch (dir) {
        case 'N': nr--; break;
        case 'S': nr++; break;
        case 'W': nc--; break;
        case 'E': nc++; break;
        default:
            pthread_mutex_unlock(&mutex);
            send_line(fd, "ERR unknown direction");
            return 1;
    }

    if (nr < 0 || nr >= MAZE_ROWS || nc < 0 || nc >= MAZE_COLS ||
            maze.grid[nr][nc].cell == CELL_WALL) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR wall");
        return 1;
    }

    p->row = nr; p->col = nc;
    player_reveal(p, nr, nc);

    if (maze_collect_object(&maze, nr, nc)) {
        p->score++;
        char msg[64];
        snprintf(msg, sizeof(msg), "COLLECT %d", p->score);
        send_line(fd, msg);
        log_write("COLLECT nick=%s score=%d pos=(%d,%d)", nick, p->score, nr, nc);
    }

    if (maze.grid[nr][nc].cell == CELL_EXIT) {
        p->exited = 1;
        char msg[64];
        snprintf(msg, sizeof(msg), "EXIT_OK %d", p->score);
        send_line(fd, msg);
        log_write("EXIT nick=%s score=%d", nick, p->score);
        pthread_mutex_unlock(&mutex);
        return 2;
    }

    send_local_map(fd, p);
    pthread_mutex_unlock(&mutex);
    return 1;
}

/* Verifica se la partita e' terminata; se si', invia il messaggio di
 * fine partita al client, logga l'evento e ritorna 1. Se la partita e'
 * ancora in corso ritorna 0. */
int check_and_handle_game_end(int fd, const char *nick) {
    char winner[MAX_NICK_LEN];
    int  winner_score, is_draw;

    pthread_mutex_lock(&mutex);
    GameStatus status = game_check_end(&maze, &pt, winner, &winner_score,
                                        &is_draw);
    if (status != GAME_RUNNING) game_over = 1;
    pthread_mutex_unlock(&mutex);

    if (status == GAME_RUNNING) return 0;

    char end_msg[MAX_MSG_LEN];
    game_build_end_msg(status, winner, winner_score, is_draw, end_msg);
    send_line(fd, end_msg);
    log_write("GAME_END nick=%s msg=%s", nick, end_msg);
    return 1;
}

void client_cleanup(int fd, int player_idx,
                            int player_ready, const char *nick) {
    if (player_idx >= 0) {
        pthread_mutex_lock(&mutex);
        if (player_ready) ready_count--;
        client_fds[player_idx] = -1;
        player_remove(&pt, player_idx);
        broadcast_lobby_update();
        if (pt.count == 0) {
            ready_count  = 0;
            game_started = 0;
            game_over    = 0;
        }
        pthread_mutex_unlock(&mutex);

        write(notify_pipe[1], "\x01", 1);
        log_write("DISCONNECT nick=%s", nick);
    }

    close(fd);
}
//OLD

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "uso: %s <porta>\n", argv[0]);
        return 1;
    }
    
    maze_generate(&maze);
    log_write("maze generated (%dx%d)", MAZE_ROWS, MAZE_COLS);

    int port = atoi(argv[1]);
    srand(time(NULL));
    log_init(LOG_FILE);

    memset(&pt, 0, sizeof(PlayerTable));
    for (int i = 0; i < MAX_PLAYERS; i++)
        client_fds[i] = -1;

    listen_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sd < 0) { perror("welcoming socket"); return 1; }

    int opt = 1;
    setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listen_sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_sd, 10) < 0) {
        perror("listen"); return 1;
    }
    log_write("listening on port %d", port);

    signal(SIGPIPE, SIG_IGN);

    if (pipe(notify_pipe) < 0) { perror("pipe"); return 1; }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_sd, (struct sockaddr *)&client_addr, &client_len);
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
        }
        pthread_detach(tid);
    }

    close(listen_sd);
    return 0;
}