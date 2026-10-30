// Chat box and slash commands.
#include "game.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace {
constexpr uint32_t ERROR_COLOR = rgba(255, 95, 95), INFO_COLOR = rgba(205, 210, 220);
constexpr int MAX_FILL = 32768;

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Items and blocks are named like their display names: "Stone Stairs" is stone_stairs.
int findItem(const std::string& name) {
    std::string n = lower(name);
    if (n == "air") return Air;
    if (n == "food") return ItemFood;
    for (int b = 1; b < BlockCount; ++b) {
        std::string id = lower(blockInfo((BlockId)b).name);
        std::replace(id.begin(), id.end(), ' ', '_');
        if (id == n) return itemOf((BlockId)b);
    }
    return -1;
}

// A number, or ~ / ~offset relative to base.
bool parseCoord(const std::string& s, double base, double& out) {
    bool rel = !s.empty() && s[0] == '~';
    std::string num = rel ? s.substr(1) : s;
    if (num.empty()) { out = base; return rel; }
    char* end = nullptr;
    double v = std::strtod(num.c_str(), &end);
    if (*end) return false;
    out = rel ? base + v : v;
    return true;
}

bool parseBool(const std::string& s, bool& out) {
    std::string v = lower(s);
    if (v == "true" || v == "1" || v == "on") { out = true; return true; }
    if (v == "false" || v == "0" || v == "off") { out = false; return true; }
    return false;
}

const char* const kHelp[] = {
    "/gamemode <survival|creative>",
    "/time set <day|noon|night|midnight|ticks>,  /time add <ticks>,  /time query",
    "/tp <x> <y> <z>   (~ for relative)",
    "/give <item> [count]",
    "/setblock <x> <y> <z> <block>",
    "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>",
    "/gamerule [rule] [true|false]",
    "/spawnpoint,  /kill,  /clear,  /seed",
};
}  // namespace

void Game::say(const std::string& text, uint32_t color) {
    chat.push_back({text, color, now});
    if (chat.size() > 100) chat.erase(chat.begin());
}

void Game::openChat(const std::string& prefix) {
    chatInput = prefix;
    sentPos = sent.size();
    chatOpenedAt = now;
    swallowText = true;
    setScreen(Screen::Chat);
}

void Game::drawChat(bool open) {
    const float lineH = 10, x = 2;
    float y = ui.height - (open ? 18 : survival() ? 58 : 48);  // above the input box, or above the hotbar
    int shown = 0;
    for (auto it = chat.rbegin(); it != chat.rend() && shown < (open ? 20 : 10); ++it, ++shown) {
        double age = now - it->at;
        if (!open && age > 10) break;
        uint32_t a = open ? 255 : (uint32_t)(255 * std::clamp(10 - age, 0.0, 1.0));
        y -= lineH;
        ui.fill(x, y, std::max(320.0f, ui.textWidth(it->text, 7) + 4), lineH, rgba(0, 0, 0, (uint8_t)(a * 0.55f)));
        ui.text(it->text, x + 2, y + 1.5f, (it->color & 0xFFFFFFu) | a << 24, 7, false);
    }
    if (!open) return;

    chatFocus = true;
    ui.textField(chatInput, x, ui.height - 16, ui.width - 4, 14, chatFocus, 256);
    if (!ui.in.enter) return;
    std::string line = chatInput;
    chatInput.clear();
    setScreen(Screen::Playing);  // before running the command, which may change the screen
    if (line.find_first_not_of(' ') == std::string::npos) return;
    if (sent.empty() || sent.back() != line) sent.push_back(line);
    if (line[0] == '/') runCommand(line);
    else say("<Player> " + line);
}

