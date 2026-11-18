/* libSceFont / libSceFontFt for Everybody's Golf (CUSA04687 1.17): the 22 imports the eboot makes.
 *
 * Derived from src/runtime_font.c of driveclub-recomp (GPL, same license as this tree). That file
 * served TrueType fonts the game hands over in memory. This game never does: it opens the PS4
 * system font set (sceFontOpenFontSet, set type 0x1808a444, six handles) and draws glyphs into six
 * 8-bit atlas surfaces. The system fonts (SST-Roman.otf and friends) are firmware files that are not
 * in the game dump, so every set is served from ONE host face instead: $GOLF_FONT_FILE, else a stock
 * system font (Arial/Segoe on Windows, DejaVu on Linux). Metrics and coverage come from stb_truetype
 * and will not match the PS4 face pixel for pixel. If no face can be loaded, sceFontOpenFontSet
 * fails with FONT_OPEN_FAILED and the game gets no glyphs.
 *
 * Not applied (stored only): sceFontSetEffectWeight / sceFontSetEffectSlant (the game sets
 * 1.04/0.96 weight and 0.15 slant on the italic sets), the device cache, the library memory
 * interface (alloc/free callbacks the game gives MemoryInit: host malloc is used instead).
 *
 * Error codes are the SCE font codes (0x8046xxxx, names from shadPS4 font_error.h). Behavior
 * marked "inferred" has no direct evidence in the game's call sites. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../gpu/third_party/imgui/imstb_truetype.h"
#pragma clang diagnostic pop

#define FONT_OK 0
#define FONT_ERR_FATAL ((int32_t)0x80460001)
#define FONT_ERR_PARAM ((int32_t)0x80460002)
#define FONT_ERR_MEMORY ((int32_t)0x80460003)
#define FONT_ERR_LIBRARY ((int32_t)0x80460004)
#define FONT_ERR_HANDLE ((int32_t)0x80460005)
#define FONT_ERR_RENDERER ((int32_t)0x80460007)
#define FONT_ERR_ALLOC ((int32_t)0x80460010)
#define FONT_ERR_OPEN_FAILED ((int32_t)0x80460036)
#define FONT_ERR_NO_GLYPH ((int32_t)0x80460042) /* the game treats this one as "skip this character" */
#define FONT_ERR_NOT_BOUND ((int32_t)0x80460061)
#define MAGIC_MEM 0x0F00u /* inferred from shadPS4: u16 at offset 0 of OrbisFontMem / library / renderer */
#define MAGIC_LIB 0x0F01u
#define MAGIC_REND 0x0F07u
#define MAGIC_FONT 0x0F05u /* ours: handles are opaque to the game */

typedef struct { uint16_t magic; uint16_t pad[3]; const void *iface; uint64_t reserved[6]; } FontMem; /* 64 bytes: the game reserves 0x40 */
typedef struct { uint16_t magic; } FontLib; /* game keeps the handle in a 0x2ccba90 slot, never reads it */
typedef struct { uint16_t magic; } Renderer;
typedef struct {
    uint16_t magic;
    stbtt_fontinfo info;
    float sx, sy;       /* sceFontSetScalePixel */
    float rx, ry;       /* sceFontSetupRenderScalePixel */
    float weightX, weightY, slant; /* stored, not applied */
    Renderer *renderer; /* bound renderer */
} Font;
typedef struct { float width, height, hbx, hby, hadv, vbx, vby, vadv; } GlyphMetrics; /* 0x20 bytes */
typedef struct { uint8_t *address; uint32_t widthByte; uint8_t pixelSizeByte, pixelFormat; uint16_t pad; } SurfaceImage;
typedef struct { float bearingX, bearingY, advance, stride; uint32_t width, height; } ImageMetrics;
typedef struct { const void *stage; SurfaceImage surface; struct { uint32_t x, y, w, h; } update; ImageMetrics image; } RenderOutput;
typedef struct {
    void *buffer; int32_t widthByte; int8_t pixelSizeByte; uint8_t pad0, styleFlag, pad2;
    int32_t width, height; uint32_t sc_x0, sc_y0, sc_x1, sc_y1; uint64_t reserved[11];
} Surface; /* 0x80 bytes: the game strides its surfaces by 0x80 */
typedef struct { float baselineOffset, lineAdvance, decorationExtent; } HorizontalLayout; /* 12 bytes */
_Static_assert(sizeof(Surface)==128,"OrbisFontRenderSurface layout");
_Static_assert(sizeof(GlyphMetrics)==32,"OrbisFontGlyphMetrics layout");
_Static_assert(sizeof(HorizontalLayout)==12,"OrbisFontHorizontalLayout layout");
_Static_assert(sizeof(FontMem)==64,"OrbisFontMem reserve");

