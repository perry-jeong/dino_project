/**
 * project_sdl.c - SDL2 Graphical Client for Dino Multi-Player Game [CD Edition]
 *
 * 변경 이력:
 *  AB → CD: C1~C5 버그 수정, Q5~Q7 코드 품질 개선,
 *            N1(최고점수) N2(일시정지) N3(레벨) N4(구름 연동)
 *            N5(구분선) N6(게임오버 하이라이트) 기능 추가
 *
 * 컴파일 명령어:
 * gcc project_sdl.c -o project_sdl -lSDL2 -lSDL2_image -lSDL2_ttf
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <libgen.h>

/* 터미널 논리 비율 */
#define WIDTH   80
#define HEIGHT  30
#define PORT    9000

/* 창 크기 */
#define CELL_W  12
#define CELL_H  24
#define WIN_W   (WIDTH  * CELL_W)   /* 960px */
#define WIN_H   (HEIGHT * CELL_H)   /* 720px */

/* 레이아웃 */
#define MY_BASE_Y   14
#define DINO_BASE   12
#define DINO_X       5
#define OP_BASE_Y   28

/* 구름 수 */
#define MAX_CLOUDS   5

/* ── 난이도 레벨 계산 (tick 50→15, 10단계) ─────────────── */
/* N3: tick_interval 50→15, 35 range / 10 steps ≈ 3.5 per level */
static inline int calc_level(int tick_interval) {
    int lvl = (int)((50 - tick_interval) / 3.5f) + 1;
    if (lvl < 1)  lvl = 1;
    if (lvl > 10) lvl = 10;
    return lvl;
}

typedef struct {
    int is_ready;
    int dino_y;
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;
    int is_dead;
} Packet;

typedef struct { int top, bot, left, right; } HitBox;

HitBox dino_hitbox(int y, int is_ducking) {
    HitBox hb;
    hb.bot = y - 1;
    if (is_ducking) {
        hb.top = y - 2; hb.left = DINO_X + 1; hb.right = DINO_X + 4;
    } else {
        hb.top = y - 4; hb.left = DINO_X + 1; hb.right = DINO_X + 3;
    }
    return hb;
}

HitBox obs_hitbox(int obs_x, int obs_type, int ground) {
    HitBox hb;
    if (obs_type == 0) {                    /* 익룡 */
        hb.top = ground-4; hb.bot = ground-3;
        hb.left = obs_x+1; hb.right = obs_x+3;
    } else if (obs_type == 4) {             /* 큰 선인장 */
        hb.top = ground-3; hb.bot = ground-1;
        hb.left = obs_x+1; hb.right = obs_x+2;
    } else {                                /* 일반 선인장 */
        int w = (obs_type==2)?3:(obs_type==3)?5:2;
        hb.top = ground-2; hb.bot = ground-1;
        hb.left = obs_x; hb.right = obs_x+w-1;
    }
    return hb;
}

int hitbox_overlap(HitBox a, HitBox b) {
    return (a.right >= b.left && a.left <= b.right &&
            a.bot   >= b.top  && a.top  <= b.bot);
}

long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

void set_nonblocking(int sock) {
    int f = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);
}

/* ── Q5: dirname 안전 버전 ──────────────────────────────── */
static char g_base_dir[512];

