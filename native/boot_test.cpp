#ifndef PEPPY_UI_PREVIEW
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#endif
#include <stdint.h>
#include <stddef.h>
#include "ui_assets.h"

static const int W = 1920, H = 1080;
static const uint32_t BG = 0x800B0F17, PANEL = 0x80131925;
static const uint32_t WHITE = 0x80F4F6FA, MUTED = 0x809BA7BA;
static const uint32_t BLUE = 0x8073B7FF, LINE = 0x80252D3C;

static uint32_t mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    uint32_t out = 0x80000000;
    for (int shift = 0; shift <= 16; shift += 8)
        out |= ((((a >> shift) & 255) * (255 - t) + ((b >> shift) & 255) * t) / 255) << shift;
    return out;
}

static void pixel(uint32_t* p, int x, int y, uint32_t color, int alpha = 255) {
    if (x < 0 || y < 0 || x >= W || y >= H || alpha <= 0) return;
    p[y * W + x] = mix(p[y * W + x], color, alpha);
}

static void rect(uint32_t* p, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) p[yy * W + xx] = color;
}

static int roundCoverage(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (r <= 0) return 255;
    int dx = x < r ? 2 * (r - x) - 1 : (x >= w - r ? 2 * (x - w + r) + 1 : 0);
    int dy = y < r ? 2 * (r - y) - 1 : (y >= h - r ? 2 * (y - h + r) + 1 : 0);
    if (!dx || !dy) return 255;
    int d = dx * dx + dy * dy;
    int inner = (2 * r - 1) * (2 * r - 1), outer = (2 * r + 1) * (2 * r + 1);
    if (d <= inner) return 255;
    if (d >= outer) return 0;
    return 255 * (outer - d) / (outer - inner);
}

static void roundRect(uint32_t* p, int x, int y, int w, int h, int r, uint32_t c, int alpha = 255) {
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r) * alpha / 255);
}

static void outline(uint32_t* p, int x, int y, int w, int h, int r, int thickness, uint32_t c) {
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            if (xx >= r && xx < w - r && yy >= thickness && yy < h - thickness) continue;
            int outer = roundCoverage(xx, yy, w, h, r);
            int inner = roundCoverage(xx - thickness, yy - thickness, w - 2 * thickness,
                                      h - 2 * thickness, r - thickness);
            pixel(p, x + xx, y + yy, c, outer > inner ? outer - inner : 0);
        }
    }
}

static void gradient(uint32_t* p, int x, int y, int w, int h, int r, uint32_t left, uint32_t right) {
    for (int xx = 0; xx < w; ++xx) {
        uint32_t c = mix(left, right, w > 1 ? xx * 255 / (w - 1) : 0);
        for (int yy = 0; yy < h; ++yy)
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r));
    }
}

static void line(uint32_t* p, float x1, float y1, float x2, float y2, float thickness, uint32_t c) {
    float radius = thickness * 0.5f, inner = radius > 0.5f ? radius - 0.5f : 0.0f;
    float outer = radius + 0.5f, vx = x2 - x1, vy = y2 - y1, length2 = vx * vx + vy * vy;
    int left = (int)(x1 < x2 ? x1 : x2) - (int)outer - 1;
    int top = (int)(y1 < y2 ? y1 : y2) - (int)outer - 1;
    int right = (int)(x1 > x2 ? x1 : x2) + (int)outer + 1;
    int bottom = (int)(y1 > y2 ? y1 : y2) + (int)outer + 1;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= W) right = W - 1;
    if (bottom >= H) bottom = H - 1;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            float dx = x + 0.5f - x1, dy = y + 0.5f - y1;
            float t = length2 > 0 ? (dx * vx + dy * vy) / length2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            dx -= t * vx; dy -= t * vy;
            float distance2 = dx * dx + dy * dy;
            if (distance2 < outer * outer) {
                int alpha = distance2 <= inner * inner ? 255 :
                    (int)(255 * (outer * outer - distance2) / (outer * outer - inner * inner));
                pixel(p, x, y, c, alpha);
            }
        }
    }
}

