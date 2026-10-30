#include "game.hpp"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

static const char* kLightingNames[LightModeCount] = {"Classic", "Ray Traced", "Path Traced"};
static const char* kAaNames[AaCount] = {"Off", "FXAA", "TAA", "DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance"};
static const char* kTracks[] = {"a_new_beginning", "crashing_waves", "endless_possibilites", "land_of_oppertunitues"};

// ---------------------------------------------------------------- key=value files

static std::map<std::string, std::string> readKeyValues(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

template <typename T>
static void readValue(const std::map<std::string, std::string>& kv, const char* key, T& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    std::istringstream ss(it->second);
    T v;
    if (ss >> v) out = v;
}

void Game::loadOptions() {
    auto kv = readKeyValues(base / "options.txt");
    readValue(kv, "lighting", opts.lighting);
    readValue(kv, "renderDistance", opts.renderDist);
    readValue(kv, "fov", opts.fov);
    readValue(kv, "renderScale", opts.renderScale);
    readValue(kv, "sensitivity", opts.sensitivity);
    readValue(kv, "music", opts.music);
    readValue(kv, "vsync", opts.vsync);
    readValue(kv, "brightness", opts.brightness);
    readValue(kv, "antialiasing", opts.aa);
    readValue(kv, "frameGeneration", opts.frameGen);
    RtSettings& rt = opts.rt;
    readValue(kv, "rtShadowRays", rt.shadowRays);
    readValue(kv, "rtSunSoftness", rt.rtSunSoftness);
    readValue(kv, "rtAmbient", rt.ambient);
    readValue(kv, "rtReflections", rt.reflections);
    readValue(kv, "ptBounces", rt.bounces);
    readValue(kv, "ptSamples", rt.samples);
    readValue(kv, "ptHistory", rt.history);
    readValue(kv, "ptSunSoftness", rt.ptSunSoftness);
    readValue(kv, "lightStrength", rt.lightStrength);
    rt.shadowRays = std::clamp(rt.shadowRays, 0, 32);
    rt.bounces = std::clamp(rt.bounces, 0, 6);
    rt.samples = std::clamp(rt.samples, 1, 8);
    rt.history = std::clamp(rt.history, 1, 256);
    opts.aa = std::clamp(opts.aa, 0, AaCount - 1);
    opts.brightness = std::clamp(opts.brightness, 0.0f, 1.0f);
    opts.lighting = std::clamp(opts.lighting, 0, LightModeCount - 1);
    opts.renderDist = std::clamp(opts.renderDist, 2, 24);
    opts.renderScale = std::clamp(opts.renderScale, 0.25f, 1.0f);
}

void Game::saveOptions() const {
    std::ofstream f(base / "options.txt");
    f << "lighting=" << opts.lighting << "\nrenderDistance=" << opts.renderDist << "\nfov=" << opts.fov
      << "\nrenderScale=" << opts.renderScale << "\nsensitivity=" << opts.sensitivity << "\nmusic=" << opts.music << "\nvsync=" << opts.vsync << "\nbrightness=" << opts.brightness
      << "\nantialiasing=" << opts.aa << "\nframeGeneration=" << opts.frameGen;
    const RtSettings& rt = opts.rt;
    f << "\nrtShadowRays=" << rt.shadowRays << "\nrtSunSoftness=" << rt.rtSunSoftness << "\nrtAmbient=" << rt.ambient
      << "\nrtReflections=" << rt.reflections << "\nptBounces=" << rt.bounces << "\nptSamples=" << rt.samples << "\nptHistory=" << rt.history
      << "\nptSunSoftness=" << rt.ptSunSoftness << "\nlightStrength=" << rt.lightStrength << "\n";
}

static WorldMeta readMeta(const fs::path& dir) {
    WorldMeta m;
    m.folder = dir.filename().string();
    auto kv = readKeyValues(dir / "level.dat");
    m.name = kv.count("name") ? kv["name"] : m.folder;
    readValue(kv, "seed", m.seed);
    readValue(kv, "lastPlayed", m.lastPlayed);
    readValue(kv, "spawned", m.spawned);
    readValue(kv, "x", m.pos.x);
    readValue(kv, "y", m.pos.y);
    readValue(kv, "z", m.pos.z);
    readValue(kv, "yaw", m.yaw);
    readValue(kv, "pitch", m.pitch);
    readValue(kv, "flying", m.flying);
    readValue(kv, "time", m.time);
    readValue(kv, "slot", m.slot);
    readValue(kv, "mode", m.mode);
    readValue(kv, "health", m.health);
    readValue(kv, "hunger", m.hunger);
    readValue(kv, "saturation", m.saturation);
    readValue(kv, "exhaustion", m.exhaustion);
    readValue(kv, "spawnX", m.spawn.x);
    readValue(kv, "spawnY", m.spawn.y);
    readValue(kv, "spawnZ", m.spawn.z);
    for (const auto& r : kGameRules) readValue(kv, (std::string("rule.") + r.name).c_str(), m.rules.*r.value);
    auto valid = [](int id) { return isBlockItem((uint8_t)id) || id == ItemFood; };
    if (bool legacy = !kv.count("items"); !legacy || kv.count("inventory")) {
        std::istringstream ss(kv[legacy ? "inventory" : "items"]);
        int id, n;
        char colon;
        for (auto& s : m.inv.slots) {
            if (!(ss >> id >> colon >> n)) break;
            if (legacy && id == LEGACY_FOOD) id = ItemFood;
            if (valid(id) && n > 0) s = {(uint8_t)id, (uint8_t)std::min(n, STACK_MAX)};
        }
    } else if (kv.count("hotbar")) {  // older saves only had a creative hotbar
        std::istringstream ss(kv["hotbar"]);
        int id;
        for (int i = 0; i < 9 && ss >> id; ++i)
            if (valid(id)) m.inv.slots[i] = {(uint8_t)id, 1};
    }
    m.slot = std::clamp(m.slot, 0, 8);
    m.mode = m.mode == Survival ? Survival : Creative;
    return m;
}

static void writeMeta(const fs::path& dir, const WorldMeta& m) {
    std::ofstream f(dir / "level.dat");
    f.precision(17);
    f << "name=" << m.name << "\nseed=" << m.seed << "\nlastPlayed=" << m.lastPlayed << "\nspawned=" << m.spawned
      << "\nx=" << m.pos.x << "\ny=" << m.pos.y << "\nz=" << m.pos.z << "\nyaw=" << m.yaw << "\npitch=" << m.pitch
      << "\nflying=" << m.flying << "\ntime=" << m.time << "\nslot=" << m.slot << "\nmode=" << m.mode
      << "\nhealth=" << m.health << "\nhunger=" << m.hunger << "\nsaturation=" << m.saturation << "\nexhaustion=" << m.exhaustion
      << "\nspawnX=" << m.spawn.x << "\nspawnY=" << m.spawn.y << "\nspawnZ=" << m.spawn.z;
    for (const auto& r : kGameRules) f << "\nrule." << r.name << '=' << m.rules.*r.value;
    f << "\nitems=";
    for (const auto& s : m.inv.slots) f << (int)s.id << ':' << (int)s.count << ' ';
    f << "\n";
}

// ---------------------------------------------------------------- lifecycle

Game::Game() {
    char* bp = SDL_GetBasePath();
    base = bp ? fs::path(bp) : fs::current_path();
    SDL_free(bp);
    savesDir = base / "saves";
    fs::create_directories(savesDir);
    loadOptions();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) throw std::runtime_error(SDL_GetError());
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();
    window = SDL_CreateWindow("Ordis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) throw std::runtime_error(SDL_GetError());

    if (!ui.font.load((base / "assets/fonts/DejaVuSans-Bold.ttf").string(), 40)) throw std::runtime_error("Failed to load font");
    vk.vsync = opts.vsync;
    vk.init(window);
    renderer.init(vk, base.string() + "/", ui.font);
    if (!vk.rtSupported) opts.lighting = LightClassic;
    if (isDlss(opts.aa) && !renderer.dlssAvailable()) opts.aa = AaTaa;
    opts.frameGen = opts.frameGen && renderer.frameGenAvailable();

    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 4096) == 0) {
        for (const char* t : kTracks)
            if (Mix_Music* m = Mix_LoadMUS((base / "assets/music" / (std::string(t) + ".wav")).string().c_str())) tracks.push_back(m);
        Mix_VolumeMusic((int)(opts.music * MIX_MAX_VOLUME));
    }
}

