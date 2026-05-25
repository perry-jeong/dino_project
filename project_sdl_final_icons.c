/**
 * project_sdl_final_icons.c - MULTI-DINO RUN [Icon Battle Edition]
 *
 * project_sdl_final.c 기반 아이콘 UI 반영 개발용 버전
 *
 * 추가/수정 기능:
 *  1) 시작 메뉴: DUO MODE / SOLO MODE / RECORDS / HOW TO PLAY
 *  2) ESC 또는 메뉴 버튼을 이용한 뒤로가기 흐름
 *  3) 프로그램 실행 중에만 유지되는 솔로/듀오 기록 목록
 *  4) 듀오 모드 공격 아이템 3종: 아이콘 UI로 표시
 *     - 주황 이중화살표: 다음 장애물 급습
 *     - 보라 눈가림 아이콘: 상대 시야 방해
 *     - 노랑 번개 아이콘: 상대 장애물 가속
 *     - 장애물 5개 회피 시 3종 중 하나 랜덤 획득
 *     - A키로 가장 먼저 받은 아이템 사용
 *  5) 듀오 모드는 한 플레이어가 죽는 순간 즉시 라운드 종료
 *  6) 공격 신호를 누적 번호(attack_seq)로 전송하여 단발 패킷 누락 위험 완화
 *  7) 듀오 진행 중 P 일시정지는 공정성 문제로 제거
 *
 * 필요 파일(실행 파일과 같은 폴더):
 *   dino_anim.png, cactus_clean.png, ptero_anim.png, NanumGothic.ttf
 *
 * Ubuntu 컴파일:
 *   gcc project_sdl_final_icons.c -o project_sdl_final_icons \
 *       -lSDL2 -lSDL2_image -lSDL2_ttf -lm
 *
 * 실행:
 *   ./project_sdl_final_icons
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>

/* ── 화면/레이아웃 ───────────────────────────────────────── */
#define WIDTH              80
#define HEIGHT             30
#define CELL_W             12
#define CELL_H             24
#define WIN_W              (WIDTH * CELL_W)
#define WIN_H              (HEIGHT * CELL_H)
#define PORT               9000
#define DUO_MY_BASE_Y      14
#define DUO_OP_BASE_Y      28
#define SOLO_BASE_Y        24
#define DINO_BASE          12
#define DINO_X              5
#define MAX_CLOUDS          5
#define MAX_STARS          60

/* ── 게임 규칙 ───────────────────────────────────────────── */
#define ITEM_CLEARS         5
#define ITEM_SLOTS          3
#define ITEM_USE_COOLDOWN 500LL
#define BLIND_DURATION    1500LL
#define SPEED_DURATION    2000LL
#define NOTICE_DURATION   1500LL
#define MAX_RECORDS        100

/* ── 점프 물리: 실제 시간 기반 ───────────────────────────── */
#define JUMP_VY_SEC        26.0f
#define GRAVITY_SEC        52.0f
#define MAX_FALL_SEC      200.0f
#define MAX_HEIGHT          8.0f

/* ── 통신 ─────────────────────────────────────────────────── */
#define PACKET_MAGIC    0x44494E4Fu  /* DINO */
#define PACKET_VERSION  3u
#define RX_BUFFER_SIZE  (sizeof(NetPacket) * 16)
#define TX_BUFFER_SIZE  (sizeof(NetPacket) * 64)

/* ── 낮/밤 주기 ──────────────────────────────────────────── */
#define DN_CYCLE_MS     25000u
#define DN_DAY_MS       10000u
#define DN_TRANS_MS      2500u
#define DN_NIGHT_MS     10000u

typedef enum {
    MODE_NONE = 0,
    MODE_SOLO = 1,
    MODE_DUO = 2
} GameMode;

typedef enum {
    RESULT_NONE = 0,
    RESULT_WIN,
    RESULT_LOSE,
    RESULT_DRAW,
    RESULT_SOLO_END
} ResultType;

typedef enum {
    ITEM_NONE = 0,
    ITEM_RUSH = 1,    /* 주황 이중화살표: 다음 장애물이 가까이 생성 */
    ITEM_BLIND = 2,   /* 보라 눈가림: 상대 화면 가림 */
    ITEM_SPEED = 3    /* 노랑 번개: 상대 장애물 일시 가속 */
} ItemType;

typedef enum {
    NOTICE_TEXT = 0,
    NOTICE_ITEM_GET,
    NOTICE_ATTACK_SENT,
    NOTICE_ATTACK_RECEIVED
} NoticeKind;

typedef enum {
    MENU_DUO = 0,
    MENU_SOLO,
    MENU_RECORDS,
    MENU_HELP,
    MENU_QUIT
} MainChoice;

typedef struct {
    uint32_t magic;
    uint32_t version;
    int is_ready;
    int dino_y;
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;
    int is_dead;
    int attack_seq;
    int attack_type;
    int session_cmd; /* 0=playing, 1=rematch request, 2=return to menu */
} NetPacket;

typedef struct {
    int top, bot, left, right;
} HitBox;

typedef struct {
    float height;
    float vy;
    int on_ground;
} PhysState;

typedef struct {
    float x;
    int y_row;
    int w;
    float base_speed;
} Cloud;

typedef struct {
    int x, y;
    Uint8 bright;
} Star;

typedef struct {
    Uint8 r, g, b;
} RGB3;

typedef struct {
    GameMode mode;
    ResultType result;
    int score;
    int peer_score;
    int clears;
    int items_used;
    int items_received;
} GameRecord;

typedef struct {
    GameRecord rec[MAX_RECORDS];
    int count;
    int best_solo;
    int best_duo;
    int duo_win;
    int duo_lose;
} SessionStats;

typedef struct {
    int sock;
    unsigned char rxbuf[RX_BUFFER_SIZE];
    size_t rxlen;
    unsigned char txbuf[TX_BUFFER_SIZE];
    size_t txlen;
    int disconnected;
    int version_error;
} NetLink;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex_dino;
    SDL_Texture *tex_cactus;
    SDL_Texture *tex_ptero;
    TTF_Font *font;
    TTF_Font *font_small;
    TTF_Font *font_large;
    Cloud clouds[MAX_CLOUDS];
    Star stars[MAX_STARS];
} App;

typedef struct {
    PhysState phys;
    int is_ducking;
    int obs_x;
    int obs_type;
    int score;
    int dead;
    int show_hitbox;
    int tick_interval;
    int tick_count;
    int clears;
    long long game_start_ms;
    long long last_tick_ms;
    long long last_duck_ms;

    ItemType inventory[ITEM_SLOTS];
    int inv_count;
    int items_used;
    int items_received;
    int attack_seq;
    int last_peer_attack_seq;
    long long attack_cooldown_until;
    int forced_next_obstacle; /* 대기 중인 RUSH 횟수 */
    long long blind_until;
    long long speed_until;
    char notice[96];
    long long notice_until;
    NoticeKind notice_kind;
    ItemType notice_item;
} RoundState;

static char g_base_dir[512];

/* ═══════════════ 공통 유틸 ════════════════════════════════ */

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void init_base_dir(const char *argv0) {
    char tmp[512];
    strncpy(tmp, argv0, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strncpy(g_base_dir, dirname(tmp), sizeof(g_base_dir) - 1);
    g_base_dir[sizeof(g_base_dir) - 1] = '\0';
}

static void asset_path(char *out, size_t sz, const char *filename) {
    snprintf(out, sz, "%s/%s", g_base_dir, filename);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * IP 탐색 우선순위:
 *  1) getifaddrs()  — 인터페이스 직접 열거 (루프백 제외)
 *  2) hostname -I   — 쉘 명령 (VMware/Docker 등 특수 환경 대응)
 *  3) UDP connect   — 라우팅 테이블 이용 (인터넷 연결 필요)
 */
static void get_local_ip(char *buf, size_t size) {
    buf[0] = '\0';

    /* ── 방법 1: getifaddrs() 인터페이스 열거 ─────────────── */
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char tmp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, tmp, sizeof(tmp));
            if (strncmp(tmp, "127.", 4) == 0) continue;   /* 루프백 제외 */
            if (strcmp(tmp, "0.0.0.0") == 0) continue;    /* 미할당 제외 */
            strncpy(buf, tmp, size - 1);
            buf[size - 1] = '\0';
            freeifaddrs(ifaddr);
            return;
        }
        freeifaddrs(ifaddr);
    }

    /* ── 방법 2: hostname -I 쉘 명령 (VMware/Ubuntu 등 대응) ─ */
    FILE *fp = popen("hostname -I 2>/dev/null", "r");
    if (fp) {
        char line[256] = {0};
        if (fgets(line, sizeof(line), fp)) {
            /* 공백·개행으로 구분된 첫 번째 IP만 사용 */
            char *sp = line;
            while (*sp == ' ') sp++;               /* 앞 공백 건너뜀 */
            char *end = sp;
            while (*end && *end != ' ' && *end != '\n') end++;
            *end = '\0';
            if (strlen(sp) > 0 && strncmp(sp, "127.", 4) != 0
                                && strcmp(sp, "0.0.0.0") != 0) {
                strncpy(buf, sp, size - 1);
                buf[size - 1] = '\0';
                pclose(fp);
                return;
            }
        }
        pclose(fp);
    }

    /* ── 방법 3: UDP connect 트릭 (인터넷 연결 시 fallback) ── */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            getsockname(sock, (struct sockaddr *)&local, &len);
            inet_ntop(AF_INET, &local.sin_addr, buf, (socklen_t)size);
            close(sock);
            return;
        }
        close(sock);
    }

    /* 최후 수단: 루프백 주소 반환 (같은 PC 내 테스트용) */
    strncpy(buf, "127.0.0.1", size - 1);
    buf[size - 1] = '\0';
}

