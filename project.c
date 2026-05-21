/*
 * project.c  —  Multi-Player Dino Game (Enhanced Art & UI)
 *
 *  team.c 에서 추가/변경점:
 *  1. 공룡 아스키아트 전면 개편 (조금 더 공룡 모양에 가깝게)
 *  2. UI 테두리(Borders) 추가로 뚜렷한 화면 분할
 *  3. 지면 바닥(Ground) 애니메이션 효과 (스크롤)
 *  4. 구름 등 배경 디테일 추가
 *  5. 커진 히트박스에 맞춘 점프 곡선 및 충돌 영역 재계산
 *
 *  컴파일: gcc project.c -o project -lncurses
 *  실행:   ./project server           (방장)
 *          ./project client [IP]      (접속자)
 */

#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#define WIDTH     80
#define HEIGHT    30   /* 점프해도 텍스트가 가려지지 않도록 터미널 넉넉히 확보 */
#define PORT      9000

/* ── 레이아웃 상수 ── */
#define MY_BASE_Y    14          /* 내 지면 테두리 기준선 */
#define GROUND_Y     (MY_BASE_Y-1) 
#define DINO_BASE    (GROUND_Y-1)  
#define DINO_X       5             

#define OP_BASE_Y    28          /* 상대방 지면 테두리 기준선 */
#define OP_GROUND_Y  (OP_BASE_Y-1)
#define OP_DINO_BASE (OP_GROUND_Y-1)

typedef struct {
    int is_ready;
    int dino_y;       /* 공룡 기준 Y(발바닥) */
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;
    int is_dead;
} Packet;

long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec) * 1000LL + (tv.tv_usec) / 1000;
}

void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

/* ════════════════════════════════════════════
 *  에셋 및 아스키 아트 그리기
 * ════════════════════════════════════════════ */

/*
 * [신형 공룡 아트] 서기 상태 (4행, 길이 약 9글자)
 *       __
 *      / _)
 *  .--/\/ 
 *  `---/\ 
 */
static void draw_dino_stand(int y, int col, int color_pair) {
    attron(COLOR_PAIR(color_pair));
    mvprintw(y - 3, col, "      __");
    mvprintw(y - 2, col, "     / _)");
    mvprintw(y - 1, col, " .--/\\/ ");
    mvprintw(y,     col, " `---/\\ ");
    attroff(COLOR_PAIR(color_pair));
}

/*
 * [신형 공룡 아트] 숙이기 상태 (2행, 길이 약 11글자)
 *  .--...__
 *  `---._  _)
 */
static void draw_dino_duck(int y, int col, int color_pair) {
    attron(COLOR_PAIR(color_pair));
    mvprintw(y - 1, col, " .--...__");
    mvprintw(y,     col, " `---._  _)");
    attroff(COLOR_PAIR(color_pair));
}

void draw_dino(int y, int col, int is_ducking, int color_pair) {
    if (is_ducking) draw_dino_duck(y, col, color_pair);
    else            draw_dino_stand(y, col, color_pair);
}

/* 선인장 장애물 */
void draw_cactus(int ground, int x, int type) {
    attron(COLOR_PAIR(3));
    if (type == 1) {
        mvprintw(ground - 1, x, " | "); mvprintw(ground, x, "+|+");
    } else if (type == 2) {
        mvprintw(ground - 1, x, " | | "); mvprintw(ground, x, "+|/|+");
    } else if (type == 3) {
        mvprintw(ground - 1, x, " | | | "); mvprintw(ground, x, "+/|/|/+");
    } else if (type == 4) {
        mvprintw(ground - 2, x, "  |  "); mvprintw(ground - 1, x, " (|) "); mvprintw(ground, x, " +|+ ");
    }
    attroff(COLOR_PAIR(3));
}

/* 익룡 장애물 */
void draw_ptero(int ground, int x) {
    int py = ground - 4; /* 공중에 띄우기: 행 ground-4 ~ ground-3 */
    attron(COLOR_PAIR(5));
    mvprintw(py,     x, "\\o/");
    mvprintw(py + 1, x, " ^ ");
    attroff(COLOR_PAIR(5));
}

/* 지면 애니메이션 효과 */
void draw_ground(int row, int tick_count) {
    attron(COLOR_PAIR(5));
    char pattern[] = "_ . _  - _ . _ _ -  _ _ ";
    int p_len = strlen(pattern);
    int offset = tick_count % p_len;
    move(row, 2);
    for (int i = 0; i < WIDTH - 4; i++) {
        addch(pattern[(i + offset) % p_len]);
    }
    attroff(COLOR_PAIR(5));
}

