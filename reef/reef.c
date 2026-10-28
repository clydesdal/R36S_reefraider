/* Reef Raider: an underwater side-scrolling shooter for the R36S (ArkOS, SDL2) and the laptop.
 * Everything is drawn through SDL_Renderer textures, so on the R36S the Mali G31 does the work (GLES2):
 * pixel-art sprites and the scenery are baked into textures at startup, text comes from the clock's SDF font,
 * glows and sparks use additive blending, the night stage lights the sea through a render target.
 * Device: d-pad/left stick move, A or B fire, X sonar, Y music, Start pause, Select+Start quit.
 * Laptop (built with -DHOST): arrows/WASD move, Z/Space fire, X sonar, M music, Enter pause, Esc quit.
 * Test hooks (env): REEF_AUTO=1 autopilot + invincible, REEF_STAGE=n, REEF_BOSS=1 boss now,
 * REEF_SHOT=out%d.bmp REEF_SHOT_AT=2,5,9 (seconds) screenshots then quit. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_font.h"

#define W 640
#define H 480
#define SURF 64                 /* mean water surface */
#define FLOOR 436               /* top of the sand */
#define STEP (1.f / 60)
#define PI 3.14159265f

/* joystick button ids for "GO-Super Gamepad" (from es_input.cfg) */
enum { B_B, B_A, B_X, B_Y, B_L1, B_R1, B_L2, B_R2, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_SELECT, B_START, NBTN };

static SDL_Renderer *R;
static float ox, oy;            /* screen shake offset, applied to the world but not the HUD */
static float gt;                /* game time in seconds */
static float scroll;            /* how far the sea floor has moved, in pixels */
static const float SCROLL_V = 42;

static uint32_t rng = 0x9e3779b9;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static float frand(float a, float b) { return a + (b - a) * (rnd() & 0xffffff) / 16777216.f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ======================================================================== textures */

struct spr { SDL_Texture *t; int w, h; };

static struct spr mk_tex(uint32_t *px, int w, int h, int smooth)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, smooth ? "1" : "0");         /* read when the texture is created */
	struct spr s = {SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h), w, h};
	SDL_UpdateTexture(s.t, NULL, px, w * 4);
	SDL_SetTextureBlendMode(s.t, SDL_BLENDMODE_BLEND);
	free(px);
	return s;
}

static uint32_t palc(char c)
{
	switch (c) {
	case 'k': return 0xff101826;        /* outline */
	case 'y': return 0xffb06a00;        /* sub */
	case 'Y': return 0xffffc81e;
	case 'h': return 0xfffff3a0;
	case 'b': return 0xff2a4a6a;        /* porthole */
	case 'g': return 0xff8ee6ff;
	case 'G': return 0xffffffff;
	case 'm': return 0xff7a7a88;        /* metal */
	case 'M': return 0xffc8c8d6;
	case 'o': return 0xffc84a10;        /* fish */
	case 'O': return 0xffff8a24;
	case 'w': return 0xffffffff;
	case 'e': return 0xff000000;
	case 'r': return 0xff8a1a1a;        /* crab, heart */
	case 'R': return 0xffe8402a;
	case 'P': return 0xd0f07ab8;        /* jelly, see-through */
	case 'p': return 0x90ffb0e0;
	case 'q': return 0xb0d060b0;
	case 'u': return 0xff9a7a2a;        /* puffer */
	case 'U': return 0xffe8c450;
	case 'z': return 0xfffff2c8;
	case 's': return 0xff5a6070;        /* gull */
	case 'S': return 0xffaab4c4;
	case 'B': return 0xff2a5ad0;        /* fish stripe */
	case 'T': return 0xff6a8a3a;        /* turtle */
	case 't': return 0xff3a5a22;
	case 'n': return 0xff9ac07a;
	case 'N': return 0xff5a8a4a;
	default: return 0;
	}
}

static const uint32_t *pal_ovr;           /* while set: replacement colours for 'O', 'o', 'B' (fish variants) */

static struct spr mk_sprite(const char *const *rows, int n)
{
	int w = 0;
	for (int y = 0; y < n; y++)
		if ((int)strlen(rows[y]) > w) w = (int)strlen(rows[y]);
	uint32_t *px = calloc((size_t)w * n, 4);
	for (int y = 0; y < n; y++)
		for (int x = 0; rows[y][x]; x++) {
			const char *k = pal_ovr ? strchr("OoB", rows[y][x]) : NULL;
			px[y * w + x] = k && rows[y][x] ? pal_ovr[k - "OoB"] : palc(rows[y][x]);
		}
	return mk_tex(px, w, n, 0);
}
#define SPRITE(a) mk_sprite(a, (int)(sizeof(a) / sizeof(a[0])))

static const char *const art_sub[] = {
	"...........kk..........",
	"..........kMMk.........",
	"..........kmMk.........",
	".......kkkkmmkkkkk.....",
	".....kkyYYYYYYYYYYkk...",
	"...kkyYYhhhhhhhhhYYYk..",
	"..kyYYYYbbbYYYbbbYYYYk.",
	"kkyYYYYbgGbYYbgGbYYYYYk",
	"kmyYYYYbggbYYbggbYYYYYk",
	"kkyYYYYYbbbYYYbbbYYYYk.",
	"..kyyYYYYYYYYYYYYYYyk..",
	"...kkyyyyyyyyyyyyyykk..",
	".....kkkkkkkkkkkkkk....",
};
static const char *const art_fish[2][8] = {{
	"....kkkkkk.....",
	"..kkOOOOOOkk..k",
	".kOwkOBOOOOOkkO",
	"kOOekOBOOOOOOOk",
	"kROOOOBOOOOOOOk",
	".koOOOBOOOOokkO",
	"..kkooooooookk.k",
	"....kkkkkk.....",
}, {
	"....kkkkkk.....",
	"..kkOOOOOOkk...",
	".kOwkOBOOOOOk.k",
	"kOOekOBOOOOOOkO",
	"kROOOOBOOOOOOkO",
	".koOOOBOOOOok.k",
	"..kkoooooookk..",
	"....kkkkkk.....",
}};
static const char *const art_puff[] = {
	".....kkkk.....",
	"...kkUUUUkk...",
	"..kUUUuUUUUk..",
	".kUwwUUUUuUUk.",
	".kUekUUuUUUUkk",
	"kUUUUUUUUuUUkU",
	"kRRUUUUUUUUUUk",
	"kUUzzzzzUUUUkU",
	".kzzzzzzzzUUkk",
	".kzzzzzzzzzUk.",
	"..kzzzzzzzzk..",
	"...kkzzzzkk...",
	".....kkkk.....",
};
static const char *const art_jelly[] = {
	"...kkkkkk...",
	"..kPPPPPPk..",
	".kPpppppPPk.",
	"kPpwpppppPPk",
	"kPppppppppPk",
	"kPPPPPPPPPPk",
	".kqkqkqkqkk.",
	".q.q.q.q.q..",
	"..q.q.q.q.q.",
	".q.q.q.q.q..",
	"..q.q.q.q.q.",
	".q...q...q..",
	"..q...q...q.",
};
static const char *const art_crab[2][12] = {{
	".kk.............kk.",
	"kRRk...........kRRk",
	"kRkRk.........kRkRk",
	".kRRk..k...k..kRRk.",
	"..kRk.kwk.kwk.kRk..",
	"...kRkkekkkekkRk...",
	"...kRRRRRRRRRRRk...",
	"..kRRRRRRRRRRRRRk..",
	"..krRRRRRRRRRRRrk..",
	"...krrrrrrrrrrrk...",
	"..k.k.k.....k.k.k..",
	".k.k.k.......k.k.k.",
}, {
	".kk.............kk.",
	"kRRk...........kRRk",
	"kRRRk.........kRRRk",
	".kRRk..k...k..kRRk.",
	"..kRk.kwk.kwk.kRk..",
	"...kRkkekkkekkRk...",
	"...kRRRRRRRRRRRk...",
	"..kRRRRRRRRRRRRRk..",
	"..krRRRRRRRRRRRrk..",
	"...krrrrrrrrrrrk...",
	"...k.k.k...k.k.k...",
	"..k.k.k.....k.k.k..",
}};
static const char *const art_gull[2][9] = {{
	"......kk.....kk.....",
	".....kSSk...kSSk....",
	"......kSSk.kSSk.....",
	"..kk...kSSkSSk......",
	".kwwkkkwwwwwwwkk....",
	"kOkewwwwwwwwwwwwkkk.",
	".kkkwwwwwwwwwwSSSssk",
	"....kkwwwwwwwkkkkk..",
	"......kk..kk........",
}, {
	"....................",
	"....................",
	"..kk................",
	".kwwkkkkkkkkkkkk....",
	"kOkewwwwwwwwwwwwkkk.",
	".kkkwwwwSSSSwwSSSssk",
	"....kkwkSSSSkwkkkk..",
	".......kSSk.kSSk....",
	"......kSSk...kSSk...",
}};
static const char *const art_turtle[2][11] = {{
	".........kkkkkkkk.........",
	".......kkTtTTtTTtkk.......",
	"......kTTtTTTtTTTtTk......",
	"..kk.kTtTTTtTTTTtTTTk.....",
	".knnkkTTTtTTTTtTTTtTTk....",
	"knenNkttttttttttttttttkkk.",
	"knnnNkNNNNNNNNNNNNNNNkNNk.",
	".kkNkknnnkkkkkkknnnkkk.kk.",
	"....knnnnk.....knnnnk.....",
	"...knnnnk.......knnnk.....",
	"...kkkkk.........kkk......",
}, {
	".........kkkkkkkk.........",
	".......kkTtTTtTTtkk.......",
	"......kTTtTTTtTTTtTk......",
	"..kk.kTtTTTtTTTTtTTTk.....",
	".knnkkTTTtTTTTtTTTtTTk....",
	"knenNkttttttttttttttttkkk.",
	"knnnNkNNNNNNNNNNNNNNNkNNk.",
	".kkNkknnnkkkkkkknnnkkk.kk.",
	".....knnnk......knnnk.....",
	"......knnnk......knnk.....",
	".......kkkk.......kk......",
}};
static const char *const art_cuda[] = {
	"................................kk.",
	"..kkkkkkkkkkkkkkkkkkkkkkkkkk...kSk.",
	".kSSSSMSSSSSSSSSSSSSSSSSSSSSkkkSSk.",
	"kwkkeMSSsSSsSSsSSsSSsSSsSSSSSSSSSSk",
	"kwwwwMMMMMMMMMMMMMMMMMMMMMMMMMkkSSk",
	".kkwkwwwwwwwwwwwwwwwwwwwwwwwwk..kSk",
	"...kkkkkkkkkkkkkkkkkkkkkkkkkk....kk",
};
static const char *const art_heart[] = {
	".kk...kk.",
	"kRRk.kRRk",
	"kRwRkRRRk",
	"kRRRRRRRk",
	".kRRRRRk.",
	"..kRRRk..",
	"...kRk...",
	"....k....",
};

static struct spr t_turtle[2], t_cuda, t_px;
static SDL_Texture *t_banner;      /* the studio banner, drawn once into a texture so it can swing */
static struct spr t_sub, t_fish[4][2], t_puff, t_jelly, t_crab[2], t_gull[2], t_heart;
static struct spr t_soft, t_disc, t_ring, t_beam, t_grad, t_far, t_mid, t_sand;
static SDL_Texture *t_dark;     /* night: darkness render target, NULL when not supported */

/* straight-alpha "over" into a pixel buffer */
static void blend_px(uint32_t *d, uint32_t rgb, float a)
{
	if (a <= 0) return;
	if (a > 1) a = 1;
	float da = (*d >> 24) / 255.f, oa = a + da * (1 - a);
	float c[3];
	for (int i = 0; i < 3; i++) {
		float s = (rgb >> (16 - 8 * i)) & 255, dc = (*d >> (16 - 8 * i)) & 255;
		c[i] = (s * a + dc * da * (1 - a)) / oa;
	}
	*d = (uint32_t)(oa * 255 + .5f) << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
}

/* anti-aliased filled circle, wrapping horizontally (for seamless scrolling layers) */
static void buf_disc(uint32_t *b, int bw, int bh, float cx, float cy, float r, uint32_t rgb, float a)
{
	for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) {
		if (y < 0 || y >= bh) continue;
		for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
			float d = r - sqrtf((x + .5f - cx) * (x + .5f - cx) + (y + .5f - cy) * (y + .5f - cy));
			if (d > -1) blend_px(&b[y * bw + ((x % bw) + bw) % bw], rgb, a * clampf(d + .5f, 0, 1));
		}
	}
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
	uint32_t o = 0;
	for (int i = 0; i < 3; i++) {
		int sa = (a >> (8 * i)) & 255, sb = (b >> (8 * i)) & 255;
		o |= (uint32_t)(sa + (sb - sa) * t + .5f) << (8 * i);
	}
	return o;
}

static void mk_shapes(void)
{
	enum { S = 64 };
	uint32_t *soft = malloc(S * S * 4), *disc = malloc(S * S * 4), *ring = malloc(S * S * 4);
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			float d = sqrtf((x + .5f - S / 2.f) * (x + .5f - S / 2.f) + (y + .5f - S / 2.f) * (y + .5f - S / 2.f)) / (S / 2.f);
			float s = clampf(1 - d, 0, 1);
			soft[y * S + x] = (uint32_t)(s * s * 255) << 24 | 0xffffff;
			disc[y * S + x] = (uint32_t)(clampf((1 - d) * S / 2 + .5f, 0, 1) * 255) << 24 | 0xffffff;
			float rr = clampf(1.6f - fabsf(d - .86f) * S / 2, 0, 1);
			ring[y * S + x] = (uint32_t)(rr * 255) << 24 | 0xffffff;
		}
	uint32_t *one = malloc(4);
	*one = 0xffffffff;
	t_px = mk_tex(one, 1, 1, 0);
	t_soft = mk_tex(soft, S, S, 1);
	t_disc = mk_tex(disc, S, S, 1);
	t_ring = mk_tex(ring, S, S, 1);

	enum { BW = 32, BH = 256 };                                                     /* a light shaft */
	uint32_t *beam = malloc(BW * BH * 4);
	for (int y = 0; y < BH; y++)
		for (int x = 0; x < BW; x++) {
			float u = (x + .5f) / BW * 2 - 1, v = (float)y / BH;
			float a = expf(-u * u * 4) * (1 - v) * (1 - v) * clampf(v * 12, 0, 1);
			beam[y * BW + x] = (uint32_t)(a * 255) << 24 | 0xffffff;
		}
	t_beam = mk_tex(beam, BW, BH, 1);
}