Game::~Game() {
    closeWorld();
    saveOptions();
    renderer.destroy();
    vk.destroy();
    Mix_HaltMusic();
    for (auto t : tracks) Mix_FreeMusic(t);
    Mix_CloseAudio();
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

void Game::setScreen(Screen s) {
    screen = s;
    SDL_SetRelativeMouseMode(s == Screen::Playing ? SDL_TRUE : SDL_FALSE);
    if (s == Screen::CreateWorld || s == Screen::Chat) SDL_StartTextInput();
    else SDL_StopTextInput();
    mouseLeft = mouseRight = false;
}

void Game::refreshWorldList() {
    worldList.clear();
    for (auto& e : fs::directory_iterator(savesDir))
        if (e.is_directory() && fs::exists(e.path() / "level.dat")) worldList.push_back(readMeta(e.path()));
    std::sort(worldList.begin(), worldList.end(), [](auto& a, auto& b) { return a.lastPlayed > b.lastPlayed; });
    selectedWorld = worldList.empty() ? -1 : 0;
    worldScroll = 0;
}

void Game::createWorld() {
    WorldMeta m;
    m.name = newName.empty() ? "New World" : newName;
    m.mode = newMode;
    if (m.mode == Creative) {
        const uint8_t hotbar[9] = {Grass, Dirt, Stone, Sand, Planks, Log, Glass, Water, WhiteLight};
        for (int i = 0; i < 9; ++i) m.inv.slots[i] = {hotbar[i], 1};
    }

    // Numeric seeds are used as-is, text seeds are hashed, blank means random.
    std::string s = newSeed;
    s.erase(0, s.find_first_not_of(' '));
    if (s.empty()) {
        m.seed = rng();
    } else {
        char* end = nullptr;
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (*end == '\0') {
            m.seed = (uint64_t)v;
        } else {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
            m.seed = h;
        }
    }

    std::string folder;
    for (char c : m.name) folder += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (folder.find_first_not_of(' ') == std::string::npos) folder = "World";
    std::string unique = folder;
    for (int i = 2; fs::exists(savesDir / unique); ++i) unique = folder + " (" + std::to_string(i) + ")";
    m.folder = unique;
    fs::create_directories(savesDir / unique);
    m.lastPlayed = std::time(nullptr);
    writeMeta(savesDir / unique, m);
    openWorld(m);
}

void Game::openWorld(const WorldMeta& m) {
    renderer.clearWorld();
    meta = m;
    world = std::make_unique<World>(savesDir / meta.folder, meta.seed);
    player = Player{};
    player.pos = meta.pos;
    player.yaw = meta.yaw;
    player.pitch = meta.pitch;
    player.flying = meta.flying && meta.mode == Creative;
    if (meta.health <= 0) {  // the game was closed on the death screen
        meta.health = meta.hunger = 20;
        if (meta.spawn.y >= 0) player.pos = meta.spawn;
    }
    fallStart = player.pos.y;
    breakProgress = 0;
    particles.clear();
    clouds = std::make_unique<Clouds>(meta.seed);
    chat.clear();
    held = {};
    loadingStart = now;
    autosaveTimer = 0;
    setScreen(Screen::Loading);
}

void Game::saveWorld() {
    if (!world) return;
    world->saveChunks();
    meta.pos = player.pos;
    meta.yaw = player.yaw;
    meta.pitch = player.pitch;
    meta.flying = player.flying;
    meta.lastPlayed = std::time(nullptr);
    writeMeta(world->directory(), meta);
}

void Game::closeWorld() {
    if (!world) return;
    saveWorld();
    world.reset();
    renderer.clearWorld();
}

// ---------------------------------------------------------------- main loop

void Game::run() {
    auto start = std::chrono::steady_clock::now();
    double last = 0;
    while (running) {
        now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        double dt = std::min(now - last, 0.1);
        last = now;
        ++frames;
        if (now - fpsTimer >= 1.0) { fps = frames; frames = 0; fpsTimer = now; }

        UiInput in;
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e, in);
        int ww, wh, dw, dh;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
        int mx, my;
        uint32_t buttons = SDL_GetMouseState(&mx, &my);
        in.mx = mx * (float)dw / std::max(ww, 1);
        in.my = my * (float)dh / std::max(wh, 1);
        in.down = buttons & SDL_BUTTON_LMASK;
        ui.begin(dw, dh, in, now);

        if (world) updateGame(dt);
        updateMusic();

        switch (screen) {
        case Screen::Title: drawTitle(); break;
        case Screen::Worlds: drawWorlds(); break;
        case Screen::CreateWorld: drawCreateWorld(); break;
        case Screen::ConfirmDelete: drawConfirmDelete(); break;
        case Screen::Options:
        case Screen::RayTracing:
            if (optionsReturn == Screen::Paused) { drawHud(); ui.fill(0, 0, ui.width, ui.height, rgba(10, 10, 20, 150)); }
            if (screen == Screen::Options) drawOptions();
            else drawRayTracing();
            break;
        case Screen::Loading: drawLoading(); break;
        case Screen::Playing: drawHud(); break;
        case Screen::Paused: drawHud(); drawPause(); break;
        case Screen::Inventory: drawHud(); drawInventory(); break;
        case Screen::Dead: drawHud(); drawDeath(); break;
        case Screen::Chat: drawHud(); drawChat(true); break;
        }

        SceneParams sp;
        sp.inWorld = world && screen != Screen::Loading && (screen == Screen::Playing || screen == Screen::Paused ||
                                                             screen == Screen::Inventory || screen == Screen::Dead || screen == Screen::Chat ||
                                                             optionsReturn == Screen::Paused);
        if (sp.inWorld) {
            sp.camPos = player.eye();
            sp.yaw = player.yaw;
            sp.pitch = player.pitch;
            sp.fov = opts.fov;
            sp.timeOfDay = (float)std::fmod(meta.time, 1.0);
            sp.seconds = (float)std::fmod(now, 3600.0);
            sp.mode = opts.lighting;
            sp.renderDist = opts.renderDist;
            sp.underwater = world->get((int)std::floor(sp.camPos.x), (int)std::floor(sp.camPos.y), (int)std::floor(sp.camPos.z)) == Water;
            sp.resetAccumulation = worldEdited;
            sp.particles = &particles.mesh(sp.camPos, sp.yaw, sp.pitch);
            sp.clouds = &clouds->mesh(sp.camPos, now, std::min(opts.renderDist * 32.0, 384.0));
            RayHit hit;
            if (screen == Screen::Playing && raycast(*world, player.eye(), glm::dvec3(player.look()), reach(), hit)) {
                sp.hasSelection = true;
                selectionBox(*world, hit.block, sp.selLo, sp.selHi);
            }
        }
        sp.renderScale = opts.renderScale;
        sp.brightness = opts.brightness;
        sp.aa = opts.aa;
        sp.frameGen = opts.frameGen;
        sp.rt = opts.rt;
        vk.vsync = opts.vsync;
        worldEdited = false;
        renderer.render(sp, ui.batch);
    }
}

