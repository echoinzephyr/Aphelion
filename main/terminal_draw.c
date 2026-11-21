#include "terminal.h"

void terminal_oled_pack(const uint8_t *pixels, unsigned height, uint8_t frame[OLED_FRAME_BYTES])
{
    for (unsigned page = 0; page < height / 8; ++page) {
        for (unsigned x = 0; x < OLED_WIDTH; ++x) {
            uint8_t column = 0;
            for (unsigned y = 0; y < 8; ++y) {
                unsigned source_row = page * 8 + y;
                if (pixels[source_row * OLED_WIDTH + x] >= 112) {
                    column |= 1u << y;
                }
            }
            frame[page * OLED_WIDTH + x] = column;
        }
    }
}
