#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#define WIDTH 60
#define PORT 9000

typedef struct {
    int is_ready;
    int dino_y;
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;
    int is_dead;
} Packet;

long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// ================== 🎨 아스키 아트 ==================
void draw_my_dino(int y, int is_ducking) {
    attron(COLOR_PAIR(1));
    if (is_ducking) {
        mvprintw(y, 5, "_o-^ "); 
    } else {
        mvprintw(y - 1, 5, " o-^ "); 
        mvprintw(y, 5,     " / \\ "); 
    }
    attroff(COLOR_PAIR(1));
}

void draw_peer_dino(int y, int offset_y, int is_ducking) {
    attron(COLOR_PAIR(2));
    if (is_ducking) {
        mvprintw(y + offset_y, 5, "_v-e ");
    } else {
        mvprintw(y + offset_y - 1, 5, " v-e ");
        mvprintw(y + offset_y, 5,     " / \\ ");
    }
    attroff(COLOR_PAIR(2));
}

void draw_cactus(int y, int x, int type) {
    attron(COLOR_PAIR(1));
    if (type == 1) { 
        mvprintw(y - 1, x, " | "); mvprintw(y, x,     "+|+");
    } else if (type == 2) { 
        mvprintw(y - 1, x, " | | "); mvprintw(y, x,     "+|/|+");
    } else if (type == 3) { 
        mvprintw(y - 1, x, " | | | "); mvprintw(y, x,     "+/|/|/+");
    } else if (type == 4) { 
        mvprintw(y - 2, x, "  |  "); mvprintw(y - 1, x, " (| |)"); mvprintw(y, x,     " +|+ ");
    }
    attroff(COLOR_PAIR(1));
}

void draw_ptero(int y, int x) {
    attron(COLOR_PAIR(5)); 
    mvprintw(y, x, " _/^"); 
    attroff(COLOR_PAIR(5));
}
// =================================================================

