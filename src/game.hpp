#pragma once
#include "clouds.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "player.hpp"
#include "renderer.hpp"
#include "ui.hpp"
#include "vk_context.hpp"
#include "world.hpp"

#include <SDL2/SDL_mixer.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct Options {
    int lighting = LightClassic;
    int renderDist = 10;
    float fov = 70;
    float renderScale = 1.0f;
    float sensitivity = 0.5f;
    float music = 0.5f;
    bool vsync = true;
    float brightness = 0.5f;
    int aa = AaOff;
    bool frameGen = false;
    RtSettings rt;
};

enum GameMode { Creative, Survival };

// Per-world rules, changed with /gamerule and saved in level.dat.
struct GameRules {
    bool doDaylightCycle = true, keepInventory = true, fallDamage = true, fireDamage = true, naturalRegeneration = true,
         doTileDrops = true;
};
struct GameRule { const char* name; bool GameRules::*value; const char* desc; };
inline constexpr GameRule kGameRules[] = {
    {"doDaylightCycle", &GameRules::doDaylightCycle, "time of day advances"},
    {"keepInventory", &GameRules::keepInventory, "keep your items when you die"},
    {"fallDamage", &GameRules::fallDamage, "falling hurts"},
    {"fireDamage", &GameRules::fireDamage, "lava hurts"},
    {"naturalRegeneration", &GameRules::naturalRegeneration, "health regenerates when well fed"},
    {"doTileDrops", &GameRules::doTileDrops, "broken blocks drop items"},
};

struct WorldMeta {
    std::string name, folder;
    uint64_t seed = 0;
    int64_t lastPlayed = 0;
    bool spawned = false;
    glm::dvec3 pos{8.5, 120, 8.5};
    float yaw = 0, pitch = 0;
    bool flying = false;
    double time = 0.05;  // fraction of a day
    Inventory inv;
    int slot = 0;

    int mode = Creative;  // worlds saved before survival existed have no mode and are creative
    int health = 20, hunger = 20;
    float saturation = 5, exhaustion = 0;
    glm::dvec3 spawn{0, -1, 0};  // y < 0 until the first spawn is chosen
    GameRules rules;
};

class Game {
public:
    Game();
    ~Game();
    void run();

private:
    enum class Screen { Title, Worlds, CreateWorld, ConfirmDelete, Options, RayTracing, Loading, Playing, Paused, Inventory, Dead, Chat };

    std::filesystem::path base, savesDir;
    SDL_Window* window = nullptr;
    VkCtx vk;
    Renderer renderer;
    Ui ui;
    Options opts;
    bool running = true;
    Screen screen = Screen::Title, optionsReturn = Screen::Title;
    double now = 0, fpsTimer = 0;
    int frames = 0, fps = 0;
    std::mt19937_64 rng{std::random_device{}()};

    // Menus
    std::vector<WorldMeta> worldList;
    int selectedWorld = -1;
    float worldScroll = 0;
    double lastClickTime = 0;
    std::string newName, newSeed;
    bool nameFocus = true, seedFocus = false;
    int newMode = Survival;

    // In game
    std::unique_ptr<World> world;
    WorldMeta meta;
    Player player;
    Particles particles;
    std::unique_ptr<Clouds> clouds;
    bool showDebug = false;
    double lastSpace = -1, breakTimer = 0, placeTimer = 0, autosaveTimer = 0, loadingStart = 0, slotChangedAt = -10;
    bool worldEdited = false;
    bool mouseLeft = false, mouseRight = false;

    // Survival
    glm::ivec3 breakTarget{0};
    double breakProgress = 0, chipTimer = 0, fallStart = 0, lavaTimer = 0, regenTimer = 0, hurtAt = -10;
    ItemStack held;  // stack carried by the mouse in the inventory
    float craftScroll = 0;

    // Chat and commands
    struct ChatLine { std::string text; uint32_t color; double at; };
    std::vector<ChatLine> chat;
    std::vector<std::string> sent;  // earlier messages, recalled with up and down
    std::string chatInput;
    size_t sentPos = 0;
    double chatOpenedAt = -10;
    bool chatFocus = true, swallowText = false;

    // Music
    std::vector<Mix_Music*> tracks;
    bool musicPlaying = false;
    double nextTrackAt = 2;
    int lastTrack = -1;

    void loadOptions();
    void saveOptions() const;
    void refreshWorldList();
    void createWorld();
    void openWorld(const WorldMeta& m);
    void saveWorld();
    void closeWorld();
    void setScreen(Screen s);

    void handleEvent(const SDL_Event& e, UiInput& in);
    void updateGame(double dt);
    void updateMusic();
    void interact(bool breakBlock);
    void breakBlock(glm::ivec3 p);
    uint8_t orient(uint8_t item, const RayHit& hit) const;
    bool survival() const { return meta.mode == Survival; }
    double reach() const { return survival() ? 5.0 : 6.0; }
    void updateSurvival(double dt, glm::dvec3 prevPos);
    void hurt(int amount);
    void respawn();
    void closeInventory();
    void openChat(const std::string& prefix);
    void say(const std::string& text, uint32_t color = 0xFFFFFFFF);
    void runCommand(const std::string& line);

    void drawTitle();
    void drawWorlds();
    void drawCreateWorld();
    void drawConfirmDelete();
    void drawOptions();
    void drawRayTracing();
    void drawLoading();
    void drawHud();
    void drawPause();
    void drawInventory();
    void drawSurvivalInventory();
    void drawDeath();
    void drawChat(bool open);
    void drawStatusBars();
    void drawStack(const ItemStack& s, float x, float y, float slot);
    void drawHotbar(float y, bool clickable);
};
