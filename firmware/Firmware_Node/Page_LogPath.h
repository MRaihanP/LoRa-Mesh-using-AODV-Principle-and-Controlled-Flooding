#pragma once
#include "PageBase.h"

class PageLogPath : public PageBase {
public:
  void begin(AppOS* os, TopBar* top){
    PageBase::begin(os, top);
    setTitle("LOG PATH");
  }
protected:
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const char* msg = "Log Path page (placeholder)";
    const int scale=2, glyphH=7*scale;
    const int textW = SimpleFont::textWidth(msg, scale, 1);
    const int textX = (TFT_WIDTH - textW)/2;
    const int textYt= _viewportY0 + _viewportH/2 - glyphH/2;
    const int yBase = (textYt - Rry) + glyphH, xLocal = (textX - Rrx);
    if (yBase >= 0 && yBase <= g.h + glyphH){
      SimpleFont::drawTextStyled(g, xLocal, yBase, msg, 255,255,255, scale,1,
                                 SimpleFont::AlignLeft, -1, SimpleFont::Bold);
    }
  }
};