static const char *result_name(ResultType r) {
    switch (r) {
        case RESULT_WIN:      return "WIN";
        case RESULT_LOSE:     return "LOSE";
        case RESULT_DRAW:     return "DRAW";
        case RESULT_SOLO_END: return "GAME OVER";
        default:              return "-";
    }
}

static int obstacle_width(int type) {
    if (type == 2) return 5;
    if (type == 3) return 7;
    if (type == 0 || type == 4) return 4;
    return 3;
}

static int calc_level(int interval) {
    int level = (int)((50 - interval) / 3.5f) + 1;
    return clamp_int(level, 1, 10);
}

static void stats_add(SessionStats *stats, GameRecord rec) {
    if (stats->count < MAX_RECORDS) {
        stats->rec[stats->count++] = rec;
    } else {
        memmove(&stats->rec[0], &stats->rec[1], sizeof(GameRecord) * (MAX_RECORDS - 1));
        stats->rec[MAX_RECORDS - 1] = rec;
    }
    if (rec.mode == MODE_SOLO && rec.score > stats->best_solo)
        stats->best_solo = rec.score;
    if (rec.mode == MODE_DUO) {
        if (rec.score > stats->best_duo) stats->best_duo = rec.score;
        if (rec.result == RESULT_WIN) stats->duo_win++;
        if (rec.result == RESULT_LOSE) stats->duo_lose++;
    }
}

/* ═══════════════ 렌더링 유틸 ═════════════════════════════ */

static void draw_filled(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

static void draw_outline(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderDrawRect(ren, &r);
}

static void draw_text_px(SDL_Renderer *ren, TTF_Font *font, const char *txt,
                         int x, int y, SDL_Color col) {
    if (!font || !txt || !txt[0]) return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, txt, col);
    if (!surface) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surface);
    if (tex) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surface);
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font, const char *txt,
                      int col, int row, SDL_Color color) {
    draw_text_px(ren, font, txt, col * CELL_W, row * CELL_H, color);
}

static void draw_text_center(SDL_Renderer *ren, TTF_Font *font, const char *txt,
                             int y, SDL_Color color) {
    if (!font || !txt || !txt[0]) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, txt, &w, &h);
    draw_text_px(ren, font, txt, (WIN_W - w) / 2, y, color);
}

static void draw_button(SDL_Renderer *ren, TTF_Font *font, const char *label,
                        SDL_Rect r, int selected) {
    SDL_Color fill = selected ? (SDL_Color){55, 95, 145, 245} : (SDL_Color){245, 247, 250, 230};
    SDL_Color border = selected ? (SDL_Color){255, 210, 70, 255} : (SDL_Color){150, 160, 170, 255};
    SDL_Color text = selected ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){50, 55, 65, 255};
    draw_filled(ren, r.x, r.y, r.w, r.h, fill);
    draw_outline(ren, r.x, r.y, r.w, r.h, border);
    draw_outline(ren, r.x + 2, r.y + 2, r.w - 4, r.h - 4, border);
    int tw = 0, th = 0;
    TTF_SizeUTF8(font, label, &tw, &th);
    draw_text_px(ren, font, label, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, text);
}

/*
 * 아이템 아이콘은 유니코드 이모지/추가 이미지에 의존하지 않고 SDL 도형으로 직접 그린다.
 * Linux 환경에서 폰트에 따라 이모지가 □로 깨지는 문제를 피하기 위한 방식이다.
 */
static SDL_Color item_color(ItemType item) {
    switch (item) {
        case ITEM_RUSH:  return (SDL_Color){232, 112, 35, 255};  /* 주황 */
        case ITEM_BLIND: return (SDL_Color){99, 73, 151, 255};   /* 보라 */
        case ITEM_SPEED: return (SDL_Color){245, 194, 45, 255};  /* 노랑 */
        default:         return (SDL_Color){234, 238, 243, 255};
    }
}

static void draw_thick_line(SDL_Renderer *ren, int x1, int y1, int x2, int y2,
                            int thickness, SDL_Color col) {
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int i = -(thickness / 2); i <= thickness / 2; i++) {
        SDL_RenderDrawLine(ren, x1, y1 + i, x2, y2 + i);
    }
}

static void draw_item_icon(SDL_Renderer *ren, ItemType item, int x, int y, int size,
                           int highlighted) {
    SDL_Color fill = item_color(item);
    SDL_Color border = highlighted ? (SDL_Color){255, 210, 55, 255}
                                   : (SDL_Color){126, 136, 150, 255};
    draw_filled(ren, x, y, size, size, fill);
    draw_outline(ren, x, y, size, size, border);
    if (highlighted) draw_outline(ren, x - 2, y - 2, size + 4, size + 4, border);

    if (item == ITEM_NONE) {
        draw_filled(ren, x + size / 3, y + size / 2 - 2, size / 3, 4,
                    (SDL_Color){157, 166, 180, 255});
        return;
    }

    if (item == ITEM_RUSH) {
        /* 빠르게 달려드는 느낌의 이중 화살표 */
        SDL_Color mark = {255, 255, 245, 255};
        int left = x + size / 5;
        int mid = x + size / 2;
        int right = x + size - size / 5;
        int top = y + size / 4;
        int center = y + size / 2;
        int bot = y + size - size / 4;
        draw_thick_line(ren, left, top, mid - 2, center, 3, mark);
        draw_thick_line(ren, left, bot, mid - 2, center, 3, mark);
        draw_thick_line(ren, mid - 2, top, right, center, 3, mark);
        draw_thick_line(ren, mid - 2, bot, right, center, 3, mark);
    } else if (item == ITEM_BLIND) {
        /* 눈 모양 위에 사선: 시야 차단 */
        SDL_Color eye = {255, 255, 255, 255};
        SDL_Color slash = {235, 68, 73, 255};
        int lx = x + size / 5, rx = x + size - size / 5;
        int cy = y + size / 2;
        draw_thick_line(ren, lx, cy, x + size / 2, y + size / 3, 2, eye);
        draw_thick_line(ren, x + size / 2, y + size / 3, rx, cy, 2, eye);
        draw_thick_line(ren, lx, cy, x + size / 2, y + size - size / 3, 2, eye);
        draw_thick_line(ren, x + size / 2, y + size - size / 3, rx, cy, 2, eye);
        draw_filled(ren, x + size / 2 - 4, cy - 4, 8, 8, eye);
        draw_thick_line(ren, x + size / 4, y + size / 4,
                        x + size - size / 4, y + size - size / 4, 4, slash);
    } else if (item == ITEM_SPEED) {
        /* 픽셀 스타일 번개 */
        SDL_Color bolt = {67, 57, 28, 255};
        draw_filled(ren, x + size / 2, y + size / 6, size / 5, size / 3, bolt);
        draw_filled(ren, x + size / 3, y + size / 2 - size / 8, size / 2, size / 5, bolt);
        draw_filled(ren, x + size / 3, y + size / 2, size / 5, size / 3, bolt);
        draw_filled(ren, x + size / 4, y + size - size / 4, size / 5, size / 8, bolt);
    }
}

static void draw_item_banner(App *app, const RoundState *state, int y) {
    if (state->notice_kind == NOTICE_TEXT || state->notice_item == ITEM_NONE) {
        draw_text_center(app->ren, app->font, state->notice, y + 10,
                         (SDL_Color){220, 60, 45, 255});
        return;
    }
    const char *caption = state->notice_kind == NOTICE_ITEM_GET ? "ITEM GET!" :
                          state->notice_kind == NOTICE_ATTACK_SENT ? "ATTACK SENT!" :
                          "INCOMING ATTACK!";
    int text_w = 0, text_h = 0;
    TTF_SizeUTF8(app->font, caption, &text_w, &text_h);
    int icon_size = 42;
    int total_w = icon_size + 14 + text_w;
    int x = (WIN_W - total_w) / 2;
    draw_filled(app->ren, x - 12, y - 8, total_w + 24, icon_size + 16,
                (SDL_Color){255, 255, 255, 225});
    draw_outline(app->ren, x - 12, y - 8, total_w + 24, icon_size + 16,
                 item_color(state->notice_item));
    draw_item_icon(app->ren, state->notice_item, x, y, icon_size, 1);
    draw_text_px(app->ren, app->font, caption, x + icon_size + 14,
                 y + (icon_size - text_h) / 2, (SDL_Color){64, 67, 78, 255});
}

