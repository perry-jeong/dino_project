/**
 * project_sdl_ef.c - SDL2 Multi-Player Dino Game [EF Edition]
 *
 * CD → EF 변경:
 *   B1: score = 생존 시간 (wall-clock 100ms 단위) — 장애물과 무관하게 공정
 *   B2: 일시정지 중 점프 물리 완전 동결
 *   B3: global_tick = SDL_GetTicks() 기반 독립 분리
 *   B4: obs_x off-by-one 수정
 *   F1: 공격 아이템 (장애물 5개 통과마다 획득, [A]키 발동)
 *   F2: 낮/밤 배경 자동 전환 (점수 1000점마다 페이즈 전환, 별/달 렌더링)
 *   F3: 포물선 점프 물리 + 더블점프 + 코요테 타임
 *   F4: SDL_mixer BGM/효과음 (파일 없으면 무음)
 *
 * 컴파일:
 *   gcc project_sdl_ef.c -o project_sdl_ef \
 *       -lSDL2 -lSDL2_image -lSDL2_ttf -lm
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
#include <math.h>

/* ── 논리 크기 ──────────────────────────────────────── */
#define WIDTH        80
#define HEIGHT       30
#define PORT         9000
#define CELL_W       12
#define CELL_H       24
#define WIN_W        (WIDTH  * CELL_W)
#define WIN_H        (HEIGHT * CELL_H)

/* ── 레이아웃 ────────────────────────────────────────── */
#define MY_BASE_Y    14
#define DINO_BASE    12
#define DINO_X        5
#define OP_BASE_Y    28
#define MAX_CLOUDS    5
#define MAX_STARS    60

/* ── F3: 점프 물리 상수 (시간 기준 — tick속도 영향 없음) ── */
/* vy 단위: rows/sec, 각 틱에서 dt = tick_interval/1000 을 사용해 Euler 적분 */
/* peak = VY²/(2*G) ≈6행, air = 2*VY/G ≈0.83초 (틱 속도와 무관)            */
#define JUMP_VY_SEC   30.0f   /* rows/sec 점프 초기 속도 */
#define GRAVITY_SEC   72.0f   /* rows/sec² 중력 */
#define MAX_FALL_SEC 200.0f   /* rows/sec 최대 낙하 속도 */
#define MAX_HEIGHT     8      /* 화면 밖 점프 방지 상한 (rows) */
#define COYOTE_MS     80
#define MAX_AIR_JUMPS  1



/* ── F1: 아이템 ─────────────────────────────────────── */
#define ITEM_CLEARS    5
#define ITEM_MAX       3

/* ── F2: 낮/밤 주기 (score 단위, elapsed_ms = score*10 으로 전달) ── */
/* 주기 = 2500 score: 낮1000 → 전환250 → 밤1000 → 전환250 → 반복 */
#define DN_CYCLE_MS   25000   /* 2500 score * 10 */
#define DN_DAY_MS     10000   /* 낮 유지 (1000 score) */
#define DN_TRANS_MS    2500   /* 전환 구간 (250 score) */
#define DN_NIGHT_MS   10000   /* 밤 유지 (1000 score) */

/* ── 레벨 계산 ─────────────────────────────────────── */
static inline int calc_level(int ti) {
    int l = (int)((50 - ti) / 3.5f) + 1;
    if (l < 1)  l = 1;
    if (l > 10) l = 10;
    return l;
}

/* ══ 구조체 ════════════════════════════════════════════ */

/* B1: score = 생존 시간 (100ms 단위); F1: attack_item 추가 */
typedef struct {
    int is_ready;
    int dino_y;
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;       /* 생존 시간 (100ms 단위, 일시정지 제외) */
    int is_dead;
    int attack_item; /* 0=없음 1=공격 발동 */
} Packet;

typedef struct { int top, bot, left, right; } HitBox;

/* F3: 점프 물리 상태 */
typedef struct {
    float height;          /* DINO_BASE 기준 위로 올라간 rows */
    float vy;              /* 속도 (양수=상승) */
    int   on_ground;
    int   air_jumps_left;  /* 남은 공중 점프 횟수 */
    long long last_ground_ms;
} PhysState;

typedef struct { float x; int y_row, w; float base_speed; } Cloud;

typedef struct { int x, y; Uint8 bright; } Star;

/* ══ 유틸 함수 ══════════════════════════════════════════ */

long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

void set_nonblocking(int sock) {
    int f = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);
}

