#pragma once
#include "PageBase.h"
#include <RP_TFTDisplay.h>
#include <math.h>
#include <string.h>

// ============================================================================
// PageSimpleChat + UiCardView + VkOverlay
// - TextBox di bawah; tap -> fokus + VK muncul; caret blink.
// - TextBox mengikuti tepi atas VK saat VK terbuka (follow).
// - Tap di luar VK & textbox -> VK tertutup, textbox balik ke bawah.
// - VK dirender TERAKHIR per tile via _vk.paintIntoTile() (anti-flicker).
// - CARD (UiCardView) berisi bubble chat kanan; scroll DI DALAM card bila overflow.
// - Auto-scroll ke bawah saat tambah pesan / tinggi card menyusut (VK muncul).
// - Painter isi card tile-aware, hard clipped ke innerRect ∩ tile.
// ============================================================================

class PageSimpleChat : public PageBase, public ITextSink {
public:
  void begin(AppOS* os, TopBar* top) {
    PageBase::begin(os, top);
    setTitle("SIMPLE CHAT");

    // VK helper
    _vk.begin(&_os->ui, &_os->bg, &_os->blit);
    _vk.attachSink(this);
    _vk.setUnderlayPainter(&PageSimpleChat::_vkUnderlay);
    _vk.setCommitOnDown(true);
    _vk.setRepeat(true, 300, 100);

    // State
    _focused        = false;
    _caretOn        = true;
    _tBlinkPrev     = millis();
    _vkWasVisible   = false;
    _followVK       = false;
    _pendingSnap    = false;

    // Chat & scroll
    _nMsgs                 = 0;
    _chatInnerW            = -1;  // force recompute
    _chatContentH          = 0;
    _scrollToBottomPending = false;
    _prevChatViewH         = -1;

    // tap-close tracking
    _tapTracking       = false;
    _tapCloseCandidate = false;
    _tapDownX = _tapDownY = 0;

    // CardView painter
    _cardV.setPainter(&_paintChatContent, this);

    recomputeLayout();
    updateContentHeight();
  }

  void onEnter() override {
    PageBase::onEnter();
    recomputeLayout();
    updateContentHeight();
  }

  void onQSClosed() override {
    if (!isActive()) return;
    recomputeLayout();
    updateContentHeight();
    PageBase::onQSClosed();
  }

  int contentHeight() const { return _contentH; }