/* the scenery layers: far reef silhouette, mid coral garden, sand. They tile horizontally. */
static void mk_layers(void)
{
	enum { FW = 512, FH = 150 };
	uint32_t *far = calloc(FW * FH, 4);
	for (int x = 0; x < FW; x++) {
		float u = x * 2 * PI / FW;
		float h = 70 + 22 * sinf(2 * u + 1) + 14 * sinf(5 * u + 2) + 6 * sinf(13 * u);
		for (int y = FH - (int)h; y < FH; y++) far[y * FW + x] = 0xff000000 | lerp_rgb(0x15707e, 0x0e4a62, (float)y / FH);
	}
	for (int i = 0; i < 26; i++)
		buf_disc(far, FW, FH, frand(0, FW), FH - frand(40, 95), frand(10, 26), 0x13606e, 1);
	t_far = mk_tex(far, FW, FH, 1);

	enum { MW = 1280, MH = 180 };
	uint32_t *mid = calloc(MW * MH, 4);
	for (int x = 0; x < MW; x++) {
		float u = x * 2 * PI / MW;
		float h = 34 + 12 * sinf(6 * u) + 6 * sinf(14 * u + 1);
		for (int y = MH - (int)h; y < MH; y++) mid[y * MW + x] = 0xff000000 | lerp_rgb(0x3a6a6a, 0x24484e, (float)(y - (MH - h)) / h);
	}
	{                                                                          /* a wrecked ship, listing to one side */
		float wx = 820, wb = MH - 26;
		for (int x = 0; x < 300; x++) {
			float u = x / 300.f, top = wb - 70 + x * .13f, bot = wb - 6 - 26 * sinf(u * PI) * .5f;
			if (x < 40) top += (40 - x) * .9f;                                   /* the bow rises */
			for (int y = (int)top; y < (int)bot; y++) mid[y * MW + (int)wx + x] = 0xff000000 | lerp_rgb(0x4a4a50, 0x2e3036, (y - top) / (bot - top));
		}
		for (int y = (int)(wb - 112); y < (int)(wb - 58); y++)                    /* cabin */
			for (int x = 150; x < 220; x++) mid[y * MW + (int)wx + x] = 0xff3e4046;
		for (float t = 0; t < 120; t += 1.5f) buf_disc(mid, MW, MH, wx + 110 + t * .35f, wb - 60 - t, 3, 0x3a3c42, 1);   /* broken mast */
		for (float t = 0; t < 50; t += 1.5f) buf_disc(mid, MW, MH, wx + 135 + t * .9f, wb - 155 + t * .5f, 2, 0x3a3c42, 1);
		for (int k = 0; k < 6; k++) buf_disc(mid, MW, MH, wx + 60 + k * 30, wb - 42 + k * 4, 5, 0x15181e, 1);              /* portholes */
		for (int k = 0; k < 3; k++) buf_disc(mid, MW, MH, wx + 165 + k * 20, wb - 92, 5, 0x15181e, 1);
	}
	static const uint32_t coral[] = {0xd8705a, 0xc05aa8, 0xe8a040, 0x8a6ad8, 0xe86a8a, 0x4ab0a0};
	for (int i = 0; i < 64; i++) {
		float cx = frand(0, MW), base = MH - 30 - frand(0, 14);
		uint32_t c = coral[rnd() % 6];
		switch (rnd() % 4) {
		case 0:                                                                  /* brain coral */
			{
				float r = frand(12, 22);
				buf_disc(mid, MW, MH, cx, base - r * .5f, r, lerp_rgb(c, 0, .35f), 1);
				buf_disc(mid, MW, MH, cx - 2, base - r * .5f - 2, r - 3, c, 1);
				for (int k = 0; k < 5; k++)
					buf_disc(mid, MW, MH, cx + frand(-r, r) * .6f, base - r * .5f + frand(-r, r) * .5f, 2, lerp_rgb(c, 0xffffff, .3f), 1);
			}
			break;
		case 1:                                                                  /* branching coral */
			for (int br = 0; br < 5; br++) {
				float x = cx, y = base, a = -PI / 2 + frand(-.7f, .7f), len = frand(40, 80);
				for (float s = 0; s < len; s += 2) {
					a += frand(-.08f, .08f);
					x += cosf(a) * 2;
					y += sinf(a) * 2;
					buf_disc(mid, MW, MH, x, y, 3.2f - s / len * 1.6f, c, 1);
				}
				buf_disc(mid, MW, MH, x, y, 2.5f, lerp_rgb(c, 0xffffff, .4f), 1);
			}
			break;
		case 2:                                                                  /* sea fan */
			for (int k = 0; k < 22; k++) {
				float a = -PI / 2 + (k - 10.5f) * .06f, len = frand(30, 55);
				for (float s = 0; s < len; s += 1.5f) buf_disc(mid, MW, MH, cx + cosf(a) * s, base + sinf(a) * s, .9f, c, .8f);
			}
			break;
		default:                                                                 /* tube sponges */
			for (int k = 0; k < 3; k++) {
				float x = cx + k * 9, top = base - frand(25, 60);
				for (float y = base; y > top; y -= 2) buf_disc(mid, MW, MH, x, y, 4.5f, lerp_rgb(c, 0, .15f), 1);
				buf_disc(mid, MW, MH, x, top, 3.5f, lerp_rgb(c, 0, .6f), 1);
			}
			break;
		}
	}
	for (int i = 0; i < MW * MH; i++)                                           /* blue haze: it is far away */
		if (mid[i] >> 24) mid[i] = (mid[i] & 0xff000000) | lerp_rgb(mid[i] & 0xffffff, 0x1a6a88, .38f);
	t_mid = mk_tex(mid, MW, MH, 1);

	enum { SW = 640, SH = 52 };
	uint32_t *sand = calloc(SW * SH, 4);
	for (int x = 0; x < SW; x++) {
		float u = x * 2 * PI / SW;
		int top = (int)(7 + 3 * sinf(4 * u) + 2 * sinf(11 * u + 1));
		for (int y = top; y < SH; y++) {
			uint32_t c = lerp_rgb(0xf0dca4, 0xb89c68, (float)(y - top) / (SH - top));
			if (y < top + 2) c = 0xfff0c0;
			for (int r = 0; r < 3; r++)                                        /* ripples */
				if (abs(y - (top + 12 + r * 11 + (int)(2.5f * sinf(x * .06f + r * 2) + 1.5f * sinf(x * .17f)))) < 1) c = lerp_rgb(c, 0x806040, .25f);
			int n = rnd() % 100;
			if (n < 6) c = lerp_rgb(c, 0, .15f);
			else if (n < 9) c = lerp_rgb(c, 0xffffff, .3f);
			sand[y * SW + x] = 0xff000000 | c;
		}
	}
	for (int i = 0; i < 30; i++) {                                              /* pebbles and shells */
		float x = frand(0, SW), y = frand(20, SH - 6);
		uint32_t c = (rnd() & 1) ? 0xe8b0a0 : 0x9a8a70;
		buf_disc(sand, SW, SH, x, y, frand(1.5f, 3.5f), c, 1);
	}
	t_sand = mk_tex(sand, SW, SH, 0);
}

/* ======================================================================== drawing helpers */