static char g_base_dir[512];
void init_base_dir(const char *argv0) {
    char tmp[512];
    strncpy(tmp, argv0, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    strncpy(g_base_dir, dirname(tmp), sizeof(g_base_dir)-1);
    g_base_dir[sizeof(g_base_dir)-1] = '\0';
}
void asset_path(char *out, size_t sz, const char *file) {
    snprintf(out, sz, "%s/%s", g_base_dir, file);
}

/* UTF-8 → 코드포인트 비교 */
static int textinput_is(const char *t, unsigned int want) {
    unsigned char a=(unsigned char)t[0], b=(unsigned char)t[1], c=(unsigned char)t[2];
    if (!a||!b||!c||t[3]) return 0;
    return (((a&0x0F)<<12)|((b&0x3F)<<6)|(c&0x3F)) == want;
}
#define KOR_BI    0x3142u
#define KOR_GIYOK 0x3131u

/* ══ 충돌 판정 ══════════════════════════════════════════ */

HitBox dino_hitbox(int y, int is_ducking) {
    HitBox h;
    h.bot = y - 1;
    if (is_ducking) { h.top=y-2; h.left=DINO_X+1; h.right=DINO_X+4; }
    else            { h.top=y-4; h.left=DINO_X+1; h.right=DINO_X+3; }
    return h;
}

HitBox obs_hitbox(int ox, int ot, int ground) {
    HitBox h;
    if (ot==0) { h.top=ground-4; h.bot=ground-3; h.left=ox+1; h.right=ox+3; }
    else if (ot==4) { h.top=ground-3; h.bot=ground-1; h.left=ox+1; h.right=ox+2; }
    else {
        int w=(ot==2)?3:(ot==3)?5:2;
        h.top=ground-2; h.bot=ground-1; h.left=ox; h.right=ox+w-1;
    }
    return h;
}

int hitbox_overlap(HitBox a, HitBox b) {
    return (a.right>=b.left && a.left<=b.right && a.bot>=b.top && a.top<=b.bot);
}

/* ══ F3: 점프 물리 ══════════════════════════════════════ */

void phys_init(PhysState *p) {
    p->height = 0.0f; p->vy = 0.0f;
    p->on_ground = 1; p->air_jumps_left = MAX_AIR_JUMPS;
    p->last_ground_ms = 0;
}

void phys_jump(PhysState *p, long long now_ms) {
    /* vy 단위: rows/sec — tick 속도와 무관하게 항상 동일한 점프 높이 */
    if (p->on_ground || (now_ms - p->last_ground_ms < COYOTE_MS)) {
        p->vy = JUMP_VY_SEC;
        p->on_ground = 0;
        p->air_jumps_left = MAX_AIR_JUMPS;
    } else if (p->air_jumps_left > 0) {
        p->vy = JUMP_VY_SEC * 0.82f;
        p->air_jumps_left--;
    }
}

/* 1 tick 마다 호출: dt = tick_interval_ms / 1000.0f */
void phys_update(PhysState *p, long long now_ms, float dt) {
    if (p->on_ground) return;
    p->vy -= GRAVITY_SEC * dt;             /* Euler 적분 */
    if (p->vy < -MAX_FALL_SEC) p->vy = -MAX_FALL_SEC;
    p->height += p->vy * dt;
    if (p->height > (float)MAX_HEIGHT) {
        p->height = (float)MAX_HEIGHT;
        p->vy = 0.0f;
    }
    if (p->height <= 0.0f) {
        p->height = 0.0f; p->vy = 0.0f;
        p->on_ground = 1; p->last_ground_ms = now_ms;
        p->air_jumps_left = MAX_AIR_JUMPS;
    }
}



/* ══ F2: 낮/밤 색상 계산 ════════════════════════════════ */

typedef struct { Uint8 r,g,b; } RGB3;

static RGB3 dn_bg[3]     = {{240,248,255},{255,165,80},{18,18,45}};
static RGB3 dn_ground[3] = {{83,83,83},   {120,80,40}, {40,40,60}};
static RGB3 dn_dino[3]   = {{83,83,83},   {100,70,50}, {60,60,90}};

RGB3 rgb_lerp(RGB3 a, RGB3 b, float t) {
    RGB3 r;
    r.r=(Uint8)(a.r+(b.r-a.r)*t);
    r.g=(Uint8)(a.g+(b.g-a.g)*t);
    r.b=(Uint8)(a.b+(b.b-a.b)*t);
    return r;
}

/* F2: 낮/밤 색상 + night_a 계산
 *   주기 2500 score: 낮(1000) → 전환(250) → 밤(1000) → 전환(250) → 반복
 *   elapsed_ms = score * 10 으로 전달 */
void dn_get_colors(Uint32 elapsed_ms,
                   RGB3 *bg, RGB3 *ground, RGB3 *dino_col, float *night_a) {
    Uint32 pos = elapsed_ms % DN_CYCLE_MS;
    static const Uint32 T1 = DN_DAY_MS;                            /* 10000 */
    static const Uint32 T2 = DN_DAY_MS + DN_TRANS_MS;              /* 12500 */
    static const Uint32 T3 = DN_DAY_MS + DN_TRANS_MS + DN_NIGHT_MS;/* 22500 */

    float blend = 0.0f;
    int fi = 0, ti = 0;
    float na = 0.0f;

    if (pos < T1) {
        fi=0; ti=0; blend=0.0f; na=0.0f;          /* 낮 */
    } else if (pos < T2) {
        blend=(float)(pos-T1)/(float)DN_TRANS_MS;
        fi=0; ti=2; na=blend;                       /* 낮→밤 전환 */
    } else if (pos < T3) {
        fi=2; ti=2; blend=0.0f; na=1.0f;           /* 밤 */
    } else {
        blend=(float)(pos-T3)/(float)DN_TRANS_MS;
        fi=2; ti=0; na=1.0f-blend;                 /* 밤→낮 전환 */
    }

    *bg      = rgb_lerp(dn_bg[fi],     dn_bg[ti],     blend);
    *ground  = rgb_lerp(dn_ground[fi], dn_ground[ti], blend);
    *dino_col= rgb_lerp(dn_dino[fi],   dn_dino[ti],   blend);
    *night_a = na;
}


/* ══ 구름 / 별 초기화 ════════════════════════════════════ */

void init_clouds(Cloud clouds[], int n) {
    for (int i=0;i<n;i++) {
        clouds[i].x          = (float)(rand()%WIN_W);
        clouds[i].y_row      = 1+(rand()%5);
        clouds[i].w          = 3+(rand()%3);
        clouds[i].base_speed = 0.3f+(float)(rand()%10)/20.0f;
    }
}

void init_stars(Star stars[], int n) {
    for (int i=0;i<n;i++) {
        stars[i].x      = rand()%WIN_W;
        stars[i].y      = rand()%(WIN_H/2);
        stars[i].bright = (Uint8)(140+rand()%116);
    }
}

/* ══ SDL 렌더링 유틸 ════════════════════════════════════= */

void draw_sprite(SDL_Renderer *ren, SDL_Texture *tex,
                 int xc, int yr, int wc, int hr,
                 int fi, int tf, float angle, SDL_Color cm) {
    if (!tex) return;
    SDL_SetTextureColorMod(tex, cm.r, cm.g, cm.b);
    int tw, th; SDL_QueryTexture(tex,NULL,NULL,&tw,&th);
    int fw = tw/(tf>0?tf:1);
    SDL_Rect src={fi*fw,0,fw,th};
    SDL_Rect dst={xc*CELL_W,yr*CELL_H,wc*CELL_W,hr*CELL_H};
    SDL_RenderCopyEx(ren,tex,&src,&dst,angle,NULL,SDL_FLIP_NONE);
}

void draw_filled(SDL_Renderer *ren, int x,int y,int w,int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,c.a);
    SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(ren,&r);
}