static void ring(uint32_t* p, int cx, int cy, int radius, int thickness, uint32_t c) {
    int inner = radius - thickness;
    for (int y = -radius - 1; y <= radius + 1; ++y) {
        for (int x = -radius - 1; x <= radius + 1; ++x) {
            int d = x * x + y * y;
            int outer = (radius + 1) * (radius + 1), full = radius * radius;
            int hole = inner * inner, edge = (inner + 1) * (inner + 1);
            int alpha = d <= full ? 255 : (d < outer ? 255 * (outer - d) / (outer - full) : 0);
            if (d <= hole) alpha = 0;
            else if (d < edge) alpha = alpha * (d - hole) / (edge - hole);
            pixel(p, cx + x, cy + y, c, alpha);
        }
    }
}

static uint32_t nextCodepoint(const char*& s) {
    uint32_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && ((uint8_t)*s & 0xC0) == 0x80)
        return ((c & 31) << 6) | ((uint8_t)*s++ & 63);
    return '?';
}

static const UiGlyph* glyph(const UiFont& font, uint32_t c) {
    for (int i = 0; i < font.count; ++i) if (font.glyphs[i].code == c) return font.glyphs + i;
    for (int i = 0; i < font.count; ++i) if (font.glyphs[i].code == '?') return font.glyphs + i;
    return 0;
}

static int textWidth(const char* s, const UiFont& font) {
    int width = 0;
    while (*s) { const UiGlyph* g = glyph(font, nextCodepoint(s)); if (g) width += g->advance; }
    return width;
}

static void text(uint32_t* p, int x, int y, const char* s, const UiFont& font, uint32_t c) {
    int baseline = y + font.ascent;
    while (*s) {
        const UiGlyph* g = glyph(font, nextCodepoint(s));
        if (!g) continue;
        for (int yy = 0; yy < g->height; ++yy)
            for (int xx = 0; xx < g->width; ++xx)
                pixel(p, x + g->left + xx, baseline + g->top + yy, c,
                      font.pixels[g->offset + yy * g->width + xx]);
        x += g->advance;
    }
}

static void pill(uint32_t* p, int x, int y, const char* label, uint32_t bg, uint32_t fg) {
    int w = textWidth(label, FONT_SMALL) + 32;
    roundRect(p, x, y, w, 38, 19, bg);
    text(p, x + 16, y + 4, label, FONT_SMALL, fg);
}

static void artwork(uint32_t* p, int x, int y, int w, int h, int r, bool fade = false) {
    int srcW = ui_peppy_width, srcH = ui_peppy_height, startX = 0, startY = 0;
    if (w > h) { srcH = ui_peppy_height * h / w; startY = (ui_peppy_height - srcH) / 2; }
    else { srcW = ui_peppy_width * w / h; startX = (ui_peppy_width - srcW) / 2; }
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            uint32_t c = 0x80000000 | ui_peppy_pixels[(startY + yy * srcH / h) * ui_peppy_width + startX + xx * srcW / w];
            if (fade) {
                c = mix(c, 0x80121F34, 20);
                if (xx < 165) c = mix(c, 0x80121F34, (165 - xx) * 255 / 165);
                if (yy > h - 90) c = mix(c, BG, (yy - h + 90) * 130 / 90);
            }
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r));
        }
    }
}

static const char* NAMES[4] = {"Apollo Save Tool", "Itemzflow", "PS4 Xplorer", "Homebrew"};
static const char* CATEGORIES[4] = {"SAVES", "BIBLIOTECA", "ARQUIVOS", "COMUNIDADE"};
static const char* CAPTIONS[4] = {"Seus saves, organizados.", "Organize sua biblioteca.", "Explore seus arquivos.", "Descubra novos projetos."};
static const uint32_t ACCENTS[4] = {0x807BDECC, 0x80B9A2F9, 0x8085BBFB, 0x80EBC48A};

