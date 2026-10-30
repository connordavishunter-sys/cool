#include "ui.hpp"
#include "items.hpp"

#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

void UiBatch::quad(int tex, const float p[8], const float uv[8], float layer, uint32_t color) {
    if (draws.empty() || (tex >= 0 && draws.back().tex != tex)) draws.push_back({tex < 0 ? TexWhite : tex, (uint32_t)verts.size(), 0});
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i : order) verts.push_back({p[i * 2], p[i * 2 + 1], uv[i * 2], uv[i * 2 + 1], layer, color});
    draws.back().count += 6;
}

void UiBatch::rect(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color, float layer) {
    const float p[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    const float uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
    quad(tex, p, uv, layer, color);
}

bool Font::load(const std::string& path, int pixelSize) {
    if (!TTF_WasInit() && TTF_Init() != 0) return false;
    TTF_Font* f = TTF_OpenFont(path.c_str(), pixelSize);
    if (!f) return false;
    lineHeight = (float)TTF_FontHeight(f);
    atlasW = 1024;
    int x = 0, y = 0, rowH = (int)lineHeight + 2;
    std::vector<SDL_Surface*> surfaces(96, nullptr);
    for (int c = 32; c < 128; ++c) {
        SDL_Surface* s = TTF_RenderGlyph_Blended(f, (Uint16)(c == 127 ? '?' : c), SDL_Color{255, 255, 255, 255});
        if (!s) continue;
        surfaces[c - 32] = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(s);
    }
    // Pack glyphs into rows.
    for (int i = 0; i < 96; ++i) {
        SDL_Surface* s = surfaces[i];
        if (!s) continue;
        if (x + s->w + 1 > atlasW) { x = 0; y += rowH; }
        glyphs[i] = {(float)x, (float)y, (float)(x + s->w), (float)(y + s->h), (float)s->w, (float)s->h, 0};
        int minx, maxx, miny, maxy, adv;
        TTF_GlyphMetrics(f, (Uint16)(i + 32), &minx, &maxx, &miny, &maxy, &adv);
        glyphs[i].advance = (float)adv;
        x += s->w + 1;
    }
    atlasH = y + rowH;
    rgba.assign(atlasW * atlasH * 4, 0);
    for (int i = 0; i < 96; ++i) {
        SDL_Surface* s = surfaces[i];
        if (!s) continue;
        Glyph& g = glyphs[i];
        for (int r = 0; r < s->h; ++r)
            for (int col = 0; col < s->w; ++col) {
                const uint8_t* src = (const uint8_t*)s->pixels + r * s->pitch + col * 4;
                uint8_t* dst = &rgba[(((int)g.v0 + r) * atlasW + (int)g.u0 + col) * 4];
                dst[0] = dst[1] = dst[2] = 255;
                dst[3] = src[3];
            }
        g.u0 /= atlasW; g.u1 /= atlasW; g.v0 /= atlasH; g.v1 /= atlasH;
        SDL_FreeSurface(s);
    }
    TTF_CloseFont(f);
    return true;
}

void Ui::begin(int screenW, int screenH, const UiInput& input, double t) {
    scale = std::max(1.0f, std::floor(std::min(screenW / 480.0f, screenH / 300.0f)));
    width = screenW / scale;
    height = screenH / scale;
    in = input;
    in.mx /= scale;
    in.my /= scale;
    time = t;
    batch.clear();
    if (!in.down) activeSlider = nullptr;
}

void Ui::fill(float x, float y, float w, float h, uint32_t color) {
    batch.rect(TexWhite, x * scale, y * scale, w * scale, h * scale, 0, 0, 1, 1, color);
}

void Ui::image(int tex, float x, float y, float w, float h, uint32_t color, float u1) {
    batch.rect(tex, x * scale, y * scale, w * u1 * scale, h * scale, 0, 0, u1, 1, color);
}

void Ui::nineSlice(int tex, float x, float y, float w, float h, int texSize, int border, uint32_t color) {
    float b = border * scale;  // one texel per gui pixel
    float xs[4] = {x * scale, x * scale + b, (x + w) * scale - b, (x + w) * scale};
    float ys[4] = {y * scale, y * scale + b, (y + h) * scale - b, (y + h) * scale};
    float t = (float)border / texSize;
    float us[4] = {0, t, 1 - t, 1};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            batch.rect(tex, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], us[i], us[j], us[i + 1], us[j + 1], color);
}