static void tint(SDL_Texture *t, uint32_t rgb, int a, SDL_BlendMode bm)
{
	SDL_SetTextureColorMod(t, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
	SDL_SetTextureAlphaMod(t, (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a));
	SDL_SetTextureBlendMode(t, bm);
}

static void quad(SDL_Texture *t, float cx, float cy, float w, float h, float ang)
{
	SDL_Rect d = {(int)lroundf(cx + ox - w / 2), (int)lroundf(cy + oy - h / 2), (int)lroundf(w), (int)lroundf(h)};
	if (ang == 0) SDL_RenderCopy(R, t, NULL, &d);
	else SDL_RenderCopyEx(R, t, NULL, &d, ang, NULL, SDL_FLIP_NONE);
}

static void blob(SDL_Texture *t, float cx, float cy, float w, float h, uint32_t rgb, int a, SDL_BlendMode bm)
{
	tint(t, rgb, a, bm);
	quad(t, cx, cy, w, h, 0);
}

/* a sprite at 2x, centred, with an optional white hit flash on top */
static void spr(const struct spr *s, float cx, float cy, float sx, float sy, float ang, int flip, float flash)
{
	SDL_Rect d = {(int)lroundf(cx + ox - s->w * sx), (int)lroundf(cy + oy - s->h * sy), (int)lroundf(s->w * sx * 2), (int)lroundf(s->h * sy * 2)};
	SDL_RenderCopyEx(R, s->t, NULL, &d, ang, NULL, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
	if (flash > 0) {
		SDL_SetTextureBlendMode(s->t, SDL_BLENDMODE_ADD);
		SDL_SetTextureAlphaMod(s->t, (Uint8)clampf(flash * 255, 0, 255));
		SDL_RenderCopyEx(R, s->t, NULL, &d, ang, NULL, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
		SDL_SetTextureBlendMode(s->t, SDL_BLENDMODE_BLEND);
		SDL_SetTextureAlphaMod(s->t, 255);
	}
}

static void fill(int x, int y, int w, int h, uint32_t rgb, int a)
{
	SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(R, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a));
	SDL_Rect r = {x, y, w, h};
	SDL_RenderFillRect(R, &r);
}

/* ======================================================================== text (baked from the clock's SDF font) */

struct bfont { SDL_Texture *t; SDL_Rect r[95]; short xo[95], yo[95], adv[95]; int px; };
static struct bfont f_small, f_big, f_huge;

static float sdf_at(const struct sdf_font *f, const struct sdf_glyph *g, float x, float y)
{
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0, v[4];
	for (int i = 0; i < 4; i++) {
		int xx = x0 + (i & 1), yy = y0 + (i >> 1);
		v[i] = (xx < 0 || yy < 0 || xx >= g->w || yy >= g->h) ? 0 : f->data[g->off + yy * g->w + xx];
	}
	return (v[0] * (1 - fx) + v[1] * fx) * (1 - fy) + (v[2] * (1 - fx) + v[3] * fx) * fy;
}

/* white glyphs with a black outline: tinting the texture colours the fill and leaves the outline black */
static void bake_font(struct bfont *bf, const struct sdf_font *f, int px, float outline)
{
	float s = (float)px / f->em;
	int m = (int)ceilf(outline) + 1, aw = 1024, x = 0, y = 0, rowh = 0;
	bf->px = px;
	for (int c = 0; c < 95; c++) {
		const struct sdf_glyph *g = &f->glyph[c];
		int gw = (int)ceilf(g->w * s) + 2 * m, gh = (int)ceilf(g->h * s) + 2 * m;
		if (x + gw > aw) { x = 0; y += rowh; rowh = 0; }
		bf->r[c] = (SDL_Rect){x, y, gw, gh};
		x += gw;
		if (gh > rowh) rowh = gh;
		bf->xo[c] = (short)(lroundf(g->bx * s) - m);
		bf->yo[c] = (short)(-lroundf(g->by * s) - m);
		bf->adv[c] = (short)lroundf(g->adv16 / 16.f * s);
	}
	int ah = y + rowh;
	uint32_t *pix = calloc((size_t)aw * ah, 4);
	for (int c = 0; c < 95; c++) {
		const struct sdf_glyph *g = &f->glyph[c];
		SDL_Rect r = bf->r[c];
		for (int j = 0; j < r.h; j++)
			for (int i = 0; i < r.w; i++) {
				float v = sdf_at(f, g, (i - m + .5f) / s - .5f, (j - m + .5f) / s - .5f);
				float d = (v - 128) / 127 * f->spread * s;
				float af = clampf(d + .5f, 0, 1), aa = clampf(d + outline + .5f, 0, 1);
				if (aa > 0) pix[(r.y + j) * aw + r.x + i] = (uint32_t)(aa * 255 + .5f) << 24 | (uint32_t)(255 * af / aa) * 0x010101;
			}
	}
	bf->t = mk_tex(pix, aw, ah, 1).t;
}

static int text_w(const struct bfont *bf, const char *s)
{
	int w = 0;
	for (; *s; s++)
		if (*s >= 32 && *s < 127) w += bf->adv[*s - 32];
	return w;
}

/* y = top of the capitals; align 0 left, 1 centre, 2 right; wave > 0 bobs the letters */
static void text(const struct bfont *bf, float x, float y, const char *s, uint32_t rgb, int a, int align, float wave)
{
	tint(bf->t, rgb, a, SDL_BLENDMODE_BLEND);
	float pen = x - (align ? text_w(bf, s) * (align == 1 ? .5f : 1.f) : 0), base = y + bf->px * .72f;
	for (int i = 0; s[i]; i++) {
		int c = s[i] - 32;
		if (c < 0 || c >= 95) continue;
		float wy = wave ? sinf(gt * 3 + i * .55f) * wave : 0;
		SDL_Rect d = {(int)lroundf(pen + bf->xo[c]), (int)lroundf(base + bf->yo[c] + wy), bf->r[c].w, bf->r[c].h};
		SDL_RenderCopy(R, bf->t, &bf->r[c], &d);
		pen += bf->adv[c];
	}
}

/* ======================================================================== sound: a tiny synth + steel-pan tune */

#define SR 44100
enum { W_OFF, W_SQR, W_NOISE, W_SINE, W_TRI, W_PAN };
struct voice { int wave, delay, len, pos; float f, fmul, vol, vmul, ph, hold; };
static struct voice sfx[14], mus[8];
static SDL_AudioDeviceID adev;
static int music_on = 1, music_boss, music_hold;
static unsigned mus_count, mus_step;

static float mtof(int n) { return 440.f * powf(2.f, (n - 69) / 12.f); }

static void voice_set(struct voice *v, int wave, float f0, float f1, float vol, float dur, float end, float delay)
{
	int n = (int)(dur * SR);
	if (n < 1) n = 1;
	*v = (struct voice){wave, (int)(delay * SR), n, 0, f0, powf(f1 / f0, 1.f / n), vol, powf(end, 1.f / n), 0, 0};
}

static struct voice *voice_free(struct voice *pool, int n)
{
	struct voice *best = &pool[0];
	for (int i = 0; i < n; i++) {
		if (pool[i].wave == W_OFF) return &pool[i];
		if (pool[i].pos > best->pos) best = &pool[i];
	}
	return best;
}

static void snd(int wave, float f0, float f1, float vol, float dur, float end, float delay)
{
	if (!adev) return;
	SDL_LockAudioDevice(adev);
	voice_set(voice_free(sfx, 14), wave, f0, f1, vol, dur, end, delay);
	SDL_UnlockAudioDevice(adev);
}

static void music_tick(void)
{
	static const int calm[4][4] = {{60, 64, 67, 72}, {57, 60, 64, 69}, {65, 69, 72, 77}, {62, 67, 71, 74}};
	static const int boss[4][4] = {{57, 60, 64, 69}, {53, 57, 60, 65}, {56, 59, 64, 68}, {57, 60, 64, 69}};
	static const int calm_bass[4] = {48, 45, 41, 43}, boss_bass[4] = {45, 41, 40, 45};
	static const signed char mel_a[16] = {0, -1, -1, 1, -1, -1, 2, -1, 3, -1, 2, 1, -1, -1, 2, -1};
	static const signed char mel_b[16] = {3, -1, 2, -1, 1, -1, 2, -1, 0, -1, -1, 1, 2, -1, 3, -1};
	int bar = (mus_step / 16) % 4, st = mus_step % 16, odd = (mus_step / 16) & 1;
	const int *ch = music_boss ? boss[bar] : calm[bar];
	int root = music_boss ? boss_bass[bar] : calm_bass[bar];
	int m = (odd ? mel_b : mel_a)[st];
	if (m >= 0) voice_set(voice_free(mus, 8), W_PAN, mtof(ch[m]), mtof(ch[m]), .15f, .45f, .02f, 0);
	if (st == 0 || st == 6 || st == 8 || st == 12) {
		int n = st == 8 ? root + 7 : st == 12 ? root + 12 : root;
		voice_set(voice_free(mus, 8), W_TRI, mtof(n), mtof(n), .28f, .22f, .05f, 0);
	}
	if (st % 4 == 2) voice_set(voice_free(mus, 8), W_NOISE, 9000, 9000, .035f, .05f, .1f, 0);
	if (music_boss && st % 4 == 0) voice_set(voice_free(mus, 8), W_NOISE, 1200, 300, .08f, .08f, .1f, 0);
}

static float voice_out(struct voice *v)
{
	if (v->wave == W_OFF) return 0;
	if (v->delay > 0) { v->delay--; return 0; }
	float p = v->ph, o;
	switch (v->wave) {
	case W_SQR: o = p < .5f ? .6f : -.6f; break;
	case W_NOISE: o = v->hold; break;
	case W_SINE: o = sinf(2 * PI * p); break;
	case W_TRI: o = 1 - 4 * fabsf(p - .5f); break;
	default: o = .65f * (sinf(2 * PI * p) + .45f * sinf(4 * PI * p) + .2f * sinf(6 * PI * p)); break;
	}
	o *= v->vol;
	v->ph += v->f / SR;
	if (v->ph >= 1) {
		v->ph -= floorf(v->ph);
		if (v->wave == W_NOISE) v->hold = (rnd() & 0xffff) / 32768.f - 1;
	}
	v->f *= v->fmul;
	v->vol *= v->vmul;
	if (++v->pos >= v->len) v->wave = W_OFF;
	return o;
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	Sint16 *out = (Sint16 *)stream;
	unsigned step_len = (unsigned)(SR * 60.f / (music_boss ? 128 : 112) / 4);
	for (int i = 0; i < len / 2; i++) {
		float s = 0;
		if (music_on && !music_hold) {
			if (++mus_count >= step_len) { mus_count = 0; mus_step++; music_tick(); }
			for (int k = 0; k < 8; k++) s += voice_out(&mus[k]);
		}
		for (int k = 0; k < 14; k++) s += voice_out(&sfx[k]);
		s = s / (1 + fabsf(s) * .6f);                                           /* soft clip */
		out[i] = (Sint16)(s * 30000);
	}
}

static void sfx_shot(void) { snd(W_SINE, 620, 1500, .14f, .06f, .3f, 0); }
static void sfx_hit(void) { snd(W_NOISE, 6000, 3000, .12f, .04f, .2f, 0); }
static void sfx_boom(float big)
{
	snd(W_NOISE, 3000, 400, .3f * big, .35f * big, .02f, 0);
	snd(W_SINE, 180, 40, .4f * big, .3f * big, .05f, 0);
}
static void sfx_pickup(void)
{
	for (int i = 0; i < 3; i++) snd(W_TRI, mtof(72 + i * 4), mtof(72 + i * 4), .25f, .1f, .3f, i * .07f);
}
static void sfx_hurt(void) { snd(W_SQR, 420, 70, .25f, .6f, .1f, 0); }
static void sfx_sonar(void)
{
	snd(W_SINE, 1600, 400, .35f, .9f, .05f, 0);
	snd(W_SINE, 1600, 400, .15f, .9f, .05f, .25f);
}
static void sfx_warn(void)
{
	for (int i = 0; i < 6; i++) snd(W_SQR, i & 1 ? 330 : 440, i & 1 ? 330 : 440, .12f, .22f, .5f, i * .25f);
}

/* ======================================================================== game state */

struct theme { const char *name; uint32_t sky0, sky1, wat0, wat1, tint, sun; float rays; int night; };
static const struct theme themes[3] = {
	{"MORNING REEF", 0x4aa8ec, 0xc8eeff, 0x30c4d0, 0x05284e, 0xffffff, 0xfff4c0, 1.f, 0},
	{"SUNSET DROP-OFF", 0x5a3a8a, 0xffa860, 0x2a8aa8, 0x081634, 0xffd2c0, 0xffb060, .6f, 0},
	{"NIGHT DIVE", 0x040818, 0x18224a, 0x0f4466, 0x020a18, 0x7a92c8, 0xe8f0ff, .15f, 1},
};
static const struct theme *th = &themes[0];

enum { P_BUBBLE, P_SPARK, P_FLASH, P_INK, P_DROP, P_RING };
struct part { int on, kind; float x, y, vx, vy, life, max, size, grow; uint32_t col; };
#define NPART 700
static struct part parts[NPART];

enum { E_FISH, E_PUFF, E_JELLY, E_CRAB, E_GULL, E_CUDA };
static const struct { int hp, score; float r, ry; } etab[] = {            /* r = half width, ry = half height of the hitbox */
	[E_FISH] = {1, 100, 11, 9}, [E_PUFF] = {6, 300, 14, 13}, [E_JELLY] = {2, 200, 13, 13}, [E_CRAB] = {4, 250, 15, 11},
	[E_GULL] = {2, 400, 13, 9}, [E_CUDA] = {7, 600, 32, 8},
};
struct enemy { int on, type, hp, phase, var, sonar; float x, y, vx, vy, t, base, flash, cool; };
#define NEN 48
static struct enemy en[NEN];

struct shot { int on, kind; float x, y, vx, vy; };       /* kind (enemy): 0 orb, 1 spine, 2 ink */
#define NPS 64
#define NEB 160
static struct shot ps[NPS], eb[NEB];

enum { K_POWER, K_HEART, K_SONAR };
struct pickup { int on, kind; float x, y, t; };
static struct pickup pk[8];

struct popup { int on; float x, y, t; char s[16]; };
static struct popup pop[16];

struct weed { float wx; int segs; float ph; uint32_t col; };
static struct weed weeds[14];
struct mote { float x, y, z, ph; };
static struct mote motes[70];
static SDL_Point stars[60];

static struct {
	float x, y, vx, vy, inv, cool, respawn, sonar_r;
	int lives, lvl, sonar, alive, sonar_id;
} pl;

#define NTENT 8
#define NSEG 12
static struct { int on, hp, maxhp, atk, dying, sonar; float x, y, t, atk_t, flash, charge, die_t, boom_t; } boss;

enum { ST_SPLASH, ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER };
static int state = ST_TITLE, stage_n, stage_mode, score, hiscore, autoplay;
static float splash_t, title_t, stage_t, spawn_t, banner_t, fade, fade_dir, over_t, shake, stage_len = 70;
static char banner[32], banner2[32];
static const char *gpu_name = "?";

static void save_hiscore(void)
{
	if (autoplay) return;                                                       /* test runs do not count */
	FILE *f = fopen("hiscore.txt", "w");
	if (f) { fprintf(f, "%d\n", hiscore); fclose(f); }
}

/* ======================================================================== particles */

static struct part *part_new(int kind, float x, float y, float vx, float vy, float life, float size, uint32_t col)
{
	static int next;
	struct part *p = &parts[next];
	for (int i = 0; i < NPART; i++) {
		struct part *q = &parts[(next + i) % NPART];
		if (!q->on) { p = q; next = (next + i + 1) % NPART; break; }
	}
	*p = (struct part){1, kind, x, y, vx, vy, life, life, size, 0, col};
	return p;
}

static void bubbles(float x, float y, int n, float spread)
{
	for (int i = 0; i < n; i++)
		part_new(P_BUBBLE, x + frand(-spread, spread), y + frand(-spread, spread), frand(-20, 20), frand(-60, -10), frand(.8f, 2), frand(2, 6), 0xffffff);
}

static void explode(float x, float y, uint32_t col, float big)
{
	part_new(P_FLASH, x, y, 0, 0, .25f, 50 * big, col)->grow = 200 * big;
	for (int i = 0; i < (int)(16 * big); i++) {
		float a = frand(0, 2 * PI), v = frand(60, 240) * big;
		part_new(P_SPARK, x, y, cosf(a) * v, sinf(a) * v, frand(.3f, .7f), frand(6, 14), i & 1 ? col : 0xffffff);
	}
	bubbles(x, y, (int)(8 * big), 10 * big);
	sfx_boom(big > 1.5f ? 1.5f : big);
	if (shake < 3 * big) shake = 3 * big;
}

static void splash(float x)
{
	for (int i = 0; i < 12; i++) part_new(P_DROP, x + frand(-8, 8), SURF, frand(-70, 70), frand(-260, -120), 1, frand(2, 4), 0xffffff);
	snd(W_NOISE, 2500, 800, .12f, .2f, .05f, 0);
}

static float wave_y(float x) { return SURF + 2.5f * sinf(x * .045f + gt * 2.1f) + 1.8f * sinf(x * .11f - gt * 2.9f); }

static void parts_update(float dt)
{
	for (int i = 0; i < NPART; i++) {
		struct part *p = &parts[i];
		if (!p->on) continue;
		if ((p->life -= dt) <= 0) { p->on = 0; continue; }
		switch (p->kind) {
		case P_BUBBLE:
			p->vy = fmaxf(p->vy - 40 * dt, -100);
			p->x += (p->vx + sinf(gt * 6 + p->size * 7) * 14 - SCROLL_V * .4f) * dt;
			if (p->y < wave_y(p->x) + 3) p->on = 0;
			break;
		case P_SPARK:
		case P_INK:
			p->vx *= 1 - 3 * dt;
			p->vy *= 1 - 3 * dt;
			break;
		case P_DROP:
			p->vy += 600 * dt;
			if (p->vy > 0 && p->y > SURF) p->on = 0;
			break;
		}
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->size += p->grow * dt;
	}
}

static void parts_draw(int glow_pass)
{
	for (int i = 0; i < NPART; i++) {
		struct part *p = &parts[i];
		if (!p->on) continue;
		float f = p->life / p->max;
		int glow = p->kind == P_SPARK || p->kind == P_FLASH || p->kind == P_RING;
		if (glow != glow_pass) continue;
		switch (p->kind) {
		case P_BUBBLE: blob(t_ring.t, p->x, p->y, p->size * 2, p->size * 2, 0xe8fbff, (int)(220 * fminf(1, f * 3)), SDL_BLENDMODE_BLEND); break;
		case P_SPARK: blob(t_soft.t, p->x, p->y, p->size * (.4f + f), p->size * (.4f + f), p->col, (int)(255 * f), SDL_BLENDMODE_ADD); break;
		case P_FLASH: blob(t_soft.t, p->x, p->y, p->size, p->size, p->col, (int)(255 * f), SDL_BLENDMODE_ADD); break;
		case P_RING: blob(t_ring.t, p->x, p->y, p->size, p->size, p->col, (int)(255 * f), SDL_BLENDMODE_ADD); break;
		case P_INK: blob(t_soft.t, p->x, p->y, p->size, p->size, p->col, (int)(230 * f), SDL_BLENDMODE_BLEND); break;
		case P_DROP: blob(t_disc.t, p->x, p->y, p->size, p->size, 0xffffff, 230, SDL_BLENDMODE_BLEND); break;
		}
	}
}

static void popup(float x, float y, const char *s)
{
	for (int i = 0; i < 16; i++)
		if (!pop[i].on) {
			pop[i] = (struct popup){1, x, y, 0, ""};
			snprintf(pop[i].s, sizeof(pop[i].s), "%s", s);
			return;
		}
}

/* ======================================================================== spawning */

static struct enemy *enemy_new(int type, float x, float y)
{
	for (int i = 0; i < NEN; i++)
		if (!en[i].on) {
			en[i] = (struct enemy){1, type, etab[type].hp + (type == E_PUFF ? stage_n : 0), 0, 0, -1, x, y, 0, 0, 0, y, 0, frand(1, 2.5f)};
			return &en[i];
		}
	return NULL;
}

static float diff(void) { return 1 + .14f * stage_n; }

static void spawn_wave(void)
{
	int r = rnd() % 15;
	struct enemy *e;
	if (r < 3) {                                                                /* a school in a line */
		float y = frand(SURF + 50, FLOOR - 60);
		int var = rnd() % 3;
		for (int i = 0; i < 5; i++)
			if ((e = enemy_new(E_FISH, W + 20 + i * 30, y))) { e->var = var; e->vx = -140 * diff(); e->t = i * .4f; }
	} else if (r < 5) {                                                         /* a vee */
		float y = frand(SURF + 80, FLOOR - 90);
		for (int i = 0; i < 7; i++)
			if ((e = enemy_new(E_FISH, W + 20 + abs(i - 3) * 26, y + (i - 3) * 24))) { e->var = 1; e->vx = -170 * diff(); e->phase = 1; }
	} else if (r < 7) {
		if ((e = enemy_new(E_PUFF, W + 30, frand(SURF + 60, FLOOR - 60)))) e->vx = -60;
	} else if (r < 9) {
		for (int i = 0; i < 3; i++)
			if ((e = enemy_new(E_JELLY, W + 20 + i * 50, frand(SURF + 50, FLOOR - 80)))) e->t = frand(0, 2);
	} else if (r < 11) {
		for (int i = 0; i < 1 + (stage_n > 0); i++)
			if ((e = enemy_new(E_CRAB, W + 20 + i * 70, FLOOR - 4))) e->vx = (rnd() & 1) ? -20 : 15;
	} else if (r < 13) {
		if ((e = enemy_new(E_GULL, W + 30, SURF - 30))) e->vx = -170;
	} else {                                                                    /* barracuda: stops, lines up, lunges */
		if ((e = enemy_new(E_CUDA, W + 40, frand(SURF + 60, FLOOR - 50)))) e->vx = -160;
	}
}

static void eshot(int kind, float x, float y, float ang, float v)
{
	for (int i = 0; i < NEB; i++)
		if (!eb[i].on) { eb[i] = (struct shot){1, kind, x, y, cosf(ang) * v, sinf(ang) * v}; return; }
}

static float aim(float x, float y) { return atan2f(pl.y - y, pl.x - x); }

static void drop(float x, float y, int force)
{
	int k = force >= 0 ? force : (rnd() % 10 < 6 ? K_POWER : rnd() & 1 ? K_SONAR : K_HEART);
	for (int i = 0; i < 8; i++)
		if (!pk[i].on) { pk[i] = (struct pickup){1, k, x, y, 0}; return; }
}

static void stage_start(int n)
{
	stage_n = n;
	th = &themes[n % 3];
	stage_t = 0;
	spawn_t = 2;
	stage_mode = 0;
	snprintf(banner, sizeof(banner), "STAGE %d", n + 1);
	snprintf(banner2, sizeof(banner2), "%s", th->name);
	banner_t = 3;
	music_boss = 0;
	/* the night stage gets a fresh seafloor too */
	for (int i = 0; i < 14; i++) weeds[i].col = th->night ? 0x2a8a5a : 0x3aa860 + (rnd() % 3) * 0x101000;
}

static void game_start(void)
{
	memset(en, 0, sizeof(en));
	memset(ps, 0, sizeof(ps));
	memset(eb, 0, sizeof(eb));
	memset(pk, 0, sizeof(pk));
	memset(&boss, 0, sizeof(boss));
	pl = (typeof(pl)){100, 240, 0, 0, 2, 0, 0, 0, 3, 1, 2, 1, 0};
	score = 0;
	state = ST_PLAY;
	const char *s = getenv("REEF_STAGE");
	stage_start(s ? atoi(s) : 0);
	if (getenv("REEF_BOSS")) stage_t = stage_len;
}

static void boss_start(void)
{
	memset(&boss, 0, sizeof(boss));
	boss.on = 1;
	boss.sonar = pl.sonar_id;
	boss.maxhp = boss.hp = 90 + 50 * stage_n;
	boss.x = W + 120;
	boss.y = 230;
	boss.atk_t = 3;
	snprintf(banner, sizeof(banner), "WARNING");
	snprintf(banner2, sizeof(banner2), "THE KRAKEN RISES");
	banner_t = 3;
	music_boss = 1;
	sfx_warn();
}

/* positions and radii of one tentacle's segments */
static void tentacle(int k, float pts[NSEG][3])
{
	float x = boss.x - 40 + k * 11.5f, y = boss.y + 40;
	float a = 1.9f + (k - 3.5f) * .24f - boss.charge * .9f;
	for (int i = 0; i < NSEG; i++) {
		a += .14f * sinf(gt * 2.4f + k * 1.3f - i * .5f);
		float len = 15 - i * .55f;
		x += cosf(a) * len;
		y += sinf(a) * len;
		pts[i][0] = x;
		pts[i][1] = y;
		pts[i][2] = 12 - i * .72f;
	}
}

/* ======================================================================== player */

static void hurt(void)
{
	if (!pl.alive || pl.inv > 0 || autoplay) return;
	explode(pl.x, pl.y, 0xffc030, 2);
	sfx_hurt();
	shake = 10;
	pl.alive = 0;
	pl.respawn = 1.6f;
	pl.lvl = pl.lvl > 1 ? pl.lvl - 1 : 1;
	if (--pl.lives < 0) {
		state = ST_OVER;
		over_t = 0;
		if (score > hiscore) { hiscore = score; save_hiscore(); }
	}
}

static void fire(void)
{
	float nx = pl.x + 24, ny = pl.y + 3;
	static const float ang[3][3] = {{0}, {0, 0}, {0, -.14f, .14f}};
	static const float dy[3][3] = {{0}, {-6, 6}, {0, -4, 4}};
	for (int i = 0; i < pl.lvl; i++)
		for (int j = 0; j < NPS; j++)
			if (!ps[j].on) {
				ps[j] = (struct shot){1, 0, nx, ny + dy[pl.lvl - 1][i], 560 * cosf(ang[pl.lvl - 1][i]), 560 * sinf(ang[pl.lvl - 1][i])};
				break;
			}
	sfx_shot();
}

/* ======================================================================== input */

struct input { float mx, my; int fire, sonar, start, music, quit; };
static SDL_Joystick *js;

static struct input read_input(void)
{
	struct input in = {0};
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	in.mx = (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) - (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]);
	in.my = (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) - (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]);
	in.fire = k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_SPACE];
	in.sonar = k[SDL_SCANCODE_X];
	in.start = k[SDL_SCANCODE_RETURN];
	in.music = k[SDL_SCANCODE_M];
	in.quit = k[SDL_SCANCODE_ESCAPE];
	if (js) {
		int b[NBTN];
		for (int i = 0; i < NBTN; i++) b[i] = SDL_JoystickGetButton(js, i);
		float ax = SDL_JoystickGetAxis(js, 0) / 32767.f, ay = SDL_JoystickGetAxis(js, 1) / 32767.f;
		if (fabsf(ax) > .25f) in.mx = ax;
		if (fabsf(ay) > .25f) in.my = ay;
		if (b[B_RIGHT] || b[B_LEFT]) in.mx = b[B_RIGHT] - b[B_LEFT];
		if (b[B_DOWN] || b[B_UP]) in.my = b[B_DOWN] - b[B_UP];
		in.fire |= b[B_A] || b[B_B] || b[B_R1];
		in.sonar |= b[B_X] || b[B_L1];
		in.music |= b[B_Y];
		if (b[B_SELECT] && b[B_START]) in.quit = 1;
		else in.start |= b[B_START];
	}
	return in;
}

/* ======================================================================== update */

