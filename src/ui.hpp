#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum UiTex { TexWhite, TexButton, TexPanel, TexSlot, TexSliderBg, TexSliderHandle, TexTitle, TexFont, TexHeart, TexHunger, UiTexCount };

struct UiVertex {
    float x, y, u, v, layer;  // layer < 0 samples the 2D texture, otherwise the block texture array
    uint32_t color;           // RGBA8, sRGB
};

struct UiDraw {
    int tex;
    uint32_t first, count;
};

struct UiBatch {
    std::vector<UiVertex> verts;
    std::vector<UiDraw> draws;

    void clear() { verts.clear(); draws.clear(); }
    // Arbitrary quad; corners are clockwise on screen from top-left. tex < 0 keeps the current texture.
    void quad(int tex, const float pos[8], const float uv[8], float layer, uint32_t color);
    void rect(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color, float layer = -1);
};

struct Glyph {
    float u0, v0, u1, v1;
    float w, h, advance;
};

struct Font {
    int atlasW = 0, atlasH = 0;
    std::vector<uint8_t> rgba;
    float lineHeight = 0;
    Glyph glyphs[96]{};
    bool load(const std::string& path, int pixelSize);
};

struct UiInput {
    float mx = 0, my = 0;
    bool down = false, pressed = false, released = false, rightPressed = false;
    float wheel = 0;
    std::string text;
    bool backspace = false, enter = false, escape = false, tab = false;
};

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}
constexpr uint32_t WHITE = rgba(255, 255, 255);

// Immediate-mode widgets drawn in "gui pixels" (screen pixels / scale).
class Ui {
public:
    UiBatch batch;
    Font font;
    UiInput in;
    float scale = 2;
    float width = 0, height = 0;  // in gui pixels
    double time = 0;

    void begin(int screenW, int screenH, const UiInput& input, double t);

    void fill(float x, float y, float w, float h, uint32_t color);
    void image(int tex, float x, float y, float w, float h, uint32_t color = WHITE, float u1 = 1);  // u1 < 1 crops the right side
    void nineSlice(int tex, float x, float y, float w, float h, int texSize, int border, uint32_t color = WHITE);
    void blockFace(int layer, float x, float y, float w, float h, uint32_t color = WHITE);
    void blockIcon(uint8_t item, float cx, float cy, float size);  // isometric block, or a flat icon for other items
    void tiledBackground(int layer, float brightness);

    float textWidth(const std::string& s, float size = 8) const;
    void text(const std::string& s, float x, float y, uint32_t color = WHITE, float size = 8, bool shadow = true);
    void textCentered(const std::string& s, float cx, float y, uint32_t color = WHITE, float size = 8);

    bool hovered(float x, float y, float w, float h) const;
    bool button(const std::string& label, float x, float y, float w, float h, bool enabled = true);
    bool slider(const std::string& label, float x, float y, float w, float h, float& value, const void* id);
    bool textField(std::string& value, float x, float y, float w, float h, bool& focused, size_t maxLen = 32,
                   const std::string& placeholder = "");
    void panel(float x, float y, float w, float h);

private:
    const void* activeSlider = nullptr;
};
