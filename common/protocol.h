#ifndef PROTOCOL_H
#define PROTOCOL_H

// Dimensioni labirinto
#define MAZE_ROWS            20
#define MAZE_COLS            20
#define VIEW_RADIUS          1  // Finestra locale: (2*VIEW_RADIUS + 1)^2

// Celle del labirinto
#define CELL_FREE           '.'#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
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
volatile int game_over = 0; //TODO se funziona togliendo volatile - magic numbers
int client_fds[MAX_PLAYERS];
int listen_sd = -1;

/* risultato finale della partita, calcolato UNA SOLA VOLTA (dal primo
   thread che rileva la fine partita) e poi condiviso con tutti gli altri,
   cosi' ogni giocatore riceve esattamente lo stesso esito invece di un
   ricalcolo indipendente che, effettuato in istanti leggermente diversi,
   potrebbe dare risultati differenti (es. pareggio per uno, vittoria per
   un altro) */
static GameStatus g_final_status = GAME_RUNNING;
static char       g_final_winner[MAX_NICK_LEN] = {0};
static int        g_final_score  = 0;
static int        g_final_draw   = 0;

typedef struct {
    int fd;
    struct sockaddr_in addr;
} ClientArgs;

unsigned long hash(char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

void send_line(int fd, const char* msg) {
    char buf[MAX_MSG_LEN + 2];
    int ret = snprintf(buf, sizeof(buf), "%s\n", msg);
    if (ret < 0) { perror("parsing"); return; }

    size_t sent = 0, n = ret;
    while (sent < n) {
        ssize_t w = send(fd, buf+sent, (size_t)n-sent, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) perror("client disconnesso");
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

void broadcast_lobby_update() {
    char line[64];
    snprintf(line, sizeof(line), "WAITING %d/%d", pt.ready_count, pt.count);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] >= 0) send_line(client_fds[i], line);
}

int auth_register(const char* nick, char* pass) {
    pthread_mutex_lock(&auth_mutex);
    FILE* f = fopen(USERS_FILE, "r");
    if (f) {
        char line[MAX_NICK_LEN + MAX_PASS_DIGITS + 1];
        while (fgets(line, sizeof(line), f)) {
            char n[MAX_NICK_LEN];
            unsigned long p = 0;
            if (sscanf(line, "%s %lu", n, &p) == 2 && strcmp(n, nick) == 0) {
                fclose(f);
                pthread_mutex_unlock(&auth_mutex);
                return -1;
            }
        }
        fclose(f);
    }
    f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&auth_mutex); return -1; }
    fprintf(f, "%s %lu\n", nick, hash(pass));
    fclose(f);
    pthread_mutex_unlock(&auth_mutex);
    return 0;
}

int auth_login(const char* nick, char* pass) {
    pthread_mutex_lock(&auth_mutex);
    FILE* f = fopen(USERS_FILE, "r");
    if (!f) { pthread_mutex_unlock(&auth_mutex); return -1; }
    char line[MAX_NICK_LEN + MAX_PASS_DIGITS + 1];
    unsigned long hashed_pass = hash(pass);
    while (fgets(line, sizeof(line), f)) {
        char n[MAX_NICK_LEN];
        unsigned long p = 0;
        if (sscanf(line, "%s %lu", n, &p) == 2 && strcmp(n, nick) == 0 && p == hashed_pass) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (strcmp(pt.slots[i].nick, nick) == 0 && pt.slots[i].active == 1) return -2;
            }
            fclose(f);
            pthread_mutex_unlock(&auth_mutex);
            return 0;
        }
    }
    fclose(f);
    pthread_mutex_unlock(&auth_mutex);
    return -1;
}

void send_local_map(int fd, Player* p) {
    char data[((2*VIEW_RADIUS+1)*(2*VIEW_RADIUS+1)) + 1];
    int rows = 2*VIEW_RADIUS + 1;
    int cols = rows;
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = maze.grid[r][c].cell;
    player_local_map(p, flat, data);
    char msg[MAX_MSG_LEN];
    snprintf(msg, sizeof(msg), "LOCAL %d %d %s", rows, cols, data);
    send_line(fd, msg);
}