void Game::handleEvent(const SDL_Event& e, UiInput& in) {
    switch (e.type) {
    case SDL_QUIT: running = false; break;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) in.pressed = true;
        if (e.button.button == SDL_BUTTON_RIGHT) in.rightPressed = true;
        if (screen == Screen::Playing) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                mouseLeft = true;
                breakProgress = 0;
                if (!survival()) { interact(true); breakTimer = 0.25; }
            }
            if (e.button.button == SDL_BUTTON_RIGHT) { mouseRight = true; interact(false); placeTimer = 0.25; }
            if (e.button.button == SDL_BUTTON_MIDDLE) {
                RayHit hit;
                if (raycast(*world, player.eye(), glm::dvec3(player.look()), reach(), hit)) {
                    uint8_t b = itemOf(world->get(hit.block.x, hit.block.y, hit.block.z));
                    if (!survival()) meta.inv.slots[meta.slot] = {b, 1};
                    for (int i = 0; i < 9 && survival(); ++i)
                        if (meta.inv.slots[i].id == b) { meta.slot = i; break; }
                    slotChangedAt = now;
                }
            }
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) { in.released = true; mouseLeft = false; }
        if (e.button.button == SDL_BUTTON_RIGHT) mouseRight = false;
        break;
    case SDL_MOUSEWHEEL:
        in.wheel += e.wheel.preciseY;
        if (screen == Screen::Playing && e.wheel.y != 0) {
            meta.slot = ((meta.slot - (e.wheel.y > 0 ? 1 : -1)) % 9 + 9) % 9;
            slotChangedAt = now;
        }
        break;
    case SDL_MOUSEMOTION:
        if (screen == Screen::Playing) {
            float s = 0.0004f + opts.sensitivity * 0.004f;
            player.yaw += e.motion.xrel * s;
            player.pitch = std::clamp(player.pitch - e.motion.yrel * s, -1.5605f, 1.5605f);
        }
        break;
    case SDL_TEXTINPUT:
        if (swallowText && now - chatOpenedAt < 0.1) { swallowText = false; break; }  // the key that opened the chat
        in.text += e.text.text;
        break;
    case SDL_KEYDOWN: {
        SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_BACKSPACE) in.backspace = true;
        if (k == SDLK_TAB) in.tab = true;
        if (k == SDLK_F11 && !e.key.repeat) {
            bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        if (e.key.repeat) break;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) in.enter = true;
        if (k == SDLK_ESCAPE) in.escape = true;
        if (screen == Screen::Playing) {
            if (k == SDLK_ESCAPE) setScreen(Screen::Paused);
            else if (k == SDLK_e) setScreen(Screen::Inventory);
            else if (k == SDLK_t) openChat("");
            else if (k == SDLK_SLASH) openChat("/");
            else if (k == SDLK_F3) showDebug = !showDebug;
            else if (k == SDLK_F4 && vk.rtSupported) opts.lighting = (opts.lighting + 1) % LightModeCount;
            else if (k >= SDLK_1 && k <= SDLK_9) { meta.slot = k - SDLK_1; slotChangedAt = now; }
            else if (k == SDLK_SPACE && !survival()) {
                if (now - lastSpace < 0.3) { player.flying = !player.flying; player.vel.y = 0; lastSpace = -1; }
                else lastSpace = now;
            }
        } else if (screen == Screen::Inventory && (k == SDLK_e || k == SDLK_ESCAPE)) {
            closeInventory();
        } else if (screen == Screen::Chat) {
            if (k == SDLK_ESCAPE) setScreen(Screen::Playing);
            if ((k == SDLK_UP || k == SDLK_DOWN) && !sent.empty()) {
                sentPos = k == SDLK_UP ? (sentPos > 0 ? sentPos - 1 : 0) : std::min(sentPos + 1, sent.size());
                chatInput = sentPos < sent.size() ? sent[sentPos] : "";
            }
        } else if (screen == Screen::Paused && k == SDLK_ESCAPE) {
            setScreen(Screen::Playing);
        }
        break;
    }
    }
}

void Game::interact(bool destroy) {
    ItemStack& item = meta.inv.slots[meta.slot];
    if (!destroy && survival() && item.id == ItemFood && !item.empty()) {
        if (meta.hunger >= 20) return;
        meta.hunger = std::min(20, meta.hunger + 4);
        meta.saturation = std::min((float)meta.hunger, meta.saturation + 2.4f);
        if (--item.count == 0) item = {};
        return;
    }
    RayHit hit;
    if (!raycast(*world, player.eye(), glm::dvec3(player.look()), reach(), hit)) return;
    if (destroy) {
        breakBlock(hit.block);
        return;
    }
    if (item.empty() || !isBlockItem(item.id)) return;

    // Clicking the open face of a panel with a panel of the same material makes it an eighth thicker, and the
    // open side of a slab with the same slab fills the block in.
    glm::ivec3 p = hit.block;
    uint8_t target = world->get(p.x, p.y, p.z), b = Air;
    if (isPanel(target) && isPanel(item.id) && panelMaterial(target) == panelMaterial(item.id) && hit.normal.y == (panelTop(target) ? -1 : 1))
        b = panelId(panelMaterial(target), panelEighths(target) + 1, panelTop(target));
    if (isShaped(target) && isShaped(item.id) && shapeMaterial(target) == shapeMaterial(item.id) &&
        shapeOf(shapeState(target)) == shapeOf(shapeState(item.id)) && shapeOf(shapeState(target)) <= ShapeVSlab) {
        static const uint8_t halves[3][2] = {{0xAA, 0x55}, {0xF0, 0x0F}, {0xCC, 0x33}};  // +/- half per axis
        int axis = hit.normal.x ? 0 : hit.normal.y ? 1 : 2;
        if ((uint8_t)~blockInfo(target).mask == halves[axis][hit.normal[axis] < 0]) b = shapeMaterial(target);
    }
    if (b == Air) {
        p += hit.normal;
        b = orient(item.id, hit);
        uint8_t existing = world->get(p.x, p.y, p.z);
        bool fills = isShaped(existing) && isShaped(b) && shapeMaterial(existing) == shapeMaterial(b) &&
                     (blockInfo(existing).mask ^ blockInfo(b).mask) == 0xFF;
        if (fills) b = shapeMaterial(b);
        else if (isPanel(existing) && isPanel(b) && panelMaterial(existing) == panelMaterial(b))
            b = panelId(panelMaterial(b), panelEighths(existing) + 1, panelTop(existing));
        else if (existing != Air && !isLiquid(existing)) return;
    }
    if (p.y < 0 || p.y >= CH) return;
    if (isFlower(b) && !(world->get(p.x, p.y - 1, p.z) == Grass || world->get(p.x, p.y - 1, p.z) == Dirt)) return;
    if (blockInfo(b).solid && player.intersects(*world, p, b)) return;
    world->set(p.x, p.y, p.z, b);
    worldEdited = true;
    if (survival() && --item.count == 0) item = {};
}

// Turns a shaped item into the oriented block placed against the hit face: slabs and stairs go upside down
// on the top half of a face, stairs climb away from the player, vertical slabs sit against the clicked face
// and sideways stairs open towards the player.
uint8_t Game::orient(uint8_t item, const RayHit& hit) const {
    double fy = hit.point.y - std::floor(hit.point.y);
    bool upper = hit.normal.y < 0 || (hit.normal.y == 0 && fy > 0.5);
    if (isPanel(item)) return panelId(panelMaterial(item), 1, upper);
    if (!isShaped(item)) return item;
    glm::vec3 look = player.look();
    int facing = std::abs(look.x) > std::abs(look.z) ? (look.x > 0 ? 0 : 1) : (look.z > 0 ? 2 : 3);
    int against = hit.normal.x ? (hit.normal.x > 0 ? 1 : 0) : (hit.normal.z > 0 ? 3 : 2);
    int state = 0;
    switch (shapeOf(shapeState(item))) {
    case ShapeSlab: state = upper; break;
    case ShapeVSlab: state = 2 + (hit.normal.y ? facing : against); break;
    case ShapeStairs: state = 6 + facing + 4 * upper; break;
    case ShapeSideStairs: state = 14 + (look.x < 0) + 2 * (look.z < 0); break;
    }
    return shapedId(shapeMaterial(item), state);
}