void Ui::blockFace(int layer, float x, float y, float w, float h, uint32_t color) {
    batch.rect(-1, x * scale, y * scale, w * scale, h * scale, 0, 0, 1, 1, color, (float)layer);
}

void Ui::tiledBackground(int layer, float brightness) {
    uint8_t v = (uint8_t)(brightness * 255);
    float tile = 32;
    float off = std::fmod((float)time * 4.0f, tile);
    batch.rect(-1, 0, 0, width * scale, height * scale, off / tile, off / tile, (width + off) / tile, (height + off) / tile,
               rgba(v, v, v), (float)layer);
}

// Isometric cube: top, left (+Z) and right (+X) faces.
void Ui::blockIcon(BlockId block, float cx, float cy, float size) {
    if (block == ItemFood) {
        float s = size * 0.8f;
        image(TexHunger, cx - s / 2, cy - s / 2, s, s);
        return;
    }
    const BlockInfo& info = blockInfo(block);
    if (info.model != ModelCube) {
        float s = size * 0.9f;
        blockFace(info.tex[2], cx - s / 2, cy - s / 2, s, s);
        return;
    }
    // Isometric view from +X +Y +Z; shaped blocks draw their half-block cells back to front.
    float a = size * 0.5f * 0.9f * scale, b = a * 0.5f, s2 = size * 0.5f * 0.95f * scale, h = 2 * s2 - 2 * b;
    float ox = cx * scale, oy = cy * scale + s2 - 2 * b;
    auto at = [&](float* out, float x, float y, float z) { out[0] = ox + (x - z) * a; out[1] = oy + (x + z) * b - y * h; };
    bool glow = isEmissive(block);
    static const int order[8] = {0, 1, 2, 4, 3, 5, 6, 7};
    const bool whole = info.mask == 0xFF || isPanel(block);  // drawn as one box
    for (int i : order) {
        if (!whole && !(info.mask >> i & 1)) continue;
        float c = whole ? 1 : 0.5f;
        float x0 = (i & 1) * c, z0 = (i >> 1 & 1) * c, y0 = (i >> 2) * c, x1 = x0 + c, y1 = y0 + c, z1 = z0 + c;
        if (isPanel(block)) y1 = panelEighths(block) / 8.0f;
        float pt[8], pl[8], pr[8];
        at(pt, x0, y1, z0); at(pt + 2, x1, y1, z0); at(pt + 4, x1, y1, z1); at(pt + 6, x0, y1, z1);
        at(pl, x0, y1, z1); at(pl + 2, x1, y1, z1); at(pl + 4, x1, y0, z1); at(pl + 6, x0, y0, z1);
        at(pr, x1, y1, z1); at(pr + 2, x1, y1, z0); at(pr + 4, x1, y0, z0); at(pr + 6, x1, y0, z1);
        const float ut[8] = {x0, z0, x1, z0, x1, z1, x0, z1};
        const float ul[8] = {x0, 1 - y1, x1, 1 - y1, x1, 1 - y0, x0, 1 - y0};
        const float ur[8] = {1 - z1, 1 - y1, 1 - z0, 1 - y1, 1 - z0, 1 - y0, 1 - z1, 1 - y0};
        batch.quad(-1, pt, ut, info.tex[2], WHITE);
        batch.quad(-1, pl, ul, info.tex[4], glow ? WHITE : rgba(200, 200, 200));
        batch.quad(-1, pr, ur, info.tex[0], glow ? WHITE : rgba(150, 150, 150));
        if (whole) break;
    }
}