void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt,
               int xc, int yr, SDL_Color col) {
    if (!font||!txt||!txt[0]) return;
    SDL_Surface *s=TTF_RenderUTF8_Solid(font,txt,col); if(!s) return;
    SDL_Texture *t=SDL_CreateTextureFromSurface(ren,s);
    if (!t){SDL_FreeSurface(s);return;}
    SDL_Rect d={xc*CELL_W,yr*CELL_H,s->w,s->h};
    SDL_RenderCopy(ren,t,NULL,&d);
    SDL_DestroyTexture(t); SDL_FreeSurface(s);
}

void draw_text_center(SDL_Renderer *ren, TTF_Font *font, const char *txt,
                      int yr, SDL_Color col) {
    if (!font||!txt||!txt[0]) return;
    SDL_Surface *s=TTF_RenderUTF8_Solid(font,txt,col); if(!s) return;
    SDL_Texture *t=SDL_CreateTextureFromSurface(ren,s);
    if (!t){SDL_FreeSurface(s);return;}
    SDL_Rect d={(WIN_W-s->w)/2,yr*CELL_H,s->w,s->h};
    SDL_RenderCopy(ren,t,NULL,&d);
    SDL_DestroyTexture(t); SDL_FreeSurface(s);
}

void draw_text_center_hl(SDL_Renderer *ren, TTF_Font *font, const char *txt,
                         int yr, SDL_Color fg, SDL_Color bg) {
    if (!font||!txt||!txt[0]) return;
    SDL_Surface *s=TTF_RenderUTF8_Solid(font,txt,fg); if(!s) return;
    SDL_Texture *t=SDL_CreateTextureFromSurface(ren,s);
    if (!t){SDL_FreeSurface(s);return;}
    int px=(WIN_W-s->w)/2, py=yr*CELL_H, pad=8;
    draw_filled(ren,px-pad,py-pad/2,s->w+pad*2,s->h+pad,bg);
    SDL_Rect d={px,py,s->w,s->h};
    SDL_RenderCopy(ren,t,NULL,&d);
    SDL_DestroyTexture(t); SDL_FreeSurface(s);
}

void draw_ground(SDL_Renderer *ren, int base_y, RGB3 gc) {
    SDL_SetRenderDrawColor(ren,gc.r,gc.g,gc.b,255);
    SDL_Rect r={0,base_y*CELL_H,WIN_W,3};
    SDL_RenderFillRect(ren,&r);
}

void draw_divider(SDL_Renderer *ren) {
    SDL_SetRenderDrawColor(ren,180,180,180,200);
    int y=((MY_BASE_Y+OP_BASE_Y)/2)*CELL_H;
    for (int x=0;x<WIN_W;x+=20){SDL_Rect r={x,y,10,2};SDL_RenderFillRect(ren,&r);}
}

/* F2: 별 렌더링 */
void draw_stars(SDL_Renderer *ren, Star stars[], int n, float night_alpha) {
    if (night_alpha <= 0.0f) return;
    Uint32 ms = SDL_GetTicks();
    for (int i=0;i<n;i++) {
        float tw = 0.7f + 0.3f*sinf(ms*0.002f + i*0.7f);
        Uint8 a = (Uint8)(night_alpha * tw * stars[i].bright);
        SDL_SetRenderDrawColor(ren,stars[i].bright,stars[i].bright,stars[i].bright,a);
        SDL_RenderDrawPoint(ren,stars[i].x,stars[i].y);
        SDL_RenderDrawPoint(ren,stars[i].x+1,stars[i].y);
    }
}

/* F2: 달 렌더링 */
void draw_moon(SDL_Renderer *ren, float night_alpha) {
    if (night_alpha <= 0.05f) return;
    Uint8 a=(Uint8)(night_alpha*230);
    SDL_SetRenderDrawColor(ren,240,240,200,a);
    SDL_Rect moon={WIN_W-100,30,40,40};
    SDL_RenderFillRect(ren,&moon);
    SDL_SetRenderDrawColor(ren,18,18,45,a);
    SDL_Rect cut={WIN_W-93,28,40,40};
    SDL_RenderFillRect(ren,&cut);
}