void Game::breakBlock(glm::ivec3 p) {
    uint8_t b = world->get(p.x, p.y, p.z), above = world->get(p.x, p.y + 1, p.z);
    world->set(p.x, p.y, p.z, Air);
    particles.burst(*world, p, b);
    if (isFlower(above)) {  // lost its footing
        world->set(p.x, p.y + 1, p.z, Air);
        particles.burst(*world, p + glm::ivec3(0, 1, 0), above);
    }
    worldEdited = true;
    if (!survival() || !meta.rules.doTileDrops) return;
    meta.inv.add(b == Grass ? (uint8_t)Dirt : itemOf(b), isPanel(b) ? panelEighths(b) : 1);  // whatever doesn't fit is lost
    if (isFlower(above)) meta.inv.add(above, 1);
    if (b == Leaves && rng() % 6 == 0) meta.inv.add(ItemFood, 1);
    meta.exhaustion += 0.005f;
}

void Game::hurt(int amount) {
    meta.health = std::max(0, meta.health - amount);
    hurtAt = now;
    if (meta.health > 0) return;
    if (!meta.rules.keepInventory) meta.inv = {};
    setScreen(Screen::Dead);
}

void Game::respawn() {
    player.pos = meta.spawn;
    player.vel = glm::dvec3(0);
    fallStart = player.pos.y;
    meta.health = meta.hunger = 20;
    meta.saturation = 5;
    meta.exhaustion = 0;
    loadingStart = now;
    setScreen(Screen::Loading);  // wait for the spawn chunks before dropping the player in
}

void Game::closeInventory() {
    if (!held.empty()) meta.inv.add(held.id, held.count);
    held = {};
    setScreen(Screen::Playing);
}

// Hunger, regeneration, fall and lava damage, following the original game's rules.
void Game::updateSurvival(double dt, glm::dvec3 prev) {
    double moved = glm::length(glm::dvec2(player.pos.x - prev.x, player.pos.z - prev.z));
    meta.exhaustion += (float)(moved * (player.inWater ? 0.01 : player.sprinting ? 0.1 : 0.0));
    if (player.jumped) meta.exhaustion += player.sprinting ? 0.2f : 0.05f;
    while (meta.exhaustion >= 4) {
        meta.exhaustion -= 4;
        if (meta.saturation > 0) meta.saturation = std::max(0.0f, meta.saturation - 1);
        else meta.hunger = std::max(0, meta.hunger - 1);
    }
    player.canSprint = meta.hunger > 6;

    if (meta.hunger >= 18 && meta.health < 20 && meta.rules.naturalRegeneration) {
        if ((regenTimer += dt) >= 4) { regenTimer = 0; ++meta.health; meta.exhaustion += 6; }
    } else if (meta.hunger == 0 && meta.health > 1) {
        if ((regenTimer += dt) >= 4) { regenTimer = 0; hurt(1); }
    } else {
        regenTimer = 0;
    }

    if (player.inWater) {
        fallStart = player.pos.y;
    } else if (player.onGround) {
        double fall = fallStart - player.pos.y - 3;
        if (fall > 0 && meta.rules.fallDamage) hurt((int)std::ceil(fall));
        fallStart = player.pos.y;
    } else {
        fallStart = std::max(fallStart, player.pos.y);
    }

    if (player.inLava && meta.rules.fireDamage && (lavaTimer -= dt) <= 0) { hurt(4); lavaTimer = 0.5; }
    if (!player.inLava) lavaTimer = 0;
}

void Game::updateGame(double dt) {
    int pcx = (int)std::floor(player.pos.x / CW), pcz = (int)std::floor(player.pos.z / CW);
    world->update(pcx, pcz, opts.renderDist);
    for (auto& m : world->takeMeshes()) renderer.queueMesh(std::move(m));

    if (screen == Screen::Loading) {
        bool ready = true;
        int r = std::min(2, opts.renderDist);
        for (int dz = -r; dz <= r && ready; ++dz)
            for (int dx = -r; dx <= r && ready; ++dx) ready = world->isMeshed(pcx + dx, pcz + dz);
        if (ready || now - loadingStart > 30) {
            if (!meta.spawned) {
                // Prefer dry land near the origin.
                glm::ivec2 best((int)std::floor(player.pos.x), (int)std::floor(player.pos.z));
                for (int z = -24; z <= 24; z += 4)
                    for (int x = -24; x <= 24; x += 4) {
                        int y = world->topBlock(best.x + x, best.y + z);
                        if (y > 0 && world->get(best.x + x, y, best.y + z) != Water) {
                            best += glm::ivec2(x, z);
                            goto found;
                        }
                    }
            found:
                player.pos = glm::dvec3(best.x + 0.5, world->topBlock(best.x, best.y) + 1.0, best.y + 0.5);
                meta.spawned = true;
            }
            if (meta.spawn.y < 0) meta.spawn = player.pos;
            fallStart = player.pos.y;
            setScreen(Screen::Playing);
        }
        return;
    }

    if (screen == Screen::Playing) {
        if (world->isLoaded((int)std::floor(player.pos.x), (int)std::floor(player.pos.z))) {
            glm::dvec3 prev = player.pos;
            player.update(*world, SDL_GetKeyboardState(nullptr), dt);
            if (survival()) updateSurvival(dt, prev);
        }
        if (survival()) {
            // Blocks take their hardness in seconds to break, with a short pause between blocks.
            RayHit hit;
            breakTimer -= dt;
            if (mouseLeft && breakTimer <= 0 && raycast(*world, player.eye(), glm::dvec3(player.look()), reach(), hit)) {
                if (hit.block != breakTarget) { breakTarget = hit.block; breakProgress = 0; }
                uint8_t target = world->get(hit.block.x, hit.block.y, hit.block.z);
                float hardness = blockInfo(target).hardness;
                if (hardness >= 0 && (chipTimer -= dt) <= 0) { particles.chip(*world, hit, target); chipTimer = 0.06; }
                if (hardness >= 0 && (breakProgress += dt / std::max(hardness, 0.05f)) >= 1) {
                    breakBlock(hit.block);
                    breakProgress = 0;
                    breakTimer = 0.2;
                }
            } else if (!mouseLeft) {
                breakProgress = 0;
            }
        } else if (mouseLeft && (breakTimer -= dt) <= 0) {
            interact(true);
            breakTimer = 0.2;
        }
        if (mouseRight && (placeTimer -= dt) <= 0) { interact(false); placeTimer = 0.2; }
    }
    if (screen != Screen::Paused && !(screen == Screen::Options && optionsReturn == Screen::Paused)) particles.update(*world, dt);
    if (meta.rules.doDaylightCycle) meta.time += dt / 1200.0;  // 20 minute day
    if ((autosaveTimer += dt) > 60) { saveWorld(); autosaveTimer = 0; }
}

void Game::updateMusic() {
    if (tracks.empty()) return;
    Mix_VolumeMusic((int)(opts.music * MIX_MAX_VOLUME));
    if (musicPlaying && !Mix_PlayingMusic()) {
        musicPlaying = false;
        nextTrackAt = now + std::uniform_real_distribution<double>(20, 90)(rng);
    }
    if (!musicPlaying && now >= nextTrackAt && opts.music > 0) {
        int t;
        do t = (int)(rng() % tracks.size());
        while (tracks.size() > 1 && t == lastTrack);
        lastTrack = t;
        Mix_PlayMusic(tracks[t], 0);
        musicPlaying = true;
    }
}

// ---------------------------------------------------------------- screens