static void appIcon(uint32_t* p, int x, int y, int size, int id, uint32_t c) {
    int u = size / 8;
    int t = size > 120 ? 6 : 3;
    if (id == 0) {
        outline(p, x + u, y + u, size - 2 * u, size - 2 * u, u / 2, t, c);
        outline(p, x + 2 * u, y + u, 4 * u, 2 * u, 2, t, c);
        outline(p, x + 2 * u, y + 4 * u, 4 * u, 3 * u, 2, t, c);
        line(p, x + 3 * u, y + 5 * u, x + 5 * u, y + 5 * u, t, c);
    } else if (id == 1) {
        outline(p, x + u, y + 2 * u, 6 * u, 4 * u, u, t, c);
        line(p, x + 2 * u, y + 4 * u, x + 4 * u, y + 4 * u, t, c);
        line(p, x + 3 * u, y + 3 * u, x + 3 * u, y + 5 * u, t, c);
        roundRect(p, x + 5 * u, y + 3 * u, t + 2, t + 2, 2, c);
        roundRect(p, x + 6 * u, y + 4 * u, t + 2, t + 2, 2, c);
    } else if (id == 2) {
        line(p, x + u, y + 2 * u, x + 3 * u, y + 2 * u, t, c);
        line(p, x + 3 * u, y + 2 * u, x + 4 * u, y + 3 * u, t, c);
        line(p, x + 4 * u, y + 3 * u, x + 7 * u, y + 3 * u, t, c);
        line(p, x + 7 * u, y + 3 * u, x + 7 * u, y + 6 * u, t, c);
        line(p, x + 7 * u, y + 6 * u, x + u, y + 6 * u, t, c);
        line(p, x + u, y + 6 * u, x + u, y + 2 * u, t, c);
    } else {
        for (int yy = 0; yy < 2; ++yy)
            for (int xx = 0; xx < 2; ++xx)
                outline(p, x + u + xx * 4 * u, y + u + yy * 4 * u, 2 * u, 2 * u, u / 2, t, c);
    }
}

static void crossButton(uint32_t* p, int x, int y) {
    ring(p, x, y, 17, 2, MUTED);
    line(p, x - 6, y - 6, x + 6, y + 6, 2, BLUE);
    line(p, x + 6, y - 6, x - 6, y + 6, 2, BLUE);
}

static void header(uint32_t* p) {
    rect(p, 0, 0, W, H, BG);
    artwork(p, 72, 48, 60, 60, 14);
    text(p, 150, 45, "PEPPY", FONT_TITLE, WHITE);
    text(p, 152, 84, "S T O R E", FONT_SMALL, MUTED);
    text(p, 480, 65, "Descobrir", FONT_CARD, WHITE);
    roundRect(p, 480, 119, 140, 3, 1, BLUE);
    text(p, 694, 67, "Apps para o seu PS4", FONT_BODY, MUTED);
    const char* label = "PS4  /  HOMEBREW";
    pill(p, W - 72 - textWidth(label, FONT_SMALL) - 32, 61, label, PANEL, MUTED);
    rect(p, 72, 130, W - 144, 1, LINE);
}

static void footer(uint32_t* p, bool details) {
    rect(p, 72, 978, W - 144, 1, LINE);
    if (details) {
        ring(p, 92, 1018, 12, 2, 0x80EBA5B4);
        text(p, 120, 1001, "Voltar à biblioteca", FONT_BODY, WHITE);
    } else {
        line(p, 81, 1018, 89, 1010, 2, MUTED);
        line(p, 81, 1018, 89, 1026, 2, MUTED);
        line(p, 109, 1018, 101, 1010, 2, MUTED);
        line(p, 109, 1018, 101, 1026, 2, MUTED);
        text(p, 130, 1001, "Navegar", FONT_BODY, MUTED);
        crossButton(p, 334, 1018);
        text(p, 366, 1001, "Ver detalhes", FONT_BODY, WHITE);
    }
    const char* label = "Feito para o seu PS4.";
    text(p, W - 72 - textWidth(label, FONT_SMALL), 1004, label, FONT_SMALL, MUTED);
}