/* UI 테두리 그리기 */
void draw_borders() {
    attron(COLOR_PAIR(4));
    
    /* 최상단 테두리 */
    mvaddch(0, 0, '+'); mvaddch(0, WIDTH-1, '+');
    mvhline(0, 1, '=', WIDTH-2);
    
    /* 중간 내 화면 지면 텍스처겸 테두리 */
    mvaddch(MY_BASE_Y, 0, '+'); mvaddch(MY_BASE_Y, WIDTH-1, '+');
    mvhline(MY_BASE_Y, 1, '=', WIDTH-2);

    /* 하단 상대 화면 화면 지면 테두리 */
    mvaddch(OP_BASE_Y, 0, '+'); mvaddch(OP_BASE_Y, WIDTH-1, '+');
    mvhline(OP_BASE_Y, 1, '=', WIDTH-2);

    /* 양옆 기둥 */
    for (int y = 1; y < MY_BASE_Y; y++) { mvaddch(y, 0, '|'); mvaddch(y, WIDTH-1, '|'); }
    for (int y = MY_BASE_Y + 1; y < OP_BASE_Y; y++) { mvaddch(y, 0, '|'); mvaddch(y, WIDTH-1, '|'); }

    attroff(COLOR_PAIR(4));
}

/* 구름 장식 (높이가 늘어난 것에 맞춰 좌표 조금 수정) */
void draw_decorations() {
    attron(COLOR_PAIR(5));
    mvprintw(2,  15, ".~.~.");
    mvprintw(4,  60, "   .~~.");
    mvprintw(16, 20, ".~.~.");
    mvprintw(18, 65, " .~~.");
    attroff(COLOR_PAIR(5));
}


/* ════════════════════════════════════════════
 *  히트박스 계산 (엄격한 충돌 처리)
 * ════════════════════════════════════════════ */
typedef struct { int top, bot, left, right; } HitBox;

HitBox dino_hitbox(int y, int is_ducking) {
    HitBox hb;
    hb.bot = y;
    if (is_ducking) {
        /* 숙이기는 가로 폭이 더 넒음. 꼬리부분(+3)부터 머리(+8)까지 */
        hb.top   = y - 1;
        hb.left  = DINO_X + 3;
        hb.right = DINO_X + 8;
    } else {
        /* 서기는 (폭 8칸). 꼬리부분(+3)부터 가슴(+7)까지 (관대한 히트박스) */
        hb.top   = y - 3;
        hb.left  = DINO_X + 3;
        hb.right = DINO_X + 7;
    }
    return hb;
}

HitBox obs_hitbox(int obs_x, int obs_type, int ground) {
    HitBox hb;
    if (obs_type == 0) {                   /* 익룡 */
        hb.top   = ground - 4;
        hb.bot   = ground - 3;
        hb.left  = obs_x;
        hb.right = obs_x + 2;
    } else if (obs_type == 4) {            /* 키 큰 선인장 */
        hb.top   = ground - 2;
        hb.bot   = ground;
        hb.left  = obs_x + 1;
        hb.right = obs_x + 3;
    } else {                               /* 일반 선인장 1,2,3 */
        int w = (obs_type == 2) ? 5 : (obs_type == 3) ? 7 : 3;
        hb.top   = ground - 1;
        hb.bot   = ground;
        hb.left  = obs_x;
        hb.right = obs_x + w - 1;
    }
    return hb;
}

int hitbox_overlap(HitBox a, HitBox b) {
    return (a.right >= b.left && a.left <= b.right &&
            a.bot   >= b.top  && a.top  <= b.bot);
}