void Game::runCommand(const std::string& line) {
    std::vector<std::string> a;
    std::istringstream ss(line.substr(1));
    for (std::string w; ss >> w;) a.push_back(w);
    if (a.empty()) return;
    const std::string cmd = lower(a[0]);
    auto error = [&](const std::string& m) { say(m, ERROR_COLOR); };
    auto usage = [&](const std::string& u) { error("Usage: " + u); };
    // Block coordinates from args[i..i+2], relative to the block the player stands in.
    auto blockPos = [&](size_t i, glm::ivec3& out) {
        glm::dvec3 base = glm::floor(player.pos), v;
        for (int k = 0; k < 3; ++k)
            if (!parseCoord(a[i + k], base[k], v[k])) return false;
        out = glm::ivec3(glm::floor(v));
        return true;
    };

    if (cmd == "help" || cmd == "?") {
        say("Commands:", INFO_COLOR);
        for (const char* h : kHelp) say(h, INFO_COLOR);
    } else if (cmd == "gamemode") {
        std::string m = a.size() > 1 ? lower(a[1]) : "";
        int mode = m == "survival" || m == "s" || m == "0" ? Survival : m == "creative" || m == "c" || m == "1" ? Creative : -1;
        if (mode < 0) return usage("/gamemode <survival|creative>");
        meta.mode = mode;
        if (mode == Survival) player.flying = false;
        fallStart = player.pos.y;
        say(std::string("Set game mode to ") + (mode == Survival ? "Survival" : "Creative"));
    } else if (cmd == "time") {
        std::string sub = a.size() > 1 ? lower(a[1]) : "";
        if (sub == "query") return say("The time is " + std::to_string((int64_t)(std::fmod(meta.time, 1.0) * 24000)));
        if ((sub != "set" && sub != "add") || a.size() < 3) return usage("/time <set|add|query> [value]");
        static const std::pair<const char*, int> named[] = {{"day", 1000}, {"noon", 6000}, {"night", 13000}, {"midnight", 18000}};
        double ticks = -1;
        for (auto& [n, t] : named)
            if (lower(a[2]) == n && sub == "set") ticks = t;
        if (ticks < 0) {
            char* end = nullptr;
            ticks = std::strtod(a[2].c_str(), &end);
            if (*end || ticks < 0) return error("Expected a number of ticks (24000 per day), got '" + a[2] + "'");
        }
        meta.time = sub == "set" ? std::floor(meta.time) + ticks / 24000 : meta.time + ticks / 24000;
        say("Set the time to " + std::to_string((int64_t)(std::fmod(meta.time, 1.0) * 24000)));
    } else if (cmd == "tp" || cmd == "teleport") {
        glm::dvec3 p;
        if (a.size() != 4) return usage("/tp <x> <y> <z>");
        for (int k = 0; k < 3; ++k)
            if (!parseCoord(a[k + 1], player.pos[k], p[k])) return error("Invalid coordinate '" + a[k + 1] + "'");
        player.pos = p;
        player.vel = glm::dvec3(0);
        fallStart = p.y;
        char buf[96];
        std::snprintf(buf, sizeof buf, "Teleported to %.1f, %.1f, %.1f", p.x, p.y, p.z);
        say(buf);
    } else if (cmd == "give") {
        if (a.size() < 2 || a.size() > 3) return usage("/give <item> [count]");
        int id = findItem(a[1]);
        if (id <= Air) return error("Unknown item '" + a[1] + "'");
        int n = a.size() > 2 ? std::atoi(a[2].c_str()) : 1;
        if (n < 1 || n > STACK_MAX * INV_SIZE) return error("Count must be between 1 and " + std::to_string(STACK_MAX * INV_SIZE));
        int given = n - meta.inv.add((BlockId)id, n);
        if (!given) return error("Your inventory is full");
        say("Gave " + std::to_string(given) + " " + itemName((BlockId)id));
    } else if (cmd == "setblock" || cmd == "fill") {
        const bool fill = cmd == "fill";
        const size_t args = fill ? 8 : 5;
        if (a.size() != args) return usage(fill ? "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>" : "/setblock <x> <y> <z> <block>");
        glm::ivec3 lo, hi;
        if (!blockPos(1, lo) || !blockPos(fill ? 4 : 1, hi)) return error("Invalid coordinates");
        int id = findItem(a[args - 1]);
        if (id < 0 || id == ItemFood) return error("Unknown block '" + a[args - 1] + "'");
        glm::ivec3 mn = glm::min(lo, hi), mx = glm::max(lo, hi);
        mn.y = std::max(mn.y, 0);
        mx.y = std::min(mx.y, CH - 1);
        if (mn.y > mx.y) return error("That position is outside the world");
        glm::ivec3 size = mx - mn + 1;
        if ((int64_t)size.x * size.y * size.z > MAX_FILL) return error("Too many blocks (the limit is " + std::to_string(MAX_FILL) + ")");
        for (int z = mn.z; z <= mx.z; ++z)
            for (int x = mn.x; x <= mx.x; ++x)
                if (!world->isLoaded(x, z)) return error("That area isn't loaded");
        for (int y = mn.y; y <= mx.y; ++y)
            for (int z = mn.z; z <= mx.z; ++z)
                for (int x = mn.x; x <= mx.x; ++x) world->set(x, y, z, (BlockId)id);
        worldEdited = true;
        say(fill ? "Filled " + std::to_string(size.x * size.y * size.z) + " blocks" : "Block placed");
    } else if (cmd == "gamerule") {
        if (a.size() == 1) {
            for (const auto& r : kGameRules)
                say(std::string(r.name) + " = " + (meta.rules.*r.value ? "true" : "false") + "   (" + r.desc + ")", INFO_COLOR);
            return;
        }
        auto rule = std::find_if(std::begin(kGameRules), std::end(kGameRules), [&](const GameRule& r) { return lower(r.name) == lower(a[1]); });
        if (rule == std::end(kGameRules)) return error("Unknown game rule '" + a[1] + "'. Type /gamerule for a list.");
        bool& v = meta.rules.*rule->value;
        if (a.size() == 2) return say(std::string(rule->name) + " is " + (v ? "true" : "false"));
        if (a.size() > 3 || !parseBool(a[2], v)) return usage(std::string("/gamerule ") + rule->name + " <true|false>");
        say(std::string(rule->name) + " is now " + (v ? "true" : "false"));
    } else if (cmd == "spawnpoint") {
        meta.spawn = player.pos;
        say("Set your spawn point here");
    } else if (cmd == "kill") {
        hurt(meta.health);
    } else if (cmd == "clear") {
        meta.inv = {};
        say("Cleared your inventory");
    } else if (cmd == "seed") {
        say("Seed: " + std::to_string((int64_t)meta.seed));
    } else {
        error("Unknown command '" + a[0] + "'. Type /help for a list.");
    }
}