/* F2: 구름 갱신+그리기 */
void update_draw_clouds(SDL_Renderer *ren, Cloud clouds[], int n,
                        int tick_interval, RGB3 cloud_col) {
    float sm=50.0f/(float)tick_interval;
    SDL_SetRenderDrawColor(ren,cloud_col.r,cloud_col.g,cloud_col.b,160);
    for (int i=0;i<n;i++) {
        clouds[i].x -= clouds[i].base_speed*sm;
        if (clouds[i].x+clouds[i].w*CELL_W<0) {
            clouds[i].x=(float)WIN_W;
            clouds[i].y_row=1+(rand()%5);
            clouds[i].w=3+(rand()%3);
            clouds[i].base_speed=0.3f+(float)(rand()%10)/20.0f;
        }
        SDL_Rect r={(int)clouds[i].x,clouds[i].y_row*CELL_H,clouds[i].w*CELL_W,CELL_H/2};
        SDL_RenderFillRect(ren,&r);
    }
}

/* B4: obs_x < 0 조건으로 수정 (기존 obs_x <= 0 이 off-by-one) */
void draw_obstacle(SDL_Renderer *ren, SDL_Texture *tp, SDL_Texture *tc,
                   int ox, int ot, int base_y,
                   int ap, SDL_Color pc, SDL_Color cc) {
    if (ox < 0 || ox >= WIDTH) return; /* B4 수정 */
    if (ot==0) {
        draw_sprite(ren,tp,ox,base_y-4,4,2,ap,2,0.0f,pc);
    } else {
        int ch=(ot==4)?3:2;
        int cnt=(ot==2)?2:(ot==3)?3:1;
        for (int i=0;i<cnt;i++)
            draw_sprite(ren,tc,ox+(i*2),base_y-ch,3,ch,0,1,0.0f,cc);
    }
}