static void update_play(float dt, const struct input *in, const struct input *pressed)
{
	stage_t += dt;
	scroll += SCROLL_V * dt;

	/* ---- player */
	if (pl.alive) {
		float tx = in->mx * 210, ty = in->my * 190;
		if (autoplay) {                                                         /* chase the nearest enemy's depth */
			float best = 1e9f, gy = 240;
			for (int i = 0; i < NEN; i++)
				if (en[i].on && en[i].x > pl.x && en[i].x - pl.x < best && en[i].y > SURF) { best = en[i].x - pl.x; gy = en[i].y; }
			if (boss.on) gy = boss.y - 10;
			tx = (110 - pl.x) * 3;
			ty = clampf((gy - pl.y) * 4, -190, 190);
		}
		pl.vx += (tx - pl.vx) * clampf(dt * 10, 0, 1);
		pl.vy += (ty - pl.vy) * clampf(dt * 10, 0, 1);
		pl.x = clampf(pl.x + pl.vx * dt, 24, W - 30);
		pl.y = clampf(pl.y + pl.vy * dt, SURF + 22, FLOOR - 10);
		if (pl.inv > 0) pl.inv -= dt;
		pl.cool -= dt;
		if ((in->fire || autoplay) && pl.cool <= 0) { fire(); pl.cool = .13f; }
		if (pressed->sonar && pl.sonar > 0 && pl.sonar_r <= 0) {
			pl.sonar--;
			pl.sonar_r = 1;
			pl.sonar_id++;
			sfx_sonar();
			shake = 6;
		}
		if (rnd() % 6 == 0) bubbles(pl.x - 26, pl.y + 4, 1, 2);
	} else if ((pl.respawn -= dt) <= 0 && state == ST_PLAY) {
		pl.alive = 1;
		pl.inv = 2.5f;
		pl.x = 90;
		pl.y = 240;
		pl.vx = pl.vy = 0;
	}

	/* ---- sonar ring: wipes enemy shots and hits everything once */
	if (pl.sonar_r > 0) {
		pl.sonar_r += 650 * dt;
		for (int i = 0; i < NEB; i++)
			if (eb[i].on && hypotf(eb[i].x - pl.x, eb[i].y - pl.y) < pl.sonar_r) {
				eb[i].on = 0;
				part_new(P_SPARK, eb[i].x, eb[i].y, 0, 0, .3f, 10, 0x80f0ff);
			}
		for (int i = 0; i < NEN; i++)
			if (en[i].on && en[i].sonar != pl.sonar_id && hypotf(en[i].x - pl.x, en[i].y - pl.y) < pl.sonar_r) {
				en[i].sonar = pl.sonar_id;
				en[i].hp -= 6;
				en[i].flash = 1;
			}
		if (boss.on && !boss.dying && boss.sonar != pl.sonar_id && hypotf(boss.x - pl.x, boss.y - pl.y) < pl.sonar_r) {
			boss.sonar = pl.sonar_id;
			boss.hp -= 15;
			boss.flash = 1;
		}
		if (pl.sonar_r > 800) pl.sonar_r = 0;
	}

	/* ---- director */
	if (stage_mode == 0) {
		if ((spawn_t -= dt) <= 0) {
			spawn_wave();
			spawn_t = fmaxf(.8f, 2.1f - .25f * stage_n) * frand(.7f, 1.3f);
		}
		if (stage_t >= stage_len) stage_mode = 1;
	} else if (stage_mode == 1) {
		int any = 0;
		for (int i = 0; i < NEN; i++) any |= en[i].on;
		if (!any) { stage_mode = 2; boss_start(); }
	}
	if (banner_t > 0) banner_t -= dt;

	/* ---- player shots */
	for (int i = 0; i < NPS; i++) {
		struct shot *s = &ps[i];
		if (!s->on) continue;
		s->x += s->vx * dt;
		s->y += s->vy * dt;
		if (s->x > W + 20 || s->y < SURF || s->y > FLOOR + 10) { s->on = 0; continue; }
		for (int j = 0; j < NEN && s->on; j++) {
			struct enemy *e = &en[j];
			if (!e->on || e->y < SURF + 4) continue;
			float grow = e->type == E_PUFF && e->hp <= 3 ? 6 : 0;
			if (fabsf(s->x - e->x) < etab[e->type].r + grow + 6 && fabsf(s->y - e->y) < etab[e->type].ry + grow + 3) {
				s->on = 0;
				e->hp--;
				e->flash = 1;
				sfx_hit();
				part_new(P_SPARK, s->x, s->y, -60, 0, .2f, 14, 0x9ff0ff);
			}
		}
		if (s->on && boss.on && !boss.dying && hypotf(s->x - boss.x, s->y - (boss.y - 14)) < 56) {
			s->on = 0;
			boss.hp--;
			boss.flash = 1;
			sfx_hit();
			part_new(P_SPARK, s->x, s->y, -60, 0, .2f, 16, 0xff9ff0);
		}
	}

	/* ---- enemies */
	for (int i = 0; i < NEN; i++) {
		struct enemy *e = &en[i];
		if (!e->on) continue;
		e->t += dt;
		if (e->flash > 0) e->flash -= dt * 6;
		switch (e->type) {
		case E_FISH:
			e->x += e->vx * dt;
			e->y = e->base + (e->phase ? 6 : 26) * sinf(e->t * 3);
			if (stage_n >= 1 && e->var == 2 && (e->cool -= dt) <= 0 && e->x < W - 40) {
				eshot(0, e->x, e->y, aim(e->x, e->y), 120 * diff());
				e->cool = 9;
			}
			break;
		case E_PUFF:
			e->x += (e->hp <= 3 ? -35 : e->vx) * dt;
			e->y = e->base + 14 * sinf(e->t * 1.7f);
			break;
		case E_JELLY:
			if (fmodf(e->t, 1.7f) < dt) e->vy = -95;
			e->vy = fminf(e->vy + 70 * dt, 28);
			e->x -= 32 * dt;
			e->y += e->vy * dt;
			if (e->y < SURF + 30) e->vy = fabsf(e->vy);
			break;
		case E_CRAB:
			e->x += (e->vx - SCROLL_V) * dt;
			if ((e->cool -= dt) <= 0 && e->x < W - 20) {
				for (int k = -1; k <= 1; k++) eshot(0, e->x, e->y - 14, -PI / 2 + k * .35f + (pl.x < e->x ? -.25f : .25f), 110 * diff());
				e->cool = 2.6f;
			}
			break;
		case E_CUDA:
			if (e->phase == 0) {
				e->x += e->vx * dt;
				if (e->x < W - 80) { e->phase = 1; e->cool = 1.1f; }
			} else if (e->phase == 1) {
				e->y += clampf((pl.y - e->y) * 3, -120, 120) * dt;
				if ((e->cool -= dt) <= 0) {
					e->phase = 2;
					e->vx = -480 * diff();
					snd(W_NOISE, 1500, 5000, .18f, .3f, .05f, 0);
				}
			} else {
				e->x += e->vx * dt;
				if (rnd() % 3 == 0) bubbles(e->x + 30, e->y, 1, 3);
			}
			break;
		case E_GULL:
			if (e->phase == 0) {                                                 /* cruise, then dive at the sub */
				e->x += e->vx * dt;
				e->y = SURF - 30 + 3 * sinf(e->t * 4);
				if (e->x < pl.x + 150 && e->x > pl.x) {
					float a = atan2f(pl.y - e->y, pl.x - 20 - e->x);
					e->vx = cosf(a) * 330;
					e->vy = sinf(a) * 330;
					e->phase = 1;
				}
				if (e->x < -40) e->on = 0;
			} else if (e->phase == 1) {
				e->x += e->vx * dt;
				e->y += e->vy * dt;
				if (e->y > SURF && e->base < SURF) { splash(e->x); e->vx *= .5f; e->vy *= .5f; }
				e->base = e->y;
				if (e->y > SURF) { e->cool = .7f; e->phase = 2; }
			} else if (e->phase == 2) {
				e->x += e->vx * dt;
				e->y += e->vy * dt;
				if ((e->cool -= dt) <= 0) { e->vy -= 500 * dt; e->vx -= 200 * dt; }
				if (rnd() % 4 == 0) bubbles(e->x, e->y, 1, 4);
				if (e->y < SURF) { splash(e->x); e->phase = 3; e->vy = -120; }
			} else {
				e->x += e->vx * dt;
				e->y += e->vy * dt;
				e->vy += 30 * dt;
				if (e->x < -40 || e->y < -40) e->on = 0;
			}
			break;
		}
		if (e->x < -60 || e->y > H + 40) e->on = 0;
		if (!e->on) continue;

		if (e->hp <= 0) {
			static const uint32_t col[] = {0xffa040, 0xffe060, 0xff80d0, 0xff5030, 0xffffff, 0xc0e0ff};
			explode(e->x, e->y, col[e->type], e->type == E_FISH ? .8f : 1.2f);
			if (e->type == E_PUFF)
				for (int k = 0; k < 10; k++) eshot(1, e->x, e->y, k * 2 * PI / 10, 110 * diff());
			int pts = etab[e->type].score * (1 + stage_n);
			score += pts;
			char s[16];
			snprintf(s, sizeof(s), "%d", pts);
			popup(e->x, e->y - 10, s);
			if (rnd() % 100 < (e->type == E_FISH ? 5 : 16)) drop(e->x, e->y, -1);
			e->on = 0;
			continue;
		}
		if (pl.alive && fabsf(e->x - pl.x) < etab[e->type].r + 8 && fabsf(e->y - pl.y) < etab[e->type].ry + 6) hurt();
	}

	/* ---- the boss */
	if (boss.on) {
		boss.t += dt;
		if (boss.flash > 0) boss.flash -= dt * 6;
		if (boss.dying) {
			boss.die_t += dt;
			boss.y += 20 * dt;
			if ((boss.boom_t -= dt) <= 0) {
				explode(boss.x + frand(-60, 60), boss.y + frand(-80, 60), (rnd() & 1) ? 0xff80ff : 0xffd060, 1.4f);
				boss.boom_t = .09f;
			}
			if (boss.die_t > 2.6f) {
				for (int k = 0; k < 40; k++)
					part_new(P_INK, boss.x + frand(-60, 60), boss.y + frand(-70, 70), frand(-80, 80), frand(-80, 80), frand(1.5f, 3), frand(40, 90), 0x1a0a28)->grow = 40;
				explode(boss.x, boss.y, 0xffffff, 3);
				boss.on = 0;
				int pts = 5000 * (stage_n + 1);
				score += pts;
				drop(boss.x - 40, boss.y, K_POWER);
				drop(boss.x - 10, boss.y + 30, K_HEART);
				snprintf(banner, sizeof(banner), "STAGE CLEAR");
				snprintf(banner2, sizeof(banner2), "BONUS %d", pts);
				banner_t = 4;
				stage_mode = 3;
				stage_t = 0;
				music_boss = 0;
				if (pl.sonar < 3) pl.sonar++;
			}
		} else {
			if (boss.charge > 0) {                                               /* lunge across the screen and back */
				boss.charge += dt * .55f;
				float u = boss.charge - 1;
				boss.x = 470 - 330 * sinf(PI * clampf(u, 0, 1));
				if (u >= 1) boss.charge = 0;
			} else {
				boss.x += (470 - boss.x) * clampf(dt * 1.2f, 0, 1);
			}
			boss.y = 230 + 70 * sinf(boss.t * .7f);
			if (boss.x < W - 40 && (boss.atk_t -= dt) <= 0) {
				int a = boss.atk % 4;
				float ex = boss.x - 6, ey = boss.y + 2;
				if (a == 0)
					for (int k = -2; k <= 2; k++) eshot(2, ex, ey, aim(ex, ey) + k * .22f, 150 * diff());
				else if (a == 1)
					for (int k = 0; k < 16; k++) eshot(2, ex, ey, k * 2 * PI / 16 + boss.t, 120 * diff());
				else if (a == 2)
					for (int k = 0; k < 3; k++) eshot(0, ex, ey + (k - 1) * 30, aim(ex, ey + (k - 1) * 30), 200 * diff());
				else
					boss.charge = 1;
				for (int k = 0; k < 6; k++)
					part_new(P_INK, ex, ey, frand(-60, 0), frand(-30, 30), 1, 20, 0x2a1040)->grow = 30;
				snd(W_SINE, 300, 90, .25f, .2f, .1f, 0);
				boss.atk++;
				boss.atk_t = (a == 3 ? 2.6f : 1.5f) / (1 + .1f * stage_n);
			}
			if (pl.alive) {
				if (hypotf(boss.x - pl.x, boss.y - 14 - pl.y) < 60) hurt();
				for (int k = 0; k < NTENT && pl.alive; k++) {
					float pts[NSEG][3];
					tentacle(k, pts);
					for (int s = 2; s < NSEG; s += 2)
						if (hypotf(pts[s][0] - pl.x, pts[s][1] - pl.y) < pts[s][2] + 7) { hurt(); break; }
				}
			}
			if (boss.hp <= 0) { boss.dying = 1; boss.die_t = 0; music_boss = 0; shake = 12; }
		}
	}

	/* ---- enemy shots */
	for (int i = 0; i < NEB; i++) {
		struct shot *s = &eb[i];
		if (!s->on) continue;
		s->x += s->vx * dt;
		s->y += s->vy * dt;
		if (s->x < -20 || s->x > W + 20 || s->y < SURF - 10 || s->y > H + 10) { s->on = 0; continue; }
		if (pl.alive && hypotf(s->x - pl.x, s->y - pl.y) < (s->kind == 2 ? 13 : 10)) { s->on = 0; hurt(); }
	}

	/* ---- pickups */
	for (int i = 0; i < 8; i++) {
		struct pickup *p = &pk[i];
		if (!p->on) continue;
		p->t += dt;
		p->x -= 40 * dt;
		p->y += sinf(p->t * 3) * 20 * dt;
		if (p->x < -20) { p->on = 0; continue; }
		if (pl.alive && hypotf(p->x - pl.x, p->y - pl.y) < 24) {
			static const char *nm[] = {"POWER UP", "EXTRA SUB", "SONAR +1"};
			if (p->kind == K_POWER) { if (pl.lvl < 3) pl.lvl++; else score += 1000; }
			if (p->kind == K_HEART && pl.lives < 5) pl.lives++;
			if (p->kind == K_SONAR && pl.sonar < 3) pl.sonar++;
			popup(p->x, p->y - 14, nm[p->kind]);
			part_new(P_RING, p->x, p->y, 0, 0, .4f, 20, 0xfff0a0)->grow = 160;
			sfx_pickup();
			p->on = 0;
		}
	}

	/* ---- stage clear: wait, then fade into the next stage */
	if (stage_mode == 3 && stage_t > 4.5f && fade_dir == 0 && fade == 0) fade_dir = 1;
}

/* ======================================================================== scenery life: big creatures and the sea floor */

static uint32_t water_at(float y) { return lerp_rgb(th->wat0, th->wat1, powf(clampf((y - SURF) / (H - SURF), 0, 1), .8f)); }
static uint32_t haze(uint32_t rgb, float y, float k) { return lerp_rgb(rgb, water_at(y), k); }

/* colour times the stage tint (sunset warmer, night bluer and darker) */
static uint32_t tn(uint32_t c)
{
	uint32_t o = 0;
	for (int i = 0; i < 3; i++) o |= (((c >> (8 * i)) & 255) * ((th->tint >> (8 * i)) & 255) / 255) << (8 * i);
	return o;
}

static float hash1(int i)
{
	float v = sinf(i * 12.9898f) * 43758.547f;
	return v - floorf(v);
}

/* a shape with an outline: the outline colour a little bigger underneath */
static void oq(SDL_Texture *t, float cx, float cy, float w, float h, float ang, uint32_t fillc, uint32_t line, float lw)
{
	if (lw > 0) {
		tint(t, line, 255, SDL_BLENDMODE_BLEND);
		quad(t, cx, cy, w + 2 * lw, h + 2 * lw, ang);
	}
	tint(t, fillc, 255, SDL_BLENDMODE_BLEND);
	quad(t, cx, cy, w, h, ang);
}

enum { C_TURTLE, C_MANTA, C_SHARK, C_BAIT };
struct creature { int on, kind; float x, y, vx, t; };
static struct creature cr[3];
static float cr_timer = 3;

static void creatures_update(float dt)
{
	if ((cr_timer -= dt) <= 0) {
		static int forced = -2;
		if (forced == -2) forced = getenv("REEF_CREATURE") ? atoi(getenv("REEF_CREATURE")) : -1;
		int kind = forced >= 0 ? forced : (int)(rnd() % 4), busy = 0;
		forced = -1;
		for (int i = 0; i < 3; i++) busy |= cr[i].on && cr[i].kind == kind;
		for (int i = 0; i < 3 && !busy; i++)
			if (!cr[i].on) {
				static const float vx[] = {-30, -45, -22, -55}, x0[] = {60, 120, 220, 100};
				cr[i] = (struct creature){1, kind, W + x0[kind], frand(SURF + 90, 310), vx[kind], 0};
				break;
			}
		cr_timer = frand(7, 15);
	}
	for (int i = 0; i < 3; i++)
		if (cr[i].on) {
			cr[i].t += dt;
			cr[i].x += cr[i].vx * dt;
			if (cr[i].x < -420) cr[i].on = 0;
		}
}