/* Host face shared by every opened set; loaded once, never freed. */
static stbtt_fontinfo host_info;
static int host_tried, host_ready;

static unsigned char *read_file(const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    unsigned char *data=NULL;
    long n=0;
    if (fseek(f,0,SEEK_END)==0 && (n=ftell(f))>0 && n<=64L*1024L*1024L && fseek(f,0,SEEK_SET)==0 && (data=malloc((size_t)n))) {
        if (fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); data=NULL; }
    }
    fclose(f);
    return data;
}
static int host_try(const char *path) {
    if (!path || !*path) return 0;
    unsigned char *data=read_file(path);
    if (!data) return 0;
    int offset=stbtt_GetFontOffsetForIndex(data,0);
    if (offset<0 || !stbtt_InitFont(&host_info,data,offset)) { free(data); return 0; }
    printf("Runtime: libSceFont serves system font sets from %s\n",path);
    return 1;
}
static int host_font(void) {
    if (host_tried) return host_ready;
    host_tried=1;
    static const char *const fallback[]={
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf","C:/Windows/Fonts/arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    host_ready=host_try(getenv("GOLF_FONT_FILE"));
    for (size_t i=0;!host_ready && i<sizeof(fallback)/sizeof(*fallback);++i) host_ready=host_try(fallback[i]);
    if (!host_ready) printf("Runtime: libSceFont found no host face (set GOLF_FONT_FILE); text will not render\n");
    return host_ready;
}
static float scale_for(float pixels) { return pixels>0.f ? stbtt_ScaleForMappingEmToPixels(&host_info,pixels) : 0.f; }

static Font *font_of(void *h) { Font *f=h; return f && f->magic==MAGIC_FONT ? f : NULL; }
static FontLib *lib_of(void *h) { FontLib *l=h; return l && l->magic==MAGIC_LIB ? l : NULL; }

/* sceFontMemoryInit(mem, region, regionSize, iface, mspaceObj, destroyCb, destroyCtx): game call at
 * 0xc1ab0 = (0x2ccba50, 0, 0, 0x133add8, 0, 0xf19de0 (a bare ret), 0); result ignored (add rsp only).
 * Describes the allocator the library will use. We keep only the descriptor tag; host malloc serves. */
static ABI int32_t font_memory_init(void *mem,void *region,uint32_t size,const void *iface,void *mspace,void *destroy,void *ctx) {
    (void)region;(void)size;(void)mspace;(void)destroy;(void)ctx;
    if (!mem) return FONT_ERR_PARAM;
    FontMem *m=memset(mem,0,sizeof(FontMem));
    m->magic=MAGIC_MEM; m->iface=iface;
    return FONT_OK;
}
/* sceFontMemoryTerm(mem): game call at 0xc185a (shutdown, after DestroyLibrary) on the same
 * descriptor; result ignored. Invalid descriptor errors are shadPS4's. */
static ABI int32_t font_memory_term(void *mem) {
    FontMem *m=mem;
    if (!m) return FONT_ERR_PARAM;
    if (m->magic!=MAGIC_MEM) return FONT_ERR_MEMORY;
    memset(m,0,sizeof(*m));
    return FONT_OK;
}
/* sceFontSelectLibraryFt(version): game passes 0 and hands the return value to
 * CreateLibraryWithEdition as its second argument without testing it. Returns an opaque token. */
static const uint64_t ft_token[2];
static ABI const void *font_select_library_ft(int32_t version) { (void)version; return &ft_token[0]; }
/* sceFontSelectRendererFt(version): same, passed to CreateRendererWithEdition. */
static ABI const void *font_select_renderer_ft(int32_t version) { (void)version; return &ft_token[1]; }
/* sceFontCreateLibraryWithEdition(mem, selection, edition, &lib): game edition 0x0500807100000000,
 * out = 0x2ccba90, result ignored. */
static ABI int32_t font_create_library(const FontMem *mem,const void *selection,uint64_t edition,void **out) {
    (void)selection;(void)edition;
    if (!mem || !out) return FONT_ERR_PARAM;
    if (mem->magic!=MAGIC_MEM) return FONT_ERR_MEMORY;
    FontLib *l=runtime_low_alloc(sizeof(*l));
    if (!l) return FONT_ERR_ALLOC;
    l->magic=MAGIC_LIB;
    *out=l;
    return FONT_OK;
}
/* sceFontDestroyLibrary(&lib): game call at 0xc184e on the 0x2ccba90 slot; clears the slot. */
static ABI int32_t font_destroy_library(void **lib) {
    if (!lib) return FONT_ERR_PARAM;
    FontLib *l=lib_of(*lib);
    if (!l) return FONT_ERR_LIBRARY;
    l->magic=0; runtime_low_free(l); *lib=NULL;
    return FONT_OK;
}
/* sceFontSupportSystemFonts(lib): enables the firmware font sets; we always serve the host face. */
static ABI int32_t font_support_system(void *lib) { return lib_of(lib) ? FONT_OK : FONT_ERR_LIBRARY; }
/* sceFontSupportExternalFonts(lib, fontMax=16, formats=0x52): the game never opens external
 * files, so the limits are not enforced. */
static ABI int32_t font_support_external(void *lib,uint32_t max,uint32_t formats) {
    (void)max;(void)formats;
    return lib_of(lib) ? FONT_OK : FONT_ERR_LIBRARY;
}
/* sceFontCreateRendererWithEdition(mem, selection, edition, &renderer): six calls (0xc1baf), out =
 * the game's renderer slots, result ignored. */
static ABI int32_t font_create_renderer(const FontMem *mem,const void *selection,uint64_t edition,void **out) {
    (void)selection;(void)edition;
    if (!mem || !out) return FONT_ERR_PARAM;
    if (mem->magic!=MAGIC_MEM) return FONT_ERR_MEMORY;
    Renderer *r=runtime_low_alloc(sizeof(*r));
    if (!r) return FONT_ERR_ALLOC;
    r->magic=MAGIC_REND;
    *out=r;
    return FONT_OK;
}
/* sceFontDestroyRenderer(&renderer): shutdown at 0xc1394 and five more, after Unbind, before Close. */
static ABI int32_t font_destroy_renderer(void **r) {
    if (!r) return FONT_ERR_PARAM;
    Renderer *rr=*r;
    if (!rr || rr->magic!=MAGIC_REND) return FONT_ERR_RENDERER;
    rr->magic=0; runtime_low_free(rr); *r=NULL;
    return FONT_OK;
}
/* sceFontRenderSurfaceInit(surf, buffer, widthByte, pixelSizeByte, width, height): six calls at
 * 0xc286b with pixelSizeByte 1 (8-bit coverage atlas), widthByte == width. Void on the PS4; the
 * scissor is the whole surface. */
static ABI void font_surface_init(Surface *s,void *buffer,int32_t widthByte,int32_t pixelSize,int32_t w,int32_t h) {
    if (!s) return;
    memset(s,0,sizeof(*s));
    s->buffer=buffer; s->widthByte=widthByte; s->pixelSizeByte=(int8_t)pixelSize; s->width=w; s->height=h;
    s->sc_x1=(uint32_t)w; s->sc_y1=(uint32_t)h;
}
/* sceFontOpenFontSet(lib, fontSetType, openMode, params, &font): six calls at 0xc1b37, every one
 * with type 0x1808a444, mode 3, params NULL; out = the game's font slots; result ignored (an
 * unset slot is passed on to the calls below). The set type is not decoded (shadPS4 maps it to
 * SST-Roman + JP/CN/KR faces); all sets are the single host face. Modes 1..3 follow shadPS4. */
static ABI int32_t font_open_set(void *lib,uint32_t type,uint32_t mode,const void *params,void **out) {
    (void)type;(void)params;
    if (!out) return FONT_ERR_PARAM;
    *out=NULL;
    if (!lib_of(lib)) return FONT_ERR_LIBRARY;
    if (mode<1 || mode>3) return FONT_ERR_PARAM;
    if (!host_font()) return FONT_ERR_OPEN_FAILED;
    Font *f=runtime_low_alloc(sizeof(*f));
    if (!f) return FONT_ERR_ALLOC;
    f->info=host_info; f->magic=MAGIC_FONT;
    f->sx=f->sy=f->rx=f->ry=16.f; f->weightX=f->weightY=1.f;
    *out=f;
    return FONT_OK;
}
/* sceFontCloseFont(font): shutdown only. */
static ABI int32_t font_close(void *h) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    f->magic=0; runtime_low_free(f);
    return FONT_OK;
}
/* sceFontBindRenderer(font, renderer): game binds renderer i to font i right after creating it. */
static ABI int32_t font_bind_renderer(void *h,void *renderer) {
    Font *f=font_of(h);
    Renderer *r=renderer;
    if (!f) return FONT_ERR_HANDLE;
    if (!r || r->magic!=MAGIC_REND) return FONT_ERR_RENDERER;
    f->renderer=r;
    return FONT_OK;
}
/* sceFontUnbindRenderer(font): shutdown only. NOT_BOUND is inferred from the error table. */
static ABI int32_t font_unbind_renderer(void *h) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!f->renderer) return FONT_ERR_NOT_BOUND;
    f->renderer=NULL;
    return FONT_OK;
}
/* sceFontSetScalePixel(font, w, h): game sets 36.0 x 36.0 once per font. Pixels per em. */
static ABI int32_t font_set_scale(void *h,float w,float hh) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    f->sx=w; f->sy=hh;
    return FONT_OK;
}
/* sceFontSetupRenderScalePixel(font, w, h): game sets 36.0 x 36.0 after binding. Scale used by
 * the render metrics and the glyph image. */