static void draw_sprite(SDL_Renderer *ren, SDL_Texture *tex,
                        int x_col, int y_row, int w_cols, int h_rows,
                        int frame_idx, int total_frames, float angle, SDL_Color mod) {
    if (!tex) return;
    SDL_SetTextureColorMod(tex, mod.r, mod.g, mod.b);
    int tw = 0, th = 0;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    int fw = tw / (total_frames > 0 ? total_frames : 1);
    SDL_Rect src = {frame_idx * fw, 0, fw, th};
    SDL_Rect dst = {x_col * CELL_W, y_row * CELL_H, w_cols * CELL_W, h_rows * CELL_H};
    SDL_RenderCopyEx(ren, tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
}

static RGB3 rgb_lerp(RGB3 a, RGB3 b, float t) {
    RGB3 r;
    r.r = (Uint8)(a.r + (b.r - a.r) * t);
    r.g = (Uint8)(a.g + (b.g - a.g) * t);
    r.b = (Uint8)(a.b + (b.b - a.b) * t);
    return r;
}

static void daynight_colors(Uint32 elapsed, RGB3 *bg, RGB3 *ground,
                            RGB3 *dino, float *night_alpha) {
    static RGB3 bg_day = {240, 248, 255}, bg_night = {18, 18, 45};
    static RGB3 gr_day = {83, 83, 83}, gr_night = {40, 40, 60};
    static RGB3 di_day = {83, 83, 83}, di_night = {95, 105, 145};
    Uint32 pos = elapsed % DN_CYCLE_MS;
    float t = 0.0f;
    if (pos < DN_DAY_MS) {
        *bg = bg_day; *ground = gr_day; *dino = di_day; *night_alpha = 0.0f;
    } else if (pos < DN_DAY_MS + DN_TRANS_MS) {
        t = (float)(pos - DN_DAY_MS) / DN_TRANS_MS;
        *bg = rgb_lerp(bg_day, bg_night, t);
        *ground = rgb_lerp(gr_day, gr_night, t);
        *dino = rgb_lerp(di_day, di_night, t);
        *night_alpha = t;
    } else if (pos < DN_DAY_MS + DN_TRANS_MS + DN_NIGHT_MS) {
        *bg = bg_night; *ground = gr_night; *dino = di_night; *night_alpha = 1.0f;
    } else {
        t = (float)(pos - DN_DAY_MS - DN_TRANS_MS - DN_NIGHT_MS) / DN_TRANS_MS;
        *bg = rgb_lerp(bg_night, bg_day, t);
        *ground = rgb_lerp(gr_night, gr_day, t);
        *dino = rgb_lerp(di_night, di_day, t);
        *night_alpha = 1.0f - t;
    }
}

static void init_clouds(Cloud clouds[], int n) {
    for (int i = 0; i < n; i++) {
        clouds[i].x = (float)(rand() % WIN_W);
        clouds[i].y_row = 1 + rand() % 5;
        clouds[i].w = 3 + rand() % 3;
        clouds[i].base_speed = 0.3f + (float)(rand() % 10) / 20.0f;
    }
}

static void init_stars(Star stars[], int n) {
    for (int i = 0; i < n; i++) {
        stars[i].x = rand() % WIN_W;
        stars[i].y = rand() % (WIN_H / 2);
        stars[i].bright = (Uint8)(140 + rand() % 116);
    }
}

static void draw_stars(SDL_Renderer *ren, Star stars[], int n, float alpha) {
    if (alpha <= 0.01f) return;
    Uint32 ms = SDL_GetTicks();
    for (int i = 0; i < n; i++) {
        float blink = 0.7f + 0.3f * sinf(ms * 0.002f + i * 0.7f);
        Uint8 a = (Uint8)(alpha * blink * stars[i].bright);
        SDL_SetRenderDrawColor(ren, stars[i].bright, stars[i].bright, stars[i].bright, a);
        SDL_RenderDrawPoint(ren, stars[i].x, stars[i].y);
        SDL_RenderDrawPoint(ren, stars[i].x + 1, stars[i].y);
    }
}

static void draw_moon(SDL_Renderer *ren, float alpha) {
    if (alpha < 0.05f) return;
    Uint8 a = (Uint8)(alpha * 230);
    draw_filled(ren, WIN_W - 105, 32, 42, 42, (SDL_Color){245, 243, 200, a});
    draw_filled(ren, WIN_W - 96, 26, 42, 42, (SDL_Color){18, 18, 45, a});
}

static void update_draw_clouds(SDL_Renderer *ren, Cloud clouds[], int count,
                               int interval, RGB3 col) {
    float mult = 50.0f / (float)interval;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, 155);
    for (int i = 0; i < count; i++) {
        clouds[i].x -= clouds[i].base_speed * mult;
        if (clouds[i].x + clouds[i].w * CELL_W < 0) {
            clouds[i].x = (float)WIN_W;
            clouds[i].y_row = 1 + rand() % 5;
            clouds[i].w = 3 + rand() % 3;
            clouds[i].base_speed = 0.3f + (float)(rand() % 10) / 20.0f;
        }
        SDL_Rect r = {(int)clouds[i].x, clouds[i].y_row * CELL_H,
                      clouds[i].w * CELL_W, CELL_H / 2};
        SDL_RenderFillRect(ren, &r);
    }
}

static void draw_ground(SDL_Renderer *ren, int base_y, RGB3 ground) {
    SDL_SetRenderDrawColor(ren, ground.r, ground.g, ground.b, 255);
    SDL_Rect r = {0, base_y * CELL_H, WIN_W, 3};
    SDL_RenderFillRect(ren, &r);
}

static void draw_divider(SDL_Renderer *ren) {
    SDL_SetRenderDrawColor(ren, 180, 180, 180, 200);
    int y = ((DUO_MY_BASE_Y + DUO_OP_BASE_Y) / 2) * CELL_H;
    for (int x = 0; x < WIN_W; x += 20) {
        SDL_Rect r = {x, y, 10, 2};
        SDL_RenderFillRect(ren, &r);
    }
}

static void draw_obstacle(SDL_Renderer *ren, App *app, int x, int type,
                          int base_y, int anim, SDL_Color mod) {
    if (x < -8 || x >= WIDTH) return;
    if (type == 0) {
        draw_sprite(ren, app->tex_ptero, x, base_y - 4, 4, 2, anim, 2, 0.0f, mod);
    } else {
        int h = (type == 4) ? 3 : 2;
        int count = (type == 2) ? 2 : (type == 3) ? 3 : 1;
        for (int i = 0; i < count; i++)
            draw_sprite(ren, app->tex_cactus, x + i * 2, base_y - h, 3, h, 0, 1, 0.0f, mod);
    }
}

/* ═══════════════ 물리/충돌/라운드 상태 ═══════════════════ */

static HitBox dino_hitbox(int y, int ducking) {
    HitBox h;
    h.bot = y - 1;
    if (ducking) {
        h.top = y - 2; h.left = DINO_X + 1; h.right = DINO_X + 4;
    } else {
        h.top = y - 4; h.left = DINO_X + 1; h.right = DINO_X + 3;
    }
    return h;
}

static HitBox obstacle_hitbox(int x, int type, int ground) {
    HitBox h;
    if (type == 0) {
        h.top = ground - 4; h.bot = ground - 3; h.left = x + 1; h.right = x + 3;
    } else if (type == 4) {
        h.top = ground - 3; h.bot = ground - 1; h.left = x + 1; h.right = x + 2;
    } else {
        int w = (type == 2) ? 3 : (type == 3) ? 5 : 2;
        h.top = ground - 2; h.bot = ground - 1; h.left = x; h.right = x + w - 1;
    }
    return h;
}

static int overlaps(HitBox a, HitBox b) {
    return a.right >= b.left && a.left <= b.right && a.bot >= b.top && a.top <= b.bot;
}

static void phys_init(PhysState *p) {
    p->height = 0.0f;
    p->vy = 0.0f;
    p->on_ground = 1;
}

static void phys_jump(PhysState *p) {
    if (p->on_ground) {
        p->vy = JUMP_VY_SEC;
        p->on_ground = 0;
    }
}

static void phys_update(PhysState *p, float dt) {
    if (p->on_ground) return;
    p->vy -= GRAVITY_SEC * dt;
    if (p->vy < -MAX_FALL_SEC) p->vy = -MAX_FALL_SEC;
    p->height += p->vy * dt;
    if (p->height > MAX_HEIGHT) {
        p->height = MAX_HEIGHT;
        p->vy = 0.0f;
    }
    if (p->height <= 0.0f) {
        p->height = 0.0f;
        p->vy = 0.0f;
        p->on_ground = 1;
    }
}

static void round_init(RoundState *s) {
    memset(s, 0, sizeof(*s));
    phys_init(&s->phys);
    s->obs_x = WIDTH - 2;
    s->obs_type = 1;
    s->tick_interval = 50;
    s->game_start_ms = now_ms();
    s->last_tick_ms = s->game_start_ms;
}

static void notice(RoundState *s, const char *text, long long duration) {
    strncpy(s->notice, text, sizeof(s->notice) - 1);
    s->notice[sizeof(s->notice) - 1] = '\0';
    s->notice_kind = NOTICE_TEXT;
    s->notice_item = ITEM_NONE;
    s->notice_until = now_ms() + duration;
}

static void item_notice(RoundState *s, NoticeKind kind, ItemType item, long long duration) {
    s->notice[0] = '\0';
    s->notice_kind = kind;
    s->notice_item = item;
    s->notice_until = now_ms() + duration;
}

static void inventory_push(RoundState *s, ItemType item) {
    if (s->inv_count >= ITEM_SLOTS) {
        notice(s, "ITEM FULL - USE [A]", NOTICE_DURATION);
        return;
    }
    s->inventory[s->inv_count++] = item;
    item_notice(s, NOTICE_ITEM_GET, item, NOTICE_DURATION);
}

static ItemType inventory_pop(RoundState *s) {
    if (s->inv_count <= 0) return ITEM_NONE;
    ItemType item = s->inventory[0];
    for (int i = 1; i < s->inv_count; i++) s->inventory[i - 1] = s->inventory[i];
    s->inv_count--;
    return item;
}

static void apply_received_item(RoundState *s, ItemType item) {
    s->items_received++;
    if (item == ITEM_RUSH) {
        if (s->forced_next_obstacle < 3) s->forced_next_obstacle++;
        item_notice(s, NOTICE_ATTACK_RECEIVED, item, NOTICE_DURATION);
    } else if (item == ITEM_BLIND) {
        s->blind_until = now_ms() + BLIND_DURATION;
        item_notice(s, NOTICE_ATTACK_RECEIVED, item, NOTICE_DURATION);
    } else if (item == ITEM_SPEED) {
        s->speed_until = now_ms() + SPEED_DURATION;
        item_notice(s, NOTICE_ATTACK_RECEIVED, item, NOTICE_DURATION);
    }
}