void Game::drawTitle() {
    ui.tiledBackground(2, 0.3f);
    float tw = std::min(ui.width * 0.8f, 360.0f), th = tw * 530.0f / 2014.0f;
    ui.image(TexTitle, (ui.width - tw) / 2, ui.height * 0.1f, tw, th);
    float splash = 1.0f + 0.05f * (float)std::sin(ui.time * 6);
    ui.textCentered("Infinite blocks, infinite light!", ui.width / 2, ui.height * 0.1f + th + 6, rgba(255, 255, 85), 8 * splash);

    float bw = 200, bh = 20, x = (ui.width - bw) / 2, y = ui.height * 0.52f;
    if (ui.button("Singleplayer", x, y, bw, bh)) { refreshWorldList(); setScreen(Screen::Worlds); }
    if (ui.button("Options...", x, y + 26, bw, bh)) { optionsReturn = Screen::Title; setScreen(Screen::Options); }
    if (ui.button("Quit Game", x, y + 52, bw, bh)) running = false;
    ui.text("Ordis 1.0", 3, ui.height - 11, rgba(200, 200, 200));
    std::string gpu = std::string(vk.props.deviceName) + (vk.rtSupported ? "  |  RT ready" : "");
    ui.text(gpu, ui.width - ui.textWidth(gpu) - 3, ui.height - 11, rgba(200, 200, 200));
}

void Game::drawWorlds() {
    ui.tiledBackground(2, 0.3f);
    ui.textCentered("Select World", ui.width / 2, 10);

    float lw = std::min(320.0f, ui.width - 20), lx = (ui.width - lw) / 2, ly = 26, lh = ui.height - ly - 62;
    ui.nineSlice(TexPanel, lx - 4, ly - 4, lw + 8, lh + 8, 15, 3, rgba(255, 255, 255, 200));
    const float rowH = 34;
    float maxScroll = std::max(0.0f, worldList.size() * rowH - lh);
    if (ui.hovered(lx, ly, lw, lh)) worldScroll = std::clamp(worldScroll - ui.in.wheel * 20, 0.0f, maxScroll);

    for (size_t i = 0; i < worldList.size(); ++i) {
        float y = ly + i * rowH - worldScroll;
        if (y + rowH < ly || y > ly + lh - rowH + 1) continue;  // keep rows inside the panel
        const WorldMeta& m = worldList[i];
        bool hover = ui.hovered(lx, y, lw, rowH - 2);
        if ((int)i == selectedWorld) ui.nineSlice(TexSliderBg, lx, y, lw, rowH - 2, 16, 2, WHITE);
        else if (hover) ui.fill(lx, y, lw, rowH - 2, rgba(255, 255, 255, 25));
        ui.blockIcon(Grass, lx + 16, y + (rowH - 2) / 2, 22);
        ui.text(m.name, lx + 32, y + 5);
        char date[64] = "never";
        if (m.lastPlayed) {
            std::time_t t = (std::time_t)m.lastPlayed;
            std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", std::localtime(&t));
        }
        ui.text(m.folder + "  (" + date + ")  " + (m.mode == Survival ? "Survival" : "Creative"), lx + 32, y + 15, rgba(160, 165, 180), 7);
        ui.text("Seed: " + std::to_string((int64_t)m.seed), lx + 32, y + 24, rgba(130, 135, 150), 6);
        if (hover && ui.in.pressed) {
            if ((int)i == selectedWorld && now - lastClickTime < 0.35) { openWorld(m); return; }
            selectedWorld = (int)i;
            lastClickTime = now;
        }
    }
    if (worldList.empty()) ui.textCentered("No worlds yet - create one!", ui.width / 2, ly + lh / 2 - 4, rgba(170, 170, 180));

    bool has = selectedWorld >= 0 && selectedWorld < (int)worldList.size();
    float bw = 152, gap = 6, x0 = ui.width / 2 - bw - gap / 2, x1 = ui.width / 2 + gap / 2, y0 = ui.height - 52;
    if (ui.button("Play Selected World", x0, y0, bw, 20, has) || (has && ui.in.enter)) { openWorld(worldList[selectedWorld]); return; }
    if (ui.button("Create New World", x1, y0, bw, 20)) {
        newName = "New World";
        newSeed.clear();
        nameFocus = true;
        seedFocus = false;
        newMode = Survival;
        setScreen(Screen::CreateWorld);
    }
    if (ui.button("Delete", x0, y0 + 26, bw, 20, has)) setScreen(Screen::ConfirmDelete);
    if (ui.button("Back", x1, y0 + 26, bw, 20) || ui.in.escape) setScreen(Screen::Title);
}

void Game::drawCreateWorld() {
    ui.tiledBackground(2, 0.3f);
    ui.textCentered("Create New World", ui.width / 2, 20);
    float w = 200, x = (ui.width - w) / 2, y = ui.height * 0.25f;
    ui.text("World Name", x, y, rgba(170, 170, 185));
    ui.textField(newName, x, y + 11, w, 20, nameFocus, 32);
    ui.text("Seed for the world generator", x, y + 44, rgba(170, 170, 185));
    ui.textField(newSeed, x, y + 55, w, 20, seedFocus, 32, "Leave blank for a random seed");
    if (ui.in.tab) std::swap(nameFocus, seedFocus);
    if (ui.button("Random Seed", x, y + 80, w, 20)) newSeed = std::to_string((int64_t)rng());
    if (ui.button(newMode == Survival ? "Game Mode: Survival" : "Game Mode: Creative", x, y + 106, w, 20))
        newMode = newMode == Survival ? Creative : Survival;
    ui.text(newMode == Survival ? "Gather blocks, craft, and keep yourself fed." : "Fly, build and break freely with every block.",
            x, y + 130, rgba(140, 145, 160), 7);

    float bw = 97, by = ui.height - 40;
    if (ui.button("Create World", x, by, bw, 20) || ui.in.enter) { createWorld(); return; }
    if (ui.button("Cancel", x + w - bw, by, bw, 20) || ui.in.escape) setScreen(Screen::Worlds);
}

void Game::drawConfirmDelete() {
    ui.tiledBackground(2, 0.2f);
    const std::string& name = worldList[selectedWorld].name;
    ui.textCentered("Are you sure you want to delete this world?", ui.width / 2, ui.height * 0.3f);
    ui.textCentered("'" + name + "' will be lost forever! (A long time!)", ui.width / 2, ui.height * 0.3f + 16, rgba(170, 170, 185));
    float bw = 120, y = ui.height * 0.55f;
    if (ui.button("Delete", ui.width / 2 - bw - 3, y, bw, 20)) {
        std::error_code ec;
        fs::remove_all(savesDir / worldList[selectedWorld].folder, ec);
        refreshWorldList();
        setScreen(Screen::Worlds);
    }
    if (ui.button("Cancel", ui.width / 2 + 3, y, bw, 20) || ui.in.escape) setScreen(Screen::Worlds);
}