void init_base_dir(const char* argv0) {
    char tmp[512];
    strncpy(tmp, argv0, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    char* res = dirname(tmp);          /* res may point into tmp */
    strncpy(g_base_dir, res, sizeof(g_base_dir)-1);
    g_base_dir[sizeof(g_base_dir)-1] = '\0';
}

void asset_path(char* out, size_t sz, const char* filename) {
    snprintf(out, sz, "%s/%s", g_base_dir, filename);
}

/* ── 구름 구조체 ─────────────────────────────────────────── */
typedef struct {
    float x;
    int   y_row;
    int   w;
    float base_speed;   /* N4: 기준 속도 (tick_interval=50 기준) */
} Cloud;

void init_clouds(Cloud clouds[], int count) {
    for (int i = 0; i < count; i++) {
        clouds[i].x          = (float)(rand() % WIN_W);
        clouds[i].y_row      = 1 + (rand() % 5);
        clouds[i].w          = 3 + (rand() % 3);
        clouds[i].base_speed = 0.3f + (float)(rand()%10) / 20.0f;
    }
}

/* N4: tick_interval이 작아질수록 구름도 빨라짐 */
void update_and_draw_clouds(SDL_Renderer* renderer, Cloud clouds[], int count,
                             int tick_interval) {
    float speed_mult = 50.0f / (float)tick_interval;
    SDL_SetRenderDrawColor(renderer, 210, 215, 220, 255);
    for (int i = 0; i < count; i++) {
        clouds[i].x -= clouds[i].base_speed * speed_mult;
        if (clouds[i].x + clouds[i].w * CELL_W < 0) {
            clouds[i].x     = (float)WIN_W;
            clouds[i].y_row = 1 + (rand() % 5);
            clouds[i].w     = 3 + (rand() % 3);
            clouds[i].base_speed = 0.3f + (float)(rand()%10) / 20.0f;
        }
        SDL_Rect r = { (int)clouds[i].x, clouds[i].y_row * CELL_H,
                       clouds[i].w * CELL_W, CELL_H / 2 };
        SDL_RenderFillRect(renderer, &r);
    }
}

/* ── SDL 렌더링 유틸 ─────────────────────────────────────── */
void draw_sprite(SDL_Renderer* renderer, SDL_Texture* tex,
                 int x_col, int y_row, int w_cols, int h_rows,
                 int frame_idx, int total_frames,
                 float angle, SDL_Color colorMod) {
    if (!tex) return;
    SDL_SetTextureColorMod(tex, colorMod.r, colorMod.g, colorMod.b);
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    SDL_Rect src = { frame_idx * (tw / (total_frames>0?total_frames:1)),
                     0,
                     tw / (total_frames>0?total_frames:1),
                     th };
    SDL_Rect dst = { x_col*CELL_W, y_row*CELL_H, w_cols*CELL_W, h_rows*CELL_H };
    SDL_RenderCopyEx(renderer, tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
}

void draw_filled_rect_px(SDL_Renderer* ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

/* C1: texture NULL 체크 추가 */
void draw_text(SDL_Renderer* renderer, TTF_Font* font, const char* text,
               int x_col, int y_row, SDL_Color color) {
    if (!font || !text || !text[0]) return;
    SDL_Surface* surf = TTF_RenderUTF8_Solid(font, text, color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) { SDL_FreeSurface(surf); return; }   /* C1 */
    SDL_Rect dst = { x_col*CELL_W, y_row*CELL_H, surf->w, surf->h };
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

void draw_text_center(SDL_Renderer* renderer, TTF_Font* font, const char* text,
                      int y_row, SDL_Color color) {
    if (!font || !text || !text[0]) return;
    SDL_Surface* surf = TTF_RenderUTF8_Solid(font, text, color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) { SDL_FreeSurface(surf); return; }   /* C1 */
    SDL_Rect dst = { (WIN_W - surf->w)/2, y_row*CELL_H, surf->w, surf->h };
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

/* N6: 배경 사각형 위에 텍스트 (게임오버 하이라이트) */
void draw_text_center_highlight(SDL_Renderer* renderer, TTF_Font* font,
                                const char* text, int y_row,
                                SDL_Color fg, SDL_Color bg) {
    if (!font || !text || !text[0]) return;
    SDL_Surface* surf = TTF_RenderUTF8_Solid(font, text, fg);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) { SDL_FreeSurface(surf); return; }
    int px = (WIN_W - surf->w) / 2;
    int py = y_row * CELL_H;
    int pad = 8;
    /* 배경 */
    draw_filled_rect_px(renderer, px-pad, py-pad/2,
                        surf->w+pad*2, surf->h+pad, bg);
    /* 텍스트 */
    SDL_Rect dst = {px, py, surf->w, surf->h};
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
}

/* UTF-8 → 코드포인트 비교 */
static int textinput_is(const char* text, unsigned int want) {
    unsigned char a=(unsigned char)text[0], b=(unsigned char)text[1], c=(unsigned char)text[2];
    if (!a || !b || !c || text[3]) return 0;
    return (((a&0x0F)<<12)|((b&0x3F)<<6)|(c&0x3F)) == want;
}
#define KOR_BI    0x3142u
#define KOR_GIYOK 0x3131u

const int jump_map[] = { 0,2,4,5,6,6,6,6,6,6,6,6,5,4,2,1,0,0 };
const int JUMP_STEPS = 17;

/* ── 지면선 그리기 ───────────────────────────────────────── */
void draw_ground(SDL_Renderer* ren, int base_y) {
    SDL_SetRenderDrawColor(ren, 83, 83, 83, 255);
    SDL_Rect r = {0, base_y*CELL_H, WIN_W, 3};
    SDL_RenderFillRect(ren, &r);
}

/* N5: 두 화면 사이 구분선 (점선) */
void draw_divider(SDL_Renderer* ren) {
    SDL_SetRenderDrawColor(ren, 180, 180, 180, 200);
    int y = ((MY_BASE_Y + OP_BASE_Y) / 2) * CELL_H;
    for (int x = 0; x < WIN_W; x += 20) {
        SDL_Rect r = {x, y, 10, 2};
        SDL_RenderFillRect(ren, &r);
    }
}

/* ── 공통 장애물 렌더링 ──────────────────────────────────── */
void draw_obstacle(SDL_Renderer* ren, SDL_Texture* tex_ptero, SDL_Texture* tex_cactus,
                   int obs_x, int obs_type, int base_y,
                   int anim_ptero, SDL_Color ptero_c, SDL_Color cactus_c) {
    if (obs_x <= 0 || obs_x >= WIDTH-1) return;
    if (obs_type == 0) {
        draw_sprite(ren, tex_ptero, obs_x, base_y-4, 4, 2,
                    anim_ptero, 2, 0.0f, ptero_c);
    } else {
        int ch    = (obs_type==4) ? 3 : 2;
        int count = (obs_type==2) ? 2 : (obs_type==3) ? 3 : 1;
        for (int i=0; i<count; i++)
            draw_sprite(ren, tex_cactus, obs_x+(i*2), base_y-ch, 3, ch, 0, 1, 0.0f, cactus_c);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1],"client")==0 && argc<3)) {
        printf("Usage:\n  방장:   %s server\n  접속자: %s client [방장IP]\n",
               argv[0], argv[0]);
        return 1;
    }

    init_base_dir(argv[0]);

    int sock;
    int is_server = (strcmp(argv[1],"server")==0);

    /* 네트워크 설정 */
    if (is_server) {
        int ssock = socket(PF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(ssock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in sa, ca;
        socklen_t ca_sz = sizeof(ca);
        memset(&sa,0,sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(PORT);
        if (bind(ssock,(struct sockaddr*)&sa,sizeof(sa))<0){perror("bind");return 1;}
        listen(ssock,1);
        printf("[SDL2 방장] 포트 %d 에서 대기 중...\n", PORT);
        sock = accept(ssock,(struct sockaddr*)&ca,&ca_sz);
        close(ssock);
    } else {
        sock = socket(PF_INET,SOCK_STREAM,0);
        struct sockaddr_in sa;
        memset(&sa,0,sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = inet_addr(argv[2]);
        sa.sin_port = htons(PORT);
        printf("[SDL2 접속] %s 에 연결 중...\n", argv[2]);
        if (connect(sock,(struct sockaddr*)&sa,sizeof(sa))==-1){
            printf("접속 실패!\n"); return 1;
        }
    }
    set_nonblocking(sock);

    /* SDL 초기화 */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("SDL: %s\n",SDL_GetError()); return 1; }
    if (!(IMG_Init(IMG_INIT_PNG)&IMG_INIT_PNG)){ printf("IMG: %s\n",IMG_GetError()); return 1; }
    if (TTF_Init()==-1){ printf("TTF: %s\n",TTF_GetError()); return 1; }

    SDL_Window* win = SDL_CreateWindow(
        "Multi-Player Dino Game [CD Edition]",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    /* 에셋 로드 (surface 즉시 해제 – B4) */
    char pb[600];
    #define LOAD_TEX(var, file) \
        asset_path(pb, sizeof(pb), file); \
        { SDL_Surface* s = IMG_Load(pb); \
          var = s ? SDL_CreateTextureFromSurface(ren,s) : NULL; \
          if(var) SDL_SetTextureBlendMode(var, SDL_BLENDMODE_BLEND); \
          if(s) SDL_FreeSurface(s); }

    SDL_Texture *tex_dino, *tex_cactus, *tex_ptero;
    LOAD_TEX(tex_dino,   "dino_anim.png")
    LOAD_TEX(tex_cactus, "cactus_clean.png")
    LOAD_TEX(tex_ptero,  "ptero_anim.png")

    if (!tex_dino||!tex_cactus||!tex_ptero)
        printf("경고: 일부 스프라이트 로드 실패 (dir=%s)\n", g_base_dir);

    asset_path(pb, sizeof(pb), "NanumGothic.ttf");
    TTF_Font* font       = TTF_OpenFont(pb, 24);
    TTF_Font* font_large = TTF_OpenFont(pb, 48);
    if (!font||!font_large)
        printf("경고: 폰트 로드 실패 – 텍스트 없이 진행\n");

    srand((unsigned)time(NULL) + (is_server?1:2));

    /* 색상 */
    SDL_Color col_gray   = {83,  83,  83,  255};
    SDL_Color col_light  = {170, 170, 170, 255};
    SDL_Color col_green  = {60,  180, 60,  180};
    SDL_Color col_red_bg = {200, 60,  60,  180};
    SDL_Color col_my     = col_gray;
    SDL_Color col_op     = col_light;
    SDL_Color col_cac    = col_gray;
    SDL_Color col_pt     = col_gray;

    /* 구름 */
    Cloud clouds[MAX_CLOUDS];
    init_clouds(clouds, MAX_CLOUDS);

    /* N1: 세션 최고 점수 */
    int best_score = 0;

    /* Q7: P1/P2 라벨 */
    const char* my_label   = is_server ? "P1 (Me)"   : "P2 (Me)";
    const char* peer_label = is_server ? "P2 (Peer)" : "P1 (Peer)";

    int quit = 0;
    while (!quit) {
        int peer_disconnected = 0;
        int my_ready = 0;
        Packet peer = {0, DINO_BASE, 0, WIDTH, 1, 0, 0};

        /* 소켓 버퍼 비우기 */
        { Packet dum; while(recv(sock,&dum,sizeof(dum),0)>0); }

        /* ════════════════  대기실  ════════════════ */
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT){ quit=1; peer_disconnected=1; break; }
                if (e.type==SDL_KEYDOWN){
                    if(e.key.keysym.sym==SDLK_q){ quit=1; peer_disconnected=1; break; }
                    if(e.key.keysym.sym==SDLK_SPACE) my_ready=1;
                }
                if (e.type==SDL_TEXTINPUT){
                    if(textinput_is(e.text.text,KOR_BI)){ quit=1; peer_disconnected=1; break; }
                }
            }
            if (quit) break;

            { Packet md={my_ready,0,0,0,0,0,0}; send(sock,&md,sizeof(md),0); }

            { Packet tp; int rl;
              while((rl=recv(sock,&tp,sizeof(tp),0))!=-1){
                  if(rl==0){peer_disconnected=1;break;}
                  peer=tp;
              }
            }
            if (peer_disconnected) break;

            SDL_SetRenderDrawColor(ren,240,248,255,255);
            SDL_RenderClear(ren);

            update_and_draw_clouds(ren, clouds, MAX_CLOUDS, 50); /* 로비는 기본 속도 */
            draw_ground(ren, MY_BASE_Y);

            int lf = 1+((SDL_GetTicks()/150)%2);
            draw_sprite(ren, tex_dino, DINO_X, MY_BASE_Y-4, 5,4, lf, 3, 0.0f, col_my);
            draw_sprite(ren, tex_cactus, DINO_X+30, MY_BASE_Y-2, 3,2, 0,1, 0.0f, col_cac);
            draw_sprite(ren, tex_ptero,
                        WIDTH-((SDL_GetTicks()/50)%WIDTH), 4, 4,2,
                        (SDL_GetTicks()/200)%2, 2, 0.0f, col_pt);

            if (font_large)
                draw_text_center(ren, font_large, "MULTI-DINO RUN", 1, col_my);
            if (font) {
                /* N1: 최고 점수 로비에서도 표시 */
                if (best_score > 0) {
                    char bs[64]; sprintf(bs,"Best: %05d", best_score);
                    draw_text_center(ren, font, bs, 4, col_gray);
                }

                int blink=(SDL_GetTicks()/500)%2;
                if (my_ready)
                    draw_text_center(ren,font,"READY!",6,col_my);
                else if (blink)
                    draw_text_center(ren,font,"> PRESS [SPACE] TO READY <",6,col_gray);

                /* Q7: 라벨에 서버/클라이언트 구분 반영 */
                char lb[64];
                sprintf(lb, "%s: %s", my_label, my_ready?"READY":"waiting...");
                draw_text_center(ren,font,lb,8,col_my);
                sprintf(lb, "%s: %s", peer_label, peer.is_ready?"READY!":"waiting...");
                draw_text_center(ren,font,lb,9,col_op);
            }

            SDL_RenderPresent(ren);
            SDL_Delay(50);

            if (my_ready && peer.is_ready) break;
        }
        if (peer_disconnected || quit) break;

        /* ════════════════  카운트다운  ════════════════ */
        if (font_large) {
            const char* ctd[] = {"3","2","1","GO!"};
            for (int ci=0; ci<4 && !quit; ci++) {
                /* C3: recv==0 이면 peer_disconnected 설정 */
                { Packet cd={1,DINO_BASE,0,WIDTH,1,0,0};
                  send(sock,&cd,sizeof(cd),0);
                  Packet ct; int rl;
                  while((rl=recv(sock,&ct,sizeof(ct),0))!=-1){
                      if(rl==0){peer_disconnected=1;break;}
                  }
                }
                if (peer_disconnected) break;

                SDL_SetRenderDrawColor(ren,240,248,255,255);
                SDL_RenderClear(ren);
                update_and_draw_clouds(ren,clouds,MAX_CLOUDS,50);
                draw_ground(ren,MY_BASE_Y);
                draw_ground(ren,OP_BASE_Y);
                draw_divider(ren);   /* N5 */
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,0,3,0.0f,col_my);
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-4,5,4,0,3,0.0f,col_op);
                SDL_Color cc=(ci<3)?(SDL_Color){200,60,60,255}:(SDL_Color){60,180,60,255};
                draw_text_center(ren,font_large,ctd[ci],6,cc);
                SDL_RenderPresent(ren);

                SDL_Event ec;
                while(SDL_PollEvent(&ec)) if(ec.type==SDL_QUIT) quit=1;
                SDL_Delay(ci<3?800:500);
            }
        }
        if (quit || peer_disconnected) break;

        /* 게임 변수 초기화 */
        int my_y        = DINO_BASE;
        int jump_stage  = 0;
        int obs_x       = WIDTH - 2;
        int obs_type    = 1;
        int score       = 0;
        int is_ducking  = 0;
        int my_dead     = 0;
        int show_hitbox = 0;
        int paused      = 0;   /* N2 */
        /* C4: 마지막 점수 스냅샷 */
        int my_score_final   = 0;
        int peer_score_final = 0;

        long long last_tick = get_time_ms();
        long long last_duck = 0;
        long long pause_start = 0;
        int  tick_interval = 50;
        int  global_tick   = 0;   /* Q6: 아래서 주기 리셋 */

        /* ════════════════  진행 루프  ════════════════ */
        while (!quit) {
            long long now = get_time_ms();

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT){ quit=1; peer_disconnected=1; break; }
                if (e.type==SDL_KEYDOWN) {
                    if (e.key.keysym.sym==SDLK_q){ quit=1; peer_disconnected=1; break; }
                    if (!my_dead) {
                        if (e.key.keysym.sym==SDLK_UP||e.key.keysym.sym==SDLK_SPACE){
                            if (!paused && jump_stage==0){ jump_stage=1; is_ducking=0; }
                        } else if (e.key.keysym.sym==SDLK_DOWN){
                            if (!paused) last_duck=now;
                        } else if (e.key.keysym.sym==SDLK_h){
                            show_hitbox=!show_hitbox;
                        } else if (e.key.keysym.sym==SDLK_p){ /* N2 */
                            paused=!paused;
                            if (paused) pause_start=now;
                            else        last_tick += (now-pause_start); /* 일시정지 시간 보정 */
                        }
                    }
                }
                if (e.type==SDL_TEXTINPUT){
                    if(textinput_is(e.text.text,KOR_BI)){ quit=1; peer_disconnected=1; break; }
                }
            }
            if (quit) break;

            /* 일시정지 중이면 로직 건너뜀 */
            if (!paused && !my_dead) {
                const Uint8* ks = SDL_GetKeyboardState(NULL);
                if (ks[SDL_SCANCODE_DOWN]) last_duck=now;
                is_ducking = (jump_stage==0 && now-last_duck<300) ? 1 : 0;
            }

            if (!paused && now-last_tick >= tick_interval) {
                last_tick = now;
                /* Q6: global_tick 오버플로우 방지 */
                global_tick = (global_tick + 1) % 10000;

                if (!my_dead) {
                    if (jump_stage>0){
                        if(jump_stage<=JUMP_STEPS) my_y=DINO_BASE-jump_map[jump_stage];
                        if(++jump_stage>JUMP_STEPS){ jump_stage=0; my_y=DINO_BASE; }
                    }

                    obs_x--;
                    score++;
                    /* C5: obs_type==0(익룡) 넓이 4로 수정 */
                    int obs_w = (obs_type==2)?5:(obs_type==3)?7:(obs_type==0||obs_type==4)?4:3;

                    if (obs_x+obs_w < 1){
                        obs_x    = WIDTH-2;
                        obs_type = rand()%5;
                        if (tick_interval>15) tick_interval--;
                    }

                    int eff_y = MY_BASE_Y+(my_y-DINO_BASE);
                    if (hitbox_overlap(dino_hitbox(eff_y,is_ducking),
                                       obs_hitbox(obs_x,obs_type,MY_BASE_Y)))
                        my_dead=1;
                }

                /* C4: 스냅샷 갱신 */
                my_score_final = score;
            }

            Packet md={1,my_y,is_ducking,obs_x,obs_type,score,my_dead};
            send(sock,&md,sizeof(md),0);

            { Packet tp; int rl;
              while((rl=recv(sock,&tp,sizeof(tp),0))!=-1){
                  if(rl==0){peer_disconnected=1;break;}
                  peer=tp;
              }
            }
            peer_score_final = peer.score;  /* C4 */

            if (peer_disconnected||(my_dead&&peer.is_dead)) break;

            /* ════  렌더링  ════ */
            SDL_SetRenderDrawColor(ren,255,255,255,255);
            SDL_RenderClear(ren);

            update_and_draw_clouds(ren,clouds,MAX_CLOUDS,tick_interval); /* N4 */

            draw_ground(ren,MY_BASE_Y);
            draw_ground(ren,OP_BASE_Y);
            draw_divider(ren);   /* N5 */

            int af_dino  = (jump_stage>0)?0:1+((global_tick/2)%2);
            int af_ptero = (global_tick/2)%2;

            /* 내 화면 */
            draw_obstacle(ren,tex_ptero,tex_cactus,obs_x,obs_type,MY_BASE_Y,
                          af_ptero,col_pt,col_cac);
            if (!my_dead){
                int joff=DINO_BASE-my_y, dh=is_ducking?2:4, dw=is_ducking?6:5;
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-dh-joff,dw,dh,
                            af_dino,3,0.0f,col_my);
            } else {
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,0,3,180.0f,(SDL_Color){80,80,80,255});
            }

            /* 상대 화면 */
            draw_obstacle(ren,tex_ptero,tex_cactus,peer.obs_x,peer.obs_type,OP_BASE_Y,
                          af_ptero,col_pt,col_cac);
            if (!peer.is_dead){
                int pjoff=DINO_BASE-peer.dino_y, ph=peer.is_ducking?2:4, pw=peer.is_ducking?6:5;
                int pa=(peer.dino_y!=DINO_BASE)?0:1+((global_tick/2)%2);
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-ph-pjoff,pw,ph,pa,3,0.0f,col_op);
            } else {
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-4,5,4,0,3,180.0f,(SDL_Color){80,80,80,255});
            }

            /* 히트박스 디버그 */
            if (show_hitbox){
                int ey=MY_BASE_Y+(my_y-DINO_BASE);
                HitBox dh=dino_hitbox(ey,is_ducking);
                SDL_SetRenderDrawColor(ren,255,0,0,180);
                SDL_Rect dr={dh.left*CELL_W,dh.top*CELL_H,
                             (dh.right-dh.left+1)*CELL_W,(dh.bot-dh.top+1)*CELL_H};
                SDL_RenderDrawRect(ren,&dr);
                if(obs_x>0&&obs_x<WIDTH-1){
                    HitBox oh=obs_hitbox(obs_x,obs_type,MY_BASE_Y);
                    SDL_SetRenderDrawColor(ren,0,0,255,180);
                    SDL_Rect or2={oh.left*CELL_W,oh.top*CELL_H,
                                 (oh.right-oh.left+1)*CELL_W,(oh.bot-oh.top+1)*CELL_H};
                    SDL_RenderDrawRect(ren,&or2);
                }

                int pey=OP_BASE_Y+(peer.dino_y-DINO_BASE);
                HitBox pdh=dino_hitbox(pey,peer.is_ducking);
                SDL_SetRenderDrawColor(ren,255,0,0,180);
                SDL_Rect pdr={pdh.left*CELL_W,pdh.top*CELL_H,
                              (pdh.right-pdh.left+1)*CELL_W,(pdh.bot-pdh.top+1)*CELL_H};
                SDL_RenderDrawRect(ren,&pdr);
                if(peer.obs_x>0&&peer.obs_x<WIDTH-1){
                    HitBox poh=obs_hitbox(peer.obs_x,peer.obs_type,OP_BASE_Y);
                    SDL_SetRenderDrawColor(ren,0,0,255,180);
                    SDL_Rect por={poh.left*CELL_W,poh.top*CELL_H,
                                  (poh.right-poh.left+1)*CELL_W,(poh.bot-poh.top+1)*CELL_H};
                    SDL_RenderDrawRect(ren,&por);
                }
            }

            /* 인게임 UI */
            if (font){
                /* N3: 레벨 표시 */
                int lv = calc_level(tick_interval);
                char b1[100], b2[100];
                sprintf(b1,"%s", my_label);
                sprintf(b2,"LV.%d  %05d", lv, score);
                draw_text(ren,font,b1,2,1,col_my);
                draw_text(ren,font,b2,WIDTH-18,1,col_gray);

                sprintf(b1,"%s %s", peer_label, peer.is_dead?"[DEAD]":"");
                sprintf(b2,"%05d", peer.score);
                draw_text(ren,font,b1,2,MY_BASE_Y+1,col_op);
                draw_text(ren,font,b2,WIDTH-10,MY_BASE_Y+1,col_gray);

                /* N1: 최고 점수 */
                if (best_score>0){
                    char bs[64]; sprintf(bs,"BEST: %05d",best_score);
                    draw_text(ren,font,bs,WIDTH-10,2,col_gray);
                }
            }

            /* N2: 일시정지 오버레이 */
            if (paused && font_large){
                draw_filled_rect_px(ren, WIN_W/4, WIN_H/4,
                                    WIN_W/2, WIN_H/2,
                                    (SDL_Color){230,230,230,210});
                draw_text_center(ren,font_large,"⏸ PAUSED",6,col_gray);
                if (font)
                    draw_text_center(ren,font,"[P] to resume",8,col_gray);
            }

            SDL_RenderPresent(ren);
            SDL_Delay(1000/60);
        }
        if (peer_disconnected || quit) break;

        /* N1: 최고 점수 갱신 (C4: 스냅샷 사용) */
        if (my_score_final > best_score) best_score = my_score_final;

        /* ════════════════  게임 오버  ════════════════ */
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)){
                if(e.type==SDL_QUIT) quit=1;
                if(e.type==SDL_KEYDOWN&&
                   (e.key.keysym.sym==SDLK_r||e.key.keysym.sym==SDLK_q)){
                    if(e.key.keysym.sym==SDLK_q) quit=1;
                    goto RESTART;
                }
                if(e.type==SDL_TEXTINPUT){
                    if(textinput_is(e.text.text,KOR_GIYOK)) goto RESTART;
                    if(textinput_is(e.text.text,KOR_BI)){quit=1;goto RESTART;}
                }
            }

            SDL_SetRenderDrawColor(ren,255,255,255,255);
            SDL_RenderClear(ren);

            if (font_large && font) {
                /* C2: 폰트 있을 때만 상세 표시 */
                draw_ground(ren,MY_BASE_Y);

                /* F4: 뒤집힌 공룡 두 마리 */
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,0,3,
                            180.0f,(SDL_Color){80,80,80,255});
                draw_sprite(ren,tex_dino,WIDTH-DINO_X-5,MY_BASE_Y-4,5,4,0,3,
                            180.0f,(SDL_Color){170,170,170,255});

                draw_text_center(ren,font_large,"==== GAME OVER ====",2,col_gray);

                /* C4: 스냅샷으로 최종 점수 표시 */
                char b1[64],b2[64];
                sprintf(b1,"My Score:   %05d",my_score_final);
                sprintf(b2,"Peer Score: %05d",peer_score_final);
                draw_text_center(ren,font,b1,6,col_gray);
                draw_text_center(ren,font,b2,7,col_gray);

                /* N1: 최고 점수 */
                {  char bs[64]; sprintf(bs,"Best This Session: %05d",best_score);
                   draw_text_center(ren,font,bs,8,(SDL_Color){100,100,200,255}); }

                /* N6: 승패 하이라이트 */
                if (my_score_final > peer_score_final)
                    draw_text_center_highlight(ren,font_large,">> YOU WIN! <<",11,
                                               (SDL_Color){30,160,30,255},col_green);
                else if (my_score_final < peer_score_final)
                    draw_text_center_highlight(ren,font_large,">> YOU LOSE... <<",11,
                                               (SDL_Color){220,50,50,255},col_red_bg);
                else
                    draw_text_center_highlight(ren,font_large,">> DRAW! <<",11,
                                               col_gray,(SDL_Color){200,200,200,180});

                /* F5: 한글 키 안내 */
                draw_text_center(ren,font,"[R / ㄱ] Restart    [Q / ㅂ] Quit",19,col_gray);
            } else {
                /* C2: 폰트 없을 때도 최소한의 안내 */
                SDL_SetRenderDrawColor(ren,200,200,200,255);
                SDL_Rect br1={WIN_W/2-100,WIN_H/2-30,200,50};
                SDL_Rect br2={WIN_W/2-100,WIN_H/2+30,200,50};
                SDL_RenderFillRect(ren,&br1);  /* R = restart */
                SDL_SetRenderDrawColor(ren,180,80,80,255);
                SDL_RenderFillRect(ren,&br2);  /* Q = quit */
            }

            SDL_RenderPresent(ren);
            SDL_Delay(50);
        }
        RESTART:;
    }

    /* 자원 해제 */
    if (font)       TTF_CloseFont(font);
    if (font_large) TTF_CloseFont(font_large);
    if (tex_dino)   SDL_DestroyTexture(tex_dino);
    if (tex_cactus) SDL_DestroyTexture(tex_cactus);
    if (tex_ptero)  SDL_DestroyTexture(tex_ptero);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit(); IMG_Quit(); SDL_Quit();
    close(sock);
    return 0;
}
