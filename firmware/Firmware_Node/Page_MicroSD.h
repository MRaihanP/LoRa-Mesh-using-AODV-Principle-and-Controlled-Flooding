#pragma once
#include "PageBase.h"

class PageMicroSD : public PageBase {
public:
  void begin(AppOS* os, TopBar* top){
    PageBase::begin(os, top);
    setTitle("MICRO SD");
  }
protected:
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const char* msg="MicroSD page (placeholder)";
    const int s=2, gh=7*s, tw=SimpleFont::textWidth(msg,s,1);
    const int xt=(TFT_WIDTH-tw)/2, yt=_viewportY0+_viewportH/2-gh/2;
    SimpleFont::drawTextStyled(g, xt-Rrx, (yt-Rry)+gh, msg, 255,255,255, s,1,
                               SimpleFont::AlignLeft, -1, SimpleFont::Bold);
  }
};