void send_global_map(int fd, Player* p) {
    char data[MAZE_ROWS*MAZE_COLS + 1];
    char flat[MAZE_ROWS][MAZE_COLS];
    for (int r = 0; r < MAZE_ROWS; r++)
        for (int c = 0; c < MAZE_COLS; c++)
            flat[r][c] = maze.grid[r][c].cell;
    player_global_map(p, flat, data);
    char msg[MAX_MSG_LEN + MAZE_ROWS*MAZE_COLS];
    snprintf(msg, sizeof(msg), "GLOBAL %d %d %s", MAZE_ROWS, MAZE_COLS, data);
    send_line(fd, msg);
}

int phase_auth(int fd, struct sockaddr_in addr, char* nick, size_t nick_size) {
    char line[MAX_MSG_LEN];

    while (1) {
        if (recv_line(fd, line) < 0) return 0;

        char cmd[16], arg1[MAX_NICK_LEN], arg2[MAX_PASS_LEN];
        int n = sscanf(line, "%15s %31s %63s", cmd, arg1, arg2);

        if (strcmp(cmd, "REGISTER") == 0 && n == 3) {
            if (auth_register(arg1, arg2) == 0) {
                send_line(fd, "OK");
                log_write("REGISTER nick=%s from %s", arg1, inet_ntoa(addr.sin_addr));
            } else {
                send_line(fd, "ERR nick already exists");
            }
        } else if (strcmp(cmd, "LOGIN") == 0 && n == 3) {
            switch (auth_login(arg1, arg2)) {
                case 0:
                    strncpy(nick, arg1, nick_size - 1);
                    send_line(fd, "OK");
                    log_write("LOGIN nick=%s from %s", nick, inet_ntoa(addr.sin_addr));
                    return 1;
                case -1:
                    send_line(fd, "ERR invalid credentials"); break;
                case -2:
                    send_line(fd, "ERR already logged in"); break;
            }
        } else {
            send_line(fd, "ERR must REGISTER or LOGIN first");
        }
    }
}

int phase_join(int fd, const char* nick) {
    pthread_mutex_lock(&mutex);

    if (maze.game_started) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR game already in progress");
        return -1;
    }

    int start_r, start_c;
    if (!maze_random_free_cell(&maze, &start_r, &start_c)) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR maze full");
        return -1;
    }

    int idx = player_add(&pt, pthread_self(), nick, start_r, start_c);
    if (idx >= 0) {
        client_fds[idx] = fd;
        broadcast_lobby_update();
    }

    pthread_mutex_unlock(&mutex);

    if (idx < 0) send_line(fd, "ERR server full");
    return idx;
}

int phase_lobby(int fd, int* player_ready) {
    pthread_mutex_lock(&mutex);
    char init_msg[64];
    snprintf(init_msg, sizeof(init_msg), "WAITING %d/%d", pt.ready_count, pt.count);
    pthread_mutex_unlock(&mutex);
    send_line(fd, init_msg);

    char line[MAX_MSG_LEN];
    while (1) {
        if (recv_line(fd, line) < 0) return 0;

        char cmd[16];
        sscanf(line, "%15s", cmd);

        if (strcmp(cmd, "READY") == 0) {
            pthread_mutex_lock(&mutex);
            pt.ready_count++;
            *player_ready = 1;
            broadcast_lobby_update();
            if (pt.ready_count >= pt.count && pt.count >= 2)
                maze.game_started = 1;
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
        int started = maze.game_started;
        pthread_mutex_unlock(&mutex);
        if (started) return 1;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("select");
            return 0;
        }

        if (r > 0 && FD_ISSET(fd, &rfds)) {
            if (recv_line(fd, line) < 0) return 0;
            char cmd[16];
            sscanf(line, "%15s", cmd);
            if (strcmp(cmd, "QUIT") == 0) {
                send_line(fd, "OK");
                return 0;
            }
        }
    }
}