static void draw_shark(const struct creature *c)                              /* whale shark, far away */
{
	float L = 330, x = c->x + ox, y = c->y + 10 * sinf(c->t * .5f) + oy, sw = sinf(c->t * 1.6f), hz = .5f;
	uint32_t body = haze(0x34485e, y, hz), belly = haze(0x8a9aa8, y, hz), spot = haze(0xd8e4ee, y, hz + .1f);
	float sox = ox, soy = oy;
	ox = oy = 0;
	tint(t_disc.t, body, 255, SDL_BLENDMODE_BLEND);
	quad(t_disc.t, x + L * .08f, y - L * .12f, 26, 66, 35);                     /* dorsal fin */
	quad(t_disc.t, x + L * .5f, y - 26 + sw * 5, 26, 84, 35 + sw * 10);           /* tail */
	quad(t_disc.t, x + L * .48f, y + 18 + sw * 5, 22, 56, -40 + sw * 10);
	quad(t_disc.t, x + L * .36f, y + sw * 2, L * .32f, L * .1f, sw * 3);
	quad(t_disc.t, x, y, L * .8f, L * .24f, 0);
	quad(t_disc.t, x - L * .33f, y + 4, L * .3f, L * .19f, 0);
	blob(t_disc.t, x - L * .1f, y + L * .075f, L * .62f, L * .08f, belly, 255, SDL_BLENDMODE_BLEND);
	tint(t_disc.t, body, 255, SDL_BLENDMODE_BLEND);
	quad(t_disc.t, x - L * .16f, y + L * .12f, 22, 74, -55 + sw * 6);           /* pectoral fin */
	tint(t_disc.t, spot, 200, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < 46; i++) {
		float u = hash1(i) * 2 - 1, v = hash1(i + 50) * 2 - 1;
		if (u * u + v * v > 1) continue;
		float r = 3 + hash1(i + 9) * 3;
		quad(t_disc.t, x - L * .05f + u * L * .38f, y - 6 + v * L * .07f, r, r, 0);
	}
	blob(t_disc.t, x - L * .43f, y + 12, 34, 4, haze(0x101820, y, hz), 255, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x - L * .4f, y - 4, 6, 6, haze(0x101820, y, hz), 255, SDL_BLENDMODE_BLEND);
	ox = sox;
	oy = soy;
}

static void draw_manta(const struct creature *c)
{
	float x = c->x, y = c->y + 20 * sinf(c->t * .6f), f = sinf(c->t * 2.2f), span = 80 + 22 * f;
	uint32_t col = haze(0x1a2838, y, .4f), edge = haze(0x48607a, y, .4f);
	tint(t_px.t, col, 255, SDL_BLENDMODE_BLEND);
	quad(t_px.t, x + 76, y, 90, 3, sinf(c->t * 2) * 6);                         /* tail */
	tint(t_disc.t, col, 255, SDL_BLENDMODE_BLEND);
	for (int sg = -1; sg <= 1; sg += 2) {                                         /* wings: a broad root and a swept, pointed tip */
		quad(t_disc.t, x + 8, y + sg * span * .32f, 76, span * .7f, sg * -12.f);
		quad(t_disc.t, x + 26, y + sg * span * .72f, 22, span * .62f, sg * -38.f);
		quad(t_disc.t, x + 36, y + sg * span * .98f, 10, span * .3f, sg * -55.f);
	}
	quad(t_disc.t, x, y, 86, 50, 0);
	quad(t_disc.t, x - 44, y - 13, 24, 9, -20);
	quad(t_disc.t, x - 44, y + 13, 24, 9, 20);
	blob(t_disc.t, x + 4, y - 8, 34, 12, edge, 160, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x + 4, y + 8, 34, 12, edge, 160, SDL_BLENDMODE_BLEND);
}

static void draw_bait(const struct creature *c)                               /* a swirling ball of small fish */
{
	tint(t_disc.t, haze(0xd8ecf8, c->y, .25f), 230, SDL_BLENDMODE_BLEND);
	for (int i = 0; i < 52; i++) {
		float a = hash1(i) * 2 * PI + c->t * (1.2f + hash1(i + 99) * .5f), r = 14 + hash1(i + 7) * 46;
		float px = c->x + cosf(a) * r * 1.3f, py = c->y + sinf(c->t * .7f) * 14 + sinf(a) * r * .75f;
		quad(t_disc.t, px, py, 10, 3.5f, (a + PI / 2) * 180 / PI);
	}
}

static void draw_creatures(int far)
{
	for (int i = 0; i < 3; i++) {
		const struct creature *c = &cr[i];
		if (!c->on || (c->kind == C_SHARK) != far) continue;
		switch (c->kind) {
		case C_SHARK: draw_shark(c); break;
		case C_MANTA: draw_manta(c); break;
		case C_BAIT: draw_bait(c); break;
		case C_TURTLE:
			{
				struct spr *t = &t_turtle[(int)(c->t * 1.8f) & 1];
				uint32_t m = haze(0xffffff, c->y, .3f);
				SDL_SetTextureColorMod(t->t, m >> 16, m >> 8 & 255, m & 255);
				spr(t, c->x, c->y + 8 * sinf(c->t * .8f), 1.5f, 1.5f, 4 * sinf(c->t * .8f), 0, 0);
				SDL_SetTextureColorMod(t->t, 255, 255, 255);
			}
			break;
		}
	}
}

enum { PR_ANEMONE, PR_STAR, PR_URCHIN, PR_CLAM, PR_EELS, PR_VENT, PR_ANCHOR, PR_CHEST, PR_ROCK };
struct prop { int kind; float wx, ph, h[3]; uint32_t col; };
#define NPROP 10
static struct prop props[NPROP];
static float prop_next = 60;

static void prop_place(struct prop *p)
{
	static const uint32_t anem[] = {0xff7ab0, 0xffa050, 0xb070ff, 0x60e0c0}, star[] = {0xff7040, 0xe84060, 0xffb030, 0x9060ff};
	p->wx = prop_next;
	prop_next += frand(80, 190);
	int r = rnd() % 22;
	p->kind = r < 4 ? PR_ANEMONE : r < 7 ? PR_STAR : r < 9 ? PR_URCHIN : r < 11 ? PR_CLAM : r < 14 ? PR_EELS : r < 16 ? PR_VENT
	        : r < 17 ? PR_ANCHOR : r < 18 ? PR_CHEST : PR_ROCK;
	p->ph = frand(0, 6);
	p->col = p->kind == PR_ANEMONE ? anem[rnd() % 4] : star[rnd() % 4];
	p->h[0] = p->h[1] = p->h[2] = 0;
}

static void props_init(void)
{
	prop_next = scroll + 40;
	for (int i = 0; i < NPROP; i++) prop_place(&props[i]);
}

static void props_update(float dt)
{
	for (int i = 0; i < NPROP; i++) {
		struct prop *p = &props[i];
		float x = p->wx - scroll;
		if (x < -120) { prop_place(p); continue; }
		if (x > W + 60) continue;
		if (p->kind == PR_VENT && rnd() % 4 == 0) bubbles(x + frand(-4, 4), FLOOR - 16, 1, 1);
		if (p->kind == PR_CHEST && rnd() % 40 == 0) bubbles(x - 4, FLOOR - 20, 1, 2);
		if (p->kind == PR_EELS)                                                 /* garden eels duck when the sub comes close */
			for (int k = 0; k < 3; k++) {
				int scared = state == ST_PLAY && pl.alive && fabsf(x + (k - 1) * 18 - pl.x) < 90 && pl.y > FLOOR - 150;
				float target = scared ? 0 : 24 + 10 * sinf(gt * .6f + p->ph + k * 1.7f);
				p->h[k] += (target - p->h[k]) * clampf(dt * (target < p->h[k] ? 9 : 1.5f), 0, 1);
			}
	}
}

static void draw_props(void)
{
	float gy = FLOOR + 4 + oy;
	for (int i = 0; i < NPROP; i++) {
		const struct prop *p = &props[i];
		float x = p->wx - scroll + ox;
		if (x < -100 || x > W + 100) continue;
		float sox = ox, soy = oy;
		ox = oy = 0;
		switch (p->kind) {
		case PR_ANEMONE:
			blob(t_disc.t, x, gy - 6, 46, 18, tn(lerp_rgb(p->col, 0, .45f)), 255, SDL_BLENDMODE_BLEND);
			for (int k = 0; k < 13; k++) {
				float a = -PI / 2 + (k - 6) * .15f, tx = x + (k - 6) * 3, ty = gy - 8;
				for (int sg = 0; sg < 7; sg++) {
					a += .12f * sinf(gt * 2 + p->ph + k * .4f - sg * .5f);
					tx += cosf(a) * 5;
					ty += sinf(a) * 5;
					float r = 9 - sg * .7f;
					blob(t_disc.t, tx, ty, r, r, tn(lerp_rgb(p->col, 0xffffff, sg / 7.f * .6f)), 255, SDL_BLENDMODE_BLEND);
				}
			}
			{                                                                    /* its clownfish */
				float a = gt * 1.1f + p->ph;
				spr(&t_fish[3][(int)(gt * 8) & 1], x + cosf(a) * 28, gy - 40 + sinf(a * 2) * 8, .7f, .7f, 0, -sinf(a) > 0, 0);
			}
			break;
		case PR_STAR:
			for (int k = 0; k < 5; k++) {
				float a = p->ph + k * 2 * PI / 5, dx = cosf(a) * 9, dy = sinf(a) * 9 * .5f;
				oq(t_disc.t, x + dx, gy - 8 + dy, 20, 8, atan2f(dy, dx) * 180 / PI, tn(p->col), tn(lerp_rgb(p->col, 0, .5f)), 1.5f);
			}
			blob(t_disc.t, x, gy - 8, 12, 8, tn(p->col), 255, SDL_BLENDMODE_BLEND);
			blob(t_disc.t, x - 2, gy - 9, 3, 3, tn(0xffffff), 200, SDL_BLENDMODE_BLEND);
			break;
		case PR_URCHIN:
			tint(t_px.t, tn(0x2a1030), 255, SDL_BLENDMODE_BLEND);
			for (int k = 0; k < 18; k++) {
				float a = PI + k * PI / 17 + .04f * sinf(gt * 3 + k), len = 18 + 4 * hash1(k + i * 20);
				quad(t_px.t, x + cosf(a) * len * .5f, gy - 8 + sinf(a) * len * .5f, len, 2, a * 180 / PI);
			}
			blob(t_disc.t, x, gy - 7, 24, 18, tn(0x4a1a5a), 255, SDL_BLENDMODE_BLEND);
			blob(t_disc.t, x - 4, gy - 11, 8, 5, tn(0x9a6aaa), 255, SDL_BLENDMODE_BLEND);
			break;
		case PR_CLAM:
			{
				float open = clampf(sinf(gt * .8f + p->ph) * 1.6f, 0, 1), th_ = -open * 32 * PI / 180, hx = x + 18, hy = gy - 8;
				oq(t_disc.t, x, gy - 6, 40, 14, 0, tn(0xb0a0c0), tn(0x5a4a6a), 1.5f);
				blob(t_disc.t, x - 4, gy - 10, 9, 9, tn(0xfff8f0), 255, SDL_BLENDMODE_BLEND);
				if (open > .5f) blob(t_soft.t, x - 4, gy - 10, 26, 26, 0xfff0e0, (int)(200 * (open - .5f) * (.6f + .4f * sinf(gt * 9))), SDL_BLENDMODE_ADD);
				float dx = -20, dy = -2;
				oq(t_disc.t, hx + dx * cosf(th_) - dy * sinf(th_), hy + dx * sinf(th_) + dy * cosf(th_), 40, 13, th_ * 180 / PI, tn(0xc8b8d8), tn(0x5a4a6a), 1.5f);
			}
			break;
		case PR_EELS:
			for (int k = 0; k < 3; k++) {
				float ex = x + (k - 1) * 18, hgt = p->h[k];
				blob(t_disc.t, ex, gy - 3, 12, 5, tn(0x403020), 255, SDL_BLENDMODE_BLEND);
				if (hgt < 2) continue;
				float hx = ex, hy = gy - 3;
				for (float sg = 0; sg < hgt; sg += 2.5f) {
					hx = ex + sinf(sg * .1f + gt * 2 + k) * 4 * sg / 30;
					hy = gy - 3 - sg;
					blob(t_disc.t, hx, hy, 7, 7, tn(((int)(sg / 5) & 1) ? 0xf0e8d0 : 0xd8d0b0), 255, SDL_BLENDMODE_BLEND);
				}
				blob(t_disc.t, hx - 2, hy - 2, 10, 8, tn(0xf0e8d0), 255, SDL_BLENDMODE_BLEND);
				blob(t_disc.t, hx - 4, hy - 3, 2.5f, 2.5f, 0x101010, 255, SDL_BLENDMODE_BLEND);
			}
			break;
		case PR_VENT:
			oq(t_disc.t, x - 10, gy - 10, 46, 30, 0, tn(0x5a5a62), tn(0x2a2a30), 1.5f);
			oq(t_disc.t, x + 12, gy - 6, 34, 22, 0, tn(0x4a4a52), tn(0x2a2a30), 1.5f);
			blob(t_disc.t, x, gy - 22, 10, 5, tn(0x1a1a20), 255, SDL_BLENDMODE_BLEND);
			break;
		case PR_ANCHOR:
			{
				float a = 18 * PI / 180, tx = x + sinf(a) * 36, ty = gy - 34 - cosf(a) * 36;
				oq(t_px.t, x + sinf(a) * 4, gy - 30, 7, 74, 18, tn(0x7a4a32), tn(0x3a2018), 1.5f);
				oq(t_px.t, tx - sinf(a) * 10, ty + cosf(a) * 10, 40, 6, 18, tn(0x7a4a32), tn(0x3a2018), 1.5f);
				blob(t_ring.t, tx, ty - 6, 16, 16, tn(0x7a4a32), 255, SDL_BLENDMODE_BLEND);
				for (int k = -4; k <= 4; k++) {
					float b = k * .22f;
					blob(t_disc.t, x - 2 + sinf(b) * 24, gy - 4 - cosf(b) * 10 + 12, 8, 8, tn(0x7a4a32), 255, SDL_BLENDMODE_BLEND);
				}
				blob(t_disc.t, x + 3, gy - 30, 5, 5, tn(0xb06a3a), 255, SDL_BLENDMODE_BLEND);
			}
			break;
		case PR_CHEST:
			blob(t_soft.t, x, gy - 22, 70, 40, 0xffd060, (int)(110 + 60 * sinf(gt * 3)), SDL_BLENDMODE_ADD);
			oq(t_px.t, x, gy - 12, 40, 20, 0, tn(0x7a4a22), tn(0x2a1808), 2);
			oq(t_px.t, x - 3, gy - 26, 42, 9, -14, tn(0x8a5a2a), tn(0x2a1808), 2);
			tint(t_px.t, tn(0xe8b030), 255, SDL_BLENDMODE_BLEND);
			quad(t_px.t, x - 12, gy - 12, 4, 20, 0);
			quad(t_px.t, x + 12, gy - 12, 4, 20, 0);
			quad(t_px.t, x, gy - 17, 6, 6, 0);
			for (int k = 0; k < 4; k++) blob(t_disc.t, x - 30 + k * 6 + (k > 1) * 46, gy - 1, 7, 4, tn(0xffd040), 255, SDL_BLENDMODE_BLEND);
			break;
		case PR_ROCK:
			oq(t_disc.t, x - 14, gy - 16, 58, 40, 0, tn(0x5e6266), tn(0x2a2e32), 2);
			oq(t_disc.t, x + 16, gy - 10, 46, 28, 0, tn(0x4e5256), tn(0x2a2e32), 2);
			blob(t_disc.t, x - 20, gy - 26, 22, 10, tn(0x7a7e82), 255, SDL_BLENDMODE_BLEND);
			for (int k = 0; k < 5; k++)
				blob(t_disc.t, x - 30 + hash1(k + i * 7) * 56, gy - 30 + hash1(k + 3) * 14, 8, 8, tn(k & 1 ? 0xff7ab0 : 0xffa040), 255, SDL_BLENDMODE_BLEND);
			break;
		}
		ox = sox;
		oy = soy;
	}
}

static void draw_caustics(void)
{
	if (th->rays < .3f) return;
	for (int i = 0; i < 10; i++) {
		float cx = fmodf(i * 83 - scroll + 30 * sinf(gt * .6f + i) + 1000.f * W, W + 120) - 60, cy = FLOOR + 6 + (i % 3) * 12 + 3 * sinf(gt * 1.3f + i * 2);
		blob(t_soft.t, cx, cy, 80, 14, th->sun, (int)(80 * th->rays * (.5f + .5f * sinf(gt * 1.7f + i * 1.3f))), SDL_BLENDMODE_ADD);
	}
}

