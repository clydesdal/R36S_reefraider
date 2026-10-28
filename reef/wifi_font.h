#ifndef WIFI_FONT_h_
#define WIFI_FONT_h_

#include <stdint.h>

/* signed-distance-field glyph (see tools/mkfont.py): an 8-bit field of w x h samples, 128 on the outline, rising inside */
struct sdf_glyph {
	uint32_t off;
	uint8_t w, h;
	int8_t bx;                    /* left edge of the field relative to the pen position, in field pixels */
	int8_t by;                    /* top edge of the field above the baseline */
	uint16_t adv16;               /* advance in 1/16 field pixel */
};

struct sdf_font {
	const unsigned char *data;
	const struct sdf_glyph *glyph;
	int first, last;
	int em;                       /* pixel height of the em square the fields were made at */
	int spread;                   /* distance, in field pixels, that 127 steps of the field cover */
};

extern const struct sdf_font font_medium, font_bold;

#endif