  // ===== ITextSink =====
  void insertChar(char c) override { _text += c; _resetBlink(); }
  void backspace() override        { if (_text.length()) _text.remove(_text.length()-1); _resetBlink(); }
  void enter() override {
    if (_text.length() > 0) {
      _appendMsg(_text);
      _text = "";
      _scrollToBottomPending = true;  // selalu ke pesan terbaru
    }
    _focused  = true;
    _followVK = true;
    _resetBlink();
  }

protected:
  // ===== Rendering =====
  void paintContentTile(GfxRGB888& g, int Rrx, int Rry) override {
    const int RryEff  = Rry + _scrollY;

    // caret blink
    const uint32_t now = millis();
    if (_focused) {
      if (now - _tBlinkPrev >= 450) { _caretOn = !_caretOn; _tBlinkPrev = now; }
    } else {
      _caretOn = true; _tBlinkPrev = now;
    }

    // VK anim step (tanpa render)
    _vk.step();

    // follow VK (tetap)
    if (_vk.visible()) {
      int kx, ky, kw, kh; _vk.rect(kx, ky, kw, kh);
      const int vpTop    = _viewportY0;
      const int vpBottom = _viewportY0 + _viewportH;

      int newY = ky - _tbRect.h - _tbMarginAboveVK;
      if (newY < vpTop + 12) newY = vpTop + 12;
      if (newY > vpBottom - _tbRect.h - 12) newY = vpBottom - _tbRect.h - 12;
      _tbRect.y = newY;
    } else if (_vkWasVisible && _pendingSnap) {
      placeTextBoxAtBottom();
      _pendingSnap = false;
    }
    _vkWasVisible = _vk.visible();

    // === Layout card ===
    {
      const int cardTop = _viewportY0 + TITLE2CARD;
      int cardBottom = _tbRect.y - CARD_GAP_FROM_TB;
      const int vpBottom = _viewportY0 + _viewportH;
      if (cardBottom > vpBottom) cardBottom = vpBottom;
      if (cardBottom < cardTop)  cardBottom = cardTop;

      const int cx = EDGE;
      const int cw = TFT_WIDTH - 2*EDGE;
      const int cy = cardTop;
      const int ch = imax(0, cardBottom - cardTop);

      _cardV.setRect(cx, cy, cw, ch);
      _cardV.setRadius(CARD_RADIUS);
      _cardV.setPadding(CARD_PAD);
      _cardV.setColors(CR,CG,CB, 26,28,38);
    }

    // Auto-scroll ke bawah bila view menyusut
    {
      UiCardView::Rect inner = _cardV.innerRect();
      if (_prevChatViewH >= 0 && inner.h < _prevChatViewH) _scrollToBottomPending = true;
      _prevChatViewH = inner.h;
    }

    // Recompute content H dan scrollMax jika innerW/H berubah
    {
      UiCardView::Rect inner = _cardV.innerRect();
      if (_chatInnerW != inner.w) {
        _chatInnerW = inner.w;
        _recomputeChatContentH();
        _cardV.setContentHeight(_chatContentH);
        if (_scrollToBottomPending) {
          _cardV.setScrollY(_cardV.scrollMax());
          _scrollToBottomPending = false;
        } else if (_cardV.scrollY() > _cardV.scrollMax()) {
          _cardV.setScrollY(_cardV.scrollMax());
        }
      } else {
        // inner width sama; cek jika contentH atau viewH berubah
        int newMax = imax(0, _chatContentH - inner.h);
        if (newMax != _cardV.scrollMax()) {
          _cardV.setContentHeight(_chatContentH);
          if (_scrollToBottomPending) {
            _cardV.setScrollY(_cardV.scrollMax());
            _scrollToBottomPending = false;
          } else if (_cardV.scrollY() > _cardV.scrollMax()) {
            _cardV.setScrollY(_cardV.scrollMax());
          }
        } else if (_scrollToBottomPending) {
          _cardV.setScrollY(_cardV.scrollMax());
          _scrollToBottomPending = false;
        }
      }
    }

    // Paint card + isi (tile-aware via callback)
    _cardV.paint(g, Rrx, Rry);

    // ==== Textbox (clip terhadap VK supaya tidak overlap) ====
    Rect r = _tbRect; // screen coords
    if (_vk.visible()) {
      int kx, ky, kw, kh; _vk.rect(kx, ky, kw, kh);
      int limitY   = ky;
      int vpBottom = _viewportY0 + _viewportH;
      if (limitY > vpBottom) limitY = vpBottom;
      if (r.y + r.h > limitY) r.h = limitY - r.y;
      if (r.h < 0) r.h = 0;
    }
    if (r.h > 0) drawTextBox(g, Rrx, RryEff, r);

    // TERAKHIR: render VK di tile ini (anti-flicker)
    _vk.paintIntoTile(g, Rrx, Rry, /*tileY=*/Rry, /*tileH=*/g.h);
  }