// Ritorna: 1 = comando gestito, continua; 2 = il giocatore e' uscito
int handle_move_command(int fd, const char* line, int player_idx, const char* nick) {
    char dir;
    if (sscanf(line, "MOVE %c", &dir) != 1) {
        send_line(fd, "ERR invalid direction");
        return 1;
    }

    pthread_mutex_lock(&mutex);
    Player* p = &pt.slots[player_idx];
    int new_r = p->row, new_c = p->col;

    switch (dir) {
        case 'N': new_r--; break;
        case 'S': new_r++; break;
        case 'W': new_c--; break;
        case 'E': new_c++; break;
        default:
            pthread_mutex_unlock(&mutex);
            send_line(fd, "ERR unknown direction");
            return 1;
    }

    if (new_r < 0 || new_r >= MAZE_ROWS || new_c < 0 || new_c >= MAZE_COLS ||
            maze.grid[new_r][new_c].cell == CELL_WALL) {
        pthread_mutex_unlock(&mutex);
        send_line(fd, "ERR wall");
        return 1;
    }

    p->row = new_r; p->col = new_c;
    player_reveal(p, new_r, new_c);

    if (maze_collect_object(&maze, new_r, new_c)) {
        p->score++;
        char msg[64];
        snprintf(msg, sizeof(msg), "COLLECT %d", p->score);
        send_line(fd, msg);
        log_write("COLLECT nick=%s score=%d pos=(%d,%d)", nick, p->score, new_r, new_c);
    }

    if (maze.grid[new_r][new_c].cell == CELL_EXIT) {
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

void handle_list_command(int fd) {
    pthread_mutex_lock(&mutex);
    char names[MAX_MSG_LEN] = "";
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (pt.slots[i].active) {
            strncat(names, " ", sizeof(names) - strlen(names) - 1);
            strncat(names, pt.slots[i].nick, sizeof(names) - strlen(names) - 1);
            count++;
        }
    }
    char msg[MAX_MSG_LEN + 16];
    snprintf(msg, sizeof(msg), "LIST %d%s", count, names);
    pthread_mutex_unlock(&mutex);

    send_line(fd, msg);
}

/* Ritorna: 0 = disconnessione/QUIT, 1 = comando gestito (continua),
            2 = il giocatore è uscito dal labirinto. */
int handle_play_command(int fd, int player_idx, const char* nick) {
    char line[MAX_MSG_LEN];
    if (recv_line(fd, line) <= 0) return 0;

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

int check_and_handle_game_end(int fd, const char* nick) {
    char winner[MAX_NICK_LEN];
    int winner_score, is_draw;

    pthread_mutex_lock(&mutex);
    GameStatus status = game_check_end(&maze, &pt, winner, &winner_score, &is_draw);
    if (status != GAME_RUNNING) game_over = 1;
    pthread_mutex_unlock(&mutex);

    if (status == GAME_RUNNING) return 0;

    char end_msg[MAX_MSG_LEN];
    game_build_end_msg(status, winner, winner_score, is_draw, end_msg);
    send_line(fd, end_msg);
    log_write("GAME_END nick=%s msg=%s", nick, end_msg);
    return 1;
}

void phase_play(int fd, int player_idx, const char* nick) {
    send_line(fd, "START");

    pthread_mutex_lock(&mutex);
    client_fds[player_idx] = -1; // esce dai destinatari del broadcast lobby
    send_local_map(fd, &pt.slots[player_idx]);
    time_t start_time = maze.start_time;
    pthread_mutex_unlock(&mutex);

    time_t last_global = time(NULL);
    time_t last_time_msg = 0; /* forza l'invio del primo TIME subito */
    int exited = 0; /* il giocatore e' uscito dal labirinto mac sta aspettando l'esito */

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

        /* invia il tempo rimanente al client una volta al secondo */
        if (now != last_time_msg) {
            int remaining_time = (int)GAME_TIMEOUT - (int)(now - start_time);
            if (remaining_time < 0) remaining_time = 0;
            char tmsg[32];
            snprintf(tmsg, sizeof(tmsg), "TIME %d", remaining_time);
            send_line(fd, tmsg);
            last_time_msg = now;
        }

        long elapsed = (long)(now - last_global);
        long remaining = (long)GLOBAL_MAP_INTERVAL - elapsed;
        if (remaining <= 0) remaining = 0;
        if (remaining > 1) remaining = 1; /* sveglia ogni secondo per aggiornare il timer */

        struct timeval tv = { .tv_sec = remaining, .tv_usec = 0 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(notify_pipe[0], &rfds);
        int maxfd = (fd > notify_pipe[0]) ? fd : notify_pipe[0];

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            return;
        }

        if (ready == 0) {
            long elapsed2 = (long)(time(NULL) - last_global);
            if (elapsed2 >= GLOBAL_MAP_INTERVAL) {
                pthread_mutex_lock(&mutex);
                send_global_map(fd, &pt.slots[player_idx]);
                pthread_mutex_unlock(&mutex);
                last_global = time(NULL);
            }
            if (check_and_handle_game_end(fd, nick)) return;
            continue;
        }

        if (FD_ISSET(notify_pipe[0], &rfds)) {
            char buf[2];
            int n = read(notify_pipe[0], buf, 2);
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x01) {
                    if (check_and_handle_game_end(fd, nick)) return;
                }
            }
        }

        if (FD_ISSET(fd, &rfds)) {
            int result = handle_play_command(fd, player_idx, nick);
            if (result == 0) return; // Disconnesso o quit
            if (result == 2) { // Uscito
                check_and_handle_game_end(fd, nick);
                return;
            }
        }
    }
}