/* ======================================================================== studio splash: the moose and the gull */

#define STUDIO "MOOSE WORKS"
struct mpose { float x, legph, lookx, looky, blink, bob; int arms, mouth, brows; };

static void mk_banner(void)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	t_banner = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 300, 80);
	if (!t_banner) return;
	SDL_SetTextureBlendMode(t_banner, SDL_BLENDMODE_BLEND);
	SDL_SetRenderTarget(R, t_banner);
	SDL_SetRenderDrawColor(R, 0, 0, 0, 0);
	SDL_RenderClear(R);
	fill(0, 24, 54, 44, 0x5e130e, 255);                                         /* ribbon tails with notches */
	fill(246, 24, 54, 44, 0x5e130e, 255);
	fill(3, 27, 50, 38, 0x9a2a22, 255);
	fill(247, 27, 50, 38, 0x9a2a22, 255);
	tint(t_px.t, 0, 0, SDL_BLENDMODE_NONE);
	SDL_Rect n1 = {-16, 30, 32, 32}, n2 = {284, 30, 32, 32};
	SDL_RenderCopyEx(R, t_px.t, NULL, &n1, 45, NULL, SDL_FLIP_NONE);
	SDL_RenderCopyEx(R, t_px.t, NULL, &n2, 45, NULL, SDL_FLIP_NONE);
	fill(24, 8, 252, 56, 0x5e130e, 255);
	fill(28, 12, 244, 48, 0xd33a2c, 255);
	fill(28, 12, 244, 6, 0xe8604a, 255);
	text(&f_big, 150, 25, STUDIO, 0xfff6e0, 255, 1, 0);
	SDL_SetRenderTarget(R, NULL);
}

static void draw_moose(const struct mpose *p, float gy, float s)
{
	const uint32_t fur = 0x6b4226, dark = 0x3d2314, hoof = 0x2b1a10, leg = 0x5a3520, ant = 0xd9b88a, antd = 0x8a6a3a;
#define MX(u) (p->x + (u) * s)
#define MY(v) (gy + (v) * s)
	float b = p->bob, hb = b * 1.3f;
	blob(t_soft.t, MX(0), MY(0), 230 * s, 28 * s, 0x000000, 70, SDL_BLENDMODE_BLEND);
	for (int k = 0; k < 2; k++) {
		float lift = fmaxf(0, sinf(p->legph + k * PI)) * 16, u = k ? 28 : -28;
		oq(t_px.t, MX(u), MY(-44 - lift), 24 * s, 80 * s, 0, leg, dark, 2 * s);
		oq(t_disc.t, MX(u), MY(-6 - lift), 34 * s, 16 * s, 0, hoof, 0, 0);
	}
	oq(t_disc.t, MX(0), MY(-135 + b), 172 * s, 150 * s, 0, fur, dark, 3 * s);
	blob(t_disc.t, MX(0), MY(-128 + b), 118 * s, 98 * s, 0x7a4c2c, 255, SDL_BLENDMODE_BLEND);
	oq(t_px.t, MX(0), MY(-195 + b), 62 * s, 50 * s, 0, fur, 0, 0);
	blob(t_disc.t, MX(0), MY(-176 + b), 16 * s, 42 * s, dark, 255, SDL_BLENDMODE_BLEND);
	for (int sg = -1; sg <= 1; sg += 2) {                                         /* antlers */
		float ang = -sg * 25, ar = ang * PI / 180, cx = MX(sg * 80), cy = MY(-318 + hb);
		oq(t_disc.t, MX(sg * 44), MY(-292 + hb), 50 * s, 16 * s, -sg * 30, ant, antd, 2.5f * s);
		for (int k = 0; k < 4; k++) {
			float pp = -.42f + k * .28f;
			float tx = cx + cosf(ar) * pp * 100 * s + sinf(ar) * 30 * s, ty = cy + sinf(ar) * pp * 100 * s - cosf(ar) * 30 * s;
			oq(t_disc.t, tx, ty, 14 * s, 40 * s, ang + pp * 50, ant, antd, 2.5f * s);
		}
		oq(t_disc.t, cx, cy, 112 * s, 46 * s, ang, ant, antd, 3 * s);
		oq(t_disc.t, MX(sg * 58), MY(-262 + hb), 54 * s, 22 * s, -sg * 25, fur, dark, 3 * s);   /* ears */
		blob(t_disc.t, MX(sg * 60), MY(-262 + hb), 32 * s, 10 * s, 0xc58f6b, 255, SDL_BLENDMODE_BLEND);
	}
	oq(t_disc.t, MX(0), MY(-232 + hb), 98 * s, 122 * s, 0, fur, dark, 3 * s);
	oq(t_disc.t, MX(0), MY(-193 + hb), 84 * s, 60 * s, 0, 0xb07a4a, dark, 3 * s);
	for (int sg = -1; sg <= 1; sg += 2) {
		blob(t_disc.t, MX(sg * 13), MY(-200 + hb), 10 * s, 13 * s, hoof, 255, SDL_BLENDMODE_BLEND);
		float ex = MX(sg * 22), ey = MY(-248 + hb), eh = 24 * p->blink;
		oq(t_disc.t, ex, ey, 22 * s, fmaxf(eh, 3) * s, 0, 0xffffff, dark, 2 * s);
		if (p->blink > .3f) {
			float ps = p->brows ? 6 : 9;
			blob(t_disc.t, ex + p->lookx * 5 * s, ey + p->looky * 6 * s, ps * s, (ps + 1) * s, 0x101010, 255, SDL_BLENDMODE_BLEND);
		}
		if (p->brows) {
			tint(t_px.t, dark, 255, SDL_BLENDMODE_BLEND);
			quad(t_px.t, ex, ey - 20 * s, 20 * s, 4 * s, sg * 15);
		}
	}
	if (p->mouth) blob(t_disc.t, MX(0), MY(-177 + hb), 14 * s, 18 * s, hoof, 255, SDL_BLENDMODE_BLEND);
	else
		for (int u = -16; u <= 16; u += 4)
			blob(t_disc.t, MX(u), MY(-181 + hb + (1 - (u / 16.f) * (u / 16.f)) * 5), 5 * s, 5 * s, hoof, 255, SDL_BLENDMODE_BLEND);

	for (int pass = 0; pass < 2; pass++)                                          /* arms: outline pass, then fur */
		for (int sg = -1; sg <= 1; sg += 2) {
			float hx = p->arms == 0 ? sg * 152 : p->arms == 1 ? sg * 98 : sg * 112, hy = p->arms == 2 ? -200 + b : -110 + b;
			float sx0 = sg * 64, sy0 = -160 + b;
			for (int k = 0; k <= 8; k++) {
				float u = k / 8.f, ax = sx0 + (hx - sx0) * u, ay = sy0 + (hy - sy0) * u + (p->arms == 2 ? 0 : 18 * sinf(u * PI));
				float r = (40 - u * 8) * s;
				blob(t_disc.t, MX(ax), MY(ay), r + (pass ? 0 : 6 * s), r + (pass ? 0 : 6 * s), pass ? fur : dark, 255, SDL_BLENDMODE_BLEND);
			}
		}
	if (p->arms == 0 && t_banner) {                                                /* the banner, in front of the arms */
		float k = s * 1.12f;
		SDL_Rect d = {(int)(MX(0) - 150 * k), (int)(MY(-112 + b) - 40 * k), (int)(300 * k), (int)(80 * k)};
		SDL_RenderCopyEx(R, t_banner, NULL, &d, 2 * sinf(gt * 2), NULL, SDL_FLIP_NONE);
	}
	for (int sg = -1; sg <= 1; sg += 2) {                                         /* hooves gripping its ends */
		float hx = p->arms == 0 ? sg * 152 : p->arms == 1 ? sg * 98 : sg * 112, hy = p->arms == 2 ? -200 + b : -110 + b;
		oq(t_disc.t, MX(hx), MY(hy), 36 * s, 30 * s, 0, hoof, 0, 0);
	}
#undef MX
#undef MY
}

static int crossed(float t0) { return splash_t >= t0 && splash_t - STEP < t0; }

static void clop(void) { snd(W_NOISE, 2500, 1200, .1f, .05f, .1f, 0); snd(W_SINE, 320, 200, .12f, .06f, .1f, 0); }
static void squawk(void)
{
	snd(W_SQR, 1300, 750, .1f, .12f, .4f, 0);
	snd(W_SQR, 1200, 700, .1f, .14f, .4f, .16f);
}

/* the splash timeline, all derived from splash_t: walk in, gull lands, gull steals the banner, moose gives chase */
static void splash_scene(struct mpose *m, float *gx, float *gy_, int *gflip, int *gwing, int *has_banner)
{
	float t = splash_t, gy = 440, s = .9f;
	*m = (struct mpose){320, 0, 0, 0, 1, 2 * sinf(t * 3), 0, 0, 0};
	if (t < 1.8f) {
		float u = t / 1.8f, e = 1 - (1 - u) * (1 - u);
		m->x = -170 + 490 * e;
		m->legph = m->x / 16;
		m->bob = -fabsf(sinf(m->legph)) * 7;
		m->lookx = 1;
	}
	if ((t > 2.5f && t < 2.6f) || (t > 2.75f && t < 2.85f)) m->blink = .1f;
	if (t > 3.4f && t < 4.5f) { m->lookx = .6f; m->looky = -.9f; }
	if (t >= 4.5f && t < 5.3f) { m->arms = 1; m->mouth = 1; m->brows = 1; m->lookx = .8f; m->looky = -.7f; }
	if (t >= 5.3f && t < 6.0f) { m->arms = 2; m->lookx = 0; m->looky = .3f; }
	if (t >= 6.0f) {
		float u = t - 6;
		m->x = 320 + 520 * u * u;
		m->legph = u * 30;
		m->bob = -fabsf(sinf(m->legph)) * 9;
		m->arms = 1;
		m->lookx = 1;
	}
	*has_banner = t < 4.5f;
	float perch_x = 320 + 92 * s, perch_y = gy - 362 * s;
	float grab_x = 320 + 140 * s, grab_y = gy - 112 * s - 46 * s;
	*gflip = 0;
	*gwing = (int)(t * 8) & 1;
	if (t < 2.6f) { *gx = 900; *gy_ = -100; }
	else if (t < 3.3f) {
		float u = (t - 2.6f) / .7f;
		*gx = 700 + (perch_x - 700) * u;
		*gy_ = 60 + (perch_y - 60) * u - sinf(u * PI) * 40;
	} else if (t < 4.2f) {
		*gx = perch_x;
		*gy_ = perch_y - fabsf(sinf(t * 6)) * 2;
		*gwing = 1;
	} else if (t < 4.5f) {
		float u = (t - 4.2f) / .3f;
		*gx = perch_x + (grab_x - perch_x) * u;
		*gy_ = perch_y + (grab_y - perch_y) * u - sinf(u * PI) * 30;
	} else {
		float u = t - 4.5f;
		*gx = grab_x + 260 * u * u + 60 * u;
		*gy_ = grab_y - 160 * u - 40 * u * u;
		*gflip = 1;
		*gwing = (int)(t * 12) & 1;
	}
}

static void splash_update(void)
{
	splash_t += STEP;
	if (splash_t < 1.8f && fmodf(splash_t, .26f) < STEP) clop();
	if (splash_t > 6 && fmodf(splash_t, .12f) < STEP) {
		clop();
		struct mpose m;
		float gx, gy;
		int f, w, hb;
		splash_scene(&m, &gx, &gy, &f, &w, &hb);
		for (int k = 0; k < 3; k++)
			part_new(P_INK, m.x - 30 + frand(-20, 20), 432 + frand(-6, 6), frand(-120, -40), frand(-40, -5), frand(.5f, .9f), frand(20, 36), 0xd8c49a)->grow = 50;
	}
	if (crossed(2.7f) || crossed(4.3f)) squawk();
	if (crossed(4.5f)) snd(W_NOISE, 5000, 2000, .2f, .12f, .1f, 0);
	if (crossed(4.62f)) snd(W_SINE, 300, 1100, .3f, .25f, .2f, 0);
	if (crossed(5.35f)) {
		snd(W_TRI, 392, 370, .3f, .3f, .5f, 0);
		snd(W_TRI, 330, 300, .3f, .5f, .2f, .32f);
	}
	if (crossed(6.0f)) snd(W_SINE, 500, 1700, .25f, .6f, .4f, 0);
	parts_update(STEP);
}

static void draw_splash(void)
{
	fill(0, 0, W, H, 0xf4ead5, 255);
	tint(t_beam.t, 0xe6d2a8, 230, SDL_BLENDMODE_BLEND);                           /* slow sunburst */
	for (int i = 0; i < 14; i++) {
		SDL_Rect d = {320 - 40, 250, 80, 460};
		SDL_Point c = {40, 0};
		SDL_RenderCopyEx(R, t_beam.t, NULL, &d, i * 360.f / 14 + splash_t * 8, &c, SDL_FLIP_NONE);
	}
	struct mpose m;
	float gx, gy;
	int gflip, gwing, has_banner;
	splash_scene(&m, &gx, &gy, &gflip, &gwing, &has_banner);
	parts_draw(0);
	if (!has_banner) m.arms = m.arms ? m.arms : 1;
	draw_moose(&m, 440, .9f);
	if (!has_banner && t_banner && gx < W + 300) {                                /* the banner, now the gull's */
		float k = 1.f;
		SDL_Rect d = {(int)(gx - 262 * k), (int)(gy + 16 - 12 * k), (int)(300 * k), (int)(80 * k)};
		SDL_Point c = {(int)(262 * k), (int)(12 * k)};
		SDL_RenderCopyEx(R, t_banner, NULL, &d, -22 + 10 * sinf(splash_t * 7), &c, SDL_FLIP_NONE);
	}
	spr(&t_gull[gwing], gx, gy, 2.2f, 2.2f, 0, gflip, 0);
	if (splash_t > 4.6f && splash_t < 5.3f) {
		float u = splash_t - 4.6f, sc = u < .15f ? u / .15f * 1.3f : 1;
		(void)sc;
		text(&f_huge, m.x + 70, 440 - 400 * .9f - (u < .15f ? (1 - u / .15f) * 20 : 0), "!", 0xe83a2a, 255, 1, 0);
	}
	if (splash_t > 5.3f && splash_t < 6.2f) {                                     /* sweat drop */
		float u = splash_t - 5.3f;
		blob(t_disc.t, m.x + 52, 440 - 250 * .9f + u * 30, 10, 14, 0x6ac8ff, 255, SDL_BLENDMODE_BLEND);
	}
	float pa = clampf((splash_t - 2) * 3, 0, 1) * clampf((6.6f - splash_t) * 3, 0, 1);
	if (pa > 0) text(&f_big, W / 2, 452 - 10, "PRESENTS", 0x6b4226, (int)(255 * pa), 1, 0);
	float f = clampf(1 - splash_t / .4f, 0, 1) + clampf((splash_t - 7.0f) / .6f, 0, 1);
	if (f > 0) fill(0, 0, W, H, 0, (int)(255 * clampf(f, 0, 1)));
}

/* the title: a sub crossing now and then, with a cross puffer in pursuit */
static int title_cruise(float *x, float *y)
{
	float c = fmodf(title_t + 4, 16);
	if (c > 10) return 0;
	*x = -140 + c * 90;
	*y = 372 + sinf(c * 2) * 6;
	return 1;
}

/* ======================================================================== render */

