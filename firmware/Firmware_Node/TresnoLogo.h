#pragma once
#include <RP_TFTDisplay.h>
#include "logo.h"
#include <math.h>

namespace TresnoLogo {

  // Gambar logo + teks "TRESNO" sebagai satu mesh (logo + text ikut scale)
  inline void drawMesh(GfxRGB888& g,
                       int Rrx, int Rry,          // tile origin
                       int centerX, int centerY,  // pusat global
                       float scale,
                       const char* text = "TRESNO")
  {
    const int bmpW = LOGO_WIDTH;
    const int bmpH = LOGO_HEIGHT;

    const int scaledW = (int)(bmpW * scale);
    const int scaledH = (int)(bmpH * scale);
    if (scaledW <= 0 || scaledH <= 0) return;

    const int x0 = centerX - scaledW / 2;
    const int y0 = centerY - scaledH / 2;
    const int boxRight  = x0 + scaledW;
    const int boxBottom = y0 + scaledH;

    // ====== LOGO (bitmap grayscale → putih, multi-tile safe) ======
    for (int ty = 0; ty < g.h; ++ty) {
      int gy = Rry + ty;
      if (gy < y0 || gy >= boxBottom) continue;

      float syf = (gy + 0.5f - y0) / scale;
      int   sy  = (int)floorf(syf);
      if (sy < 0 || sy >= bmpH) continue;

      for (int tx = 0; tx < g.w; ++tx) {
        int gx = Rrx + tx;
        if (gx < x0 || gx >= boxRight) continue;

        float sxf = (gx + 0.5f - x0) / scale;
        int   sx  = (int)floorf(sxf);
        if (sx < 0 || sx >= bmpW) continue;

        uint8_t v = pgm_read_byte(&logo[sy * bmpW + sx]);
        if (v == 0) continue;          // background → transparan

        uint8_t* p = g.pix + ((ty * g.w + tx) * 3);
        p[0] = v;
        p[1] = v;
        p[2] = v;
      }
    }

    // ====== TEKS "TRESNO" di bawah logo (jarak 8px) ======
    const char* msg = text ? text : "TRESNO";

    const float textFactor = 1.4f;  // sama seperti Page_MicroSD versi terakhir
    int textScale = (int)floorf(scale * textFactor + 0.5f);
    if (textScale < 1)  textScale = 1;
    if (textScale > 16) textScale = 16;

    const int glyphH  = 7 * textScale;
    const int textW   = SimpleFont::textWidth(msg, textScale, 1);
    const int logoBottom = y0 + scaledH - 1;
    const int textTop     = logoBottom + 8;
    const int textBaseY   = textTop + glyphH;

    const int xt = centerX - textW / 2;

    SimpleFont::drawTextStyled(
      g,
      xt - Rrx,             // tile-local X
      textBaseY - Rry,      // tile-local baseline Y
      msg,
      255,255,255,          // putih
      textScale,
      1,
      SimpleFont::AlignLeft,
      -1,
      SimpleFont::Bold
    );
  }

} // namespace TresnoLogo