void client_cleanup(int fd, int player_idx, int player_ready, const char* nick) {
    if (player_idx >= 0) {
        pthread_mutex_lock(&mutex);
        if (player_ready) pt.ready_count--;
        client_fds[player_idx] = -1;
        player_remove(&pt, player_idx);
        broadcast_lobby_update();
        if (pt.count == 0) {
            pt.ready_count = 0;
            maze.game_started = 0;
            game_over = 0;
            maze_generate(&maze);
            log_write("maze regenerated (%dx%d)", MAZE_ROWS, MAZE_COLS);
        }
        pthread_mutex_unlock(&mutex);

        write(notify_pipe[1], "\x01", 1);
        log_write("DISCONNECT nick=%s", nick);
    }

    close(fd);
}

void* handle_client(void* arg) {
    ClientArgs args = *(ClientArgs*)arg;
    free(arg);

    int fd = args.fd;
    struct sockaddr_in addr = args.addr;

    char nick[MAX_NICK_LEN] = {0};
    int player_idx = -1;
    int player_ready = 0;

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

int main(int argc, char* argv[]) {
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
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_sd, 10) < 0) {
        perror("listen"); return 1;
    }
    log_write("listening on port %d", port);

    signal(SIGPIPE, SIG_IGN);

    if (pipe(notify_pipe) < 0) { perror("pipe"); return 1; }
    /* il lato di lettura e' condiviso da tutti i thread dei giocatori: se
       piu' thread vengono svegliati insieme da select() ma solo uno riesce
       a leggere il byte, gli altri non devono bloccarsi su read() in attesa
       di altri dati che potrebbero non arrivare mai */
    fcntl(notify_pipe[0], F_SETFL, O_NONBLOCK);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_sd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        log_write("new connection from %s:%d",
                  inet_ntoa(client_addr.sin_addr),
                  ntohs(client_addr.sin_port));
        ClientArgs* cargs = malloc(sizeof(ClientArgs));
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