void Game::drawOptions() {
    if (optionsReturn == Screen::Title) ui.tiledBackground(2, 0.3f);
    ui.textCentered("Options", ui.width / 2, 16);
    float w = 150, gap = 8, x0 = ui.width / 2 - w - gap / 2, x1 = ui.width / 2 + gap / 2, y = ui.height * 0.18f;

    std::string light = std::string("Lighting: ") + kLightingNames[opts.lighting];
    if (ui.button(vk.rtSupported ? light : "Lighting: Classic (no RT GPU)", x0, y, w, 20, vk.rtSupported))
        opts.lighting = (opts.lighting + 1) % LightModeCount;
    if (ui.button("Ray Tracing Settings...", x1, y, w, 20, vk.rtSupported)) setScreen(Screen::RayTracing);
    const char* desc[LightModeCount] = {"Smooth flood-fill lighting, like the classic game.",
                                        "Hardware ray traced shadows, block lights and water reflections.",
                                        "Path traced global illumination, accumulated over frames."};
    ui.textCentered(desc[opts.lighting], ui.width / 2, y + 24, rgba(160, 165, 180), 7);

    y += 40;
    if (ui.button(std::string("Anti-aliasing: ") + kAaNames[opts.aa], x0, y, w * 2 + gap, 20))
        do opts.aa = (opts.aa + 1) % AaCount; while (isDlss(opts.aa) && !renderer.dlssAvailable());
    const char* aaDesc[AaCount] = {"No anti-aliasing.", "Fast edge smoothing as a post effect.",
                                   "Temporal anti-aliasing: smooth edges and less ray tracing noise, slightly soft.",
                                   "NVIDIA DLAA: DLSS anti-aliasing at full resolution.", "NVIDIA DLSS upscaling from 67% resolution.",
                                   "NVIDIA DLSS upscaling from 58% resolution.", "NVIDIA DLSS upscaling from 50% resolution.",
                                   "NVIDIA DLSS upscaling from 33% resolution."};
    ui.textCentered(renderer.dlssAvailable() || opts.aa != AaTaa ? aaDesc[opts.aa] : "Temporal anti-aliasing. DLSS needs an NVIDIA RTX GPU.",
                    ui.width / 2, y + 24, rgba(160, 165, 180), 7);

    y += 40;
    float t = (opts.renderDist - 2) / 22.0f;
    if (ui.slider("Render Distance: " + std::to_string(opts.renderDist) + " chunks", x0, y, w, 20, t, &opts.renderDist))
        opts.renderDist = 2 + (int)std::round(t * 22);
    t = (opts.fov - 50) / 60.0f;
    if (ui.slider("FOV: " + std::to_string((int)opts.fov), x1, y, w, 20, t, &opts.fov)) opts.fov = std::round(50 + t * 60);

    y += 26;
    t = (opts.renderScale - 0.25f) / 0.75f;
    if (isDlss(opts.aa)) ui.button("Render Scale: set by DLSS", x0, y, w, 20, false);
    else if (ui.slider("Render Scale: " + std::to_string((int)std::round(opts.renderScale * 100)) + "%", x0, y, w, 20, t, &opts.renderScale))
        opts.renderScale = std::round((0.25f + t * 0.75f) * 20) / 20;
    if (ui.button(opts.vsync ? "VSync: ON" : "VSync: OFF", x1, y, w, 20)) opts.vsync = !opts.vsync;

    y += 26;
    if (ui.button(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP ? "Fullscreen: ON" : "Fullscreen: OFF", x0, y, w, 20)) {
        bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    t = opts.sensitivity;
    if (ui.slider("Sensitivity: " + std::to_string((int)std::round(opts.sensitivity * 200)) + "%", x1, y, w, 20, t, &opts.sensitivity))
        opts.sensitivity = t;

    y += 26;
    t = opts.music;
    if (ui.slider("Music: " + (opts.music > 0 ? std::to_string((int)std::round(opts.music * 100)) + "%" : std::string("OFF")), x0, y, w, 20, t, &opts.music))
        opts.music = t;
    t = opts.brightness;
    int pct = (int)std::round(opts.brightness * 100);
    if (ui.slider("Brightness: " + (pct == 0 ? std::string("Moody") : pct == 100 ? std::string("Bright") : std::to_string(pct) + "%"),
                  x1, y, w, 20, t, &opts.brightness))
        opts.brightness = std::round(t * 20) / 20;

    y += 26;
    if (ui.button(!renderer.frameGenAvailable() ? "Frame Generation: Unavailable" : opts.frameGen ? "Frame Generation: DLSS 2x" : "Frame Generation: OFF",
                  x0, y, w * 2 + gap, 20, renderer.frameGenAvailable()))
        opts.frameGen = !opts.frameGen;
    ui.textCentered(renderer.frameGenAvailable() ? "Shows an AI-generated frame between each rendered frame. Smoother, slightly more latency."
                                                 : "DLSS Frame Generation needs an NVIDIA RTX 40 series GPU or newer.",
                    ui.width / 2, y + 24, rgba(160, 165, 180), 7);

    if (ui.button("Done", (ui.width - 200) / 2, ui.height - 36, 200, 20) || ui.in.escape) {
        saveOptions();
        setScreen(optionsReturn);
    }
}

void Game::drawRayTracing() {
    if (optionsReturn == Screen::Title) ui.tiledBackground(2, 0.3f);
    ui.textCentered("Ray Tracing Settings", ui.width / 2, 16);
    float w = 150, gap = 8, x0 = ui.width / 2 - w - gap / 2, x1 = ui.width / 2 + gap / 2, y = ui.height * 0.18f;
    RtSettings& rt = opts.rt;
    auto pct = [](float v) { return std::to_string((int)std::round(v * 100)) + "%"; };
    const uint32_t dim = rgba(160, 165, 180);
    float t;

    ui.textCentered("Ray Traced", x0 + w / 2, y, opts.lighting == LightRayTraced ? WHITE : dim, 8);
    ui.textCentered("Path Traced", x1 + w / 2, y, opts.lighting == LightPathTraced ? WHITE : dim, 8);
    y += 14;
    t = rt.shadowRays / 32.0f;
    if (ui.slider("Shadowed Lights: " + std::to_string(rt.shadowRays), x0, y, w, 20, t, &rt.shadowRays)) rt.shadowRays = (int)std::round(t * 32);
    t = rt.bounces / 6.0f;
    if (ui.slider("Bounces: " + std::to_string(rt.bounces), x1, y, w, 20, t, &rt.bounces)) rt.bounces = (int)std::round(t * 6);

    y += 26;
    t = rt.rtSunSoftness;
    if (ui.slider("Soft Sun Shadows: " + (rt.rtSunSoftness > 0 ? pct(rt.rtSunSoftness) : std::string("OFF")), x0, y, w, 20, t, &rt.rtSunSoftness))
        rt.rtSunSoftness = std::round(t * 20) / 20;
    t = (rt.samples - 1) / 7.0f;
    if (ui.slider("Samples per Pixel: " + std::to_string(rt.samples), x1, y, w, 20, t, &rt.samples)) rt.samples = 1 + (int)std::round(t * 7);

    y += 26;
    t = rt.ambient / 2;
    if (ui.slider("Sky Ambient: " + pct(rt.ambient), x0, y, w, 20, t, &rt.ambient)) rt.ambient = std::round(t * 40) / 20;
    t = std::log2((float)rt.history) / 8;
    if (ui.slider("Accumulation: " + std::to_string(rt.history) + " frames", x1, y, w, 20, t, &rt.history))
        rt.history = (int)std::round(std::exp2(t * 8));

    y += 26;
    if (ui.button(rt.reflections ? "Water Reflections: ON" : "Water Reflections: OFF", x0, y, w, 20)) rt.reflections = !rt.reflections;
    t = rt.ptSunSoftness;
    if (ui.slider("Soft Sun Shadows: " + (rt.ptSunSoftness > 0 ? pct(rt.ptSunSoftness) : std::string("OFF")), x1, y, w, 20, t, &rt.ptSunSoftness))
        rt.ptSunSoftness = std::round(t * 20) / 20;

    y += 34;
    t = (rt.lightStrength - 0.25f) / 2.75f;
    if (ui.slider("Block Light Strength: " + pct(rt.lightStrength), x0, y, w, 20, t, &rt.lightStrength))
        rt.lightStrength = std::round((0.25f + t * 2.75f) * 20) / 20;
    if (ui.button("Reset to Defaults", x1, y, w, 20, !(rt == RtSettings{}))) rt = {};
    ui.textCentered("Higher bounces, samples and shadowed lights look better but cost frame rate.", ui.width / 2, y + 30, dim, 7);
    if (opts.lighting == LightClassic) ui.textCentered("Classic lighting is selected, so these only apply after switching modes.", ui.width / 2, y + 40, dim, 7);

    if (ui.button("Done", (ui.width - 200) / 2, ui.height - 36, 200, 20) || ui.in.escape) {
        saveOptions();
        setScreen(Screen::Options);
    }
}

void Game::drawLoading() {
    ui.tiledBackground(2, 0.3f);
    ui.textCentered("Generating terrain...", ui.width / 2, ui.height / 2 - 20);
    float w = 160, x = (ui.width - w) / 2, y = ui.height / 2;
    int r = std::min(2, opts.renderDist), total = (2 * r + 1) * (2 * r + 1), done = 0;
    int pcx = (int)std::floor(player.pos.x / CW), pcz = (int)std::floor(player.pos.z / CW);
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) done += world->isMeshed(pcx + dx, pcz + dz);
    ui.fill(x, y, w, 4, rgba(40, 45, 60));
    ui.fill(x, y, w * done / total, 4, rgba(130, 220, 120));
    ui.textCentered(std::to_string(world->loadedChunks()) + " chunks loaded", ui.width / 2, y + 10, rgba(160, 165, 180), 7);
}