static void draw_scenery(void)
{
	SDL_Rect full = {0, 0, W, H};
	SDL_RenderCopy(R, t_grad.t, NULL, &full);

	/* sky: sun or moon and stars */
	if (th->night) {
		for (int i = 0; i < 60; i++) fill(stars[i].x, stars[i].y, 2, 2, 0xffffff, (int)(140 + 110 * sinf(gt * 2 + i)));
		blob(t_soft.t, 520, 26, 70, 70, 0x8090c0, 120, SDL_BLENDMODE_ADD);
		blob(t_disc.t, 520, 26, 26, 26, th->sun, 255, SDL_BLENDMODE_BLEND);
		blob(t_disc.t, 527, 22, 22, 22, th->sky0, 255, SDL_BLENDMODE_BLEND);
	} else {
		blob(t_soft.t, 520, 34, 140, 140, th->sun, 160, SDL_BLENDMODE_ADD);
		blob(t_disc.t, 520, 34, 34, 34, th->sun, 255, SDL_BLENDMODE_BLEND);
	}

	/* far reef and light shafts */
	uint32_t tn = th->tint;
	tint(t_far.t, tn, 255, SDL_BLENDMODE_BLEND);
	int fx = -(int)fmodf(scroll * .2f, (float)t_far.w);
	for (int x = fx; x < W; x += t_far.w) {
		SDL_Rect d = {x, FLOOR - t_far.h + 10, t_far.w, t_far.h};
		SDL_RenderCopy(R, t_far.t, NULL, &d);
	}
	draw_creatures(1);
	if (th->rays > 0) {
		tint(t_beam.t, th->night ? 0x80a0ff : th->sun, (int)(55 * th->rays), SDL_BLENDMODE_ADD);
		for (int i = 0; i < 6; i++) {
			float bx = fmodf(i * 131 + 40 - scroll * .1f + 800, 800) - 80;
			float w = 50 + 30 * sinf(i * 1.7f), sway = 6 * sinf(gt * .5f + i);
			SDL_Rect d = {(int)(bx - w / 2), SURF - 6, (int)w, 420};
			SDL_Point c = {(int)(w / 2), 0};
			SDL_SetTextureAlphaMod(t_beam.t, (Uint8)(55 * th->rays * (.7f + .3f * sinf(gt * .8f + i * 2))));
			SDL_RenderCopyEx(R, t_beam.t, NULL, &d, 18 + sway, &c, SDL_FLIP_NONE);
		}
	}
	tint(t_mid.t, tn, 255, SDL_BLENDMODE_BLEND);
	int mx = -(int)fmodf(scroll * .45f, (float)t_mid.w);
	for (int x = mx; x < W; x += t_mid.w) {
		SDL_Rect d = {x, FLOOR - t_mid.h + 26, t_mid.w, t_mid.h};
		SDL_RenderCopy(R, t_mid.t, NULL, &d);
	}

	/* drifting plankton */
	for (int i = 0; i < 70; i++) {
		struct mote *m = &motes[i];
		float x = fmodf(m->x - scroll * m->z + 2000 * W, W), y = m->y + 6 * sinf(gt * .7f + m->ph);
		blob(t_soft.t, x, y, 4 + 4 * m->z, 4 + 4 * m->z, th->night ? 0x60ffd0 : 0xd0ffff,
		     (int)((th->night ? 160 : 70) * (.6f + .4f * sinf(gt * 2 + m->ph))), SDL_BLENDMODE_ADD);
	}

	draw_creatures(0);

	/* seaweed, then the sand in front of its roots */
	for (int i = 0; i < 14; i++) {
		struct weed *wd = &weeds[i];
		float x = wd->wx - scroll;
		if (x < -60) { wd->wx = scroll + W + frand(20, 200); wd->segs = 12 + rnd() % 14; wd->ph = frand(0, 6); continue; }
		if (x > W + 40) continue;
		float y = FLOOR + 10, a = -PI / 2;
		for (int s = 0; s < wd->segs; s++) {
			a += .1f * sinf(gt * 1.6f + wd->ph + s * .3f);
			x += cosf(a) * 4.5f;
			y += sinf(a) * 4.5f;
			uint32_t c = lerp_rgb(wd->col, 0xc0ff80, (float)s / wd->segs * .4f);
			float r = 11 - s * 6.f / wd->segs;
			blob(t_disc.t, x, y, r, r, lerp_rgb(c, 0, 1 - ((th->tint >> 8) & 255) / 255.f), 255, SDL_BLENDMODE_BLEND);
		}
	}
	tint(t_sand.t, tn, 255, SDL_BLENDMODE_BLEND);
	int sx = -(int)fmodf(scroll, (float)t_sand.w);
	for (int x = sx; x < W; x += t_sand.w) {
		SDL_Rect d = {x + (int)ox, FLOOR - 8 + (int)oy, t_sand.w, t_sand.h};
		SDL_RenderCopy(R, t_sand.t, NULL, &d);
	}
	draw_caustics();
	draw_props();
}

static void draw_surface(void)
{
	SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
	for (int x = 0; x < W; x += 4) {
		int y = (int)wave_y(x + 2);
		fill(x, y, 4, SURF + 7 - y, th->wat0, 255);
		fill(x, y, 4, 2, 0xffffff, th->night ? 90 : 170);
	}
}

static void draw_enemy(const struct enemy *e)
{
	float fl = e->flash > 0 ? e->flash : 0;
	int fr = (int)(e->t * 8) & 1;
	switch (e->type) {
	case E_FISH:
		spr(&t_fish[e->var][fr], e->x, e->y, 1, 1, 0, 0, fl);
		break;
	case E_PUFF:
		if (e->hp <= 3) {                                                        /* puffed up: bigger, with spines */
			SDL_SetRenderDrawColor(R, 40, 30, 10, 255);
			for (int k = 0; k < 12; k++) {
				float a = k * PI / 6 + e->t;
				SDL_RenderDrawLine(R, (int)(e->x + ox + cosf(a) * 16), (int)(e->y + oy + sinf(a) * 16), (int)(e->x + ox + cosf(a) * 25),
				                   (int)(e->y + oy + sinf(a) * 25));
			}
			spr(&t_puff, e->x, e->y, 1.45f, 1.45f, 0, 0, fl);
		} else {
			spr(&t_puff, e->x, e->y, 1, 1, sinf(e->t * 2) * 8, 0, fl);
		}
		break;
	case E_JELLY:
		{
			float u = fmodf(e->t, 1.7f), sq = u < .3f ? 1 - .25f * sinf(u / .3f * PI) : 1;
			blob(t_soft.t, e->x, e->y - 4, 60, 60, 0xff60c0, th->night ? 150 : 60, SDL_BLENDMODE_ADD);
			spr(&t_jelly, e->x, e->y, 1 + (1 - sq) * .5f, sq, 0, 0, fl);
		}
		break;
	case E_CRAB:
		spr(&t_crab[(int)(e->t * 6) & 1], e->x, e->y - 10, 1, 1, 0, 0, fl);
		break;
	case E_CUDA:
		spr(&t_cuda, e->x + (e->phase == 1 ? frand(-1.5f, 1.5f) : 0), e->y, 1, 1, 0, 0, fl);
		break;
	case E_GULL:
		{
			float ang = e->phase == 0 ? 0 : atan2f(e->vy, -e->vx) * -180 / PI;
			int wing = e->phase == 0 ? (int)(e->t * 5) & 1 : e->phase == 2;
			spr(&t_gull[wing], e->x, e->y, 1, 1, ang, 0, fl);
		}
		break;
	}
}

static void draw_boss(void)
{
	if (!boss.on) return;
	float fl = boss.flash > 0 ? boss.flash : 0;
	uint32_t body = lerp_rgb(0x9a3aa8, 0xffffff, fl * .6f), dark = 0x5a1a70, light = lerp_rgb(0xd078e0, 0xffffff, fl * .6f);
	blob(t_soft.t, boss.x, boss.y, 300, 300, 0xa040ff, 70, SDL_BLENDMODE_ADD);
	for (int k = 0; k < NTENT; k++) {
		float pts[NSEG][3];
		tentacle(k, pts);
		for (int s = 0; s < NSEG; s++) {
			blob(t_disc.t, pts[s][0], pts[s][1], pts[s][2] * 2 + 3, pts[s][2] * 2 + 3, dark, 255, SDL_BLENDMODE_BLEND);
			blob(t_disc.t, pts[s][0], pts[s][1], pts[s][2] * 2, pts[s][2] * 2, body, 255, SDL_BLENDMODE_BLEND);
			if (s & 1) blob(t_disc.t, pts[s][0] - 2, pts[s][1] + 2, pts[s][2] * .7f, pts[s][2] * .7f, light, 255, SDL_BLENDMODE_BLEND);
		}
	}
	float x = boss.x, y = boss.y;
	blob(t_disc.t, x + 8, y - 40, 128, 148, dark, 255, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x + 8, y - 40, 120, 140, body, 255, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x - 8, y - 66, 60, 70, light, 120, SDL_BLENDMODE_BLEND);
	static const float spots[][3] = {{30, -80, 14}, {40, -40, 10}, {12, -100, 8}, {-20, -30, 9}, {44, -70, 7}};
	for (int i = 0; i < 5; i++) blob(t_disc.t, x + spots[i][0], y + spots[i][1], spots[i][2] * 2, spots[i][2] * 2, dark, 200, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x, y + 14, 104, 76, dark, 255, SDL_BLENDMODE_BLEND);
	blob(t_disc.t, x, y + 12, 98, 70, body, 255, SDL_BLENDMODE_BLEND);
	for (int e = 0; e < 2; e++) {                                               /* eyes follow the sub */
		float ex = x - 22 + e * 40, ey = y + 4, a = aim(ex, ey);
		blob(t_disc.t, ex, ey, 30, 30, 0xfff8d0, 255, SDL_BLENDMODE_BLEND);
		blob(t_disc.t, ex + cosf(a) * 6, ey + sinf(a) * 6, 13, 15, 0x101010, 255, SDL_BLENDMODE_BLEND);
		tint(t_disc.t, dark, 255, SDL_BLENDMODE_BLEND);
		quad(t_disc.t, ex, ey - 14, 32, 8, e ? -18 : 18);
	}
	blob(t_disc.t, x - 2, y + 34, 26, 12, 0x2a0a30, 255, SDL_BLENDMODE_BLEND);
}

static void draw_player(void)
{
	if (!pl.alive) return;
	if (pl.inv > 0 && ((int)(pl.inv * 12) & 1)) return;
	float tilt = clampf(pl.vy * .06f, -12, 12);
	spr(&t_sub, pl.x, pl.y, 1, 1, tilt, 0, 0);
	SDL_SetRenderDrawColor(R, 200, 200, 214, 255);                               /* spinning propeller */
	int px = (int)(pl.x + ox - 25), py = (int)(pl.y + oy + 2), b = (int)(gt * 30) & 1;
	SDL_Rect r1 = {px, py - (b ? 9 : 5), 3, b ? 7 : 4}, r2 = {px, py + (b ? 2 : 1), 3, b ? 7 : 4};
	SDL_RenderFillRect(R, &r1);
	SDL_RenderFillRect(R, &r2);
}

static void draw_shots(void)
{
	for (int i = 0; i < NPS; i++)
		if (ps[i].on) {
			blob(t_soft.t, ps[i].x - 6, ps[i].y, 34, 14, 0x40c0ff, 220, SDL_BLENDMODE_ADD);
			blob(t_disc.t, ps[i].x, ps[i].y, 12, 6, 0xe8ffff, 255, SDL_BLENDMODE_BLEND);
		}
	for (int i = 0; i < NEB; i++)
		if (eb[i].on) {
			static const uint32_t glow[] = {0xff40c0, 0xffd040, 0xa040ff}, core[] = {0xffd0f0, 0xfff0b0, 0x401060};
			float r = eb[i].kind == 2 ? 16 : 11;
			blob(t_soft.t, eb[i].x, eb[i].y, r * 2.6f, r * 2.6f, glow[eb[i].kind], 230, SDL_BLENDMODE_ADD);
			blob(t_disc.t, eb[i].x, eb[i].y, r, r, core[eb[i].kind], 255, SDL_BLENDMODE_BLEND);
		}
}

static void draw_pickups(void)
{
	static const char *lbl[] = {"P", "+", "S"};
	static const uint32_t col[] = {0xffd030, 0xff5060, 0x40e0ff};
	for (int i = 0; i < 8; i++)
		if (pk[i].on) {
			float s = 1 + .1f * sinf(pk[i].t * 8);
			blob(t_soft.t, pk[i].x, pk[i].y, 54 * s, 54 * s, col[pk[i].kind], 200, SDL_BLENDMODE_ADD);
			blob(t_disc.t, pk[i].x, pk[i].y, 22, 22, 0xffffff, 255, SDL_BLENDMODE_BLEND);
			blob(t_ring.t, pk[i].x, pk[i].y, 24, 24, col[pk[i].kind], 255, SDL_BLENDMODE_BLEND);
			text(&f_small, pk[i].x + ox, pk[i].y + oy - 6, lbl[pk[i].kind], col[pk[i].kind], 255, 1, 0);
		}
}

/* night: darken the sea except where something gives off light */
static void draw_darkness(void)
{
	if (!th->night || !t_dark) return;
	SDL_SetRenderTarget(R, t_dark);
	SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(R, 2, 6, 20, 175);
	SDL_RenderClear(R);
	SDL_BlendMode cut = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
	                                               SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
	float sox = ox, soy = oy;
	ox = oy = 0;
	if (pl.alive) {
		blob(t_soft.t, pl.x, pl.y, 150, 150, 0xffffff, 255, cut);
		tint(t_beam.t, 0xffffff, 255, cut);                                      /* headlight */
		SDL_Rect d = {(int)pl.x + 10 - 60, (int)pl.y + 2, 120, 380};
		SDL_Point c = {60, 0};
		SDL_RenderCopyEx(R, t_beam.t, NULL, &d, -90 + clampf(pl.vy * .06f, -12, 12), &c, SDL_FLIP_NONE);
	}
	for (int i = 0; i < NEN; i++)
		if (en[i].on && en[i].type == E_JELLY) blob(t_soft.t, en[i].x, en[i].y, 110, 110, 0xffffff, 220, cut);
	for (int i = 0; i < NPS; i++)
		if (ps[i].on) blob(t_soft.t, ps[i].x, ps[i].y, 50, 50, 0xffffff, 160, cut);
	for (int i = 0; i < NEB; i++)
		if (eb[i].on) blob(t_soft.t, eb[i].x, eb[i].y, 40, 40, 0xffffff, 160, cut);
	for (int i = 0; i < NPART; i++)
		if (parts[i].on && (parts[i].kind == P_FLASH || parts[i].kind == P_SPARK))
			blob(t_soft.t, parts[i].x, parts[i].y, parts[i].size * 3, parts[i].size * 3, 0xffffff, 255, cut);
	for (int i = 0; i < 8; i++)
		if (pk[i].on) blob(t_soft.t, pk[i].x, pk[i].y, 80, 80, 0xffffff, 255, cut);
	if (boss.on) blob(t_soft.t, boss.x, boss.y - 10, 260, 260, 0xffffff, 140, cut);
	blob(t_soft.t, 520, 26, 300, 300, 0xffffff, 200, cut);                        /* moonlight near the surface */
	ox = sox;
	oy = soy;
	SDL_SetRenderTarget(R, NULL);
	SDL_Rect d = {0, SURF - 8, W, H - SURF + 8}, s = d;
	SDL_RenderCopy(R, t_dark, &s, &d);
	if (pl.alive) {                                                              /* the beam itself, faintly visible in the water */
		tint(t_beam.t, 0xfff0b0, 70, SDL_BLENDMODE_ADD);
		SDL_Rect bd = {(int)(pl.x + ox) + 10 - 50, (int)(pl.y + oy) + 2, 100, 340};
		SDL_Point c = {50, 0};
		SDL_RenderCopyEx(R, t_beam.t, NULL, &bd, -90 + clampf(pl.vy * .06f, -12, 12), &c, SDL_FLIP_NONE);
	}
}

static void draw_hud(void)
{
	char s[48];
	snprintf(s, sizeof(s), "SCORE %07d", score);
	text(&f_small, 10, 8, s, 0xffffff, 255, 0, 0);
	snprintf(s, sizeof(s), "HI %07d", hiscore > score ? hiscore : score);
	text(&f_small, W / 2, 8, s, 0xffe080, 255, 1, 0);
	for (int i = 0; i < pl.lives && i < 5; i++) {                                 /* spare subs */
		SDL_Rect d = {W - 30 - i * 26, 6, 23, 13};
		SDL_RenderCopy(R, t_sub.t, NULL, &d);
	}
	for (int i = 0; i < pl.sonar; i++) {
		tint(t_ring.t, 0x60e8ff, 255, SDL_BLENDMODE_BLEND);
		SDL_Rect d = {W - 30 - i * 18, 24, 14, 14};
		SDL_RenderCopy(R, t_ring.t, NULL, &d);
	}
	for (int i = 0; i < 3; i++) fill(10 + i * 9, 28, 7, 6, i < pl.lvl ? 0xffd030 : 0x404858, 220);

	if (boss.on && !boss.dying) {
		fill(W / 2 - 152, H - 22, 304, 12, 0x000000, 170);
		fill(W / 2 - 150, H - 20, (int)(300.f * boss.hp / boss.maxhp), 8, 0xd050ff, 255);
		text(&f_small, W / 2, H - 42, "KRAKEN", 0xff9ff0, 255, 1, 0);
	}
	for (int i = 0; i < 16; i++)
		if (pop[i].on) text(&f_small, pop[i].x, pop[i].y - pop[i].t * 30, pop[i].s, 0xfff0a0, (int)(255 * (1 - pop[i].t)), 1, 0);
	if (banner_t > 0) {
		int a = (int)(255 * clampf(banner_t, 0, 1));
		text(&f_big, W / 2, 170, banner, strcmp(banner, "WARNING") ? 0xffffff : ((int)(gt * 6) & 1 ? 0xff4040 : 0xffd040), a, 1, 2);
		text(&f_small, W / 2, 212, banner2, 0xbfefff, a, 1, 0);
	}
}