static void drawStore(uint32_t* p, int selected, int padState) {
    (void)padState;
    header(p);
    gradient(p, 72, 164, 1776, 380, 24, 0x80192540, 0x80121F34);
    artwork(p, 1288, 164, 560, 380, 24, true);
    pill(p, 112, 201, "BEM-VINDO À PEPPY", 0x8023334E, BLUE);
    text(p, 108, 252, "Seu PS4.", FONT_HERO, WHITE);
    text(p, 108, 332, "Do seu jeito.", FONT_HERO, WHITE);
    text(p, 112, 444, "Homebrews e ferramentas em um só lugar.", FONT_BODY, 0x80B6C4DB);
    pill(p, 1604, 482, "PEPPY ORIGINAL", 0x80111B2B, WHITE);

    text(p, 72, 578, "Explore sua biblioteca", FONT_TITLE, WHITE);
    text(p, 1718, 587, "04 apps", FONT_SMALL, MUTED);
    const int start = 72, y = 650, cw = 426, ch = 294, gap = 24;
    for (int i = 0; i < 4; ++i) {
        int x = start + i * (cw + gap);
        bool focus = selected == i;
        if (focus) roundRect(p, x - 5, y - 5, cw + 10, ch + 10, 25, 0x80243A58);
        roundRect(p, x, y, cw, ch, 20, focus ? 0x801B283B : PANEL);
        outline(p, x, y, cw, ch, 20, focus ? 2 : 1, focus ? BLUE : LINE);
        roundRect(p, x + 26, y + 27, 104, 104, 24, mix(PANEL, ACCENTS[i], 22));
        appIcon(p, x + 42, y + 43, 72, i, ACCENTS[i]);
        text(p, x + 152, y + 53, CATEGORIES[i], FONT_SMALL, ACCENTS[i]);
        text(p, x + 152, y + 88, "PS4 app", FONT_SMALL, MUTED);
        text(p, x + 26, y + 157, NAMES[i], FONT_CARD, WHITE);
        text(p, x + 26, y + 203, CAPTIONS[i], FONT_SMALL, MUTED);
        text(p, x + 26, y + 254, "Ver detalhes", FONT_SMALL, focus ? BLUE : MUTED);
        line(p, x + cw - 51, y + 268, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
        line(p, x + cw - 40, y + 261, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
        line(p, x + cw - 40, y + 275, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
    }
    footer(p, false);
}

static void drawDetails(uint32_t* p, int selected) {
    header(p);
    text(p, 72, 169, "Biblioteca", FONT_BODY, MUTED);
    text(p, 225, 169, "/", FONT_BODY, MUTED);
    text(p, 254, 169, NAMES[selected], FONT_BODY, WHITE);
    gradient(p, 72, 246, 552, 626, 28, mix(PANEL, ACCENTS[selected], 28), PANEL);
    outline(p, 72, 246, 552, 626, 28, 1, LINE);
    roundRect(p, 232, 359, 232, 232, 46, mix(PANEL, ACCENTS[selected], 20));
    appIcon(p, 260, 387, 176, selected, ACCENTS[selected]);
    int labelWidth = textWidth(NAMES[selected], FONT_TITLE);
    text(p, 348 - labelWidth / 2, 659, NAMES[selected], FONT_TITLE, WHITE);
    int categoryWidth = textWidth(CATEGORIES[selected], FONT_SMALL) + 32;
    pill(p, 348 - categoryWidth / 2, 725, CATEGORIES[selected], 0x80212B3B, ACCENTS[selected]);

    pill(p, 708, 252, "APP HOMEBREW", 0x80212D41, BLUE);
    text(p, 704, 321, NAMES[selected], FONT_HEADING, WHITE);
    text(p, 708, 396, CAPTIONS[selected], FONT_BODY, MUTED);
    rect(p, 708, 463, 1140, 1, LINE);
    text(p, 708, 496, "Sobre este app", FONT_TITLE, WHITE);
    const char* first[4] = {
        "Uma ferramenta para gerenciar seus arquivos de save",
        "Um gerenciador para explorar e organizar sua biblioteca",
        "Um explorador para navegar pelas pastas e arquivos",
        "Um espaço para descobrir aplicativos e projetos"
    };
    const char* second[4] = {
        "e manter tudo organizado no seu PlayStation 4.",
        "de aplicativos no PlayStation 4.",
        "armazenados no seu PlayStation 4.",
        "criados pela comunidade de homebrew do PS4."
    };
    text(p, 708, 558, first[selected], FONT_BODY, MUTED);
    text(p, 708, 599, second[selected], FONT_BODY, MUTED);
    roundRect(p, 708, 713, 480, 72, 16, 0x801C2432);
    text(p, 738, 732, "Download indisponível", FONT_BODY, MUTED);
    text(p, 708, 811, "Explore os outros apps na biblioteca.", FONT_SMALL, MUTED);
    footer(p, true);
}

#ifndef PEPPY_UI_PREVIEW
int main(void){
 sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
 int32_t video=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,0);
 if(video<0)for(;;)sceKernelUsleep(1000000);

 const size_t one=(size_t)W*H*4,align=0x200000;
 const size_t sz=one*2,alloc=((sz+align-1)/align)*align;
 off_t off=0;int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),alloc,align,3,&off);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 void*mem=0;rc=sceKernelMapDirectMemory(&mem,alloc,0x33,0,off,align);
 if(rc<0||!mem)for(;;)sceKernelUsleep(1000000);

 uint32_t*fb[2]={(uint32_t*)mem,(uint32_t*)((char*)mem+one)};
 OrbisVideoOutBufferAttribute attr;sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,W,H,W);
 void*bufs[2]={fb[0],fb[1]};rc=sceVideoOutRegisterBuffers(video,0,bufs,2,&attr);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 sceVideoOutSetFlipRate(video,0);

 int32_t userModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
 int32_t userId=-1,userRc=userModule;
 if(userModule>=0){
  OrbisUserServiceInitializeParams usp;
  usp.priority=ORBIS_KERNEL_PRIO_FIFO_LOWEST;
  userRc=sceUserServiceInitialize(&usp);
  if(userRc==0) userRc=sceUserServiceGetInitialUser(&userId);
 }
 int32_t padModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
 int32_t padInit=(padModule>=0)?scePadInit():padModule;
 int32_t pad=(padInit==0 && userRc==0)?scePadOpen(userId,0,0,0):-1;

 int selected=0,front=0;bool details=false;uint32_t prev=0;int64_t frame=1;
 drawStore(fb[front],selected,2);
 sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);

 for(;;){
  OrbisPadData pd;
  if(pad>=0 && scePadReadState(pad,&pd)>=0){
   static bool readShown=false;
   if(!readShown){front=1-front;drawStore(fb[front],selected,2);sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);readShown=true;}
   uint32_t now=pd.buttons;
   bool changed=false;
   if(!details && (now&ORBIS_PAD_BUTTON_RIGHT)&&!(prev&ORBIS_PAD_BUTTON_RIGHT)){selected=(selected+1)%4;changed=true;}
   if(!details && (now&ORBIS_PAD_BUTTON_LEFT)&&!(prev&ORBIS_PAD_BUTTON_LEFT)){selected=(selected+3)%4;changed=true;}
   if(!details && (now&ORBIS_PAD_BUTTON_CROSS)&&!(prev&ORBIS_PAD_BUTTON_CROSS)){details=true;changed=true;}
   if(details && (now&ORBIS_PAD_BUTTON_CIRCLE)&&!(prev&ORBIS_PAD_BUTTON_CIRCLE)){details=false;changed=true;}
   prev=now;
   if(changed){
    front=1-front;
    if(details) drawDetails(fb[front],selected); else drawStore(fb[front],selected,2);
    sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);
   }
  }
  sceKernelUsleep(16000);
 }
 return 0;
}
#endif