static void update_player(RoundState *s, GameMode mode, int ground_y) {
    long long now = now_ms();
    if (s->dead) return;
    if (now - s->last_tick_ms < s->tick_interval) return;

    float dt = (float)(now - s->last_tick_ms) / 1000.0f;
    if (dt > 0.15f) dt = 0.15f;
    s->last_tick_ms = now;
    s->tick_count++;
    phys_update(&s->phys, dt);
    s->score = (int)((now - s->game_start_ms) / 100);

    int move_step = 1;
    if (now < s->speed_until && (s->tick_count % 2 == 0)) move_step = 2;
    s->obs_x -= move_step;

    if (s->obs_x + obstacle_width(s->obs_type) < 1) {
        s->clears++;
        s->obs_x = s->forced_next_obstacle > 0 ? WIDTH * 2 / 3 : WIDTH - 2;
        if (s->forced_next_obstacle > 0) s->forced_next_obstacle--;
        s->obs_type = rand() % 5;
        if (s->tick_interval > 15) s->tick_interval--;
        if (mode == MODE_DUO && s->clears % ITEM_CLEARS == 0) {
            inventory_push(s, (ItemType)(ITEM_RUSH + rand() % 3));
        }
    }

    int dino_y = ground_y - (int)s->phys.height;
    if (overlaps(dino_hitbox(dino_y, s->is_ducking),
                 obstacle_hitbox(s->obs_x, s->obs_type, ground_y))) {
        s->dead = 1;
        s->score = (int)((now - s->game_start_ms) / 100);
    }
}

/* ═══════════════ 네트워크 ════════════════════════════════ */

static void net_init(NetLink *net) {
    memset(net, 0, sizeof(*net));
    net->sock = -1;
}

static void net_close(NetLink *net) {
    if (net->sock >= 0) close(net->sock);
    net->sock = -1;
    net->rxlen = 0;
}

static NetPacket make_packet(const RoundState *s, int ready) {
    NetPacket p;
    memset(&p, 0, sizeof(p));
    p.magic = PACKET_MAGIC;
    p.version = PACKET_VERSION;
    p.is_ready = ready;
    p.dino_y = DINO_BASE - (int)s->phys.height;
    p.is_ducking = s->is_ducking;
    p.obs_x = s->obs_x;
    p.obs_type = s->obs_type;
    p.score = s->score;
    p.is_dead = s->dead;
    p.attack_seq = s->attack_seq;
    p.attack_type = ITEM_NONE; /* 게임 루프에서 마지막으로 사용한 아이템 종류를 채운다. */
    return p;
}

static int net_flush(NetLink *net) {
    while (net->txlen > 0 && net->sock >= 0) {
        ssize_t sent = send(net->sock, net->txbuf, net->txlen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            memmove(net->txbuf, net->txbuf + sent, net->txlen - (size_t)sent);
            net->txlen -= (size_t)sent;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            net->disconnected = 1;
            return -1;
        }
    }
    return 0;
}

static int net_send_packet(NetLink *net, const NetPacket *packet) {
    if (net->sock < 0) return -1;
    if (net_flush(net) < 0) return -1;
    if (net->txlen + sizeof(*packet) > sizeof(net->txbuf)) {
        /* 송신을 오래 처리하지 못한 경우 연결 문제로 판단하여 잘못된 스트림을 만들지 않는다. */
        net->disconnected = 1;
        return -1;
    }
    memcpy(net->txbuf + net->txlen, packet, sizeof(*packet));
    net->txlen += sizeof(*packet);
    return net_flush(net);
}

static int net_poll_latest(NetLink *net, NetPacket *latest, int *has_latest) {
    unsigned char temp[512];
    *has_latest = 0;
    while (net->sock >= 0) {
        ssize_t n = recv(net->sock, temp, sizeof(temp), MSG_DONTWAIT);
        if (n > 0) {
            if (net->rxlen + (size_t)n > sizeof(net->rxbuf)) {
                net->rxlen = 0;
                net->version_error = 1;
                return -1;
            }
            memcpy(net->rxbuf + net->rxlen, temp, (size_t)n);
            net->rxlen += (size_t)n;
            while (net->rxlen >= sizeof(NetPacket)) {
                NetPacket p;
                memcpy(&p, net->rxbuf, sizeof(p));
                memmove(net->rxbuf, net->rxbuf + sizeof(p), net->rxlen - sizeof(p));
                net->rxlen -= sizeof(p);
                if (p.magic != PACKET_MAGIC || p.version != PACKET_VERSION) {
                    net->version_error = 1;
                    return -1;
                }
                *latest = p;
                *has_latest = 1;
            }
        } else if (n == 0) {
            net->disconnected = 1;
            return -1;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            net->disconnected = 1;
            return -1;
        }
    }
    return 0;
}

/* ═══════════════ SDL/에셋 초기화 ═════════════════════════ */