static float bounce(float v)
{
	const float n = 7.5625f, d = 2.75f;
	if (v < 1 / d) return n * v * v;
	if (v < 2 / d) { v -= 1.5f / d; return n * v * v + .75f; }
	if (v < 2.5f / d) { v -= 2.25f / d; return n * v * v + .9375f; }
	v -= 2.625f / d;
	return n * v * v + .984375f;
}

static void draw_title(void)
{
	float cx, cy;
	if (title_cruise(&cx, &cy)) {
		spr(&t_puff, cx - 120, cy + 4 + 6 * sinf(title_t * 5), 1.3f, 1.3f, 8 * sinf(title_t * 7), 1, 0);
		spr(&t_sub, cx, cy, 1.5f, 1.5f, 3 * sinf(title_t * 2), 0, 0);
	}
	blob(t_soft.t, W / 2, 150, 640, 240, 0xffc060, (int)(70 + 25 * sinf(gt * 2)), SDL_BLENDMODE_ADD);

	/* the logo: letters drop in and bounce, a gradient, a drop shadow and a shine sweeping across */
	const char *ttl = "REEF RAIDER";
	int n = (int)strlen(ttl);
	float pen = W / 2 - text_w(&f_huge, ttl) / 2.f, base = 112 + f_huge.px * .72f, shine = fmodf(title_t * 320, W + 900) - 300;
	for (int i = 0; i < n; i++) {
		int c = ttl[i] - 32;
		float u = title_t - .25f - i * .07f;
		if (u > 0 && c > 0) {
			float yo = u < .7f ? -320 * (1 - bounce(u / .7f)) : sinf(gt * 3 + i * .55f) * 5;
			SDL_Rect d = {(int)(pen + f_huge.xo[c]), (int)(base + f_huge.yo[c] + yo), f_huge.r[c].w, f_huge.r[c].h}, sh = d;
			sh.x += 5;
			sh.y += 6;
			tint(f_huge.t, 0x000000, 110, SDL_BLENDMODE_BLEND);
			SDL_RenderCopy(R, f_huge.t, &f_huge.r[c], &sh);
			tint(f_huge.t, lerp_rgb(0xffe848, 0xff7020, (float)i / (n - 1)), 255, SDL_BLENDMODE_BLEND);
			SDL_RenderCopy(R, f_huge.t, &f_huge.r[c], &d);
			float dist = fabsf(d.x + d.w / 2.f - shine);
			if (dist < 80) {
				tint(f_huge.t, 0xffffff, (int)((1 - dist / 80) * 230), SDL_BLENDMODE_ADD);
				SDL_RenderCopy(R, f_huge.t, &f_huge.r[c], &d);
			}
		}
		pen += f_huge.adv[c];
	}
	if (title_t > 1.2f)                                                         /* sparkles */
		for (int i = 0; i < 7; i++) {
			float ph = gt * .7f + i * .37f, p = ph - floorf(ph);
			int cyc = (int)ph;
			float sx = 110 + hash1(i * 31 + cyc) * 420, sy = 100 + hash1(i * 17 + cyc * 3) * 80, sz = sinf(p * PI) * 20;
			blob(t_soft.t, sx, sy, sz * 1.6f, sz * 1.6f, 0xfff0c0, 200, SDL_BLENDMODE_ADD);
			tint(t_px.t, 0xffffff, 230, SDL_BLENDMODE_ADD);
			quad(t_px.t, sx, sy, sz * 2, 2, 0);
			quad(t_px.t, sx, sy, 2, sz * 2, 0);
		}
	int a = (int)(255 * clampf((title_t - 1.2f) * 2, 0, 1));
	text(&f_small, W / 2, 196, "AN UNDERWATER SHOOTER FROM GRAND CAYMAN", 0xbfefff, a, 1, 0);
	if (title_t > 1.6f) text(&f_big, W / 2, 262, js ? "PRESS START" : "PRESS ENTER", 0xffffff, (int)(150 + 105 * sinf(gt * 4)), 1, 0);
	text(&f_small, W / 2, 320, js ? "MOVE: D-PAD   FIRE: A   SONAR: X   MUSIC: Y" : "MOVE: ARROWS   FIRE: Z   SONAR: X   MUSIC: M", 0xffffff, a * 9 / 10, 1, 0);
	char s[48];
	snprintf(s, sizeof(s), "HIGH SCORE %07d", hiscore);
	text(&f_small, W / 2, 352, s, 0xffe080, a, 1, 0);
	snprintf(s, sizeof(s), "renderer: %s", gpu_name);
	text(&f_small, W / 2, H - 22, s, 0x9fd0e0, a * 3 / 4, 1, 0);
}

/* rebuild the sky + water gradient for the current theme */
static void mk_gradient(void)
{
	uint32_t *px = malloc(H * 4);
	for (int y = 0; y < H; y++) {
		uint32_t c;
		if (y < SURF + 7) c = lerp_rgb(th->sky0, th->sky1, (float)y / (SURF + 7));
		else c = lerp_rgb(th->wat0, th->wat1, powf((float)(y - SURF) / (H - SURF), .8f));
		px[y] = 0xff000000 | c;
	}
	if (t_grad.t) SDL_DestroyTexture(t_grad.t);
	t_grad = mk_tex(px, 1, H, 1);
}

static void draw_game(void)
{
	draw_scenery();
	draw_pickups();
	for (int i = 0; i < NEN; i++)
		if (en[i].on) draw_enemy(&en[i]);
	draw_boss();
	parts_draw(0);
	draw_player();
	draw_shots();
	parts_draw(1);
	if (pl.sonar_r > 0) {
		float a = 1 - pl.sonar_r / 800;
		blob(t_ring.t, pl.x, pl.y, pl.sonar_r * 2, pl.sonar_r * 2, 0x60f0ff, (int)(255 * a), SDL_BLENDMODE_ADD);
		blob(t_ring.t, pl.x, pl.y, pl.sonar_r * 1.7f, pl.sonar_r * 1.7f, 0x60f0ff, (int)(120 * a), SDL_BLENDMODE_ADD);
	}
	draw_darkness();
	draw_surface();
	ox = oy = 0;
	if (state == ST_TITLE) draw_title();
	else draw_hud();
	if (state == ST_PAUSE) {
		fill(0, 0, W, H, 0x000814, 140);
		text(&f_big, W / 2, 210, "PAUSED", 0xffffff, 255, 1, 0);
	}
	if (state == ST_OVER) {
		fill(0, 0, W, H, 0x000814, (int)(150 * clampf(over_t, 0, 1)));
		text(&f_huge, W / 2, 170, "GAME OVER", 0xff6060, 255, 1, 3);
		if (score >= hiscore && score > 0) text(&f_big, W / 2, 250, "NEW HIGH SCORE!", 0xffe060, 255, 1, 2);
	}
	if (fade > 0) fill(0, 0, W, H, 0, (int)(255 * fade));
}

/* ======================================================================== main */

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO) < 0) {
		SDL_Log("SDL_Init: %s", SDL_GetError());
		return 1;
	}
#ifdef HOST
	SDL_Window *win = SDL_CreateWindow("Reef Raider", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 3 / 2, H * 3 / 2, SDL_WINDOW_RESIZABLE);
#else
	SDL_Window *win = SDL_CreateWindow("Reef Raider", 0, 0, W, H, SDL_WINDOW_FULLSCREEN);
#endif
	R = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE) : NULL;
	if (!R) R = win ? SDL_CreateRenderer(win, -1, 0) : NULL;
	if (!R) {
		SDL_Log("window/renderer: %s", SDL_GetError());
		return 1;
	}
	SDL_RendererInfo ri;
	SDL_GetRendererInfo(R, &ri);
	gpu_name = ri.name;
	SDL_Log("video driver %s, renderer %s (%s), max texture %dx%d", SDL_GetCurrentVideoDriver(), ri.name,
	        ri.flags & SDL_RENDERER_ACCELERATED ? "accelerated" : "software", ri.max_texture_width, ri.max_texture_height);
	SDL_RenderSetLogicalSize(R, W, H);
	SDL_ShowCursor(SDL_DISABLE);
	js = SDL_NumJoysticks() > 0 ? SDL_JoystickOpen(0) : NULL;
	SDL_Log("joystick: %s", js ? SDL_JoystickName(js) : "none");

	SDL_AudioSpec want = {0}, have;
	want.freq = SR;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 1024;
	want.callback = audio_cb;
	adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (adev) SDL_PauseAudioDevice(adev, 0);
	else SDL_Log("no audio: %s", SDL_GetError());

	rng ^= (uint32_t)SDL_GetPerformanceCounter();
	Uint32 t0 = SDL_GetTicks();
	t_sub = SPRITE(art_sub);
	for (int i = 0; i < 2; i++) {
		static const uint32_t fishpal[4][3] = {{0xffff8a24, 0xffc84a10, 0xff2a5ad0},      /* orange */
		                                       {0xff3a7aff, 0xff1a3aa0, 0xffffd020},      /* blue tang */
		                                       {0xff60d860, 0xff2a8a3a, 0xfffff080},      /* green, shoots back */
		                                       {0xffff7a1a, 0xffb04010, 0xffffffff}};     /* clownfish */
		for (int v = 0; v < 4; v++) {
			pal_ovr = fishpal[v];
			t_fish[v][i] = mk_sprite(art_fish[i], 8);
		}
		pal_ovr = NULL;
		t_crab[i] = mk_sprite(art_crab[i], 12);
		t_gull[i] = mk_sprite(art_gull[i], 9);
	}
	t_puff = SPRITE(art_puff);
	t_cuda = SPRITE(art_cuda);
	t_turtle[0] = mk_sprite(art_turtle[0], 11);
	t_turtle[1] = mk_sprite(art_turtle[1], 11);
	t_jelly = SPRITE(art_jelly);
	t_heart = SPRITE(art_heart);
	mk_shapes();
	mk_layers();
	bake_font(&f_small, &font_bold, 16, 1.6f);
	bake_font(&f_big, &font_bold, 30, 2.5f);
	bake_font(&f_huge, &font_bold, 60, 4);
	mk_gradient();
	mk_banner();
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	t_dark = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, W, H);
	if (t_dark) {
		SDL_BlendMode cut = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
		                                               SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
		if (SDL_SetTextureBlendMode(t_soft.t, cut) < 0) {                          /* renderer cannot do it: no night lighting */
			SDL_Log("custom blend unsupported, night lighting off");
			SDL_DestroyTexture(t_dark);
			t_dark = NULL;
		} else {
			SDL_SetTextureBlendMode(t_dark, SDL_BLENDMODE_BLEND);
		}
	}
	SDL_Log("assets built in %u ms", SDL_GetTicks() - t0);

	for (int i = 0; i < 14; i++) weeds[i] = (struct weed){frand(0, W + 200), 12 + rnd() % 14, frand(0, 6), 0x3aa860};
	for (int i = 0; i < 70; i++) motes[i] = (struct mote){frand(0, W), frand(SURF + 10, FLOOR), frand(.2f, 1), frand(0, 6)};
	props_init();
	for (int i = 0; i < 60; i++) stars[i] = (SDL_Point){(int)frand(0, W), (int)frand(0, SURF - 4)};
	FILE *hf = fopen("hiscore.txt", "r");
	if (hf) { if (fscanf(hf, "%d", &hiscore) != 1) hiscore = 0; fclose(hf); }

	autoplay = getenv("REEF_AUTO") != NULL;
	if (getenv("REEF_LEN")) stage_len = (float)atof(getenv("REEF_LEN"));
	const char *shot_path = getenv("REEF_SHOT"), *shot_at = getenv("REEF_SHOT_AT");
	int shot_n = 0;
	if (autoplay) game_start();
	else {
		stage_start(0);
		banner_t = 0;
		state = getenv("REEF_NOSPLASH") ? ST_TITLE : ST_SPLASH;
		music_hold = state == ST_SPLASH;
	}
	const struct theme *drawn_theme = NULL;

	struct input prev = {0};
	Uint32 last = SDL_GetTicks(), fps_t = last;
	float acc = 0;
	int frames = 0;
	for (int running = 1; running;) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
			if (ev.type == SDL_QUIT) running = 0;
		Uint32 now = SDL_GetTicks();
		float dt = (now - last) / 1000.f;
		last = now;
		if (shot_path) dt = STEP;                                                   /* screenshots: a fixed clock */
		acc += fminf(dt, .1f);

		struct input in = read_input(), pr = in;
		pr.fire = in.fire && !prev.fire;
		pr.sonar = in.sonar && !prev.sonar;
		pr.start = in.start && !prev.start;
		pr.music = in.music && !prev.music;
		prev = in;
		if (in.quit) running = 0;
		if (pr.music) music_on = !music_on;

		if (state == ST_SPLASH && (pr.start || pr.fire || pr.sonar)) splash_t = 99;
		else if (state == ST_TITLE && pr.start) game_start();
		else if (state == ST_PLAY && pr.start) state = ST_PAUSE;
		else if (state == ST_PAUSE && pr.start) state = ST_PLAY;
		else if (state == ST_OVER && over_t > 1.5f && pr.start) {
			state = ST_TITLE;
			title_t = 0;
			memset(en, 0, sizeof(en));
			memset(eb, 0, sizeof(eb));
			memset(&boss, 0, sizeof(boss));
			stage_start(0);
		}

		while (acc >= STEP) {
			acc -= STEP;
			if (state == ST_PAUSE) continue;
			gt += STEP;
			if (state == ST_SPLASH) {
				splash_update();
				if (splash_t > 7.6f) {
					state = ST_TITLE;
					title_t = 0;
					music_hold = 0;
					memset(parts, 0, sizeof(parts));
				}
				continue;
			}
			creatures_update(STEP);
			props_update(STEP);
			if (state == ST_PLAY || state == ST_OVER) update_play(STEP, &in, &pr);
			else {                                                               /* title: scenery and some harmless fish */
				scroll += SCROLL_V * STEP;
				title_t += STEP;
				float cx, cy;
				if (title_cruise(&cx, &cy) && rnd() % 3 == 0) bubbles(cx - 36, cy + 4, 1, 2);
				if ((spawn_t -= STEP) <= 0) {
					float y = frand(SURF + 220, FLOOR - 40);
					for (int i = 0; i < 4; i++) {
						struct enemy *e = enemy_new(E_FISH, W + 20 + i * 30, y);
						if (e) { e->var = rnd() % 3; e->vx = -90; e->t = i * .4f; }
					}
					spawn_t = 4;
				}
				for (int i = 0; i < NEN; i++)
					if (en[i].on) {
						en[i].t += STEP;
						en[i].x += en[i].vx * STEP;
						en[i].y = en[i].base + 20 * sinf(en[i].t * 3);
						if (en[i].x < -40) en[i].on = 0;
					}
			}
			if (state == ST_OVER) over_t += STEP;
			parts_update(STEP);
			for (int i = 0; i < 16; i++)
				if (pop[i].on && (pop[i].t += STEP) > 1) pop[i].on = 0;
			if (shake > 0) shake = fmaxf(0, shake - STEP * 20);
			if (fade_dir) {
				fade += fade_dir * STEP * 1.5f;
				if (fade >= 1) {
					fade = 1;
					fade_dir = -1;
					stage_start(stage_n + 1);
				}
				if (fade <= 0) fade = fade_dir = 0;
			}
		}
		if (drawn_theme != th) { mk_gradient(); drawn_theme = th; }

		/* ---- draw */
		ox = shake > 0 ? frand(-shake, shake) : 0;
		oy = shake > 0 ? frand(-shake, shake) : 0;
		SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
		SDL_RenderClear(R);
		if (state == ST_SPLASH) draw_splash();
		else draw_game();

		if (shot_path && shot_at) {                                                /* REEF_SHOT_AT=2,5,9 */
			const char *p = shot_at;
			for (int i = 0; i < shot_n && p; i++) p = (p = strchr(p, ',')) ? p + 1 : NULL;
			if (!p) running = 0;
			else if (gt >= atof(p)) {
				int ow, oh;
				SDL_GetRendererOutputSize(R, &ow, &oh);
				SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
				SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
				char name[256];
				snprintf(name, sizeof(name), shot_path, shot_n);
				SDL_SaveBMP(s, name);
				SDL_FreeSurface(s);
				SDL_Log("saved %s at %.1fs", name, gt);
				shot_n++;
			}
		}
		SDL_RenderPresent(R);

		frames++;
		if (now - fps_t >= 10000) {
			SDL_Log("%.1f fps", frames * 1000.f / (now - fps_t));
			frames = 0;
			fps_t = now;
		}
	}
	if (score > hiscore) { hiscore = score; save_hiscore(); }
	SDL_Quit();
	return 0;
}