static ABI int32_t font_setup_render_scale(void *h,float w,float hh) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    f->rx=w; f->ry=hh;
    return FONT_OK;
}
/* sceFontSetEffectWeight(font, xScale, yScale, mode): game passes 1.04/1.04/0.96 per font with
 * mode 0. Stored only: stb_truetype has no emboldening, so glyphs render at weight 1. */
static ABI int32_t font_set_effect_weight(void *h,uint32_t mode,float x,float y) {
    Font *f=font_of(h);
    (void)mode;
    if (!f) return FONT_ERR_HANDLE;
    f->weightX=x; f->weightY=y;
    return FONT_OK;
}
/* sceFontSetEffectSlant(font, slant): game passes 0.0 or 0.15. Stored only (inferred argument:
 * the game loads one float in xmm0 and nothing else). */
static ABI int32_t font_set_effect_slant(void *h,float slant) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    f->slant=slant;
    return FONT_OK;
}
/* sceFontGetHorizontalLayout(font, &layout): the game calls it after SetScalePixel and later
 * subtracts layout.baselineOffset from the y it passes to RenderCharGlyphImage (0xf1acfb), so the
 * value must equal what RenderCharGlyphImage adds back: the ascent in pixels (inferred). */
static ABI int32_t font_horizontal_layout(void *h,HorizontalLayout *out) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!out) return FONT_ERR_PARAM;
    int asc,desc,gap; stbtt_GetFontVMetrics(&f->info,&asc,&desc,&gap);
    float s=scale_for(f->sy);
    out->baselineOffset=asc*s; out->lineAdvance=(asc-desc+gap)*s; out->decorationExtent=0.f;
    return FONT_OK;
}
static int glyph_metrics(Font *f,uint32_t code,GlyphMetrics *m,int *x0,int *y0,int *x1,int *y1,int *glyph) {
    *glyph=stbtt_FindGlyphIndex(&f->info,(int)code);
    if (!*glyph) return 0;
    float xs=scale_for(f->rx), ys=scale_for(f->ry);
    int adv,lsb; stbtt_GetGlyphHMetrics(&f->info,*glyph,&adv,&lsb);
    stbtt_GetGlyphBitmapBox(&f->info,*glyph,xs,ys,x0,y0,x1,y1);
    memset(m,0,sizeof(*m));
    m->width=(float)(*x1-*x0); m->height=(float)(*y1-*y0);
    m->hbx=(float)*x0; m->hby=(float)-*y0; m->hadv=adv*xs;
    return 1;
}
/* sceFontGetRenderCharGlyphMetrics(font, code, &metrics): the cache-miss path of the game's glyph
 * loader (0xf1abb3). Tested: 0 continues to the render, anything else (the game names
 * 0x80460042 explicitly) abandons the character with no cache entry. Fields the game keeps:
 * width, height, bearingX (+8), bearingY (+0xc), advance (+0x10), in pixels at the render scale,
 * bearingY pointing up. A code the face lacks reports NO_SUPPORT_GLYPH instead of a box. */