  // ===== Input =====
  void handleContentInput(bool pressed, int x, int y) override {
    const int cy = y + _scrollY;
    const int gx = x;
    const int gy = _mapTouchScreenY(y);

    UiCardView::Rect inner = _cardV.innerRect();
    const bool inCard =
      (cy >= inner.y && cy < inner.y + inner.h &&
       x  >= inner.x && x  < inner.x + inner.w);

    // Saat VK terbuka
    if (_vk.visible()) {
      int kx, ky, kw, kh; _vk.rect(kx, ky, kw, kh);
      const bool inKeyboard = (gx>=kx && gx<kx+kw && gy>=ky && gy<ky+kh);
      const bool inTextbox  = hitRect(x, cy, _tbRect);

      if (pressed) {
        if (inKeyboard) {
          _vk.handleTouch(true, gx, gy);
          _draggingChat = false;
          _tapTracking = _tapCloseCandidate = false;
          return;
        }
        _vk.handleTouch(false, 0, 0);

        if (inTextbox) {
          _focused = true;
          _tapCloseCandidate = false;
          _draggingChat = false;
          return;
        }

        if (!_tapTracking) {
          _tapTracking       = true;
          _tapCloseCandidate = true;
          _tapDownX = x; _tapDownY = y;
        }

        if (inCard) {
          if (!_draggingChat) {
            _draggingChat = true;
            _dragChatY0   = y;
            _chatScrollY0 = _cardV.scrollY();
          } else {
            if (abs(y - _tapDownY) > TAP_SLOP || abs(x - _tapDownX) > TAP_SLOP) {
              _tapCloseCandidate = false;
            }
            int dy = y - _dragChatY0;
            int newScroll = _chatScrollY0 - dy;
            _cardV.setScrollY(newScroll);
          }
        } else {
          if (abs(y - _tapDownY) > TAP_SLOP || abs(x - _tapDownX) > TAP_SLOP) {
            _tapCloseCandidate = false;
          }
        }
        return;
      } else {
        _vk.handleTouch(false, 0, 0);

        if (_tapTracking && _tapCloseCandidate) {
          _vk.hide();
          _followVK    = false;
          _focused     = false;
          _pendingSnap = true;
        }
        _tapTracking = false;
        _tapCloseCandidate = false;
        _draggingChat = false;
        return;
      }
    }

    // VK tidak terbuka
    if (pressed) {
      if (hitRect(x, cy, _tbRect)) {
        _focused  = true;
        _followVK = true;
        _vk.show();
        _resetBlink();
        return;
      }
      if (inCard) {
        if (!_draggingChat) {
          _draggingChat = true;
          _dragChatY0   = y;
          _chatScrollY0 = _cardV.scrollY();
        } else {
          int dy = y - _dragChatY0;
          _cardV.setScrollY(_chatScrollY0 - dy);
        }
        return;
      }

      if (_focused) { _focused = false; _resetBlink(); }
    } else {
      _draggingChat = false;
    }
  }

private:
  // ===== Helpers / types =====
  struct Rect { int x=0,y=0,w=0,h=0; };
  static inline int imax(int a,int b){ return (a>b)?a:b; }
  static inline int imin(int a,int b){ return (a<b)?a:b; }

  static constexpr int TAP_SLOP = 6;   // piksel
  bool      _tapTracking        = false;
  bool      _tapCloseCandidate  = false;
  int       _tapDownX           = 0;
  int       _tapDownY           = 0;

  static inline bool hitRect(int x,int y,const Rect& r){
    return (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h);
  }
  static inline bool hitRect(int x,int y,const UiCardView::Rect& r){
    return (x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h);
  }

  int _mapTouchScreenY(int y_from_router){
    if (y_from_router < 0) y_from_router = 0;
    if (y_from_router >= TFT_HEIGHT) y_from_router = TFT_HEIGHT - 1;
    return y_from_router;
  }

  static void _vkUnderlay(GfxRGB888& g,int Rrx,int Rry,int x,int y,int w,int h){
    g.fillRect(x - Rrx, y - Rry, w, h, 20,22,28);
  }

  void _resetBlink(){ _caretOn = true; _tBlinkPrev = millis(); }

  void placeTextBoxAtBottom(){
    _tbRect.x = EDGE;
    _tbRect.w = TFT_WIDTH - 2*EDGE;
    _tbRect.h = TB_H;
    _tbRect.y = _viewportY0 + _viewportH - _tbRect.h - 16;
  }

  void recomputeLayout(){ placeTextBoxAtBottom(); }

  void updateContentHeight(){
    const int bottom = _tbRect.y + _tbRect.h;
    _contentH = bottom + BOT_PAD;

    const int vpH = _viewportH;
    int maxY = _contentH - vpH;
    if (maxY < 0) maxY = 0;
    _scrollMax = maxY;
    if (_scrollY > _scrollMax) _scrollY = _scrollMax;
  }