static SDL_Texture *load_texture(App *app, const char *file) {
    char path[600];
    asset_path(path, sizeof(path), file);
    SDL_Surface *surface = IMG_Load(path);
    if (!surface) {
        fprintf(stderr, "이미지 로드 실패: %s (%s)\n", path, IMG_GetError());
        return NULL;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(app->ren, surface);
    SDL_FreeSurface(surface);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

static int app_init(App *app, const char *argv0) {
    memset(app, 0, sizeof(*app));
    init_base_dir(argv0);
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 초기화 실패: %s\n", SDL_GetError());
        return -1;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        fprintf(stderr, "SDL_image 초기화 실패: %s\n", IMG_GetError());
        return -1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "SDL_ttf 초기화 실패: %s\n", TTF_GetError());
        return -1;
    }
    app->win = SDL_CreateWindow("MULTI-DINO RUN", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!app->win) {
        fprintf(stderr, "창 생성 실패: %s\n", SDL_GetError());
        return -1;
    }
    app->ren = SDL_CreateRenderer(app->win, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app->ren) app->ren = SDL_CreateRenderer(app->win, -1, SDL_RENDERER_SOFTWARE);
    if (!app->ren) {
        fprintf(stderr, "렌더러 생성 실패: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(app->ren, SDL_BLENDMODE_BLEND);
    app->tex_dino = load_texture(app, "dino_anim.png");
    app->tex_cactus = load_texture(app, "cactus_clean.png");
    app->tex_ptero = load_texture(app, "ptero_anim.png");
    char fontpath[600];
    asset_path(fontpath, sizeof(fontpath), "NanumGothic.ttf");
    app->font_small = TTF_OpenFont(fontpath, 18);
    app->font = TTF_OpenFont(fontpath, 25);
    app->font_large = TTF_OpenFont(fontpath, 52);
    if (!app->tex_dino || !app->tex_cactus || !app->tex_ptero ||
        !app->font_small || !app->font || !app->font_large) {
        fprintf(stderr, "필수 이미지 또는 폰트를 찾을 수 없습니다. 실행 파일과 같은 폴더를 확인하세요.\n");
        return -1;
    }
    init_clouds(app->clouds, MAX_CLOUDS);
    init_stars(app->stars, MAX_STARS);
    return 0;
}

static void app_destroy(App *app) {
    if (app->font_small) TTF_CloseFont(app->font_small);
    if (app->font) TTF_CloseFont(app->font);
    if (app->font_large) TTF_CloseFont(app->font_large);
    if (app->tex_dino) SDL_DestroyTexture(app->tex_dino);
    if (app->tex_cactus) SDL_DestroyTexture(app->tex_cactus);
    if (app->tex_ptero) SDL_DestroyTexture(app->tex_ptero);
    if (app->ren) SDL_DestroyRenderer(app->ren);
    if (app->win) SDL_DestroyWindow(app->win);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

/* ═══════════════ 배경/메뉴 화면 ══════════════════════════ */

static void render_menu_background(App *app, int score_hint) {
    RGB3 bg, gr, di;
    float night = 0.0f;
    daynight_colors((Uint32)(score_hint * 10), &bg, &gr, &di, &night);
    SDL_SetRenderDrawColor(app->ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(app->ren);
    draw_stars(app->ren, app->stars, MAX_STARS, night);
    draw_moon(app->ren, night);
    update_draw_clouds(app->ren, app->clouds, MAX_CLOUDS, 50, (RGB3){210,215,220});
    draw_ground(app->ren, 25, gr);
    SDL_Color dc = {di.r, di.g, di.b, 255};
    int anim = 1 + (int)((SDL_GetTicks() / 180) % 2);
    draw_sprite(app->ren, app->tex_dino, 9, 21, 5, 4, anim, 3, 0.0f, dc);
    draw_obstacle(app->ren, app, 63, 1, 25, 0, dc);
}

static MainChoice main_menu(App *app, const SessionStats *stats, int *quit) {
    int selected = 0;
    SDL_Rect buttons[4] = {
        {160, 310, 290, 82}, {510, 310, 290, 82},
        {160, 425, 290, 82}, {510, 425, 290, 82}
    };
    const char *labels[4] = {"DUO MODE", "SOLO MODE", "RECORDS", "HOW TO PLAY"};
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return MENU_QUIT; }
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_LEFT:  if (selected % 2 == 1) selected--; break;
                    case SDLK_RIGHT: if (selected % 2 == 0) selected++; break;
                    case SDLK_UP:    if (selected >= 2) selected -= 2; break;
                    case SDLK_DOWN:  if (selected < 2) selected += 2; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: return (MainChoice)selected;
                    case SDLK_q:
                    case SDLK_ESCAPE: *quit = 1; return MENU_QUIT;
                    default: break;
                }
            }
        }
        render_menu_background(app, stats->best_solo + stats->best_duo);
        draw_text_center(app->ren, app->font_large, "MULTI-DINO RUN", 78,
                         (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font_small,
                         "SURVIVE. CHARGE. ATTACK.", 145,
                         (SDL_Color){90,100,120,255});
        char line[120];
        snprintf(line, sizeof(line), "SESSION  |  SOLO BEST %05d   DUO %dW %dL",
                 stats->best_solo, stats->duo_win, stats->duo_lose);
        draw_text_center(app->ren, app->font_small, line, 205,
                         (SDL_Color){70,80,95,255});
        for (int i = 0; i < 4; i++) draw_button(app->ren, app->font, labels[i], buttons[i], i == selected);
        draw_text_center(app->ren, app->font_small,
                         "ARROW KEY : SELECT     ENTER : OK     Q : QUIT",
                         550, (SDL_Color){80,88,100,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
    return MENU_QUIT;
}

static void show_help(App *app, int *quit) {
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return; }
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE ||
                                          e.key.keysym.sym == SDLK_BACKSPACE ||
                                          e.key.keysym.sym == SDLK_RETURN)) return;
        }
        render_menu_background(app, 900);
        draw_filled(app->ren, 90, 45, WIN_W - 180, WIN_H - 105,
                    (SDL_Color){252,253,255,235});
        draw_outline(app->ren, 90, 45, WIN_W - 180, WIN_H - 105,
                     (SDL_Color){70,95,125,255});
        draw_text_center(app->ren, app->font_large, "HOW TO PLAY", 70,
                         (SDL_Color){45,55,70,255});
        draw_text(app->ren, app->font, "SOLO MODE", 10, 7, (SDL_Color){50,85,130,255});
        draw_text(app->ren, app->font_small, "SPACE / UP : Jump    DOWN : Duck", 10, 9, (SDL_Color){60,60,70,255});
        draw_text(app->ren, app->font_small, "Survive as long as possible. Items are disabled.", 10, 10, (SDL_Color){60,60,70,255});
        draw_text(app->ren, app->font, "DUO MODE", 10, 13, (SDL_Color){50,85,130,255});
        draw_text(app->ren, app->font_small, "Avoid 5 obstacles to receive one random item icon.", 10, 15, (SDL_Color){60,60,70,255});
        draw_text(app->ren, app->font_small, "A : Use the left-most icon first.", 10, 16, (SDL_Color){60,60,70,255});

        draw_item_icon(app->ren, ITEM_RUSH, 122, 430, 38, 0);
        draw_text_px(app->ren, app->font_small, "급습 : 상대의 다음 장애물이 더 가까이 등장", 180, 436,
                     (SDL_Color){132,72,25,255});
        draw_item_icon(app->ren, ITEM_BLIND, 122, 482, 38, 0);
        draw_text_px(app->ren, app->font_small, "시야 방해 : 상대 화면이 잠시 가려짐", 180, 488,
                     (SDL_Color){75,64,120,255});
        draw_item_icon(app->ren, ITEM_SPEED, 122, 534, 38, 0);
        draw_text_px(app->ren, app->font_small, "가속 : 상대 장애물 속도가 잠시 증가", 180, 540,
                     (SDL_Color){120,90,25,255});
        draw_text_center(app->ren, app->font_small, "ESC / ENTER : BACK", 615,
                         (SDL_Color){70,80,95,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
}

static void show_records(App *app, const SessionStats *stats, int *quit) {
    int offset = 0;
    const int rows = 14;
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_BACKSPACE ||
                    e.key.keysym.sym == SDLK_RETURN) return;
                if (e.key.keysym.sym == SDLK_UP && offset > 0) offset--;
                if (e.key.keysym.sym == SDLK_DOWN && offset + rows < stats->count) offset++;
            }
        }
        render_menu_background(app, 1300);
        draw_filled(app->ren, 55, 35, WIN_W - 110, WIN_H - 90,
                    (SDL_Color){252,253,255,242});
        draw_outline(app->ren, 55, 35, WIN_W - 110, WIN_H - 90,
                     (SDL_Color){70,95,125,255});
        draw_text_center(app->ren, app->font_large, "SESSION RECORDS", 55,
                         (SDL_Color){45,55,70,255});
        char summary[150];
        snprintf(summary, sizeof(summary), "Games %d   Solo Best %05d   Duo Best %05d   Duo %dW / %dL",
                 stats->count, stats->best_solo, stats->best_duo, stats->duo_win, stats->duo_lose);
        draw_text_center(app->ren, app->font_small, summary, 120,
                         (SDL_Color){65,75,95,255});
        draw_text_px(app->ren, app->font_small,
                     "NO. MODE RESULT      SCORE  PEER   CLEAR  USED  HIT", 125, 172,
                     (SDL_Color){40,60,95,255});
        SDL_SetRenderDrawColor(app->ren, 150, 160, 175, 255);
        SDL_RenderDrawLine(app->ren, 125, 202, WIN_W - 125, 202);
        if (stats->count == 0) {
            draw_text_center(app->ren, app->font, "No games played in this session.", 300,
                             (SDL_Color){100,105,115,255});
        } else {
            int shown = 0;
            for (int i = offset; i < stats->count && shown < rows; i++, shown++) {
                char row[160];
                const GameRecord *r = &stats->rec[i];
                if (r->mode == MODE_DUO) {
                    snprintf(row, sizeof(row), "%02d  DUO  %-9s  %05d  %05d   %03d    %02d    %02d",
                             i + 1, result_name(r->result), r->score, r->peer_score,
                             r->clears, r->items_used, r->items_received);
                } else {
                    snprintf(row, sizeof(row), "%02d  SOLO %-9s  %05d    -     %03d     -     -",
                             i + 1, result_name(r->result), r->score, r->clears);
                }
                draw_text_px(app->ren, app->font_small, row, 125, 218 + shown * 27,
                             (SDL_Color){60,65,75,255});
            }
        }
        draw_text_center(app->ren, app->font_small,
                         "UP / DOWN : SCROLL      ESC / ENTER : BACK      Records reset when program closes.",
                         WIN_H - 64, (SDL_Color){70,80,95,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
}

/* ═══════════════ 연결/듀오 선택 화면 ════════════════════ */

static int show_message(App *app, const char *title, const char *msg, int *quit) {
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return 0; }
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE ||
                                          e.key.keysym.sym == SDLK_RETURN ||
                                          e.key.keysym.sym == SDLK_BACKSPACE)) return 1;
        }
        render_menu_background(app, 500);
        draw_filled(app->ren, 150, 220, WIN_W - 300, 220, (SDL_Color){255,255,255,245});
        draw_outline(app->ren, 150, 220, WIN_W - 300, 220, (SDL_Color){75,100,130,255});
        draw_text_center(app->ren, app->font_large, title, 252, (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font, msg, 340, (SDL_Color){70,75,85,255});
        draw_text_center(app->ren, app->font_small, "ENTER / ESC : BACK", 390,
                         (SDL_Color){85,95,110,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
    return 0;
}

static int duo_role_menu(App *app, int *quit) {
    int sel = 0;
    SDL_Rect boxes[3] = {{130, 315, 220, 85}, {370, 315, 220, 85}, {610, 315, 220, 85}};
    const char *labels[3] = {"HOST", "JOIN", "BACK"};
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return 0; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) return 0;
                if (e.key.keysym.sym == SDLK_LEFT && sel > 0) sel--;
                if (e.key.keysym.sym == SDLK_RIGHT && sel < 2) sel++;
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)
                    return sel == 0 ? 1 : sel == 1 ? 2 : 0;
            }
        }
        render_menu_background(app, 500);
        draw_text_center(app->ren, app->font_large, "DUO MODE", 92, (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font_small,
                         "HOST creates a room. JOIN connects with host IP.", 176,
                         (SDL_Color){70,80,95,255});
        for (int i = 0; i < 3; i++) draw_button(app->ren, app->font, labels[i], boxes[i], i == sel);
        draw_text_center(app->ren, app->font_small, "ESC : BACK", 505, (SDL_Color){70,80,95,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
    return 0;
}

static int input_ip_screen(App *app, char *ip, size_t size, int *quit) {
    /* 기본값 없이 빈 상태로 시작 — 사용자가 직접 접속할 IP 를 입력한다 */
    ip[0] = '\0';
    SDL_StartTextInput();
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; SDL_StopTextInput(); return 0; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) { SDL_StopTextInput(); return 0; }
                if (e.key.keysym.sym == SDLK_BACKSPACE && strlen(ip) > 0) ip[strlen(ip)-1] = '\0';
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                    struct in_addr test;
                    if (inet_pton(AF_INET, ip, &test) == 1) { SDL_StopTextInput(); return 1; }
                }
            }
            if (e.type == SDL_TEXTINPUT) {
                for (const char *p = e.text.text; *p; p++) {
                    if (((*p >= '0' && *p <= '9') || *p == '.') && strlen(ip) + 1 < size) {
                        size_t len = strlen(ip);
                        ip[len] = *p;
                        ip[len + 1] = '\0';
                    }
                }
            }
        }
        render_menu_background(app, 500);
        draw_text_center(app->ren, app->font_large, "JOIN ROOM", 110, (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font, "HOST IP ADDRESS", 250, (SDL_Color){65,75,90,255});
        draw_filled(app->ren, 270, 310, 420, 70, (SDL_Color){255,255,255,245});
        draw_outline(app->ren, 270, 310, 420, 70, (SDL_Color){65,100,145,255});
        /* 입력 내용이 없으면 회색 플레이스홀더 표시 */
        if (ip[0] == '\0') {
            draw_text_center(app->ren, app->font, "ex) 192.168.0.10", 326,
                             (SDL_Color){185,190,200,255});
        } else {
            draw_text_center(app->ren, app->font, ip, 326, (SDL_Color){50,60,75,255});
        }
        draw_text_center(app->ren, app->font_small,
                         "HOST 의 IP 를 입력하세요   ENTER : 연결   ESC : 뒤로", 440,
                         (SDL_Color){70,80,95,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    return 0;
}

static int host_wait(App *app, NetLink *net, int *quit) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { show_message(app, "HOST ERROR", "socket() failed.", quit); return 0; }
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listener, 1) < 0) {
        close(listener);
        show_message(app, "HOST ERROR", "Port 9000 cannot be opened.", quit);
        return 0;
    }
    set_nonblocking(listener);

    /* 한 번만 조회해서 루프 안에서 반복 사용 */
    char local_ip[64];
    get_local_ip(local_ip, sizeof(local_ip));
    char ip_line[100];
    snprintf(ip_line, sizeof(ip_line), "MY IP : %s     PORT : 9000", local_ip);

    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; close(listener); return 0; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                close(listener); return 0;
            }
        }
        int accepted = accept(listener, NULL, NULL);
        if (accepted >= 0) {
            close(listener);
            set_nonblocking(accepted);
            net->sock = accepted;
            return 1;
        }
        render_menu_background(app, 500);
        draw_text_center(app->ren, app->font_large, "WAITING FOR PLAYER", 145,
                         (SDL_Color){45,55,70,255});
        /* LAN IP 표시 박스 */
        draw_filled(app->ren, 190, 255, WIN_W - 380, 68, (SDL_Color){255,255,255,230});
        draw_outline(app->ren, 190, 255, WIN_W - 380, 68, (SDL_Color){65,100,145,255});
        draw_text_center(app->ren, app->font, ip_line, 272,
                         (SDL_Color){35,100,175,255});
        /* 루프백이면 VMware 네트워크 안내, 아니면 정상 접속 안내 */
        if (strncmp(local_ip, "127.", 4) == 0) {
            draw_text_center(app->ren, app->font_small,
                             "네트워크 미설정 - 같은 PC 안에서만 접속 가능합니다.", 336,
                             (SDL_Color){200,100,30,255});
            draw_text_center(app->ren, app->font_small,
                             "VMware: 설정 > 네트워크 어댑터 > Bridged 또는 NAT 로 변경", 362,
                             (SDL_Color){140,90,50,255});
        } else {
            draw_text_center(app->ren, app->font_small,
                             "상대방은 JOIN 을 선택하고 위 IP 를 입력하세요.", 348,
                             (SDL_Color){75,85,100,255});
        }
        draw_text_center(app->ren, app->font_small, "ESC : CANCEL", 420,
                         (SDL_Color){75,85,100,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(30);
    }
    close(listener);
    return 0;
}

