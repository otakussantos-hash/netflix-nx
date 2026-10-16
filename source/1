// Netflix Player para Nintendo Switch (.nro)
// UI: SDL2 | Streaming: libmpv (cache em RAM, sem baixar o arquivo inteiro)
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#define BASE "sdmc:/switch/netflix-player/"

struct Item {
    std::string titulo, url, capa;
    std::vector<uint8_t> data;
    std::atomic<bool> ready{false};
    SDL_Texture* tex = nullptr;
};

static TTF_Font* F[3];  // 0 pequena, 1 media, 2 grande
static const SDL_Color WHITE{255, 255, 255, 255}, GRAY{170, 170, 170, 255}, RED{229, 9, 20, 255};
struct Tx { SDL_Texture* t; int w, h; };
static std::map<std::string, Tx> TC;

// ---------- rede ----------
static size_t wr(void* p, size_t s, size_t n, void* u) {
    auto* v = (std::vector<uint8_t>*)u;
    v->insert(v->end(), (uint8_t*)p, (uint8_t*)p + s * n);
    return s * n;
}
static bool http_get(const std::string& url, std::vector<uint8_t>& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NetflixPlayerNX/0.1");
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return r == CURLE_OK && code < 400;
}

// ---------- Google Drive ----------
// Aceita: drive.google.com/file/d/ID/view, open?id=ID, uc?id=ID ou "gdrive:ID"
static std::string drive_id(const std::string& u) {
    if (u.rfind("gdrive:", 0) == 0) return u.substr(7);
    if (u.find("drive.google.com") == std::string::npos &&
        u.find("drive.usercontent.google.com") == std::string::npos) return "";
    size_t s, p = u.find("/d/");
    if (p != std::string::npos) s = p + 3;
    else {
        p = u.find("id=");
        if (p == std::string::npos) return "";
        s = p + 3;
    }
    size_t e = u.find_first_of("/?&#", s);
    return u.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
static std::string fix_video(const std::string& u) {
    std::string id = drive_id(u);
    if (id.empty()) return u;
    return "https://drive.usercontent.google.com/download?id=" + id + "&export=download&confirm=t";
}
static std::string fix_cover(const std::string& u) {
    std::string id = drive_id(u);
    if (id.empty()) return u;
    return "https://drive.google.com/thumbnail?id=" + id + "&sz=w640";
}

// ---------- JSON ----------
static bool load_items(std::vector<Item*>& items, std::string& err) {
    std::vector<uint8_t> buf;
    std::ifstream u(BASE "videos_url.txt");
    std::string url;
    if (u && std::getline(u, url) && !url.empty()) {
        while (!url.empty() && (url.back() == '\r' || url.back() == ' ')) url.pop_back();
        if (!http_get(url, buf)) { err = "Falha ao baixar o JSON online"; return false; }
    } else {
        std::ifstream f(BASE "videos.json", std::ios::binary);
        if (!f) { err = "Nao achei videos.json em /switch/netflix-player/"; return false; }
        buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    try {
        auto j = nlohmann::json::parse(buf.begin(), buf.end());
        for (auto& v : j.at("videos")) {
            auto* it = new Item;
            it->titulo = v.value("titulo", "Sem titulo");
            it->url = fix_video(v.at("url").get<std::string>());
            it->capa = fix_cover(v.value("capa", ""));
            items.push_back(it);
        }
    } catch (...) { err = "JSON invalido"; return false; }
    if (items.empty()) { err = "Nenhum video no JSON"; return false; }
    return true;
}

// ---------- desenho ----------
static void rect(SDL_Renderer* r, int x, int y, int w, int h, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_Rect d{x, y, w, h};
    SDL_RenderFillRect(r, &d);
}
static void txt(SDL_Renderer* r, int f, const std::string& s, int x, int y, SDL_Color c = WHITE) {
    if (s.empty()) return;
    std::string k = std::to_string(f) + "|" + std::to_string(c.r) + "|" + s;
    auto it = TC.find(k);
    if (it == TC.end()) {
        if (TC.size() > 400) { for (auto& p : TC) SDL_DestroyTexture(p.second.t); TC.clear(); }
        SDL_Surface* sf = TTF_RenderUTF8_Blended(F[f], s.c_str(), c);
        if (!sf) return;
        Tx t{SDL_CreateTextureFromSurface(r, sf), sf->w, sf->h};
        SDL_FreeSurface(sf);
        it = TC.emplace(k, t).first;
    }
    SDL_Rect d{x, y, it->second.w, it->second.h};
    SDL_RenderCopy(r, it->second.t, nullptr, &d);
}
static std::string fmt(double s) {
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    char b[32];
    if (h) snprintf(b, sizeof b, "%d:%02d:%02d", h, m, sec);
    else snprintf(b, sizeof b, "%d:%02d", m, sec);
    return b;
}

static void draw_hero(SDL_Renderer* r, Item* it) {
    if (it->tex) { SDL_Rect d{480, 0, 800, 360}; SDL_RenderCopy(r, it->tex, nullptr, &d); }
    rect(r, 0, 0, 480, 360, 20, 20, 20, 255);
    for (int i = 0; i < 48; i++) rect(r, 480 + i * 10, 0, 10, 360, 20, 20, 20, 255 - i * 255 / 47);
    for (int i = 0; i < 60; i++) rect(r, 0, 300 + i, 1280, 1, 20, 20, 20, i * 255 / 59);
    txt(r, 1, "PLAYER", 48, 40, RED);
    SDL_Rect clip{48, 110, 700, 80};
    SDL_RenderSetClipRect(r, &clip);
    txt(r, 2, it->titulo, 48, 110);
    SDL_RenderSetClipRect(r, nullptr);
    rect(r, 48, 230, 190, 52, 255, 255, 255, 255);
    txt(r, 1, "A  Assistir", 68, 240, SDL_Color{0, 0, 0, 255});
}

static void draw_card(SDL_Renderer* r, Item* it, int x, int y, bool sel) {
    int w = 220, h = 124;
    if (sel) { x -= 8; y -= 8; w += 16; h += 16; }
    SDL_Rect d{x, y, w, h};
    rect(r, x, y, w, h, 50, 50, 50, 255);
    if (it->tex) SDL_RenderCopy(r, it->tex, nullptr, &d);
    SDL_RenderSetClipRect(r, &d);
    rect(r, x, y + h - 30, w, 30, 0, 0, 0, 170);
    txt(r, 0, it->titulo, x + 8, y + h - 27);
    SDL_RenderSetClipRect(r, nullptr);
    if (sel) {
        for (int i = 0; i < 3; i++) {
            SDL_Rect b{x - i, y - i, w + 2 * i, h + 2 * i};
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_RenderDrawRect(r, &b);
        }
    }
}

// ---------- player (mpv) ----------
static void play(SDL_Renderer* ren, PadState& pad, const Item* item) {
    mpv_handle* mpv = mpv_create();
    if (!mpv) return;
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "no");
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "50MiB");      // buffer a frente (RAM)
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "10MiB"); // buffer para voltar
    mpv_set_option_string(mpv, "demuxer-readahead-secs", "30");
    mpv_set_option_string(mpv, "cache-pause-wait", "4");
    mpv_set_option_string(mpv, "network-timeout", "15");
    mpv_set_option_string(mpv, "user-agent", "NetflixPlayerNX/0.1");
    mpv_set_option_string(mpv, "tls-verify", "no");
    mpv_set_option_string(mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    mpv_set_option_string(mpv, "terminal", "no");
    if (mpv_initialize(mpv) < 0) { mpv_terminate_destroy(mpv); return; }

    mpv_render_param ip[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
                             {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rc = nullptr;
    if (mpv_render_context_create(&rc, mpv, ip) < 0) { mpv_terminate_destroy(mpv); return; }

    const int W = 960, H = 540;  // renderiza em 540p e o SDL escala para 720p
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    const char* cmd[] = {"loadfile", item->url.c_str(), nullptr};
    mpv_command(mpv, cmd);

    bool run = true;
    std::string erro;
    Uint32 ui = SDL_GetTicks();
    while (run && appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_B) run = false;
        if (d & HidNpadButton_A) { mpv_command_string(mpv, "cycle pause"); ui = SDL_GetTicks(); }
        if (d & HidNpadButton_Left)  { mpv_command_string(mpv, "seek -10"); ui = SDL_GetTicks(); }
        if (d & HidNpadButton_Right) { mpv_command_string(mpv, "seek 10");  ui = SDL_GetTicks(); }
        if (d & HidNpadButton_ZL)    { mpv_command_string(mpv, "seek -60"); ui = SDL_GetTicks(); }
        if (d & HidNpadButton_ZR)    { mpv_command_string(mpv, "seek 60");  ui = SDL_GetTicks(); }
        if (d & (HidNpadButton_Up | HidNpadButton_Down)) ui = SDL_GetTicks();

        for (;;) {
            mpv_event* e = mpv_wait_event(mpv, 0);
            if (e->event_id == MPV_EVENT_NONE) break;
            if (e->event_id == MPV_EVENT_SHUTDOWN) run = false;
            if (e->event_id == MPV_EVENT_END_FILE) {
                auto* ef = (mpv_event_end_file*)e->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) erro = mpv_error_string(ef->error);
                run = false;
            }
        }

        if (mpv_render_context_update(rc) & MPV_RENDER_UPDATE_FRAME) {
            int sz[2] = {W, H};
            size_t stride = (size_t)W * 4;
            mpv_render_param rp[] = {{MPV_RENDER_PARAM_SW_SIZE, sz},
                                     {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
                                     {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                                     {MPV_RENDER_PARAM_SW_POINTER, px.data()},
                                     {MPV_RENDER_PARAM_INVALID, nullptr}};
            mpv_render_context_render(rc, rp);
            SDL_UpdateTexture(tex, nullptr, px.data(), W * 4);
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);

        double pos = 0, dur = 0;
        int pause = 0, wait = 0;
        mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
        mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
        mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &pause);
        mpv_get_property(mpv, "paused-for-cache", MPV_FORMAT_FLAG, &wait);

        if (wait) txt(ren, 1, "Carregando buffer...", 520, 340);
        if (pause || SDL_GetTicks() - ui < 3500) {
            rect(ren, 0, 600, 1280, 120, 0, 0, 0, 180);
            txt(ren, 1, item->titulo, 48, 612);
            rect(ren, 48, 668, 1184, 6, 90, 90, 90, 255);
            if (dur > 0) rect(ren, 48, 668, (int)(1184 * std::min(1.0, pos / dur)), 6, 229, 9, 20, 255);
            txt(ren, 0, fmt(pos) + " / " + fmt(dur) + (pause ? "   PAUSADO" : ""), 48, 682, GRAY);
        }
        SDL_RenderPresent(ren);
    }

    if (!erro.empty()) {
        SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
        SDL_RenderClear(ren);
        txt(ren, 1, "Erro ao reproduzir: " + erro, 48, 320);
        SDL_RenderPresent(ren);
        SDL_Delay(3000);
    }
    mpv_render_context_free(rc);
    mpv_terminate_destroy(mpv);
    SDL_DestroyTexture(tex);
}

// ---------- main ----------
int main() {
    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    TTF_Init();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_Window* win = SDL_CreateWindow("player", 0, 0, 1280, 720, SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);
    int sizes[3] = {18, 26, 56};
    for (int i = 0; i < 3; i++) F[i] = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 0, sizes[i]);

    std::vector<Item*> items;
    std::string err;
    bool ok = load_items(items, err);

    std::atomic<bool> quit{false};
    std::thread th;
    if (ok) th = std::thread([&] {  // baixa as capas em segundo plano
        for (auto* it : items) {
            if (quit) break;
            if (!it->capa.empty() && http_get(it->capa, it->data)) it->ready = true;
        }
    });

    int row = 0, col = 0, first[2] = {0, 0};
    bool exitApp = false;
    while (!exitApp && appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_Plus) break;

        // cria 1 textura de capa por frame (evita travadas)
        for (auto* it : items) {
            if (it->ready && !it->tex && !it->data.empty()) {
                SDL_Surface* s = IMG_Load_RW(SDL_RWFromConstMem(it->data.data(), (int)it->data.size()), 1);
                if (s) { it->tex = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s); }
                std::vector<uint8_t>().swap(it->data);
                break;
            }
        }

        SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
        SDL_RenderClear(ren);

        if (!ok) {
            txt(ren, 1, err, 48, 320);
            txt(ren, 0, "Coloque videos.json (ou videos_url.txt) em sdmc:/switch/netflix-player/  |  + sai", 48, 370, GRAY);
            SDL_RenderPresent(ren);
            continue;
        }

        int n = (int)items.size();
        int counts[2] = {std::min(5, n), n};
        if (d & HidNpadButton_Up) row = 0;
        if (d & HidNpadButton_Down) row = 1;
        if (d & HidNpadButton_Left) col--;
        if (d & HidNpadButton_Right) col++;
        col = std::max(0, std::min(col, counts[row] - 1));
        if (col < first[row]) first[row] = col;
        if (col > first[row] + 4) first[row] = col - 4;
        if (d & HidNpadButton_A) play(ren, pad, items[col]);

        draw_hero(ren, items[col]);
        const char* labels[2] = {"Novidades", "Todos os titulos"};
        int ly[2] = {366, 538}, cy[2] = {402, 572};
        for (int r = 0; r < 2; r++) {
            txt(ren, 1, labels[r], 48, ly[r]);
            for (int i = first[r]; i < std::min(counts[r], first[r] + 5); i++)
                draw_card(ren, items[i], 48 + (i - first[r]) * 232, cy[r], r == row && i == col);
        }
        SDL_RenderPresent(ren);
    }

    quit = true;
    if (th.joinable()) th.join();
    curl_global_cleanup();
    TTF_Quit(); IMG_Quit(); SDL_Quit();
    plExit(); socketExit();
    return 0;
}