void Game::drawHotbar(float y, bool clickable) {
    const float slot = 22, w = slot * 9;
    float x = (ui.width - w) / 2;
    for (int i = 0; i < 9; ++i) {
        float sx = x + i * slot;
        ui.image(TexSlot, sx, y, slot, slot, rgba(255, 255, 255, 220));
        drawStack(meta.inv.slots[i], sx, y, slot);
        if (clickable && ui.hovered(sx, y, slot, slot) && ui.in.pressed) meta.slot = i;
    }
    float sx = x + meta.slot * slot;
    uint32_t c = rgba(255, 255, 255);
    ui.fill(sx - 1, y - 1, slot + 2, 2, c);
    ui.fill(sx - 1, y + slot - 1, slot + 2, 2, c);
    ui.fill(sx - 1, y - 1, 2, slot + 2, c);
    ui.fill(sx + slot - 1, y - 1, 2, slot + 2, c);
}

void Game::drawHud() {
    if (screen == Screen::Playing) {
        float cx = ui.width / 2, cy = ui.height / 2;
        ui.fill(cx - 5, cy - 0.5f, 10, 1, rgba(255, 255, 255, 220));
        ui.fill(cx - 0.5f, cy - 5, 1, 10, rgba(255, 255, 255, 220));
    }
    glm::dvec3 eye = player.eye();
    if (world->get((int)std::floor(eye.x), (int)std::floor(eye.y), (int)std::floor(eye.z)) == Lava)
        ui.fill(0, 0, ui.width, ui.height, rgba(230, 90, 20, 220));
    if (now - hurtAt < 0.4) ui.fill(0, 0, ui.width, ui.height, rgba(200, 20, 20, (uint8_t)(90 * (1 - (now - hurtAt) / 0.4))));
    if (breakProgress > 0 && screen == Screen::Playing) {
        ui.fill(ui.width / 2 - 10, ui.height / 2 + 8, 20, 2, rgba(0, 0, 0, 140));
        ui.fill(ui.width / 2 - 10, ui.height / 2 + 8, 20 * (float)breakProgress, 2, WHITE);
    }

    drawHotbar(ui.height - 26, false);
    if (survival()) drawStatusBars();
    const ItemStack& sel = meta.inv.slots[meta.slot];
    if (now - slotChangedAt < 2.0 && !sel.empty()) {
        uint8_t a = (uint8_t)(255 * std::clamp(2.0 - (now - slotChangedAt), 0.0, 1.0));
        ui.textCentered(itemName(sel.id), ui.width / 2, ui.height - (survival() ? 50 : 40), rgba(255, 255, 255, a));
    }
    if (screen != Screen::Chat) drawChat(false);
    if (player.flying && screen == Screen::Playing) ui.text("Flying", 3, ui.height - 11, rgba(200, 200, 210), 7);

    if (showDebug) {
        char buf[256];
        auto p = player.pos;
        std::vector<std::string> lines;
        lines.push_back("Ordis  " + std::to_string(fps) + " fps" + (renderer.frameGenActive() ? "  (" + std::to_string(fps * 2) + " with frame generation)" : ""));
        std::snprintf(buf, sizeof buf, "XYZ: %.2f / %.2f / %.2f", p.x, p.y, p.z);
        lines.push_back(buf);
        std::snprintf(buf, sizeof buf, "Chunk: %d %d   loaded %zu   meshes %zu", (int)std::floor(p.x / 16), (int)std::floor(p.z / 16),
                      world->loadedChunks(), renderer.gpuChunkCount());
        lines.push_back(buf);
        lines.push_back(survival() ? "Mode: Survival" : "Mode: Creative");
        lines.push_back(std::string("Lighting: ") + kLightingNames[vk.rtSupported ? opts.lighting : 0] + "  (F4 to cycle)");
        VkExtent2D rs = renderer.renderSize();
        std::snprintf(buf, sizeof buf, "AA: %s   render %ux%u -> %ux%u", kAaNames[opts.aa], rs.width, rs.height, vk.swapExtent.width, vk.swapExtent.height);
        lines.push_back(buf);
        int hours = (int)(std::fmod(meta.time + 0.25, 1.0) * 24);
        std::snprintf(buf, sizeof buf, "Time: %02d:00   Seed: %lld", (hours + 1) % 24, (long long)meta.seed);
        lines.push_back(buf);
        lines.push_back(vk.props.deviceName);
        for (size_t i = 0; i < lines.size(); ++i) {
            ui.fill(1, 1 + i * 10, ui.textWidth(lines[i], 7) + 4, 10, rgba(0, 0, 0, 110));
            ui.text(lines[i], 3, 2 + i * 10, WHITE, 7, false);
        }
    }
}

void Game::drawPause() {
    ui.fill(0, 0, ui.width, ui.height, rgba(10, 10, 20, 150));
    ui.textCentered("Game Menu", ui.width / 2, ui.height * 0.22f);
    float bw = 200, x = (ui.width - bw) / 2, y = ui.height * 0.35f;
    if (ui.button("Back to Game", x, y, bw, 20)) setScreen(Screen::Playing);
    if (ui.button("Options...", x, y + 26, bw, 20)) { optionsReturn = Screen::Paused; setScreen(Screen::Options); }
    if (ui.button("Save and Quit to Title", x, y + 52, bw, 20)) {
        closeWorld();
        setScreen(Screen::Title);
    }
}

void Game::drawInventory() {
    if (survival()) { drawSurvivalInventory(); return; }
    ui.fill(0, 0, ui.width, ui.height, rgba(10, 10, 20, 120));
    const int cols = 9;
    const float slot = 22;
    const auto& items = creativeItems();
    int count = (int)items.size();
    int rows = (count + cols - 1) / cols;
    float pw = cols * slot + 16, ph = rows * slot + 36;
    float px = (ui.width - pw) / 2, py = (ui.height - ph) / 2 - 20;
    ui.panel(px, py, pw, ph);
    ui.text("Creative Inventory", px + 8, py + 8, rgba(220, 220, 230));
    ui.text("Click a block to put it in the selected hotbar slot", px + 8, py + ph + 4, rgba(170, 170, 185), 6);

    std::string tooltip;
    for (int i = 0; i < count; ++i) {
        uint8_t b = items[i];
        float sx = px + 8 + (i % cols) * slot, sy = py + 24 + (i / cols) * slot;
        bool hover = ui.hovered(sx, sy, slot, slot);
        ui.image(TexSlot, sx, sy, slot, slot, hover ? WHITE : rgba(220, 220, 230));
        ui.blockIcon(b, sx + slot / 2, sy + slot / 2, 15);
        if (hover) {
            tooltip = blockInfo(b).name;
            if (ui.in.pressed) { meta.inv.slots[meta.slot] = {b, 1}; slotChangedAt = now; }
        }
    }
    drawHotbar(ui.height - 26, true);
    if (!tooltip.empty()) {
        float tw = ui.textWidth(tooltip) + 8;
        ui.nineSlice(TexPanel, ui.in.mx + 8, ui.in.my - 14, tw, 14, 15, 3);
        ui.text(tooltip, ui.in.mx + 12, ui.in.my - 11);
    }
}

void Game::drawStack(const ItemStack& s, float x, float y, float slot) {
    if (s.empty()) return;
    ui.blockIcon(s.id, x + slot / 2, y + slot / 2, 15);
    if (!survival() || s.count < 2) return;
    std::string n = std::to_string(s.count);
    ui.text(n, x + slot - 2 - ui.textWidth(n, 7), y + slot - 9, WHITE, 7);
}