static int join_connect(App *app, const char *ip, NetLink *net, int *quit) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { show_message(app, "CONNECT ERROR", "socket() failed.", quit); return 0; }
    set_nonblocking(sock);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(sock); show_message(app, "CONNECT ERROR", "Invalid IP address.", quit); return 0;
    }
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) { net->sock = sock; return 1; }
    if (errno != EINPROGRESS) {
        close(sock); show_message(app, "CONNECT ERROR", "Connection failed.", quit); return 0;
    }
    long long start = now_ms();
    while (!*quit && now_ms() - start < 8000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; close(sock); return 0; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                close(sock); return 0;
            }
        }
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval tv = {0, 0};
        if (select(sock + 1, NULL, &writefds, NULL, &tv) > 0) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) { net->sock = sock; return 1; }
            close(sock); show_message(app, "CONNECT ERROR", "Host is not available.", quit); return 0;
        }
        render_menu_background(app, 500);
        char msg[100]; snprintf(msg, sizeof(msg), "Connecting to %s : 9000", ip);
        draw_text_center(app->ren, app->font_large, "CONNECTING...", 180,
                         (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font, msg, 300, (SDL_Color){65,75,90,255});
        draw_text_center(app->ren, app->font_small, "ESC : CANCEL", 400,
                         (SDL_Color){75,85,100,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(20);
    }
    close(sock);
    show_message(app, "CONNECT ERROR", "Connection timed out.", quit);
    return 0;
}

/* ═══════════════ 게임 렌더링 ═════════════════════════════ */

static void render_world_background(App *app, int score, int interval,
                                    RGB3 *ground_out, SDL_Color *dino_out) {
    RGB3 bg, ground, dino;
    float night = 0.0f;
    daynight_colors((Uint32)(score * 10), &bg, &ground, &dino, &night);
    SDL_SetRenderDrawColor(app->ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(app->ren);
    draw_stars(app->ren, app->stars, MAX_STARS, night);
    draw_moon(app->ren, night);
    update_draw_clouds(app->ren, app->clouds, MAX_CLOUDS, interval, (RGB3){210,215,220});
    *ground_out = ground;
    *dino_out = (SDL_Color){dino.r, dino.g, dino.b, 255};
}

static void draw_inventory(App *app, const RoundState *state) {
    const int size = 38;
    const int y = 66;
    const int start_x = 690;
    draw_text_px(app->ren, app->font_small, "ITEM [A]", 574, y + 8,
                 (SDL_Color){76,82,95,255});
    for (int i = 0; i < ITEM_SLOTS; i++) {
        ItemType item = i < state->inv_count ? state->inventory[i] : ITEM_NONE;
        draw_item_icon(app->ren, item, start_x + i * (size + 10), y, size,
                       i == 0 && state->inv_count > 0);
    }
}

static void render_solo_game(App *app, const RoundState *s) {
    RGB3 ground;
    SDL_Color dino;
    render_world_background(app, s->score, s->tick_interval, &ground, &dino);
    draw_ground(app->ren, SOLO_BASE_Y, ground);
    int anim = s->phys.height > 0.5f ? 0 : 1 + (s->tick_count / 2) % 2;
    int ptero_anim = (s->tick_count / 2) % 2;
    draw_obstacle(app->ren, app, s->obs_x, s->obs_type, SOLO_BASE_Y, ptero_anim, dino);
    if (!s->dead) {
        int h = s->is_ducking ? 2 : 4;
        int w = s->is_ducking ? 6 : 5;
        draw_sprite(app->ren, app->tex_dino, DINO_X, SOLO_BASE_Y - h - (int)s->phys.height,
                    w, h, anim, 3, 0.0f, dino);
    }
    char line[120];
    draw_text(app->ren, app->font, "SOLO MODE", 2, 1, dino);
    snprintf(line, sizeof(line), "LV.%d  SCORE %05d  CLEAR %03d", calc_level(s->tick_interval), s->score, s->clears);
    draw_text(app->ren, app->font, line, 49, 1, dino);
    if (s->show_hitbox) {
        HitBox dh = dino_hitbox(SOLO_BASE_Y - (int)s->phys.height, s->is_ducking);
        HitBox oh = obstacle_hitbox(s->obs_x, s->obs_type, SOLO_BASE_Y);
        SDL_SetRenderDrawColor(app->ren, 230, 30, 30, 220);
        SDL_Rect dr = {dh.left * CELL_W, dh.top * CELL_H, (dh.right - dh.left + 1) * CELL_W, (dh.bot - dh.top + 1) * CELL_H};
        SDL_RenderDrawRect(app->ren, &dr);
        SDL_SetRenderDrawColor(app->ren, 30, 70, 230, 220);
        SDL_Rect orc = {oh.left * CELL_W, oh.top * CELL_H, (oh.right - oh.left + 1) * CELL_W, (oh.bot - oh.top + 1) * CELL_H};
        SDL_RenderDrawRect(app->ren, &orc);
    }
    draw_text(app->ren, app->font_small, "SPACE/UP JUMP   DOWN DUCK   H HITBOX   ESC BACK", 2, 27,
              (SDL_Color){75,80,95,255});
    draw_text(app->ren, app->font_small, "ITEMS OFF : pure survival record mode", 2, 28,
              (SDL_Color){100,105,120,255});
}

static void render_duo_game(App *app, const RoundState *s, const NetPacket *peer,
                            int is_host) {
    RGB3 ground;
    SDL_Color dino;
    render_world_background(app, s->score, s->tick_interval, &ground, &dino);
    SDL_Color opponent = {170,170,170,255};
    draw_ground(app->ren, DUO_MY_BASE_Y, ground);
    draw_ground(app->ren, DUO_OP_BASE_Y, ground);
    draw_divider(app->ren);
    int anim = s->phys.height > 0.5f ? 0 : 1 + (s->tick_count / 2) % 2;
    int ptero = (s->tick_count / 2) % 2;
    draw_obstacle(app->ren, app, s->obs_x, s->obs_type, DUO_MY_BASE_Y, ptero, dino);
    if (!s->dead) {
        int h = s->is_ducking ? 2 : 4, w = s->is_ducking ? 6 : 5;
        draw_sprite(app->ren, app->tex_dino, DINO_X, DUO_MY_BASE_Y - h - (int)s->phys.height,
                    w, h, anim, 3, 0.0f, dino);
    } else {
        draw_sprite(app->ren, app->tex_dino, DINO_X, DUO_MY_BASE_Y - 4, 5, 4,
                    0, 3, 180.0f, (SDL_Color){80,80,80,255});
    }
    draw_obstacle(app->ren, app, peer->obs_x, peer->obs_type, DUO_OP_BASE_Y, ptero, opponent);
    if (!peer->is_dead) {
        int jump = DINO_BASE - peer->dino_y;
        int h = peer->is_ducking ? 2 : 4, w = peer->is_ducking ? 6 : 5;
        int pa = jump > 0 ? 0 : 1 + (s->tick_count / 2) % 2;
        draw_sprite(app->ren, app->tex_dino, DINO_X, DUO_OP_BASE_Y - h - jump,
                    w, h, pa, 3, 0.0f, opponent);
    } else {
        draw_sprite(app->ren, app->tex_dino, DINO_X, DUO_OP_BASE_Y - 4, 5, 4,
                    0, 3, 180.0f, (SDL_Color){80,80,80,255});
    }
    char my[120], op[120];
    snprintf(my, sizeof(my), "%s  LV.%d SCORE %05d CLEAR %03d",
             is_host ? "P1 (ME)" : "P2 (ME)", calc_level(s->tick_interval), s->score, s->clears);
    snprintf(op, sizeof(op), "%s  SCORE %05d%s",
             is_host ? "P2 (PEER)" : "P1 (PEER)", peer->score, peer->is_dead ? "  [DEAD]" : "");
    draw_text(app->ren, app->font_small, my, 2, 1, dino);
    draw_text(app->ren, app->font_small, op, 2, DUO_MY_BASE_Y + 1, opponent);
    draw_inventory(app, s);
    if (s->show_hitbox) {
        HitBox dh = dino_hitbox(DUO_MY_BASE_Y - (int)s->phys.height, s->is_ducking);
        HitBox oh = obstacle_hitbox(s->obs_x, s->obs_type, DUO_MY_BASE_Y);
        HitBox ph = dino_hitbox(DUO_OP_BASE_Y - (DINO_BASE - peer->dino_y), peer->is_ducking);
        HitBox po = obstacle_hitbox(peer->obs_x, peer->obs_type, DUO_OP_BASE_Y);
        SDL_SetRenderDrawColor(app->ren, 230, 30, 30, 220);
        SDL_Rect a = {dh.left * CELL_W, dh.top * CELL_H, (dh.right-dh.left+1)*CELL_W, (dh.bot-dh.top+1)*CELL_H};
        SDL_Rect b = {ph.left * CELL_W, ph.top * CELL_H, (ph.right-ph.left+1)*CELL_W, (ph.bot-ph.top+1)*CELL_H};
        SDL_RenderDrawRect(app->ren, &a); SDL_RenderDrawRect(app->ren, &b);
        SDL_SetRenderDrawColor(app->ren, 30, 70, 230, 220);
        SDL_Rect c = {oh.left * CELL_W, oh.top * CELL_H, (oh.right-oh.left+1)*CELL_W, (oh.bot-oh.top+1)*CELL_H};
        SDL_Rect d = {po.left * CELL_W, po.top * CELL_H, (po.right-po.left+1)*CELL_W, (po.bot-po.top+1)*CELL_H};
        SDL_RenderDrawRect(app->ren, &c); SDL_RenderDrawRect(app->ren, &d);
    }
    if (now_ms() < s->notice_until) {
        draw_item_banner(app, s, 94);
    }
    if (now_ms() < s->blind_until) {
        draw_filled(app->ren, 0, 2 * CELL_H, WIN_W, (DUO_MY_BASE_Y - 2) * CELL_H,
                    (SDL_Color){10,10,20,220});
        draw_item_icon(app->ren, ITEM_BLIND, WIN_W / 2 - 42, 138, 84, 1);
        draw_text_center(app->ren, app->font, "VISION BLOCKED", 242,
                         (SDL_Color){255,110,100,255});
    }
    if (now_ms() < s->speed_until) {
        draw_item_icon(app->ren, ITEM_SPEED, WIN_W - 174, 111, 36, 1);
        draw_text_px(app->ren, app->font_small, "ACTIVE", WIN_W - 128, 120,
                     (SDL_Color){185,65,42,255});
    }
    draw_text(app->ren, app->font_small, "SPACE/UP JUMP  DOWN DUCK  A ITEM  H HITBOX  ESC FORFEIT", 2, 29,
              (SDL_Color){80,85,100,255});
}

/* ═══════════════ 결과 화면 ═══════════════════════════════ */

static int result_screen(App *app, const GameRecord *rec, int allow_retry, int *quit) {
    /* return: 0 menu, 1 retry, -1 quit */
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return -1; }
            if (e.type == SDL_KEYDOWN) {
                if (allow_retry && e.key.keysym.sym == SDLK_r) return 1;
                if (e.key.keysym.sym == SDLK_m || e.key.keysym.sym == SDLK_ESCAPE ||
                    e.key.keysym.sym == SDLK_RETURN) return 0;
                if (e.key.keysym.sym == SDLK_q) { *quit = 1; return -1; }
            }
        }
        render_menu_background(app, rec->score);
        draw_filled(app->ren, 170, 95, WIN_W - 340, 480, (SDL_Color){255,255,255,240});
        draw_outline(app->ren, 170, 95, WIN_W - 340, 480, (SDL_Color){75,100,130,255});
        const char *title = rec->mode == MODE_SOLO ? "GAME OVER" :
                            rec->result == RESULT_WIN ? "YOU WIN!" :
                            rec->result == RESULT_LOSE ? "YOU LOSE" : "DRAW";
        SDL_Color title_color = rec->result == RESULT_WIN ? (SDL_Color){30,150,65,255} :
                                rec->result == RESULT_LOSE ? (SDL_Color){210,55,55,255} :
                                (SDL_Color){55,65,80,255};
        draw_text_center(app->ren, app->font_large, title, 135, title_color);
        char line[100];
        snprintf(line, sizeof(line), "SCORE : %05d   SURVIVAL : %.1f sec", rec->score, rec->score / 10.0);
        draw_text_center(app->ren, app->font, line, 260, (SDL_Color){55,65,80,255});
        snprintf(line, sizeof(line), "OBSTACLES CLEARED : %d", rec->clears);
        draw_text_center(app->ren, app->font, line, 305, (SDL_Color){55,65,80,255});
        if (rec->mode == MODE_DUO) {
            snprintf(line, sizeof(line), "PEER SCORE : %05d", rec->peer_score);
            draw_text_center(app->ren, app->font, line, 350, (SDL_Color){55,65,80,255});
            snprintf(line, sizeof(line), "ITEM USED : %d    ATTACK RECEIVED : %d", rec->items_used, rec->items_received);
            draw_text_center(app->ren, app->font_small, line, 405, (SDL_Color){70,80,95,255});
        }
        if (allow_retry)
            draw_text_center(app->ren, app->font_small, "[R] RETRY     [M / ESC] MAIN MENU     [Q] QUIT", 500,
                             (SDL_Color){65,75,90,255});
        else
            draw_text_center(app->ren, app->font_small, "[M / ESC] MAIN MENU     [Q] QUIT", 500,
                             (SDL_Color){65,75,90,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(16);
    }
    return -1;
}

/* ═══════════════ 솔로 모드 ═══════════════════════════════ */

static void play_solo(App *app, SessionStats *stats, int *quit) {
    int retry = 1;
    while (retry && !*quit) {
        retry = 0;
        RoundState state;
        round_init(&state);
        int cancelled = 0;
        while (!*quit && !state.dead && !cancelled) {
            long long t = now_ms();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) { *quit = 1; break; }
                if (e.type == SDL_KEYDOWN) {
                    if (e.key.keysym.sym == SDLK_ESCAPE) { cancelled = 1; break; }
                    if (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_UP) phys_jump(&state.phys);
                    if (e.key.keysym.sym == SDLK_DOWN) state.last_duck_ms = t;
                    if (e.key.keysym.sym == SDLK_h) state.show_hitbox = !state.show_hitbox;
                }
            }
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            state.is_ducking = (state.phys.on_ground &&
                                 (keys[SDL_SCANCODE_DOWN] || t - state.last_duck_ms < 260));
            update_player(&state, MODE_SOLO, SOLO_BASE_Y);
            render_solo_game(app, &state);
            SDL_RenderPresent(app->ren);
            SDL_Delay(16);
        }
        if (*quit || cancelled) return;
        GameRecord rec = {MODE_SOLO, RESULT_SOLO_END, state.score, 0,
                          state.clears, 0, 0};
        stats_add(stats, rec);
        int action = result_screen(app, &rec, 1, quit);
        retry = (action == 1);
    }
}

