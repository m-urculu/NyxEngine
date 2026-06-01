// Strip the RGB channels of a PNG and save just the alpha as a grayscale PNG,
// so we can visually inspect what the alpha-cutout pass actually sees. The
// engine's hair material discards pixels with alpha < 0.5.
#define STB_IMAGE_IMPLEMENTATION
#include "../external/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/stb/stb_image_write.h"
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: dump_alpha in.png out.png\n"); return 1; }
    int w=0, h=0, ch=0;
    auto px = stbi_load(argv[1], &w, &h, &ch, 4);
    if (!px) { std::fprintf(stderr, "load fail: %s\n", stbi_failure_reason()); return 1; }
    std::vector<unsigned char> g(w * h);
    int zero = 0, full = 0;
    for (int i = 0; i < w * h; ++i) {
        g[i] = px[i * 4 + 3];
        if (g[i] == 0) zero++;
        if (g[i] == 255) full++;
    }
    stbi_write_png(argv[2], w, h, 1, g.data(), w);
    std::printf("alpha stats: %d pixels, %d zero (%.1f%%), %d full255 (%.1f%%)\n",
                w*h, zero, 100.0*zero/(w*h), full, 100.0*full/(w*h));
    return 0;
}