/* ══ main ════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1],"client")==0 && argc<3)) {
        printf("Usage:\n  방장:   %s server\n  접속자: %s client [방장IP]\n",
               argv[0], argv[0]);
        return 1;
    }
    init_base_dir(argv[0]);

    /* ── 네트워크 설정 ─────────────────────────────────── */
    int sock;
    int is_server = (strcmp(argv[1],"server")==0);

    if (is_server) {
        int ss = socket(PF_INET,SOCK_STREAM,0);
        int opt=1; setsockopt(ss,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        struct sockaddr_in sa, ca; socklen_t ca_sz=sizeof(ca);
        memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET; sa.sin_addr.s_addr=htonl(INADDR_ANY);
        sa.sin_port=htons(PORT);
        if (bind(ss,(struct sockaddr*)&sa,sizeof(sa))<0){perror("bind");return 1;}
        listen(ss,1);
        printf("[EF 방장] 포트 %d 대기 중...\n",PORT);
        sock=accept(ss,(struct sockaddr*)&ca,&ca_sz);
        close(ss);
    } else {
        sock=socket(PF_INET,SOCK_STREAM,0);
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family=AF_INET; sa.sin_addr.s_addr=inet_addr(argv[2]);
        sa.sin_port=htons(PORT);
        printf("[EF 접속] %s 연결 중...\n",argv[2]);
        if (connect(sock,(struct sockaddr*)&sa,sizeof(sa))==-1){
            printf("접속 실패!\n"); return 1;
        }
    }
    set_nonblocking(sock);

    /* ── SDL 초기화 ────────────────────────────────────── */
    if (SDL_Init(SDL_INIT_VIDEO)<0){printf("SDL: %s\n",SDL_GetError());return 1;}
    if (!(IMG_Init(IMG_INIT_PNG)&IMG_INIT_PNG)){printf("IMG: %s\n",IMG_GetError());return 1;}
    if (TTF_Init()==-1){printf("TTF: %s\n",TTF_GetError());return 1;}

    SDL_Window *win=SDL_CreateWindow(
        "Multi-Player Dino [EF Edition]",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        WIN_W,WIN_H,SDL_WINDOW_SHOWN);
    SDL_Renderer *ren=SDL_CreateRenderer(win,-1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);

    /* ── 에셋 로드 ─────────────────────────────────────── */
    char pb[600];
#define LOAD_TEX(var,file) \
    asset_path(pb,sizeof(pb),file); \
    { SDL_Surface *s=IMG_Load(pb); \
      var=s?SDL_CreateTextureFromSurface(ren,s):NULL; \
      if(var)SDL_SetTextureBlendMode(var,SDL_BLENDMODE_BLEND); \
      if(s)SDL_FreeSurface(s); }

    SDL_Texture *tex_dino, *tex_cactus, *tex_ptero;
    LOAD_TEX(tex_dino,   "dino_anim.png")
    LOAD_TEX(tex_cactus, "cactus_clean.png")
    LOAD_TEX(tex_ptero,  "ptero_anim.png")
    if (!tex_dino||!tex_cactus||!tex_ptero)
        printf("경고: 일부 스프라이트 로드 실패 (dir=%s)\n",g_base_dir);

    asset_path(pb,sizeof(pb),"NanumGothic.ttf");
    TTF_Font *font       = TTF_OpenFont(pb,24);
    TTF_Font *font_large = TTF_OpenFont(pb,48);
    if (!font||!font_large) printf("경고: 폰트 로드 실패\n");


    srand((unsigned)time(NULL)+(is_server?1:2));

    /* ── 공통 색상 ─────────────────────────────────────── */
    SDL_Color col_gray  ={83, 83, 83, 255};
    SDL_Color col_light ={170,170,170,255};
    SDL_Color col_green ={60, 180,60, 180};
    SDL_Color col_red_bg={200,60, 60, 180};
    SDL_Color col_op    = col_light;

    const char *my_label   = is_server ? "P1 (Me)"   : "P2 (Me)";
    const char *peer_label = is_server ? "P2 (Peer)" : "P1 (Peer)";

    /* ── 구름 / 별 초기화 ──────────────────────────────── */
    Cloud clouds[MAX_CLOUDS]; init_clouds(clouds,MAX_CLOUDS);
    Star  stars[MAX_STARS];   init_stars(stars,MAX_STARS);

    int best_score=0, quit=0;

    /* ════════════════════════════════════════════════
       외부 게임 루프 (라운드 반복)
       ════════════════════════════════════════════════ */
    while (!quit) {
        int peer_disconnected=0, my_ready=0;
        Packet peer={0,DINO_BASE,0,WIDTH,1,0,0,0};

        /* 소켓 버퍼 비우기 */
        { Packet dum; while(recv(sock,&dum,sizeof(dum),0)>0); }

        /* ── 낮/밤 기준 시각 (라운드마다 리셋) ── */
        Uint32 dn_start = SDL_GetTicks();

        /* ════════  대기실  ════════ */
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT){quit=1;peer_disconnected=1;break;}
                if (e.type==SDL_KEYDOWN){
                    if(e.key.keysym.sym==SDLK_q){quit=1;peer_disconnected=1;break;}
                    if(e.key.keysym.sym==SDLK_SPACE) my_ready=1;
                }
                if (e.type==SDL_TEXTINPUT)
                    if(textinput_is(e.text.text,KOR_BI)){quit=1;peer_disconnected=1;break;}
            }
            if (quit) break;

            { Packet md={my_ready,0,0,0,0,0,0,0};
              send(sock,&md,sizeof(md),0); }
            { Packet tp; int rl;
              while((rl=recv(sock,&tp,sizeof(tp),0))!=-1){
                  if(rl==0){peer_disconnected=1;break;}
                  peer=tp;
              }
            }
            if (peer_disconnected) break;

            /* 낮/밤 배경 (대기실: 시간 기반) */
            Uint32 elapsed=SDL_GetTicks()-dn_start;
            RGB3 bg_c, gr_c, dc_c;
            float night_a=0.0f;
            dn_get_colors(elapsed,&bg_c,&gr_c,&dc_c,&night_a);

            SDL_SetRenderDrawColor(ren,bg_c.r,bg_c.g,bg_c.b,255);
            SDL_RenderClear(ren);

            draw_stars(ren,stars,MAX_STARS,night_a);
            draw_moon(ren,night_a);

            RGB3 cloud_col={210,215,220};
            update_draw_clouds(ren,clouds,MAX_CLOUDS,50,cloud_col);
            draw_ground(ren,MY_BASE_Y,gr_c);

            SDL_Color dc={(Uint8)dc_c.r,(Uint8)dc_c.g,(Uint8)dc_c.b,255};
            int lf=1+((SDL_GetTicks()/150)%2);
            draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,lf,3,0.0f,dc);
            draw_sprite(ren,tex_cactus,DINO_X+30,MY_BASE_Y-2,3,2,0,1,0.0f,dc);
            draw_sprite(ren,tex_ptero,
                        WIDTH-((SDL_GetTicks()/50)%WIDTH),4,4,2,
                        (SDL_GetTicks()/200)%2,2,0.0f,dc);

            if (font_large)
                draw_text_center(ren,font_large,"MULTI-DINO RUN",1,col_gray);
            if (font) {
                if (best_score>0){
                    char bs[64]; sprintf(bs,"Best: %05d",best_score);
                    draw_text_center(ren,font,bs,4,col_gray);
                }
                int blink=(SDL_GetTicks()/500)%2;
                if (my_ready)
                    draw_text_center(ren,font,"READY!",6,col_gray);
                else if (blink)
                    draw_text_center(ren,font,"> PRESS [SPACE] TO READY <",6,col_gray);
                char lb[80];
                sprintf(lb,"%s: %s",my_label,my_ready?"READY":"waiting...");
                draw_text_center(ren,font,lb,8,col_gray);
                sprintf(lb,"%s: %s",peer_label,peer.is_ready?"READY!":"waiting...");
                draw_text_center(ren,font,lb,9,col_op);
                draw_text_center(ren,font,"[A]: 공격 아이템  [P]: 일시정지  [H]: 히트박스",11,col_gray);
            }

            SDL_RenderPresent(ren);
            SDL_Delay(50);
            if (my_ready&&peer.is_ready) break;
        }
        if (peer_disconnected||quit) break;

        /* ════════  카운트다운  ════════ */
        if (font_large) {
            const char *ctd[]={"3","2","1","GO!"};
            for (int ci=0;ci<4&&!quit;ci++) {
                { Packet cd={1,DINO_BASE,0,WIDTH,1,0,0,0};
                  send(sock,&cd,sizeof(cd),0);
                  Packet ct; int rl;
                  while((rl=recv(sock,&ct,sizeof(ct),0))!=-1)
                      if(rl==0){peer_disconnected=1;break;}
                }
                if (peer_disconnected) break;

                Uint32 elapsed=SDL_GetTicks()-dn_start;
                RGB3 bg_c,gr_c,dc_c;
                float night_a=0.0f;
                dn_get_colors(elapsed,&bg_c,&gr_c,&dc_c,&night_a);

                SDL_SetRenderDrawColor(ren,bg_c.r,bg_c.g,bg_c.b,255);
                SDL_RenderClear(ren);
                RGB3 cloud_col={210,215,220};
                update_draw_clouds(ren,clouds,MAX_CLOUDS,50,cloud_col);
                draw_ground(ren,MY_BASE_Y,gr_c);
                draw_ground(ren,OP_BASE_Y,gr_c);
                draw_divider(ren);

                SDL_Color dc={(Uint8)dc_c.r,(Uint8)dc_c.g,(Uint8)dc_c.b,255};
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,0,3,0.0f,dc);
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-4,5,4,0,3,0.0f,col_op);

                SDL_Color cc=(ci<3)?(SDL_Color){200,60,60,255}:(SDL_Color){60,180,60,255};
                draw_text_center(ren,font_large,ctd[ci],6,cc);
                SDL_RenderPresent(ren);

                SDL_Event ec;
                while(SDL_PollEvent(&ec)) if(ec.type==SDL_QUIT) quit=1;
                SDL_Delay(ci<3?800:500);
            }
        }
        if (quit||peer_disconnected) break;

        /* ════════════════════════════════════════════════
           게임 변수 초기화
           ════════════════════════════════════════════════ */
        /* F3: 포물선 점프 물리 */
        PhysState phy; phys_init(&phy);

        int obs_x      = WIDTH - 2;
        int obs_type   = 1;
        int is_ducking = 0;
        int my_dead    = 0;
        int show_hitbox= 0;
        int paused     = 0;

        /* B1: 점수 = 생존 시간 (100ms 단위, 일시정지 제외, 죽는 순간 동결) */
        int score      = 0;
        int my_score_final   = 0;
        int peer_score_final = 0;
        long long game_start_ms  = get_time_ms(); /* 생존 시간 기준점 */
        long long total_paused_ms = 0;            /* 누적 일시정지 시간 */

        /* F1: 공격 아이템 */
        int my_items         = 0;   /* 보유 아이템 수 */
        int clears_for_item  = 0;   /* 다음 아이템까지 통과 횟수 */
        int send_attack      = 0;   /* 이번 틱에 공격 전송 플래그 */
        int recv_attack_ticks= 0;   /* 공격 받았을 때 표시 타이머 */
        int send_notify_ticks= 0;   /* 공격 보냈을 때 표시 타이머 */
        int forced_obs       = 0;   /* 1이면 장애물 강제 근접 소환 */

        long long last_tick  = get_time_ms();
        long long last_duck  = 0;
        long long pause_start= 0;
        int tick_interval    = 50;

        /* B3: global_tick은 SDL_GetTicks() 기반으로 독립 계산 */
        /* (렌더링 시 SDL_GetTicks() 직접 사용) */

        /* ════════  진행 루프  ════════ */
        while (!quit) {
            long long now = get_time_ms();

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT){quit=1;peer_disconnected=1;break;}
                if (e.type==SDL_KEYDOWN && e.key.repeat==0) {  /* repeat==0: 키 홀드 무시 */
                    if (e.key.keysym.sym==SDLK_q){quit=1;peer_disconnected=1;break;}
                    if (!my_dead) {
                        /* F3: 점프 (코요테 타임 / 더블점프 포함) */
                        if (e.key.keysym.sym==SDLK_UP||e.key.keysym.sym==SDLK_SPACE) {
                            if (!paused) {
                                phys_jump(&phy, now);
                            }
                        }
                        if (e.key.keysym.sym==SDLK_DOWN && !paused) last_duck=now;
                        if (e.key.keysym.sym==SDLK_h) show_hitbox=!show_hitbox;
                        /* B2: 일시정지 — pause_start 저장 */
                        if (e.key.keysym.sym==SDLK_p) {
                            paused=!paused;
                            if (paused) {
                                pause_start=now;
                            } else {
                                long long pd = now - pause_start;
                                last_tick       += pd; /* 틱 타이머 보정 */
                                total_paused_ms += pd; /* 점수에서 제외할 시간 누적 */
                            }
                        }
                        /* F1: 아이템 사용 */
                        if (e.key.keysym.sym==SDLK_a && my_items>0 && !paused) {
                            my_items--;
                            send_attack=1;
                            send_notify_ticks=90;
                        }
                    }
                }
                if (e.type==SDL_TEXTINPUT)
                    if(textinput_is(e.text.text,KOR_BI)){quit=1;peer_disconnected=1;break;}
            }
            if (quit) break;

            /* 오리기 (B2: 일시정지 중 처리 안 함) */
            if (!paused && !my_dead) {
                const Uint8 *ks=SDL_GetKeyboardState(NULL);
                if (ks[SDL_SCANCODE_DOWN]) last_duck=now;
                is_ducking=(phy.on_ground && now-last_duck<300)?1:0;
            }

            /* 전프렌임 my_y_int 계산: 틱 바락 + 네트워크 송신 공통사용 */
            int my_y_int = DINO_BASE - (int)phy.height;

            /* ── 게임 틱 ── */
            if (!paused && now-last_tick>=tick_interval) {
                last_tick=now;

                if (!my_dead) {
                    /* B2: 일시정지 중 점프 물리 동결 — 틱 안에서만 업데이트 */
                    phys_update(&phy, now, (float)tick_interval / 1000.0f);

                    /* B1: 생존 시간 = (경과 시간 - 일시정지 시간) / 100ms */
                    score = (int)((now - game_start_ms - total_paused_ms) / 100);

                    obs_x--;
                    int obs_w=(obs_type==2)?5:(obs_type==3)?7:
                              (obs_type==0||obs_type==4)?4:3;

                    if (obs_x+obs_w<1) {
                        clears_for_item++;
                        if (clears_for_item>=ITEM_CLEARS && my_items<ITEM_MAX) {
                            my_items++;
                            clears_for_item=0;
                        }

                        /* F1: 공격 받은 경우 장애물 가까이 소환 */
                        obs_x = forced_obs ? WIDTH*2/3 : WIDTH-2;
                        forced_obs=0;
                        obs_type=rand()%5;
                        if (tick_interval>15) tick_interval--;
                    }

                    /* 충돌 판정 */
                    int eff_y = MY_BASE_Y + (my_y_int - DINO_BASE);
                    if (hitbox_overlap(dino_hitbox(eff_y,is_ducking),
                                       obs_hitbox(obs_x,obs_type,MY_BASE_Y))) {
                        my_dead=1;
                    }
                    my_score_final=score; /* 사망 시점의 점수 동결 */
                }

                /* 알림 타이머 감소 */
                if (recv_attack_ticks>0) recv_attack_ticks--;
                if (send_notify_ticks>0) send_notify_ticks--;
            }

            /* ── 네트워크 송수신 ── */
            Packet md={1,my_y_int,is_ducking,obs_x,obs_type,
                       score,my_dead,send_attack};
            send(sock,&md,sizeof(md),0);
            send_attack=0;

            { Packet tp; int rl;
              while((rl=recv(sock,&tp,sizeof(tp),0))!=-1){
                  if(rl==0){peer_disconnected=1;break;}
                  peer=tp;
              }
            }
            peer_score_final=peer.score;

            /* F1: 공격 받으면 플래그 설정 (recv_attack_ticks==0 가드로 중복 방지) */
            if (peer.attack_item==1 && !my_dead && recv_attack_ticks==0) {
                forced_obs=1;
                recv_attack_ticks=90;
            }

            if (peer_disconnected||(my_dead&&peer.is_dead)) break;

            /* ════  렌더링  ════ */
            /* B3: global_tick = SDL_GetTicks() 기반 */
            Uint32 ms_now   = SDL_GetTicks();
            int global_tick = (int)(ms_now / tick_interval);

            /* F2: 낮/밤 색상 — score 1000점마다 낮↔밤, 전환은 250점 구간 */
            Uint32 elapsed = (Uint32)(score * 10);
            RGB3 bg_c, gr_c, dc_c;
            float night_a = 0.0f;
            dn_get_colors(elapsed,&bg_c,&gr_c,&dc_c,&night_a);

            SDL_SetRenderDrawColor(ren,bg_c.r,bg_c.g,bg_c.b,255);
            SDL_RenderClear(ren);

            draw_stars(ren,stars,MAX_STARS,night_a);
            draw_moon(ren,night_a);

            RGB3 cloud_col={210,215,220};
            update_draw_clouds(ren,clouds,MAX_CLOUDS,tick_interval,cloud_col);

            draw_ground(ren,MY_BASE_Y,gr_c);
            draw_ground(ren,OP_BASE_Y,gr_c);
            draw_divider(ren);

            SDL_Color dc={(Uint8)dc_c.r,(Uint8)dc_c.g,(Uint8)dc_c.b,255};

            int af_dino  = (phy.height>0.5f)?0:1+((global_tick/2)%2);
            int af_ptero = (global_tick/2)%2;

            /* 내 화면 */
            draw_obstacle(ren,tex_ptero,tex_cactus,obs_x,obs_type,
                          MY_BASE_Y,af_ptero,dc,dc);
            if (!my_dead) {
                int joff=(int)phy.height;
                int dh=is_ducking?2:4, dw=is_ducking?6:5;
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-dh-joff,dw,dh,
                            af_dino,3,0.0f,dc);
            } else {
                draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,
                            0,3,180.0f,(SDL_Color){80,80,80,255});
            }

            /* 상대 화면 */
            draw_obstacle(ren,tex_ptero,tex_cactus,peer.obs_x,peer.obs_type,
                          OP_BASE_Y,af_ptero,col_op,col_op);
            if (!peer.is_dead) {
                int pjoff=DINO_BASE-peer.dino_y;
                int ph=peer.is_ducking?2:4, pw=peer.is_ducking?6:5;
                int pa=(pjoff>0)?0:1+((global_tick/2)%2);
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-ph-pjoff,pw,ph,
                            pa,3,0.0f,col_op);
            } else {
                draw_sprite(ren,tex_dino,DINO_X,OP_BASE_Y-4,5,4,
                            0,3,180.0f,(SDL_Color){80,80,80,255});
            }

            /* 히트박스 디버그 */
            if (show_hitbox) {
                /* 충돌 판정과 동일한 eff_y 계산 */
                int hb_y_int = DINO_BASE - (int)phy.height;
                int ey = MY_BASE_Y + (hb_y_int - DINO_BASE);
                HitBox dh2=dino_hitbox(ey,is_ducking);
                SDL_SetRenderDrawColor(ren,255,0,0,180);
                SDL_Rect dr={dh2.left*CELL_W,dh2.top*CELL_H,
                             (dh2.right-dh2.left+1)*CELL_W,(dh2.bot-dh2.top+1)*CELL_H};
                SDL_RenderDrawRect(ren,&dr);
                if (obs_x>=0&&obs_x<WIDTH) {
                    HitBox oh=obs_hitbox(obs_x,obs_type,MY_BASE_Y);
                    SDL_SetRenderDrawColor(ren,0,0,255,180);
                    SDL_Rect or2={oh.left*CELL_W,oh.top*CELL_H,
                                  (oh.right-oh.left+1)*CELL_W,(oh.bot-oh.top+1)*CELL_H};
                    SDL_RenderDrawRect(ren,&or2);
                }
            }

            /* ── 인게임 UI ── */
            if (font) {
                int lv=calc_level(tick_interval);
                char b1[120],b2[80];

                /* 내 정보 (B1: 생존 점수 표시) */
                sprintf(b1,"%s",my_label);
                sprintf(b2,"LV.%d  %05d",lv,score);
                draw_text(ren,font,b1,2,1,dc);
                draw_text(ren,font,b2,WIDTH-20,1,(SDL_Color){(Uint8)gr_c.r,(Uint8)gr_c.g,(Uint8)gr_c.b,255});

                /* F1: 아이템 UI */
                {
                    char itm[32]="[A] ";
                    for(int i=0;i<ITEM_MAX;i++)
                        strcat(itm, i<my_items ? "●" : "○");
                    draw_text(ren,font,itm,WIDTH-10,3,
                              (SDL_Color){220,180,40,255});
                }

                /* 상대 정보 */
                sprintf(b1,"%s %s",peer_label,peer.is_dead?"[DEAD]":"");
                sprintf(b2,"%05d",peer.score);
                draw_text(ren,font,b1,2,MY_BASE_Y+1,col_op);
                draw_text(ren,font,b2,WIDTH-10,MY_BASE_Y+1,col_op);

                /* 최고 기록 */
                if (best_score>0) {
                    char bs[64]; sprintf(bs,"BEST:%05d",best_score);
                    draw_text(ren,font,bs,WIDTH-10,2,
                              (SDL_Color){100,100,200,255});
                }

                /* F1: 공격 알림 */
                if (send_notify_ticks>0 && (ms_now/200)%2) {
                    draw_text_center(ren,font,"⚡ ATTACK SENT!",MY_BASE_Y-6,
                                     (SDL_Color){255,200,0,255});
                }
                if (recv_attack_ticks>0 && (ms_now/150)%2) {
                    draw_text_center(ren,font,"!! DANGER !!",2,
                                     (SDL_Color){255,50,50,255});
                }
            }

            /* 일시정지 오버레이 */
            if (paused && font_large) {
                draw_filled(ren,WIN_W/4,WIN_H/4,WIN_W/2,WIN_H/2,
                            (SDL_Color){230,230,230,210});
                draw_text_center(ren,font_large,"PAUSED",6,
                                 (SDL_Color){83,83,83,255});
                if (font)
                    draw_text_center(ren,font,"[P] to resume",8,
                                     (SDL_Color){83,83,83,255});
            }

            SDL_RenderPresent(ren);
            SDL_Delay(1000/60);
        } /* 진행 루프 끝 */

        if (peer_disconnected||quit) break;

        /* N1: 최고 기록 갱신 */
        if (my_score_final>best_score) best_score=my_score_final;

        /* ════════════════════════════════════════════════
           게임 오버 화면
           ════════════════════════════════════════════════ */
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type==SDL_QUIT) quit=1;
                if (e.type==SDL_KEYDOWN &&
                    (e.key.keysym.sym==SDLK_r||e.key.keysym.sym==SDLK_q)){
                    if(e.key.keysym.sym==SDLK_q) quit=1;
                    goto RESTART;
                }
                if (e.type==SDL_TEXTINPUT) {
                    if(textinput_is(e.text.text,KOR_GIYOK)) goto RESTART;
                    if(textinput_is(e.text.text,KOR_BI)){quit=1;goto RESTART;}
                }
            }

            /* F2: 게임오버 화면에서도 마지막 점수 기준 유지 */
            Uint32 elapsed = (Uint32)(my_score_final * 10);
            RGB3 bg_c,gr_c,dc_c;
            float night_a=0.0f;
            dn_get_colors(elapsed,&bg_c,&gr_c,&dc_c,&night_a);


            SDL_SetRenderDrawColor(ren,bg_c.r,bg_c.g,bg_c.b,255);
            SDL_RenderClear(ren);
            draw_ground(ren,MY_BASE_Y,gr_c);

            SDL_Color dc={(Uint8)dc_c.r,(Uint8)dc_c.g,(Uint8)dc_c.b,255};
            draw_sprite(ren,tex_dino,DINO_X,MY_BASE_Y-4,5,4,
                        0,3,180.0f,(SDL_Color){80,80,80,255});
            draw_sprite(ren,tex_dino,WIDTH-DINO_X-5,MY_BASE_Y-4,5,4,
                        0,3,180.0f,(SDL_Color){170,170,170,255});

            if (font_large&&font) {
                draw_text_center(ren,font_large,"==== GAME OVER ====",2,dc);

                char b1[80],b2[80];
                sprintf(b1,"My Score:   %05d",my_score_final);
                sprintf(b2,"Peer Score: %05d",peer_score_final);
                draw_text_center(ren,font,b1,6,dc);
                draw_text_center(ren,font,b2,7,col_op);

                char bs[80]; sprintf(bs,"Best This Session: %05d",best_score);
                draw_text_center(ren,font,bs,8,(SDL_Color){100,100,200,255});

                /* 승패 판정 (B1: 생존 시간 비교 — 더 오래 산 플레이어가 승리) */
                if (my_score_final>peer_score_final)
                    draw_text_center_hl(ren,font_large,">> YOU WIN! <<",11,
                                        (SDL_Color){30,160,30,255},col_green);
                else if (my_score_final<peer_score_final)
                    draw_text_center_hl(ren,font_large,">> YOU LOSE... <<",11,
                                        (SDL_Color){220,50,50,255},col_red_bg);
                else
                    draw_text_center_hl(ren,font_large,">> DRAW! <<",11,
                                        dc,(SDL_Color){200,200,200,180});

                draw_text_center(ren,font,"[R / ㄱ] Restart    [Q / ㅂ] Quit",
                                 19,dc);
            }

            SDL_RenderPresent(ren);
            SDL_Delay(50);
        }
        RESTART:;
    }

    /* ── 자원 해제 ─────────────────────────────────────── */
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