float Ui::textWidth(const std::string& s, float size) const {
    float w = 0, k = size / font.lineHeight * 1.25f;
    for (unsigned char ch : s) w += font.glyphs[(ch >= 32 && ch < 128 ? ch : '?') - 32].advance * k;
    return w;
}

void Ui::text(const std::string& s, float x, float y, uint32_t color, float size, bool shadow) {
    float k = size / font.lineHeight * 1.25f * scale;
    auto draw = [&](float ox, float oy, uint32_t col) {
        float px = (x + ox) * scale, py = (y + oy) * scale - size * 0.2f * scale;
        for (unsigned char ch : s) {
            const Glyph& g = font.glyphs[(ch >= 32 && ch < 128 ? ch : '?') - 32];
            batch.rect(TexFont, px, py, g.w * k, g.h * k, g.u0, g.v0, g.u1, g.v1, col);
            px += g.advance * k;
        }
    };
    if (shadow) draw(size / 8, size / 8, rgba(40, 40, 50, (color >> 24) & 255));
    draw(0, 0, color);
}

void Ui::textCentered(const std::string& s, float cx, float y, uint32_t color, float size) {
    text(s, cx - textWidth(s, size) / 2, y, color, size);
}

bool Ui::hovered(float x, float y, float w, float h) const {
    return in.mx >= x && in.my >= y && in.mx < x + w && in.my < y + h;
}

bool Ui::button(const std::string& label, float x, float y, float w, float h, bool enabled) {
    bool hover = enabled && hovered(x, y, w, h);
    uint32_t tint = !enabled ? rgba(110, 110, 120) : hover ? rgba(255, 255, 255) : rgba(205, 210, 220);
    nineSlice(TexButton, x, y, w, h, 15, 3, tint);
    textCentered(label, x + w / 2, y + (h - 8) / 2, enabled ? (hover ? rgba(255, 255, 160) : WHITE) : rgba(160, 160, 170));
    return hover && in.pressed;
}

bool Ui::slider(const std::string& label, float x, float y, float w, float h, float& value, const void* id) {
    nineSlice(TexSliderBg, x, y, w, h, 16, 2);
    if (in.pressed && hovered(x, y, w, h)) activeSlider = id;
    bool changed = false;
    if (activeSlider == id) {
        float v = std::clamp((in.mx - x - 3) / (w - 6), 0.0f, 1.0f);
        changed = v != value;
        value = v;
    }
    float hw = 5;
    image(TexSliderHandle, x + 1 + value * (w - hw - 2), y, hw, h);
    textCentered(label, x + w / 2, y + (h - 8) / 2, hovered(x, y, w, h) ? rgba(255, 255, 160) : WHITE);
    return changed;
}

bool Ui::textField(std::string& value, float x, float y, float w, float h, bool& focused, size_t maxLen,
                   const std::string& placeholder) {
    if (in.pressed) focused = hovered(x, y, w, h);
    nineSlice(TexSliderBg, x, y, w, h, 16, 2, focused ? WHITE : rgba(190, 190, 200));
    bool changed = false;
    if (focused) {
        for (char c : in.text)
            if (value.size() < maxLen && c >= 32 && c < 127) { value += c; changed = true; }
        if (in.backspace && !value.empty()) { value.pop_back(); changed = true; }
    }
    if (value.empty() && !focused) text(placeholder, x + 5, y + (h - 8) / 2, rgba(120, 125, 140), 8, false);
    std::string shown = value;
    while (!shown.empty() && textWidth(shown) > w - 14) shown.erase(0, 1);
    text(shown, x + 5, y + (h - 8) / 2);
    if (focused && std::fmod(time, 1.0) < 0.5) fill(x + 6 + textWidth(shown), y + (h - 8) / 2, 1, 9, WHITE);
    return changed;
}

void Ui::panel(float x, float y, float w, float h) { nineSlice(TexPanel, x, y, w, h, 15, 3, rgba(255, 255, 255, 235)); }