int main(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1], "client") == 0 && argc < 3)) {
        printf("사용법 오류!\n 방장: %s server\n 접속자: %s client [방장IP]\n", argv[0], argv[0]);
        return 1;
    }

    int sock;
    int is_server = (strcmp(argv[1], "server") == 0);

    if (is_server) {
        int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_adr, clnt_adr;
        socklen_t clnt_adr_sz = sizeof(clnt_adr);
        memset(&serv_adr, 0, sizeof(serv_adr));
        serv_adr.sin_family = AF_INET; serv_adr.sin_addr.s_addr = htonl(INADDR_ANY); serv_adr.sin_port = htons(PORT);
        bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr));
        listen(serv_sock, 1);
        printf("방장 대기 중... 접속자를 기다립니다.\n");
        sock = accept(serv_sock, (struct sockaddr*)&clnt_adr, &clnt_adr_sz);
        close(serv_sock); 
    } else {
        sock = socket(PF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_adr;
        memset(&serv_adr, 0, sizeof(serv_adr));
        serv_adr.sin_family = AF_INET; serv_adr.sin_addr.s_addr = inet_addr(argv[2]); serv_adr.sin_port = htons(PORT);
        printf("서버에 접속 중...\n");
        if(connect(sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) { printf("접속 실패!\n"); return 1; }
    }

    set_nonblocking(sock);

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);   
    init_pair(2, COLOR_RED, COLOR_BLACK);     
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);  
    init_pair(4, COLOR_CYAN, COLOR_BLACK);    
    init_pair(5, COLOR_WHITE, COLOR_BLACK);   

    srand(time(NULL) + (is_server ? 1 : 2)); 

    // 🔥 H키로 껐다 켤 수 있는 히트박스 디버그 모드 플래그
    int show_hitbox = 1; 

    while (1) {
        int peer_disconnected = 0;
        int my_ready = 0;
        Packet peer = {0, 7, 0, WIDTH - 1, 1, 0, 0}; 

        Packet dummy;
        while (recv(sock, &dummy, sizeof(Packet), 0) > 0); 

        // ================== 대기실 ==================
        while (1) {
            int ch = getch();
            if (ch == 'q' || ch == 'Q') { peer_disconnected = 1; break; }
            if (ch == ' ') my_ready = 1; 

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
            attron(COLOR_PAIR(4)); mvprintw(6, WIDTH/2 - 13, "==== DINO MULTIPLAYER ===="); attroff(COLOR_PAIR(4));
            
            if (my_ready) {
                attron(COLOR_PAIR(1)); mvprintw(9, WIDTH/2 - 14, "You: [ READY! ] Waiting..."); attroff(COLOR_PAIR(1));
            } else {
                mvprintw(9, WIDTH/2 - 18, "Press [SPACE] to Ready / [Q] to Quit");
            }

            if (peer.is_ready) {
                attron(COLOR_PAIR(1)); mvprintw(11, WIDTH/2 - 10, "Opponent: [ READY! ]"); attroff(COLOR_PAIR(1));
            } else {
                attron(COLOR_PAIR(2)); mvprintw(11, WIDTH/2 - 11, "Opponent: Not Ready..."); attroff(COLOR_PAIR(2));
            }
            
            refresh(); usleep(50000); 
            if (my_ready && peer.is_ready) break;
        }

        if (peer_disconnected) break; 

        nodelay(stdscr, FALSE); 
        for (int i = 3; i > 0; i--) {
            erase(); attron(COLOR_PAIR(1)); mvprintw(10, WIDTH/2 - 8, "Game starting in %d...", i); attroff(COLOR_PAIR(1));
            refresh(); sleep(1); 
        }
        nodelay(stdscr, TRUE); 

        int my_y = 7, jump_stage = 0, obs_x = WIDTH - 1, obs_type = 1;
        int score = 0, is_ducking = 0, my_dead = 0;
        long long last_tick = get_time_ms(), last_duck = 0;
        int tick_interval = 60;

        // ================== 본 게임 루프 ==================
        while (1) {
            long long current_time = get_time_ms();

            int ch = getch();
            if (ch == 'q' || ch == 'Q') { peer_disconnected = 1; break; }
            if (ch == 'h' || ch == 'H') { show_hitbox = !show_hitbox; } // H키로 히트박스 ON/OFF
            
            if (!my_dead) { 
                if (ch == KEY_UP || ch == ' ') {
                    if (jump_stage == 0) { jump_stage = 1; is_ducking = 0; }
                } else if (ch == KEY_DOWN) {
                    last_duck = current_time;
                }
                if (current_time - last_duck < 600 && jump_stage == 0) is_ducking = 1;
                else is_ducking = 0;
            }

            if (current_time - last_tick >= tick_interval) {
                last_tick = current_time;

                if (!my_dead) {
                    if (jump_stage > 0) {
                        int jump_y_map[] = {7, 5, 4, 3, 2, 2, 2, 2, 2, 2, 3, 4, 5}; 
                        if (jump_stage <= 12) {
                            my_y = jump_y_map[jump_stage];
                        }
                        jump_stage++;
                        if (jump_stage > 12) { jump_stage = 0; my_y = 7; } 
                    }

                    obs_x--;
                    
                    int obs_width = 3; 
                    if (obs_type == 2) obs_width = 5;
                    else if (obs_type == 3) obs_width = 7;
                    else if (obs_type == 0) obs_width = 4; 
                    else if (obs_type == 4) obs_width = 5;

                    if (obs_x + obs_width < 0) {
                        obs_x = WIDTH - 1;
                        obs_type = rand() % 5;
                        score += 10;
                        if (tick_interval > 20) tick_interval -= 1; 
                    }

                    // 🎯 정밀 타격 패치! 아스키아트의 공백(여백)을 히트박스에서 완전히 도려냄
                    int d_top = is_ducking ? my_y : my_y - 1; 
                    int d_bot = my_y;
                    int d_left = is_ducking ? 5 : 6;  // 왼쪽 공백 제거!
                    int d_right = 8;                  // 오른쪽 공백 제거!

                    int o_top, o_bot, o_left, o_right;
                    if (obs_type == 0) { // 익룡 (" _/^")
                        o_top = 6; o_bot = 6; 
                        o_left = obs_x + 1; o_right = obs_x + 3; // 왼쪽 공백 제거!
                    } else if (obs_type == 4) { // 키 큰 선인장 ("  |  ", " (| |)", " +|+ ")
                        o_top = 5; o_bot = 7; 
                        o_left = obs_x + 1; o_right = obs_x + 3; // 양옆 공백 제거!
                    } else { // 일반 선인장
                        o_top = 6; o_bot = 7; 
                        o_left = obs_x; o_right = obs_x + obs_width - 1;
                    }

                    if (d_right >= o_left && d_left <= o_right) { 
                        if (d_bot >= o_top && d_top <= o_bot) {   
                            my_dead = 1;
                        }
                    }
                }
            }

            Packet my_data = {1, my_y, is_ducking, obs_x, obs_type, score, my_dead};
            send(sock, &my_data, sizeof(Packet), 0);
            
            Packet temp_peer;
            int recv_len;
            while ((recv_len = recv(sock, &temp_peer, sizeof(Packet), 0)) != -1) {
                if (recv_len == 0) { peer_disconnected = 1; break; }
                peer = temp_peer;
            }

            if (peer_disconnected || (my_dead && peer.is_dead)) break;

            // ================== 화면 렌더링 ==================
            erase();
            
            // --- 🏁 내 화면 ---
            attron(COLOR_PAIR(4)); mvprintw(0, 0, "[ MY SCREEN ] Score: %d | Hitbox[H]: %s", score, show_hitbox ? "ON" : "OFF"); attroff(COLOR_PAIR(4));
            attron(COLOR_PAIR(5)); mvhline(8, 0, '=', WIDTH); attroff(COLOR_PAIR(5));
            
            if (obs_x < WIDTH && !my_dead) {
                if (obs_type == 0) draw_ptero(6, obs_x); 
                else draw_cactus(7, obs_x, obs_type); 
            }
            
            if (!my_dead) draw_my_dino(my_y, is_ducking);
            else { attron(COLOR_PAIR(2)); mvprintw(7, 5, "X_X"); attroff(COLOR_PAIR(2)); }

            // 🔥 [디버그] 내 히트박스 시각화 (빨강 반전 = 공룡, 노랑 반전 = 장애물)
            if (show_hitbox && !my_dead) {
                int d_top = is_ducking ? my_y : my_y - 1; 
                int d_bot = my_y;
                int d_left = is_ducking ? 5 : 6; 
                int d_right = 8; 
                for(int y=d_top; y<=d_bot; y++) mvchgat(y, d_left, d_right-d_left+1, A_REVERSE, 2, NULL);

                int o_top, o_bot, o_left, o_right;
                if (obs_type == 0) { o_top = 6; o_bot = 6; o_left = obs_x + 1; o_right = obs_x + 3; }
                else if (obs_type == 4) { o_top = 5; o_bot = 7; o_left = obs_x + 1; o_right = obs_x + 3; }
                else { o_top = 6; o_bot = 7; o_left = obs_x; int o_w = (obs_type==2)?5:(obs_type==3)?7:3; o_right = obs_x + o_w - 1; }

                if (obs_x < WIDTH) {
                    for(int y=o_top; y<=o_bot; y++) {
                        int draw_left = o_left < 0 ? 0 : o_left;
                        int draw_right = o_right >= WIDTH ? WIDTH - 1 : o_right;
                        if (draw_left <= draw_right) mvchgat(y, draw_left, draw_right-draw_left+1, A_REVERSE, 3, NULL);
                    }
                }
            }

            // --- 🏁 상대방 화면 ---
            int offset_y = 12;  
            attron(COLOR_PAIR(4)); mvprintw(offset_y, 0, "[ OPPONENT ] Score: %d %s", peer.score, peer.is_dead ? "(DEAD)" : ""); attroff(COLOR_PAIR(4));
            attron(COLOR_PAIR(5)); mvhline(offset_y + 8, 0, '=', WIDTH); attroff(COLOR_PAIR(5));
            
            if (peer.obs_x < WIDTH && !peer.is_dead) {
                if (peer.obs_type == 0) draw_ptero(offset_y + 6, peer.obs_x); 
                else draw_cactus(offset_y + 7, peer.obs_x, peer.obs_type);
            }
            
            if (!peer.is_dead) draw_peer_dino(peer.dino_y, offset_y, peer.is_ducking);
            else { attron(COLOR_PAIR(2)); mvprintw(offset_y + 7, 5, "X_X"); attroff(COLOR_PAIR(2)); }
            
            // 🔥 [디버그] 상대방 히트박스 시각화
            if (show_hitbox && !peer.is_dead) {
                int p_d_top = peer.is_ducking ? peer.dino_y : peer.dino_y - 1; 
                int p_d_bot = peer.dino_y;
                int p_d_left = peer.is_ducking ? 5 : 6; 
                int p_d_right = 8; 
                for(int y=p_d_top; y<=p_d_bot; y++) mvchgat(y + offset_y, p_d_left, p_d_right-p_d_left+1, A_REVERSE, 2, NULL);

                int p_o_top, p_o_bot, p_o_left, p_o_right;
                if (peer.obs_type == 0) { p_o_top = 6; p_o_bot = 6; p_o_left = peer.obs_x + 1; p_o_right = peer.obs_x + 3; }
                else if (peer.obs_type == 4) { p_o_top = 5; p_o_bot = 7; p_o_left = peer.obs_x + 1; p_o_right = peer.obs_x + 3; }
                else { p_o_top = 6; p_o_bot = 7; p_o_left = peer.obs_x; int p_o_w = (peer.obs_type==2)?5:(peer.obs_type==3)?7:3; p_o_right = peer.obs_x + p_o_w - 1; }

                if (peer.obs_x < WIDTH) {
                    for(int y=p_o_top; y<=p_o_bot; y++) {
                        int draw_left = p_o_left < 0 ? 0 : p_o_left;
                        int draw_right = p_o_right >= WIDTH ? WIDTH - 1 : p_o_right;
                        if (draw_left <= draw_right) mvchgat(y + offset_y, draw_left, draw_right-draw_left+1, A_REVERSE, 3, NULL);
                    }
                }
            }
            
            refresh(); usleep(2000); 
        }

        if (peer_disconnected) break;

        // ================== 게임 오버 및 다시하기 ==================
        nodelay(stdscr, FALSE); 
        clear();
        attron(COLOR_PAIR(4)); mvprintw(8, WIDTH / 2 - 10, "==== GAME OVER ===="); attroff(COLOR_PAIR(4));
        mvprintw(10, WIDTH / 2 - 10, "My Score:   %d", score);
        mvprintw(11, WIDTH / 2 - 10, "Peer Score: %d", peer.score);
        
        if (score > peer.score) { attron(COLOR_PAIR(1)); mvprintw(13, WIDTH / 2 - 10, ">> YOU WIN! <<"); attroff(COLOR_PAIR(1)); } 
        else if (score < peer.score) { attron(COLOR_PAIR(2)); mvprintw(13, WIDTH / 2 - 10, ">> YOU LOSE... <<"); attroff(COLOR_PAIR(2)); } 
        else { attron(COLOR_PAIR(1)); mvprintw(13, WIDTH / 2 - 10, ">> DRAW! <<"); attroff(COLOR_PAIR(1)); }
        
        attron(COLOR_PAIR(3)); mvprintw(16, WIDTH / 2 - 18, "[R] (Restart)  /  [Q] (Quit)"); attroff(COLOR_PAIR(3));
        refresh(); 
        
        int choice;
        while (1) {
            choice = getch();
            if (choice == 'q' || choice == 'Q' || choice == 'r' || choice == 'R') break;
        }

        if (choice == 'q' || choice == 'Q') break; 
        
        nodelay(stdscr, TRUE); 
    }

    endwin();
    printf("gmae over.\n");
    close(sock);
    return 0;
}