static ABI int32_t font_render_char_metrics(void *h,uint32_t code,GlyphMetrics *out) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!out) return FONT_ERR_PARAM;
    int a,b,c,d,g;
    return glyph_metrics(f,code,out,&a,&b,&c,&d,&g) ? FONT_OK : FONT_ERR_NO_GLYPH;
}
/* sceFontRenderCharGlyphImage(font, code, surf, x, y, &metrics, &result) (0xf1ad19; return value
 * and result are ignored, only the pixels matter). Unlike ...ImageHorizontal, this entry adds the
 * horizontal baseline offset to y (shadPS4 does the same), and the game compensates by passing
 * y = cellY + bearingY - layout.baselineOffset + 2, x = cellX - bearingX + 2. So the baseline
 * lands at y + ascent and the glyph box starts at (cellX + 2, cellY + 2). The 8-bit coverage is
 * stored, not blended, into the surface clipped to its scissor. */
static ABI int32_t font_render_glyph(void *h,uint32_t code,Surface *surf,float x,float y,GlyphMetrics *metrics,RenderOutput *result) {
    Font *f=font_of(h);
    if (!f) return FONT_ERR_HANDLE;
    if (!surf || !result) return FONT_ERR_PARAM;
    GlyphMetrics local; if (!metrics) metrics=&local;
    int x0,y0,x1,y1,glyph;
    if (!glyph_metrics(f,code,metrics,&x0,&y0,&x1,&y1,&glyph)) return FONT_ERR_NO_GLYPH;
    int asc,desc,gap; stbtt_GetFontVMetrics(&f->info,&asc,&desc,&gap);
    y+=asc*scale_for(f->ry);
    int gw=x1-x0, gh=y1-y0, bpp=surf->pixelSizeByte;
    int dest_x=(int)floorf(x)+x0, dest_y=(int)floorf(y)+y0;
    int cx0=(int)surf->sc_x0, cy0=(int)surf->sc_y0, cx1=(int)surf->sc_x1, cy1=(int)surf->sc_y1;
    if (cx1>surf->width) cx1=surf->width;
    if (cy1>surf->height) cy1=surf->height;
    int ux=dest_x, uy=dest_y, uw=0, uh=0;
    if (gw>0 && gh>0 && bpp>0 && surf->buffer && cx1>cx0 && cy1>cy0) {
        uint8_t *bitmap=malloc((size_t)gw*gh);
        if (!bitmap) return FONT_ERR_ALLOC;
        stbtt_MakeGlyphBitmap(&f->info,bitmap,gw,gh,gw,scale_for(f->rx),scale_for(f->ry),glyph);
        int r0=dest_y>cy0?dest_y:cy0, r1=dest_y+gh<cy1?dest_y+gh:cy1;
        int c0=dest_x>cx0?dest_x:cx0, c1=dest_x+gw<cx1?dest_x+gw:cx1;
        ux=c0; uy=r0; uw=c1>c0?c1-c0:0; uh=r1>r0?r1-r0:0;
        for (int row=r0;row<r1;++row) {
            uint8_t *dst=(uint8_t *)surf->buffer+(size_t)row*(size_t)surf->widthByte;
            const uint8_t *src=bitmap+(size_t)(row-dest_y)*gw;
            for (int col=c0;col<c1;++col) {
                uint8_t *p=dst+(size_t)col*bpp;
                for (int k=0;k<bpp;++k) p[k]=src[col-dest_x];
            }
        }
        free(bitmap);
    }
    memset(result,0,sizeof(*result));
    result->surface.widthByte=(uint32_t)surf->widthByte; result->surface.pixelSizeByte=(uint8_t)bpp;
    result->update.x=ux>0?(uint32_t)ux:0; result->update.y=uy>0?(uint32_t)uy:0; result->update.w=(uint32_t)uw; result->update.h=(uint32_t)uh;
    result->surface.address=(uint8_t *)surf->buffer+(size_t)result->update.y*(size_t)surf->widthByte+(size_t)result->update.x*(size_t)(bpp>0?bpp:0);
    float left=x+metrics->hbx, top=y+metrics->hby, right=left+metrics->width, bottom=top-metrics->height;
    int li=(int)floorf(left), ti=(int)ceilf(top), ri=(int)ceilf(right), bi=(int)floorf(bottom);
    int ai=(int)ceilf(x+metrics->hadv);
    result->image.bearingX=(float)li-x; result->image.bearingY=(float)ti-y; result->image.advance=(float)ai-x;
    result->image.stride=(float)(ri>ai?ri:ai)-x;
    result->image.width=ri>li?(uint32_t)(ri-li):0; result->image.height=ti>bi?(uint32_t)(ti-bi):0;
    return FONT_OK;
}