/* ════════════════════════════════════════════
 *  MAIN
 * ════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1], "client") == 0 && argc < 3)) {
        printf("사용법 오류!\n 방장: %s server\n 접속자: %s client [방장IP]\n", argv[0], argv[0]);
        return 1;
    }

    int sock;
    int is_server = (strcmp(argv[1], "server") == 0);

    system("resize -s 30 80"); /* 화면 크기 강제 고정 30x80 */

    if (is_server) {
        int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in serv_adr, clnt_adr;
        socklen_t clnt_adr_sz = sizeof(clnt_adr);
        memset(&serv_adr, 0, sizeof(serv_adr));
        serv_adr.sin_family      = AF_INET;
        serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_adr.sin_port        = htons(PORT);
        bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr));
        listen(serv_sock, 1);
        printf("방장 대기 중... 접속자를 기다립니다.\n");
        sock = accept(serv_sock, (struct sockaddr*)&clnt_adr, &clnt_adr_sz);
        close(serv_sock);
    } else {
        sock = socket(PF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_adr;
        memset(&serv_adr, 0, sizeof(serv_adr));
        serv_adr.sin_family      = AF_INET;
        serv_adr.sin_addr.s_addr = inet_addr(argv[2]);
        serv_adr.sin_port        = htons(PORT);
        printf("서버에 접속 중...\n");
        if (connect(sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) {
            printf("접속 실패!\n"); return 1;
        }
    }

    set_nonblocking(sock);

    initscr(); cbreak(); noecho();
    keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN,  COLOR_BLACK);  /* 내 공룡 */
    init_pair(2, COLOR_RED,    COLOR_BLACK);  /* 상대 공룡 */
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);  /* 장애물 */
    init_pair(4, COLOR_CYAN,   COLOR_BLACK);  /* UI 테두리 및 텍스트 */
    init_pair(5, COLOR_WHITE,  COLOR_BLACK);  /* 익룡, 지면, 구름 */

    srand(time(NULL) + (is_server ? 1 : 2));

    int show_hitbox = 0;   

    /* ========================================================
     * 점프 곡선: 체공 시간을 늘려 폭 7칸짜리 장애물을 완벽히 넘도록 조정
     * ======================================================== */
    // 최고점 6행을 유지하는 프레임을 늘려서 멀리 뜁니다.
    const int jump_map[] = { 
        0, 2, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 5, 4, 2, 1, 0, 0 
    };
    const int JUMP_STEPS = 17;

    while (1) {
        int peer_disconnected = 0;
        int my_ready = 0;
        Packet peer = {0, OP_DINO_BASE, 0, WIDTH, 1, 0, 0};

        Packet dummy;
        while (recv(sock, &dummy, sizeof(Packet), 0) > 0);

        /* ── 대기실 ── */
        while (1) {
            int ch = getch();
            if (ch == 'q' || ch == 'Q') { peer_disconnected = 1; break; }
            if (ch == ' ')              my_ready = 1;

            Packet my_data = {my_ready, 0, 0, 0, 0, 0, 0};
            send(sock, &my_data, sizeof(Packet), 0);

            Packet temp_peer;
            int recv_len;
            while ((recv_len = recv(sock, &temp_peer, sizeof(Packet), 0)) != -1) {
                if (recv_len == 0) { peer_disconnected = 1; break; }
                peer = temp_peer;
            }
            if (peer_disconnected) break;

            erase();
            draw_borders();
            
            /* 멋진 타이틀 텍스트 */
            attron(COLOR_PAIR(1) | A_BOLD);
            mvprintw(2, WIDTH/2 - 15, "==== DINO BATTLE PRO ====");
            attroff(COLOR_PAIR(1) | A_BOLD);

            /* 아트 미리보기 */
            draw_dino(DINO_BASE, DINO_X, 0, 1);
            draw_ground(GROUND_Y, 0);

            if (my_ready) {
                attron(COLOR_PAIR(1));
                mvprintw(5, WIDTH/2 - 14, "You: [ READY! ] Waiting...");
                attroff(COLOR_PAIR(1));
            } else {
                mvprintw(5, WIDTH/2 - 18, "Press [SPACE] to Ready / [Q] to Quit");
            }

            if (peer.is_ready) {
                attron(COLOR_PAIR(1));
                mvprintw(7, WIDTH/2 - 10, "Opponent: [ READY! ]");
                attroff(COLOR_PAIR(1));
            } else {
                attron(COLOR_PAIR(2));
                mvprintw(7, WIDTH/2 - 11, "Opponent: Not Ready...");
                attroff(COLOR_PAIR(2));
            }

            refresh(); usleep(50000);
            if (my_ready && peer.is_ready) break;
        }

        if (peer_disconnected) break;

        nodelay(stdscr, FALSE);
        for (int i = 3; i > 0; i--) {
            erase(); draw_borders();
            attron(COLOR_PAIR(3) | A_BOLD);
            mvprintw(MY_BASE_Y/2, WIDTH/2 - 8, "Game starting in %d...", i);
            attroff(COLOR_PAIR(3) | A_BOLD);
            refresh(); sleep(1);
        }
        nodelay(stdscr, TRUE);

        int my_y        = DINO_BASE;
        int target_y    = DINO_BASE; 
        int jump_stage  = 0;
        int obs_x       = WIDTH - 2;
        int obs_type    = 1;
        int score       = 0;
        int is_ducking  = 0;
        int my_dead     = 0;
        int global_tick = 0;
        
        long long last_tick = get_time_ms();
        long long last_duck = 0;
        int  tick_interval  = 50;  

        /* ── 본 게임 루프 ── */
        while (1) {
            long long current_time = get_time_ms();

            int ch = getch();
            if (ch == 'q' || ch == 'Q') { peer_disconnected = 1; break; }
            if (ch == 'h' || ch == 'H') { show_hitbox = !show_hitbox; }

            if (!my_dead) {
                if (ch == KEY_UP || ch == ' ') {
                    if (jump_stage == 0) { jump_stage = 1; is_ducking = 0; }
                } else if (ch == KEY_DOWN) {
                    last_duck = current_time;
                }
                
                if (jump_stage == 0 && current_time - last_duck < 500)
                    is_ducking = 1;
                else
                    is_ducking = 0;
            }

            if (current_time - last_tick >= tick_interval) {
                last_tick = current_time;
                global_tick++;

                if (!my_dead) {
                    /* 점프 로직 반영 */
                    if (jump_stage > 0) {
                        if (jump_stage <= JUMP_STEPS)
                            my_y = DINO_BASE - jump_map[jump_stage];
                        
                        jump_stage++;
                        if (jump_stage > JUMP_STEPS) {
                            jump_stage = 0;
                            my_y = DINO_BASE;
                        }
                    }

                    /* 장애물 이동 */
                    obs_x--;
                    int obs_w = 3;
                    if      (obs_type == 2) obs_w = 5;
                    else if (obs_type == 3) obs_w = 7;
                    else if (obs_type == 0) obs_w = 3;   
                    else if (obs_type == 4) obs_w = 5;

                    if (obs_x + obs_w < 1) { /* 벽에 닿으면 소멸 */
                        obs_x    = WIDTH - 2;
                        obs_type = rand() % 5;
                        score   += 10;
                        if (tick_interval > 15) tick_interval -= 1;
                    }

                    HitBox dh = dino_hitbox(my_y, is_ducking);
                    HitBox oh = obs_hitbox(obs_x, obs_type, GROUND_Y);
                    if (hitbox_overlap(dh, oh)) my_dead = 1;
                }
            }

            /* 패킷 송수신 */
            Packet my_data = {1, my_y, is_ducking, obs_x, obs_type, score, my_dead};
            send(sock, &my_data, sizeof(Packet), 0);

            Packet temp_peer;
            int recv_len;
            while ((recv_len = recv(sock, &temp_peer, sizeof(Packet), 0)) != -1) {
                if (recv_len == 0) { peer_disconnected = 1; break; }
                peer = temp_peer;
            }

            if (peer_disconnected || (my_dead && peer.is_dead)) break;

            /* ════════════════════════════════════
             *  화면 렌더링
             * ════════════════════════════════════ */
            erase();
            draw_borders();
            draw_decorations();

            /* ── 내 화면 ── */
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(1, 2, "PLAYER 1 (YOU)");
            mvprintw(1, WIDTH - 25, "Score: %05d  Hitbox[H]", score);
            attroff(COLOR_PAIR(4) | A_BOLD);

            draw_ground(GROUND_Y, global_tick);

            if (obs_x < WIDTH - 1 && obs_x > 0 && !my_dead) {
                if (obs_type == 0) draw_ptero(GROUND_Y, obs_x);
                else               draw_cactus(GROUND_Y, obs_x, obs_type);
            }

            if (!my_dead) draw_dino(my_y, DINO_X, is_ducking, 1);
            else          { attron(COLOR_PAIR(2)); mvprintw(DINO_BASE, DINO_X, " X_X "); attroff(COLOR_PAIR(2)); }

            /* 히트박스 시각화 (내꺼) */
            if (show_hitbox && !my_dead) {
                HitBox dh = dino_hitbox(my_y, is_ducking);
                for (int y = dh.top; y <= dh.bot; y++)
                    mvchgat(y, dh.left, dh.right - dh.left + 1, A_REVERSE, 2, NULL);

                HitBox oh = obs_hitbox(obs_x, obs_type, GROUND_Y);
                if (obs_x < WIDTH - 1 && obs_x > 0) {
                    for (int y = oh.top; y <= oh.bot; y++) {
                        int dl = oh.left < 1 ? 1 : oh.left;
                        int dr = oh.right >= WIDTH - 1 ? WIDTH - 2 : oh.right;
                        if (dl <= dr) mvchgat(y, dl, dr - dl + 1, A_REVERSE, 3, NULL);
                    }
                }
            }

            /* ── 상대 화면 ── */
            attron(COLOR_PAIR(4) | A_BOLD);
            mvprintw(MY_BASE_Y + 1, 2, "PLAYER 2 (OPPONENT) %s", peer.is_dead ? "[DEAD]" : "");
            mvprintw(MY_BASE_Y + 1, WIDTH - 15, "Score: %05d", peer.score);
            attroff(COLOR_PAIR(4) | A_BOLD);

            draw_ground(OP_GROUND_Y, global_tick);

            if (peer.obs_x < WIDTH - 1 && peer.obs_x > 0 && !peer.is_dead) {
                if (peer.obs_type == 0) draw_ptero(OP_GROUND_Y, peer.obs_x);
                else                    draw_cactus(OP_GROUND_Y, peer.obs_x, peer.obs_type);
            }

            if (!peer.is_dead) {
                /* peer.dino_y 는 DINO_BASE 기준값이므로 상대 지면으로 변환 */
                int peer_render_y = OP_DINO_BASE - (DINO_BASE - peer.dino_y);
                draw_dino(peer_render_y, DINO_X, peer.is_ducking, 2);
            }
            else { 
                attron(COLOR_PAIR(2)); mvprintw(OP_DINO_BASE, DINO_X, " X_X "); attroff(COLOR_PAIR(2)); 
            }

            /* 히트박스 (상대꺼) */
            if (show_hitbox && !peer.is_dead) {
                int peer_render_y = OP_DINO_BASE - (DINO_BASE - peer.dino_y);
                HitBox pdh = dino_hitbox(peer_render_y, peer.is_ducking);
                for (int y = pdh.top; y <= pdh.bot; y++)
                    mvchgat(y, pdh.left, pdh.right - pdh.left + 1, A_REVERSE, 2, NULL);

                HitBox poh = obs_hitbox(peer.obs_x, peer.obs_type, OP_GROUND_Y);
                if (peer.obs_x < WIDTH - 1 && peer.obs_x > 0) {
                    for (int y = poh.top; y <= poh.bot; y++) {
                        int dl = poh.left < 1 ? 1 : poh.left;
                        int dr = poh.right >= WIDTH - 1 ? WIDTH - 2 : poh.right;
                        if (dl <= dr) mvchgat(y, dl, dr - dl + 1, A_REVERSE, 3, NULL);
                    }
                }
            }

            refresh(); usleep(2000);
        } /* end game loop */

        if (peer_disconnected) break;

        /* ── 게임 오버 ── */
        nodelay(stdscr, FALSE);
        clear(); draw_borders();
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(8, WIDTH/2 - 10, "==== GAME OVER ====");
        attroff(COLOR_PAIR(4) | A_BOLD);
        mvprintw(10, WIDTH/2 - 10, "My Score:   %05d", score);
        mvprintw(11, WIDTH/2 - 10, "Peer Score: %05d", peer.score);

        if (score > peer.score) {
            attron(COLOR_PAIR(1) | A_BOLD); mvprintw(13, WIDTH/2 - 10, ">> YOU WIN! <<"); attroff(COLOR_PAIR(1) | A_BOLD);
        } else if (score < peer.score) {
            attron(COLOR_PAIR(2) | A_BOLD); mvprintw(13, WIDTH/2 - 10, ">> YOU LOSE... <<"); attroff(COLOR_PAIR(2) | A_BOLD);
        } else {
            attron(COLOR_PAIR(1) | A_BOLD); mvprintw(13, WIDTH/2 - 10, ">> DRAW! <<"); attroff(COLOR_PAIR(1) | A_BOLD);
        }

        attron(COLOR_PAIR(3));
        mvprintw(16, WIDTH/2 - 18, "[R] Restart  /  [Q] Quit");
        attroff(COLOR_PAIR(3));
        refresh();

        int choice;
        while (1) {
            choice = getch();
            if (choice == 'q' || choice == 'Q' ||
                choice == 'r' || choice == 'R') break;
        }
        if (choice == 'q' || choice == 'Q') break;

        nodelay(stdscr, TRUE);
    } 

    endwin();
    printf("Game Over.\n");
    close(sock);
    return 0;
}