/* ═══════════════ 듀오 로비/게임 ══════════════════════════ */

static int duo_ready_lobby(App *app, NetLink *net, int is_host, int *quit) {
    int ready = 0;
    int peer_ready = 0;
    RoundState neutral;
    round_init(&neutral);
    NetPacket peer;
    memset(&peer, 0, sizeof(peer));
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return 0; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) return 0;
                if (e.key.keysym.sym == SDLK_SPACE) ready = 1;
            }
        }
        NetPacket mine = make_packet(&neutral, ready);
        mine.attack_type = 0;
        net_send_packet(net, &mine);
        int has = 0;
        if (net_poll_latest(net, &peer, &has) < 0) {
            show_message(app, "CONNECTION LOST",
                         net->version_error ? "Different program version connected." : "Opponent disconnected.", quit);
            return 0;
        }
        if (has) peer_ready = peer.is_ready;
        render_menu_background(app, 400);
        draw_text_center(app->ren, app->font_large, "DUO LOBBY", 90, (SDL_Color){45,55,70,255});
        draw_text_center(app->ren, app->font_small,
                         is_host ? "ROLE : HOST (P1)" : "ROLE : JOIN (P2)", 170,
                         (SDL_Color){65,75,95,255});
        draw_text_center(app->ren, app->font,
                         ready ? "YOU : READY" : "YOU : PRESS [SPACE] TO READY", 270,
                         ready ? (SDL_Color){35,145,65,255} : (SDL_Color){60,70,90,255});
        draw_text_center(app->ren, app->font,
                         peer_ready ? "OPPONENT : READY" : "OPPONENT : WAITING...", 325,
                         peer_ready ? (SDL_Color){35,145,65,255} : (SDL_Color){110,110,120,255});
        draw_text_center(app->ren, app->font_small, "ESC : LEAVE ROOM", 435,
                         (SDL_Color){70,80,95,255});
        SDL_RenderPresent(app->ren);
        if (ready && peer_ready) {
            for (int n = 3; n >= 1 && !*quit; n--) {
                render_menu_background(app, 400);
                char count[16]; snprintf(count, sizeof(count), "%d", n);
                draw_text_center(app->ren, app->font_large, count, 270, (SDL_Color){210,60,45,255});
                SDL_RenderPresent(app->ren);
                SDL_Delay(650);
            }
            return 1;
        }
        SDL_Delay(30);
    }
    return 0;
}