static const RuntimeExport exports[]={
    {"sceFontMemoryInit",font_memory_init}, {"sceFontMemoryTerm",font_memory_term},
    {"sceFontSelectLibraryFt",font_select_library_ft}, {"sceFontSelectRendererFt",font_select_renderer_ft},
    {"sceFontCreateLibraryWithEdition",font_create_library}, {"sceFontDestroyLibrary",font_destroy_library},
    {"sceFontSupportSystemFonts",font_support_system}, {"sceFontSupportExternalFonts",font_support_external},
    {"sceFontCreateRendererWithEdition",font_create_renderer}, {"sceFontDestroyRenderer",font_destroy_renderer},
    {"sceFontRenderSurfaceInit",font_surface_init}, {"sceFontOpenFontSet",font_open_set},
    {"sceFontCloseFont",font_close}, {"sceFontBindRenderer",font_bind_renderer},
    {"sceFontUnbindRenderer",font_unbind_renderer}, {"sceFontSetScalePixel",font_set_scale},
    {"sceFontSetupRenderScalePixel",font_setup_render_scale}, {"sceFontSetEffectWeight",font_set_effect_weight},
    {"sceFontSetEffectSlant",font_set_effect_slant}, {"sceFontGetHorizontalLayout",font_horizontal_layout},
    {"sceFontGetRenderCharGlyphMetrics",font_render_char_metrics}, {"sceFontRenderCharGlyphImage",font_render_glyph},
};
uintptr_t runtime_font_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