  void fillRoundedClipped(GfxRGB888& g, int Rrx, int Rry,
                          const Rect& r, int rad,
                          uint8_t rr,uint8_t gg,uint8_t bb){
    if (r.w<=0 || r.h<=0) return;
    const int x0=r.x, y0=r.y, w=r.w, h=r.h;
    const int drawTop = imax(y0, Rry);
    const int drawBot = imin(y0+h, Rry+g.h);
    if (drawBot <= drawTop) return;

    const int rlim = imax(0, imin(rad, imin(w,h)/2));
    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yLoc = gy - Rry;
      const int yy = gy - y0;
      int inset=0;
      if (rlim>0){
        if (yy < rlim){
          float dy = float(rlim - 1 - yy);
          float dx = sqrtf(float(rlim)*rlim - dy*dy);
          inset = int(rlim - floorf(dx));
        }
        int y2 = (h - 1 - yy);
        if (y2 < rlim){
          float dy = float(rlim - 1 - y2);
          float dx = sqrtf(float(rlim)*rlim - dy*dy);
          int inset2 = int(rlim - floorf(dx));
          if (inset2 > inset) inset = inset2;
        }
      }
      int gx0 = imax(x0 + inset, Rrx);
      int gx1 = imin(x0 + w - inset, Rrx + g.w);
      if (gx1 <= gx0) continue;

      uint8_t* row = g.pix + ((size_t)yLoc*g.w + (gx0 - Rrx))*3;
      for (int gx=gx0; gx<gx1; ++gx){ row[0]=rr; row[1]=gg; row[2]=bb; row+=3; }
    }
  }

  void drawTextBox(GfxRGB888& g, int Rrx, int RryEff, const Rect& r) {
    if (r.h<=0) return;
    if (r.y >= RryEff + g.h || (r.y + r.h) <= RryEff) return;

    // outer
    fillRoundedClipped(g, Rrx, RryEff, r, RADIUS, 36,38,46);
    // inner
    Rect inner{ r.x+2, r.y+2, r.w-4, r.h-4 };
    fillRoundedClipped(g, Rrx, RryEff, inner, RADIUS-1,
                       _focused?24:18, _focused?26:20, _focused?32:26);

    const char* s = _text.c_str();
    int size = 2;
    if (SimpleFont::textWidth(s, size, 0) > r.w - 24) size = 1;

    const int tx   = r.x + 10;
    const int base = r.y + r.h/2 + SimpleFont::lineAdvance(size)/3;

    SimpleFont::drawTextStyled(g, tx - Rrx, base - RryEff, s, 230,230,235, size, 0,
                               SimpleFont::AlignLeft, -1, SimpleFont::Bold);

    if (_focused && _caretOn) {
      const int tw  = SimpleFont::textWidth(s, size, 0);
      const int cx  = tx + tw + 2;
      const int cy0 = r.y + 8;
      const int cy1 = r.y + r.h - 8;
      g.fillRect(cx - Rrx, cy0 - RryEff, 2, (cy1 - cy0), 240,240,240);
    }
  }

  // ===== Chat data & rendering ==============================================
  struct ChatMsg { String text; };     // <-- HANYA SEKALI didefinisikan
  static constexpr int CHAT_MAX = 64;

  void _appendMsg(const String& s){
    if (_nMsgs < CHAT_MAX){ _msgs[_nMsgs++].text = s; }
    else {
      for (int i=1;i<CHAT_MAX;i++) _msgs[i-1] = _msgs[i];
      _msgs[CHAT_MAX-1].text = s;
      _nMsgs = CHAT_MAX;
    }
    _recomputeChatContentH();
    _cardV.setContentHeight(_chatContentH);
    _scrollToBottomPending = true;
  }

  // wrap measure sederhana
  void _measureWrapped(const char* s, int size, int maxW, int& outLines, int& outMaxPx){
    outLines = 0; outMaxPx = 0;
    if (!s || !*s){ outLines=1; outMaxPx=0; return; }

    const int BUFSZ = 192;
    int i = 0; int n = (int)strlen(s);
    while (i < n){
      int len = 0, lastSpace = -1;
      while (i + len < n){
        char c = s[i + len];
        if (c == '\n') break;
        if (c == ' ') lastSpace = len;
        char tmp[BUFSZ];
        int cut = len + 1; if (cut >= BUFSZ) cut = BUFSZ - 1;
        memcpy(tmp, s + i, cut); tmp[cut] = 0;
        int w = SimpleFont::textWidth(tmp, size, 0);
        if (w > maxW) break;
        len++;
      }
      bool hadNewline = (i + len < n && s[i + len] == '\n');
      int cut = 0;
      if (hadNewline) { cut = len; }
      else if (i + len < n) { cut = (lastSpace>=0)? lastSpace : (len>0? len : 1); }
      else { cut = len; }

      char tmp[BUFSZ];
      if (cut >= BUFSZ) cut = BUFSZ - 1;
      memcpy(tmp, s + i, cut); tmp[cut] = 0;
      int w = SimpleFont::textWidth(tmp, size, 0);
      if (w > outMaxPx) outMaxPx = w;
      outLines++;

      i += cut;
      if (hadNewline) i++;
      while (i < n && s[i] == ' ') i++;
    }
    if (outLines <= 0) outLines = 1;
  }

  // draw wrapped dgn clip tile ∩ inner
  void _drawWrapped(GfxRGB888& g, int Rrx,int RryEff,
                    int x,int y,int maxW,int size,
                    const char* s, uint8_t r,uint8_t gg,uint8_t b,
                    int visTop,int visBot){
    if (!s) s="";
    const int BUFSZ = 192;
    const int lh = SimpleFont::lineAdvance(size);

    int i = 0; int n = (int)strlen(s); int yy = y;
    while (i < n){
      int len = 0, lastSpace = -1;
      while (i + len < n){
        char c = s[i + len];
        if (c == '\n') break;
        if (c == ' ') lastSpace = len;
        char tmp[BUFSZ];
        int cut = len + 1; if (cut >= BUFSZ) cut = BUFSZ - 1;
        memcpy(tmp, s + i, cut); tmp[cut] = 0;
        if (SimpleFont::textWidth(tmp, size, 0) > maxW) break;
        len++;
      }
      int cut=0;
      bool hadNewline = (i + len < n && s[i+len]=='\n');
      if (hadNewline) { cut = len; }
      else if (i + len < n) { cut = (lastSpace>=0)? lastSpace : (len>0? len : 1); }
      else { cut = len; }

      const int lineTop    = yy;
      const int lineBottom = yy + lh;

      if (lineBottom > visTop && lineTop <= visBot){
        char tmp[BUFSZ]; if (cut >= BUFSZ) cut = BUFSZ - 1;
        memcpy(tmp, s + i, cut); tmp[cut] = 0;

        const int base = yy + (lh - size);
        SimpleFont::drawTextStyled(g, x - Rrx, base - RryEff,
                                   tmp, r,gg,b, size, 0,
                                   SimpleFont::AlignLeft, -1, SimpleFont::Bold);
      }

      i += cut;
      if (hadNewline) i++;        // skip '\n'
      while (i < n && s[i] == ' ') i++;
      yy += lh;
      if (yy > visBot) break;
    }
  }

  // hitung total tinggi chat
  void _recomputeChatContentH(){
    UiCardView::Rect inner = _cardV.innerRect();
    const int size = CHAT_FONT_SIZE;
    const int lh   = SimpleFont::lineAdvance(size);
    const int maxBubbleW = (inner.w * 78) / 100;
    int y = 0;
    for (int i=0;i<_nMsgs;i++){
      int lines=1, maxpx=0;
      _measureWrapped(_msgs[i].text.c_str(), size, maxBubbleW - 2*BUBBLE_PAD_X, lines, maxpx);
      int bubbleW = imax(maxpx + 2*BUBBLE_PAD_X, MIN_BUBBLE_W);
      if (bubbleW > maxBubbleW) bubbleW = maxBubbleW;
      int bubbleH = lines*lh + 2*BUBBLE_PAD_Y;
      y += bubbleH + CHAT_GAP_Y;
    }
    _chatContentH = (y>0)? (y - CHAT_GAP_Y) : 0;
  }

  // Painter callback static → memanggil method instance
  static void _paintChatContent(GfxRGB888& g, int Rrx, int RryEff,
                                const UiCardView::Rect& visClip,
                                const UiCardView::Rect& inner,
                                int scrollY, void* user){
    PageSimpleChat* self = static_cast<PageSimpleChat*>(user);
    self->_paintChatContentImpl(g, Rrx, RryEff, visClip, inner, scrollY);
  }

  void _fillRoundedClippedInRegion(GfxRGB888& g, int Rrx, int Rry,
                                   const Rect& r, int rad,
                                   uint8_t rr,uint8_t gg,uint8_t bb,
                                   const UiCardView::Rect& clip){
    if (r.w<=0 || r.h<=0 || clip.w<=0 || clip.h<=0) return;
    const int x0=r.x, y0=r.y, w=r.w, h=r.h;

    const int drawTop = imax(y0, imax(Rry, clip.y));
    const int drawBot = imin(y0+h, imin(Rry+g.h, clip.y+clip.h));
    if (drawBot <= drawTop) return;

    const int rlim = imax(0, imin(rad, imin(w,h)/2));

    const int clipL = imax(Rrx, clip.x);
    const int clipR = imin(Rrx + g.w, clip.x + clip.w);

    for (int gy=drawTop; gy<drawBot; ++gy){
      const int yLoc = gy - Rry;
      const int yy = gy - y0;
      int inset=0;
      if (rlim>0){
        if (yy < rlim){
          float dy = float(rlim - 1 - yy);
          float dx = sqrtf(float(rlim)*rlim - dy*dy);
          inset = int(rlim - floorf(dx));
        }
        int y2 = (h - 1 - yy);
        if (y2 < rlim){
          float dy = float(rlim - 1 - y2);
          float dx = sqrtf(float(rlim)*rlim - dy*dy);
          int inset2 = int(rlim - floorf(dx));
          if (inset2 > inset) inset = inset2;
        }
      }
      int gx0 = imax(x0 + inset, clipL);
      int gx1 = imin(x0 + w - inset, clipR);
      if (gx1 <= gx0) continue;

      uint8_t* row = g.pix + ((size_t)yLoc*g.w + (gx0 - Rrx))*3;
      for (int gx=gx0; gx<gx1; ++gx){ row[0]=rr; row[1]=gg; row[2]=bb; row+=3; }
    }
  }

  // Implementasi painter isi chat (tile-aware, scroll-aware)
  void _paintChatContentImpl(GfxRGB888& g, int Rrx, int RryEff,
                             const UiCardView::Rect& visClip,
                             const UiCardView::Rect& inner,
                             int scrollY){
    if (inner.h <= 0 || inner.w <= 0) return;

    const int size = CHAT_FONT_SIZE;
    const int lh   = SimpleFont::lineAdvance(size);
    const int maxBubbleW = (inner.w * 78) / 100;

    // Mulai dari top inner - scroll
    int y = inner.y - scrollY;

    // Skip cepat sampai bubble pertama yg masuk visClip
    int idx = 0;
    for (; idx<_nMsgs; ++idx){
      const char* s = _msgs[idx].text.c_str();
      int lines=1, maxpx=0;
      _measureWrapped(s, size, maxBubbleW - 2*BUBBLE_PAD_X, lines, maxpx);
      int bubbleW = imax(maxpx + 2*BUBBLE_PAD_X, MIN_BUBBLE_W);
      if (bubbleW > maxBubbleW) bubbleW = maxBubbleW;
      int bubbleH = lines*lh + 2*BUBBLE_PAD_Y;

      if (y + bubbleH > visClip.y) break; // mulai terlihat
      y += bubbleH + CHAT_GAP_Y;
    }

    const int vTop = inner.y;
    const int vBot = inner.y + inner.h - 1;
    const int visTop = imax(vTop, visClip.y);
    const int visBot = imin(vBot, visClip.y + visClip.h - 1);

    for (; idx<_nMsgs; ++idx){
      const char* s = _msgs[idx].text.c_str();

      int lines=1, maxpx=0;
      _measureWrapped(s, size, maxBubbleW - 2*BUBBLE_PAD_X, lines, maxpx);
      int bubbleW = imax(maxpx + 2*BUBBLE_PAD_X, MIN_BUBBLE_W);
      if (bubbleW > maxBubbleW) bubbleW = maxBubbleW;
      int bubbleH = lines*lh + 2*BUBBLE_PAD_Y;

      if (y >= visClip.y + visClip.h) break; // di bawah tile

      Rect br;
      br.w = bubbleW;
      br.h = bubbleH;
      br.x = inner.x + inner.w - bubbleW;  // kanan
      br.y = y;

      _fillRoundedClippedInRegion(g, Rrx, RryEff, br, BUBBLE_RADIUS,
                                  BUBBLE_OR_R, BUBBLE_OR_G, BUBBLE_OR_B, visClip);
      Rect in2{ br.x+1, br.y+1, br.w-2, br.h-2 };
      _fillRoundedClippedInRegion(g, Rrx, RryEff, in2, BUBBLE_RADIUS-1,
                                  BUBBLE_IN_R, BUBBLE_IN_G, BUBBLE_IN_B, visClip);

      int tx = br.x + BUBBLE_PAD_X;
      int ty = br.y + BUBBLE_PAD_Y;

      _drawWrapped(g, Rrx, RryEff, tx, ty,
                   bubbleW - 2*BUBBLE_PAD_X, size,
                   s, BUBBLE_TX_R, BUBBLE_TX_G, BUBBLE_TX_B,
                   visTop, visBot);

      y += bubbleH + CHAT_GAP_Y;
      if (y > visClip.y + visClip.h) break;
    }
  }

  // === Theme & geometry ===
  static constexpr int   EDGE        = Theme::EDGE_BUF;
  static constexpr int   RADIUS      = 8;
  static constexpr int   CARD_RADIUS = 12;
  static constexpr int   TB_H        = 48;
  static constexpr int   BOT_PAD     = 24;
  static constexpr int   TITLE2CARD  = 10;
  static constexpr int   CARD_GAP_FROM_TB = 8;
  static constexpr uint8_t CR = Theme::UI_R, CG = Theme::UI_G, CB = Theme::UI_B;

  static constexpr int   CARD_PAD       = 10;
  static constexpr int   BUBBLE_PAD_X   = 10;
  static constexpr int   BUBBLE_PAD_Y   = 6;
  static constexpr int   BUBBLE_RADIUS  = 10;
  static constexpr int   CHAT_GAP_Y     = 6;
  static constexpr int   CHAT_FONT_SIZE = 2;
  static constexpr int   MIN_BUBBLE_W   = 48;

  static constexpr uint8_t BUBBLE_OR_R = 10, BUBBLE_OR_G = 80, BUBBLE_OR_B = 255;
  static constexpr uint8_t BUBBLE_IN_R = 10, BUBBLE_IN_G = 80, BUBBLE_IN_B = 255;
  static constexpr uint8_t BUBBLE_TX_R = 255,  BUBBLE_TX_G = 255,  BUBBLE_TX_B = 255;

  // State: textbox, vk
  struct Rect _tbRect{};
  String    _text;
  bool      _focused=false;
  bool      _caretOn=true;
  uint32_t  _tBlinkPrev=0;

  VkOverlay   _vk;
  bool        _vkWasVisible=false;
  bool        _followVK=false;
  bool        _pendingSnap=false;
  int         _tbMarginAboveVK = 12;

  // CardView & Chat data
  UiCardView  _cardV;
  ChatMsg     _msgs[CHAT_MAX];
  int         _nMsgs=0;

  // Chat layout/scroll bookkeeping
  int       _chatInnerW      = -1;
  int       _chatContentH    = 0;
  bool      _scrollToBottomPending = false;
  int       _prevChatViewH   = -1;

  // Page outer scroll (tidak dipakai untuk content)
  int  _contentH = 0;
  int  _scrollY  = 0;
  int  _scrollMax= 0;
  bool _dragging = false;
  int  _dragY0   = 0;
  int  _scrollY0 = 0;

  // drag chat
  bool _draggingChat = false;
  int  _dragChatY0   = 0;
  int  _chatScrollY0 = 0;
};