static int play_duo_round(App *app, NetLink *net, int is_host,
                           GameRecord *completed, int *quit) {
    RoundState state;
    round_init(&state);
    NetPacket peer;
    memset(&peer, 0, sizeof(peer));
    peer.magic = PACKET_MAGIC;
    peer.version = PACKET_VERSION;
    peer.dino_y = DINO_BASE;
    peer.obs_x = WIDTH - 2;
    peer.obs_type = 1;
    int sent_attack_type = ITEM_NONE;
    int cancelled = 0;

    /* 이전 결과 화면/카운트다운에서 남은 수신 상태를 버리고 새 라운드를 시작한다. */
    net->rxlen = 0;
    { unsigned char drain[512]; while (recv(net->sock, drain, sizeof(drain), MSG_DONTWAIT) > 0) {} }

    while (!*quit && !cancelled) {
        long long t = now_ms();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; break; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    state.dead = 1;
                    cancelled = 1;
                    notice(&state, "FORFEIT", NOTICE_DURATION);
                    break;
                }
                if (!state.dead && (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_UP))
                    phys_jump(&state.phys);
                if (!state.dead && e.key.keysym.sym == SDLK_DOWN)
                    state.last_duck_ms = t;
                if (e.key.keysym.sym == SDLK_h)
                    state.show_hitbox = !state.show_hitbox;
                if (!state.dead && e.key.keysym.sym == SDLK_a && state.inv_count > 0 &&
                    t >= state.attack_cooldown_until) {
                    ItemType used = inventory_pop(&state);
                    state.attack_seq++;
                    sent_attack_type = used;
                    state.items_used++;
                    state.attack_cooldown_until = t + ITEM_USE_COOLDOWN;
                    item_notice(&state, NOTICE_ATTACK_SENT, used, NOTICE_DURATION);
                }
            }
        }
        if (*quit) break;
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        state.is_ducking = (state.phys.on_ground &&
                             (keys[SDL_SCANCODE_DOWN] || t - state.last_duck_ms < 260));
        update_player(&state, MODE_DUO, DUO_MY_BASE_Y);

        NetPacket mine = make_packet(&state, 1);
        mine.attack_type = sent_attack_type;
        if (net_send_packet(net, &mine) < 0) {
            show_message(app, "CONNECTION LOST", "Opponent disconnected.", quit);
            return 0;
        }
        int has = 0;
        if (net_poll_latest(net, &peer, &has) < 0) {
            show_message(app, "CONNECTION LOST",
                         net->version_error ? "Different program version connected." : "Opponent disconnected.", quit);
            return 0;
        }
        if (has && peer.attack_seq > state.last_peer_attack_seq) {
            state.last_peer_attack_seq = peer.attack_seq;
            apply_received_item(&state, (ItemType)peer.attack_type);
        }
        render_duo_game(app, &state, &peer, is_host);
        SDL_RenderPresent(app->ren);

        /* 대전은 한 명이 죽는 순간 종료. ESC는 현재 플레이어의 기권으로 처리. */
        if (state.dead || peer.is_dead) {
            NetPacket final_packet = make_packet(&state, 1);
            final_packet.attack_type = sent_attack_type;
            for (int i = 0; i < 3; i++) {
                net_send_packet(net, &final_packet);
                SDL_Delay(10);
            }
            break;
        }
        SDL_Delay(16);
    }
    if (*quit || net->disconnected || net->version_error) return 0;

    ResultType result;
    if (state.dead && peer.is_dead) result = RESULT_DRAW;
    else if (state.dead) result = RESULT_LOSE;
    else result = RESULT_WIN;
    *completed = (GameRecord){MODE_DUO, result, state.score, peer.score,
                              state.clears, state.items_used, state.items_received};
    return 1;
}

static int duo_rematch_countdown(App *app, int *quit) {
    for (int n = 3; n >= 1 && !*quit; n--) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; return 0; }
        }
        render_menu_background(app, 400);
        draw_text_center(app->ren, app->font_small, "REMATCH", 190,
                         (SDL_Color){65,75,95,255});
        char count[16];
        snprintf(count, sizeof(count), "%d", n);
        draw_text_center(app->ren, app->font_large, count, 270,
                         (SDL_Color){210,60,45,255});
        SDL_RenderPresent(app->ren);
        SDL_Delay(650);
    }
    return !*quit;
}

static int duo_result_screen(App *app, NetLink *net, const GameRecord *rec, int *quit) {
    /* return: 1=rematch, 0=main menu, -1=quit or lost connection */
    int my_action = 0;   /* 0 waiting, 1 rematch, 2 menu */
    int peer_action = 0;
    while (!*quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { *quit = 1; my_action = 2; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_r) my_action = 1;
                if (e.key.keysym.sym == SDLK_m || e.key.keysym.sym == SDLK_ESCAPE ||
                    e.key.keysym.sym == SDLK_RETURN) my_action = 2;
                if (e.key.keysym.sym == SDLK_q) { *quit = 1; my_action = 2; }
            }
        }

        NetPacket control;
        memset(&control, 0, sizeof(control));
        control.magic = PACKET_MAGIC;
        control.version = PACKET_VERSION;
        control.score = rec->score;
        control.is_dead = 1;
        control.session_cmd = my_action;
        if (net_send_packet(net, &control) < 0) return -1;

        NetPacket incoming;
        int has = 0;
        if (net_poll_latest(net, &incoming, &has) < 0) {
            if (!*quit && my_action != 2)
                show_message(app, "CONNECTION LOST", "Opponent left the room.", quit);
            return -1;
        }
        if (has && incoming.session_cmd != 0) peer_action = incoming.session_cmd;

        render_menu_background(app, rec->score);
        draw_filled(app->ren, 170, 82, WIN_W - 340, 520, (SDL_Color){255,255,255,242});
        draw_outline(app->ren, 170, 82, WIN_W - 340, 520, (SDL_Color){75,100,130,255});
        const char *title = rec->result == RESULT_WIN ? "YOU WIN!" :
                            rec->result == RESULT_LOSE ? "YOU LOSE" : "DRAW";
        SDL_Color title_color = rec->result == RESULT_WIN ? (SDL_Color){30,150,65,255} :
                                rec->result == RESULT_LOSE ? (SDL_Color){210,55,55,255} :
                                (SDL_Color){55,65,80,255};
        draw_text_center(app->ren, app->font_large, title, 125, title_color);
        char line[120];
        snprintf(line, sizeof(line), "MY SCORE %05d        PEER SCORE %05d", rec->score, rec->peer_score);
        draw_text_center(app->ren, app->font, line, 250, (SDL_Color){55,65,80,255});
        snprintf(line, sizeof(line), "CLEAR %03d       ITEM USED %02d       HIT %02d",
                 rec->clears, rec->items_used, rec->items_received);
        draw_text_center(app->ren, app->font_small, line, 315, (SDL_Color){70,80,95,255});
        draw_text_center(app->ren, app->font_small,
                         "[R] REMATCH       [M / ESC] MAIN MENU       [Q] QUIT", 435,
                         (SDL_Color){65,75,90,255});
        if (my_action == 1 && peer_action != 1)
            draw_text_center(app->ren, app->font_small, "Waiting for opponent's rematch choice...", 500,
                             (SDL_Color){170,105,20,255});
        else if (peer_action == 1 && my_action == 0)
            draw_text_center(app->ren, app->font_small, "Opponent wants a rematch. Press [R] to accept.", 500,
                             (SDL_Color){30,130,75,255});
        SDL_RenderPresent(app->ren);

        if (my_action == 2 || peer_action == 2) return 0;
        if (my_action == 1 && peer_action == 1) return 1;
        SDL_Delay(16);
    }
    return -1;
}

static void play_duo(App *app, SessionStats *stats, int *quit) {
    int role = duo_role_menu(app, quit);
    if (*quit || role == 0) return;
    NetLink net;
    net_init(&net);
    int connected = 0;
    if (role == 1) {
        connected = host_wait(app, &net, quit);
    } else {
        char ip[64];
        if (!input_ip_screen(app, ip, sizeof(ip), quit)) return;
        connected = join_connect(app, ip, &net, quit);
    }
    if (!connected || *quit) { net_close(&net); return; }
    int start_game = duo_ready_lobby(app, &net, role == 1, quit);
    while (start_game && !*quit && !net.disconnected && !net.version_error) {
        GameRecord rec;
        if (!play_duo_round(app, &net, role == 1, &rec, quit)) break;
        stats_add(stats, rec);
        int action = duo_result_screen(app, &net, &rec, quit);
        if (action == 1) {
            start_game = duo_rematch_countdown(app, quit);
        } else {
            start_game = 0;
        }
    }
    net_close(&net);
}

/* ═══════════════ main ════════════════════════════════════ */

int main(int argc, char *argv[]) {
    (void)argc;
    App app;
    if (app_init(&app, argv[0]) < 0) {
        app_destroy(&app);
        return 1;
    }
    srand((unsigned)time(NULL));
    SessionStats stats;
    memset(&stats, 0, sizeof(stats));
    int quit = 0;
    while (!quit) {
        MainChoice choice = main_menu(&app, &stats, &quit);
        if (choice == MENU_DUO) play_duo(&app, &stats, &quit);
        else if (choice == MENU_SOLO) play_solo(&app, &stats, &quit);
        else if (choice == MENU_RECORDS) show_records(&app, &stats, &quit);
        else if (choice == MENU_HELP) show_help(&app, &quit);
    }
    app_destroy(&app);
    return 0;
}