void Game::drawStatusBars() {
    // Ten icons per bar, two points each; hunger fills from the right like the original.
    const float x0 = (ui.width - 198) / 2, y = ui.height - 36;
    const uint32_t dim = rgba(50, 45, 60, 200);
    bool shake = now - hurtAt < 0.4;
    for (int i = 0; i < 10; ++i) {
        float hx = x0 + i * 8, hy = y - (shake && i % 2 ? 1 : 0);
        int hp = meta.health - i * 2;
        ui.image(TexHeart, hx, hy, 7, 7, dim);
        if (hp > 0) ui.image(TexHeart, hx, hy, 7, 7, WHITE, hp >= 2 ? 1 : 0.5f);

        float fx = x0 + 198 - 7 - i * 8;
        int food = meta.hunger - i * 2;
        ui.image(TexHunger, fx, y, 7, 7, dim);
        if (food > 0) ui.image(TexHunger, fx, y, 7, 7, WHITE, food >= 2 ? 1 : 0.5f);
    }
}

void Game::drawDeath() {
    ui.fill(0, 0, ui.width, ui.height, rgba(120, 10, 10, 110));
    ui.textCentered("You died!", ui.width / 2, ui.height * 0.3f, WHITE, 16);
    ui.textCentered(meta.rules.keepInventory ? "Your inventory is safe." : "Your items are lost.", ui.width / 2, ui.height * 0.3f + 22, rgba(220, 200, 200), 7);
    float bw = 200, x = (ui.width - bw) / 2, y = ui.height * 0.5f;
    if (ui.button("Respawn", x, y, bw, 20)) respawn();
    if (ui.button("Save and Quit to Title", x, y + 26, bw, 20)) {
        closeWorld();  // openWorld revives a player saved while dead
        setScreen(Screen::Title);
    }
}

void Game::drawSurvivalInventory() {
    ui.fill(0, 0, ui.width, ui.height, rgba(10, 10, 20, 120));
    const float slot = 22, rowH = 17;
    // Recipes you can make right now are listed first; the list scrolls when it doesn't fit.
    const auto& all = recipes();
    auto craftable = [&](const Recipe& rc) {
        bool can = meta.inv.fits(rc.out.id, rc.out.count);
        for (const auto& in : rc.in) can &= in.empty() || meta.inv.count(in.id) >= in.count;
        return can;
    };
    std::vector<const Recipe*> list;
    for (const auto& rc : all) list.push_back(&rc);
    std::stable_partition(list.begin(), list.end(), [&](const Recipe* rc) { return craftable(*rc); });
    int rows = std::min((int)list.size(), std::max(4, (int)((ui.height - 80) / rowH)));
    const float pw = 9 * slot + 16, ph = 4 * slot + 36, cw = 120, ch = rows * rowH + 28;
    float px = (ui.width - pw - cw - 6) / 2, py = std::max(4.0f, (ui.height - std::max(ph, ch)) / 2 - 16);
    bool shift = SDL_GetModState() & KMOD_SHIFT;
    std::string tooltip;

    // Left click picks up, drops, merges or swaps; right click splits a stack or drops one item;
    // shift-click moves a stack between the hotbar and storage.
    auto clickSlot = [&](int i) {
        ItemStack& s = meta.inv.slots[i];
        if (ui.in.pressed && shift && !s.empty()) {
            int lo = i < 9 ? 9 : 0, hi = i < 9 ? INV_SIZE : 9;
            for (int pass = 0; pass < 2 && !s.empty(); ++pass)
                for (int j = lo; j < hi && !s.empty(); ++j) {
                    ItemStack& t = meta.inv.slots[j];
                    if (pass == 0 ? t.empty() || t.id != s.id : !t.empty()) continue;
                    int k = std::min((int)s.count, STACK_MAX - t.count);
                    t = {s.id, (uint8_t)(t.count + k)};
                    s.count = (uint8_t)(s.count - k);
                }
            if (s.empty()) s = {};
        } else if (ui.in.pressed) {
            if (!held.empty() && s.id == held.id) {
                int k = std::min((int)held.count, STACK_MAX - s.count);
                s.count = (uint8_t)(s.count + k);
                held.count = (uint8_t)(held.count - k);
                if (held.empty()) held = {};
            } else {
                std::swap(held, s);
            }
        } else if (ui.in.rightPressed) {
            if (held.empty() && !s.empty()) {
                uint8_t k = (uint8_t)((s.count + 1) / 2);
                held = {s.id, k};
                s.count = (uint8_t)(s.count - k);
                if (s.empty()) s = {};
            } else if (!held.empty() && (s.empty() || (s.id == held.id && s.count < STACK_MAX))) {
                s = {held.id, (uint8_t)(s.count + 1)};
                if (--held.count == 0) held = {};
            }
        }
    };

    ui.panel(px, py, pw, ph);
    ui.text("Inventory", px + 8, py + 8, rgba(220, 220, 230));
    for (int i = 0; i < INV_SIZE; ++i) {
        int row = i < 9 ? 3 : (i - 9) / 9, col = i % 9;
        float sx = px + 8 + col * slot, sy = py + 22 + row * slot + (i < 9 ? 6 : 0);
        bool hover = ui.hovered(sx, sy, slot, slot);
        ui.image(TexSlot, sx, sy, slot, slot, hover ? WHITE : rgba(220, 220, 230));
        drawStack(meta.inv.slots[i], sx, sy, slot);
        if (!hover) continue;
        if (!meta.inv.slots[i].empty()) tooltip = itemName(meta.inv.slots[i].id);
        clickSlot(i);
    }

    float cx = px + pw + 6;
    ui.panel(cx, py, cw, ch);
    ui.text("Crafting", cx + 8, py + 8, rgba(220, 220, 230));
    if (ui.hovered(cx, py, cw, ch)) craftScroll -= ui.in.wheel;
    craftScroll = std::clamp(craftScroll, 0.0f, (float)(list.size() - rows));
    if (list.size() > (size_t)rows) {
        float track = ch - 30, bar = track * rows / list.size();
        ui.fill(cx + cw - 4, py + 22 + (track - bar) * craftScroll / (list.size() - rows), 2, bar, rgba(255, 255, 255, 90));
    }
    for (int r = 0; r < rows; ++r) {
        const Recipe& rc = *list[r + (int)craftScroll];
        float ry = py + 22 + r * rowH;
        bool can = craftable(rc);
        bool hover = ui.hovered(cx + 4, ry, cw - 10, rowH - 1);
        ui.fill(cx + 4, ry, cw - 10, rowH - 1, hover && can ? rgba(255, 255, 255, 50) : rgba(0, 0, 0, can ? 40 : 90));
        uint32_t tc = can ? WHITE : rgba(130, 130, 140);
        auto item = [&](const ItemStack& st, float x) {
            ui.blockIcon(st.id, x + 6, ry + rowH / 2, 11);
            ui.text(std::to_string(st.count), x + 14, ry + 4, tc, 7);
        };
        item(rc.out, cx + 6);
        ui.text("<", cx + 34, ry + 4, tc, 7);
        item(rc.in[0], cx + 46);
        if (!rc.in[1].empty()) item(rc.in[1], cx + 76);
        if (hover) {
            tooltip = itemName(rc.out.id);
            if (ui.in.pressed && can) {
                for (const auto& in : rc.in)
                    if (!in.empty()) meta.inv.remove(in.id, in.count);
                meta.inv.add(rc.out.id, rc.out.count);
            }
        }
    }
    ui.text("Shift-click moves a stack; right click splits it", px + 8, py + ph + 4, rgba(170, 170, 185), 6);

    if (!held.empty()) drawStack(held, ui.in.mx - slot / 2, ui.in.my - slot / 2, slot);
    if (!tooltip.empty() && held.empty()) {
        float tw = ui.textWidth(tooltip) + 8;
        ui.nineSlice(TexPanel, ui.in.mx + 8, ui.in.my - 14, tw, 14, 15, 3);
        ui.text(tooltip, ui.in.mx + 12, ui.in.my - 11);
    }
}
