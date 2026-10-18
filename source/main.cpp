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
#include "curlstream.h"
#include <cctype>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <string>
#include <thread>
#include <vector>

#define BASE "sdmc:/switch/netflix-player/"
// JSON online (GitHub raw). Edite o videos.json no repositorio e o app puxa daqui.
#define JSON_URL "https://raw.githubusercontent.com/otakussantos-hash/netflix-nx/main/videos.json"

struct Episode { std::string titulo, url; };
struct Season { std::string nome; std::vector<Episode> eps; };
struct Item {
    std::string titulo, url, capa, cpath;  // cpath = capa em cache no SD
    std::string sinopse, ano, genero, duracao;
    std::vector<Season> seasons;           // vazio = filme (usa "url"); preenchido = serie
    bool serie() const { return !seasons.empty(); }
    std::atomic<int> st{0};                // 0 = pendente, 1 = baixando, 2 = no disco, 3 = sem capa/falhou
    SDL_Texture* tex = nullptr;            // so existe enquanto o card esta visivel
};

static TTF_Font* F[4];  // 0 pequena, 1 media, 2 grande, 3 titulo longo (2 linhas)
static const SDL_Color WHITE{255, 255, 255, 255}, GRAY{170, 170, 170, 255}, RED{229, 9, 20, 255};
struct Tx { SDL_Texture* t; int w, h; };
static std::map<std::string, Tx> TC;
static SDL_Texture* OV = nullptr;            // degrade do destaque (1 textura pronta)
static std::atomic<bool> g_play{false};      // true durante o video: pausa o download de capas
static std::atomic<bool> g_quit{false};
static std::mutex g_mx;
static std::atomic<bool> g_items_ready{false};  // o JSON ja foi lido (o worker de capas espera isto)
static float g_a = 1.f;                      // transparencia global (animacao de entrada dos icones)
static SDL_Texture* SPIN = nullptr;          // circulo vermelho girando
static SDL_Texture* LOGO = nullptr;          // logo da Netflix (romfs:/logo.png)
static int LOGO_W = 1, LOGO_H = 1;
static std::vector<int> g_want;              // capas que o app quer agora (visiveis primeiro)

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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NetflixPlayerNX/0.2");
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return r == CURLE_OK && code < 400;
}

// Baixa direto para o SD (nao usa RAM) e limita o tamanho
struct FileSink { FILE* f; size_t n, max; };
static size_t wr_file(void* p, size_t sz, size_t n, void* u) {
    auto* k = (FileSink*)u;
    k->n += sz * n;
    if (k->n > k->max) return 0;
    return fwrite(p, sz, n, k->f) == n ? sz * n : 0;
}
static bool http_get_file(const std::string& url, const std::string& path, size_t maxBytes) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    FileSink k{f, 0, maxBytes};
    CURL* c = curl_easy_init();
    if (!c) { fclose(f); remove(tmp.c_str()); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NetflixPlayerNX/0.2");
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    fclose(f);
    if (r != CURLE_OK || code >= 400 || k.n == 0) { remove(tmp.c_str()); return false; }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

// ---------- Google Drive ----------
// Aceita: drive.google.com/file/d/ID/view, open?id=ID, uc?id=ID ou "gdrive:ID"
static bool bare_drive_id(const std::string& u) {  // so o ID colado no JSON, sem "gdrive:"
    if (u.size() < 20) return false;
    for (char c : u) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}
static std::string drive_id(const std::string& u) {
    if (u.rfind("gdrive:", 0) == 0) return u.substr(7);
    if (bare_drive_id(u)) return u;
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

// Arquivos grandes do Drive passam por uma pagina de confirmacao (HTML).
// Aqui a gente le essa pagina, monta o link final do download e avisa o motivo se o Drive negar.
struct Cap { std::string s; size_t max; };
static size_t cap_wr(void* p, size_t sz, size_t n, void* u) {
    auto* c = (Cap*)u;
    if (c->s.size() >= c->max) return 0;  // ja temos o suficiente: aborta o download
    c->s.append((char*)p, sz * n);
    return sz * n;
}
static std::string html_attr(const std::string& tag, const std::string& name) {
    std::string k = name + "=\"";
    size_t p = tag.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = tag.find('"', p);
    return e == std::string::npos ? "" : tag.substr(p, e - p);
}
static std::string unamp(std::string s) {
    for (size_t p; (p = s.find("&amp;")) != std::string::npos;) s.replace(p, 5, "&");
    return s;
}
// Devolve a URL final para o mpv. Se der problema, devolve "" e preenche err.
static std::string resolve_drive(const std::string& url, std::string& err) {
    Cap cap{"", 262144};
    CURL* c = curl_easy_init();
    if (!c) { err = "curl indisponivel"; return ""; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cap_wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &cap);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0");
    CURLcode r = curl_easy_perform(c);
    char* ctp = nullptr;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctp);
    std::string ct = ctp ? ctp : "";
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (cap.s.empty() && r != CURLE_OK) { err = "Sem conexao com o Google Drive"; return ""; }
    if (ct.find("text/html") == std::string::npos) return url;  // ja e o arquivo, o mpv le direto

    if (cap.s.find("Too many users") != std::string::npos || cap.s.find("quota") != std::string::npos) {
        err = "Drive: limite de downloads excedido (tente em 24h)";
        return "";
    }
    size_t f = cap.s.find("id=\"download-form\"");
    if (f == std::string::npos) {
        err = "Drive negou o acesso: compartilhe como 'Qualquer pessoa com o link'";
        return "";
    }
    size_t fs = cap.s.rfind("<form", f);
    size_t fe = cap.s.find("</form>", f);
    if (fs == std::string::npos || fe == std::string::npos) { err = "Pagina do Drive inesperada"; return ""; }
    std::string form = cap.s.substr(fs, fe - fs);
    std::string action = unamp(html_attr(form.substr(0, form.find('>')), "action"));
    if (action.empty()) action = "https://drive.usercontent.google.com/download";

    std::string q;
    CURL* e = curl_easy_init();
    for (size_t p = 0; (p = form.find("<input", p)) != std::string::npos;) {
        size_t end = form.find('>', p);
        if (end == std::string::npos) break;
        std::string tag = form.substr(p, end - p);
        p = end;
        std::string name = html_attr(tag, "name");
        if (name.empty()) continue;
        std::string val = unamp(html_attr(tag, "value"));
        char* enc = curl_easy_escape(e, val.c_str(), (int)val.size());
        q += (q.empty() ? "" : "&") + name + "=" + (enc ? enc : "");
        if (enc) curl_free(enc);
    }
    curl_easy_cleanup(e);
    if (q.empty()) { err = "Pagina do Drive inesperada"; return ""; }
    return action + "?" + q;
}

// ---------- JSON ----------
static bool load_items(std::vector<Item*>& items, std::string& err) {
    std::vector<uint8_t> buf;
    std::string url = JSON_URL;
    // videos_url.txt (opcional) sobrescreve a URL padrao
    std::ifstream u(BASE "videos_url.txt");
    std::string custom;
    if (u && std::getline(u, custom)) {
        while (!custom.empty() && (custom.back() == '\r' || custom.back() == ' ')) custom.pop_back();
        if (!custom.empty()) url = custom;
    }
    // 1) tenta baixar do GitHub
    bool online = http_get(url, buf) && !buf.empty();
    // 2) se falhar (sem internet), usa o videos.json local como reserva
    if (!online) {
        buf.clear();
        std::ifstream f(BASE "videos.json", std::ios::binary);
        if (!f) { err = "Falha ao baixar o JSON do GitHub (sem internet?)"; return false; }
        buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    try {
        auto j = nlohmann::json::parse(buf.begin(), buf.end());
        for (auto& v : j.at("videos")) {
            auto* it = new Item;
            auto str = [&](const char* k) -> std::string {  // texto ou numero (ex.: "ano": 2019)
                if (!v.contains(k)) return "";
                auto& x = v[k];
                return x.is_string() ? x.get<std::string>() : x.dump();
            };
            it->titulo = v.value("titulo", "Sem titulo");
            it->sinopse = str("sinopse");
            it->ano = str("ano");
            it->genero = str("genero");
            it->duracao = str("duracao");
            if (v.contains("temporadas") && v["temporadas"].is_array()) {
                int sn = 0;
                for (auto& t : v["temporadas"]) {
                    Season se;
                    se.nome = t.value("nome", "Temporada " + std::to_string(++sn));
                    if (t.contains("episodios") && t["episodios"].is_array()) {
                        int en = 0;
                        for (auto& e : t["episodios"]) {
                            Episode ep;
                            ep.titulo = e.value("titulo", "Episodio " + std::to_string(++en));
                            ep.url = fix_video(e.at("url").get<std::string>());
                            se.eps.push_back(ep);
                        }
                    }
                    it->seasons.push_back(se);
                }
            }
            if (!it->serie()) it->url = fix_video(v.at("url").get<std::string>());
            it->capa = fix_cover(v.value("capa", ""));
            if (it->capa.empty()) it->st = 3;
            else {
                char h[32];
                snprintf(h, sizeof h, "%016zx", std::hash<std::string>{}(it->capa));
                it->cpath = std::string(BASE "cache/") + h + ".img";
                struct stat sb;
                if (stat(it->cpath.c_str(), &sb) == 0 && sb.st_size > 0) it->st = 2;  // ja baixada antes
            }
            items.push_back(it);
        }
    } catch (...) { err = "JSON invalido"; return false; }
    if (items.empty()) { err = "Nenhum video no JSON"; return false; }
    return true;
}

// ---------- historico: retomar de onde parou + "Volte a assistir" ----------
#define PROG_FILE BASE "progress.json"
struct PlayCtx { std::string key, item; int ss = -1, ep = -1; };
struct Prog { std::string key, item; int ss = -1, ep = -1; double pos = 0, dur = 0; long long ts = 0; };
static std::vector<Prog> g_hist;
static int g_hist_ver = 0;  // muda a cada gravacao (a tela inicial se atualiza)

static void hist_load() {
    g_hist.clear();
    std::ifstream f(PROG_FILE, std::ios::binary);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& e : j.at("h")) {
            Prog p;
            p.key = e.value("k", "");  p.item = e.value("i", "");
            p.ss = e.value("s", -1);   p.ep = e.value("e", -1);
            p.pos = e.value("p", 0.0); p.dur = e.value("d", 0.0);
            p.ts = e.value("t", 0LL);
            if (!p.key.empty()) g_hist.push_back(p);
        }
    } catch (...) {}
}
static void hist_save() {
    nlohmann::json a = nlohmann::json::array();
    for (auto& p : g_hist)
        a.push_back({{"k", p.key}, {"i", p.item}, {"s", p.ss}, {"e", p.ep}, {"p", p.pos}, {"d", p.dur}, {"t", p.ts}});
    nlohmann::json j;
    j["h"] = a;
    std::string tmp = PROG_FILE ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    remove(PROG_FILE);
    rename(tmp.c_str(), PROG_FILE);
}
static Prog* hist_find(const std::string& key) {
    for (auto& p : g_hist) if (p.key == key) return &p;
    return nullptr;
}
static void hist_put(const std::string& key, const std::string& item, int ss, int ep, double pos, double dur) {
    long long t = (long long)time(nullptr);
    for (auto& q : g_hist) if (q.ts >= t) t = q.ts + 1;  // sempre o mais recente
    Prog* p = hist_find(key);
    if (!p) { g_hist.push_back(Prog()); p = &g_hist.back(); p->key = key; }
    p->item = item; p->ss = ss; p->ep = ep; p->pos = pos; p->dur = dur; p->ts = t;
    if (g_hist.size() > 60) {  // guarda so os 60 mais recentes
        auto old = std::min_element(g_hist.begin(), g_hist.end(), [](const Prog& a, const Prog& b) { return a.ts < b.ts; });
        g_hist.erase(old);
    }
    g_hist_ver++;
}
static const Prog* last_prog(const std::string& item) {
    const Prog* b = nullptr;
    for (auto& p : g_hist) if (p.item == item && (!b || p.ts > b->ts)) b = &p;
    return b;
}
// progresso (0..1) do ultimo ponto de saida; -1 se nao ha o que continuar
static float item_prog(const Item* it) {
    const Prog* p = last_prog(it->titulo);
    return (p && p->dur > 0 && p->pos > 15) ? (float)std::min(1.0, p->pos / p->dur) : -1.f;
}
// os ultimos 5 titulos assistidos (indices em items)
static std::vector<int> recent_idx(const std::vector<Item*>& items) {
    std::vector<const Prog*> v;
    for (auto& p : g_hist) v.push_back(&p);
    std::sort(v.begin(), v.end(), [](const Prog* a, const Prog* b) { return a->ts > b->ts; });
    std::vector<int> out;
    for (const Prog* p : v) {
        for (size_t i = 0; i < items.size(); i++)
            if (items[i]->titulo == p->item) {
                if (std::find(out.begin(), out.end(), (int)i) == out.end()) out.push_back((int)i);
                break;
            }
        if (out.size() >= 5) break;
    }
    return out;
}

struct Row { const char* label; std::vector<int> idx; bool cont; };
static std::vector<Row> build_rows(const std::vector<Item*>& items) {
    std::vector<Row> R;
    std::vector<int> rc = recent_idx(items);
    if (!rc.empty()) R.push_back(Row{"Volte a assistir", rc, true});
    Row nov{"Novidades", {}, false}, all{"Todos os títulos", {}, false};
    for (int i = 0; i < (int)items.size(); i++) { if (i < 5) nov.idx.push_back(i); all.idx.push_back(i); }
    R.push_back(nov);
    R.push_back(all);
    return R;
}
// o que esta visivel agora (e um pouco a frente) tem prioridade nas capas
static void compute_vis(const std::vector<Row>& rows, int row, int col, int vtop, const int* first, size_t n,
                        std::vector<char>& vis, std::vector<int>& want) {
    vis.assign(n, 0);
    want.clear();
    int fi = rows[row].idx[col];
    vis[fi] = 1;
    want.push_back(fi);
    for (int r = vtop; r < std::min((int)rows.size(), vtop + 2); r++)
        for (int i = first[r]; i < std::min((int)rows[r].idx.size(), first[r] + 5); i++) {
            int k = rows[r].idx[i];
            if (!vis[k]) { vis[k] = 1; want.push_back(k); }
        }
    int lr = (int)rows.size() - 1;
    for (int i = first[lr] + 5; i < std::min((int)rows[lr].idx.size(), first[lr] + 10); i++) want.push_back(rows[lr].idx[i]);
}

// ---------- desenho ----------
static void rect(SDL_Renderer* r, int x, int y, int w, int h, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    A = (Uint8)(A * g_a);
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
    SDL_SetTextureAlphaMod(it->second.t, (Uint8)(255 * g_a));
    SDL_RenderCopy(r, it->second.t, nullptr, &d);
}
static std::string fmt(double s) {
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    char b[32];
    if (h) snprintf(b, sizeof b, "%d:%02d:%02d", h, m, sec);
    else snprintf(b, sizeof b, "%d:%02d", m, sec);
    return b;
}

// ---------- texto que cabe, logo da Netflix e circulo de loading ----------
static void blit(SDL_Renderer* r, SDL_Texture* t, const SDL_Rect* d) {
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(t, (Uint8)(255 * g_a));
    SDL_RenderCopy(r, t, nullptr, d);
}
static int tw(int f, const std::string& s) { int w = 0; TTF_SizeUTF8(F[f], s.c_str(), &w, nullptr); return w; }
// Corta o texto com "..." para caber em maxw pixels (nunca mais corta letra no meio)
static std::string fit(int f, const std::string& s, int maxw) {
    static std::map<std::string, std::string> C;
    std::string k = std::to_string(f) + "|" + std::to_string(maxw) + "|" + s;
    auto it = C.find(k);
    if (it != C.end()) return it->second;
    std::string t = s;
    if (tw(f, t) > maxw) {
        for (;;) {
            while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();  // nao quebra letra acentuada (UTF-8)
            if (!t.empty()) t.pop_back();
            std::string c = t;
            while (!c.empty() && c.back() == ' ') c.pop_back();
            c += "...";
            if (tw(f, c) <= maxw || t.empty()) { t = c; break; }
        }
    }
    if (C.size() > 500) C.clear();
    return C[k] = t;
}
static std::vector<std::string> wrap_px(int f, const std::string& s, int maxw, size_t maxl) {
    std::vector<std::string> out;
    std::string line, word;
    auto push_word = [&]() {
        if (word.empty()) return;
        std::string c = line.empty() ? word : line + " " + word;
        if (line.empty() || tw(f, c) <= maxw) line = c;
        else { out.push_back(line); line = word; }
        word.clear();
    };
    for (char ch : s) { if (ch == ' ') push_word(); else word += ch; }
    push_word();
    if (!line.empty()) out.push_back(line);
    if (out.size() > maxl) {  // o que passar de maxl linhas vira "..."
        std::string rest;
        for (size_t i = maxl - 1; i < out.size(); i++) rest += (rest.empty() ? "" : " ") + out[i];
        out.resize(maxl);
        out[maxl - 1] = rest;
    }
    for (auto& l : out) l = fit(f, l, maxw);
    return out;
}
// Titulo grande; se nao couber em 1 linha, usa fonte menor em ate 2 linhas
static void draw_title(SDL_Renderer* r, const std::string& s, int x, int y1, int y2, int step, int maxw) {
    if (tw(2, s) <= maxw) { txt(r, 2, s, x, y1); return; }
    auto L = wrap_px(3, s, maxw, 2);
    for (size_t i = 0; i < L.size(); i++) txt(r, 3, L[i], x, y2 + (int)i * step);
}
static SDL_Texture* load_logo(SDL_Renderer* r) {
    const char* paths[] = {BASE "logo.png", "romfs:/logo.png"};  // o do SD (se existir) vence
    for (const char* p : paths) {
        SDL_Surface* sf = IMG_Load(p);
        if (!sf) continue;
        LOGO_W = sf->w; LOGO_H = sf->h;
        SDL_Texture* t = SDL_CreateTextureFromSurface(r, sf);
        SDL_FreeSurface(sf);
        if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); return t; }
    }
    return nullptr;
}
static void draw_logo(SDL_Renderer* r, int x, int y, int h) {
    if (!LOGO) { txt(r, 1, "NETFLIX", x, y, RED); return; }
    SDL_Rect d{x, y, LOGO_W * h / LOGO_H, h};
    blit(r, LOGO, &d);
}
// Anel vermelho com rastro que some (estilo Netflix); o giro e feito pelo SDL_RenderCopyEx
static SDL_Texture* make_spinner(SDL_Renderer* r) {
    const int S = 128;
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, S, S, 32, SDL_PIXELFORMAT_RGBA32);
    if (!sf) return nullptr;
    const float c = (S - 1) / 2.f, R1 = 58.f, R0 = 44.f, PI2 = 6.2831853f;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float dx = x - c, dy = y - c, dist = sqrtf(dx * dx + dy * dy);
            float edge = std::min(std::min(dist - R0, R1 - dist) + 0.5f, 1.f);
            float t = fmodf(PI2 * 2 - atan2f(dy, dx), PI2);        // 0 na ponta, cresce para tras
            float fade = std::max(0.f, 1.f - t / (PI2 * 0.78f));
            float a = std::max(0.f, edge) * fade;
            Uint8* p = (Uint8*)sf->pixels + y * sf->pitch + x * 4;
            p[0] = 229; p[1] = 9; p[2] = 20; p[3] = (Uint8)(a * 255);
        }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, sf);
    SDL_FreeSurface(sf);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}
static void draw_spinner(SDL_Renderer* r, int cx, int cy, int size) {
    if (!SPIN) return;
    SDL_Rect d{cx - size / 2, cy - size / 2, size, size};
    SDL_SetTextureAlphaMod(SPIN, (Uint8)(255 * g_a));
    SDL_RenderCopyEx(r, SPIN, nullptr, &d, (SDL_GetTicks() % 900) * 360.0 / 900.0, nullptr, SDL_FLIP_NONE);
}
static void spin_frame(SDL_Renderer* r) {  // tela preta so com o circulo (sem nenhum texto)
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    draw_spinner(r, 640, 360, 96);
    SDL_RenderPresent(r);
}

// Mesmo degrade de antes (faixa lateral + base), mas montado 1 vez so em vez de 108 retangulos por quadro
static SDL_Texture* make_overlay(SDL_Renderer* r) {
    SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, 1280, 360, 32, SDL_PIXELFORMAT_RGBA32);
    if (!sf) return nullptr;
    for (int y = 0; y < 360; y++) {
        int a2 = y >= 300 ? (y - 300) * 255 / 59 : 0;
        for (int x = 0; x < 1280; x++) {
            int a1 = x < 480 ? 255 : (x < 960 ? 255 - ((x - 480) / 10) * 255 / 47 : 0);
            Uint8* px = (Uint8*)sf->pixels + y * sf->pitch + x * 4;
            px[0] = px[1] = px[2] = 20;
            px[3] = (Uint8)(255 - (255 - a1) * (255 - a2) / 255);
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, sf);
    SDL_FreeSurface(sf);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static void draw_hero(SDL_Renderer* r, Item* it, bool resume) {
    if (it->tex) { SDL_Rect d{480, 0, 800, 360}; blit(r, it->tex, &d); }
    if (OV) { SDL_Rect o{0, 0, 1280, 360}; blit(r, OV, &o); }
    else {
        rect(r, 0, 0, 480, 360, 20, 20, 20, 255);
        for (int i = 0; i < 48; i++) rect(r, 480 + i * 10, 0, 10, 360, 20, 20, 20, 255 - i * 255 / 47);
        for (int i = 0; i < 60; i++) rect(r, 0, 300 + i, 1280, 1, 20, 20, 20, i * 255 / 59);
    }
    draw_logo(r, 48, 28, 60);
    draw_title(r, it->titulo, 48, 110, 100, 48, 660);
    rect(r, 48, 230, 220, 52, 255, 255, 255, 255);
    txt(r, 1, resume ? "A  Continuar" : "A  Assistir", 68, 240, SDL_Color{0, 0, 0, 255});
}

static void draw_card(SDL_Renderer* r, Item* it, int x, int y, bool sel, float prog) {
    int w = 220, h = 124;
    if (sel) { x -= 8; y -= 8; w += 16; h += 16; }
    SDL_Rect d{x, y, w, h};
    rect(r, x, y, w, h, 50, 50, 50, 255);
    if (it->tex) blit(r, it->tex, &d);
    rect(r, x, y + h - 30, w, 30, 0, 0, 0, 170);
    txt(r, 0, fit(0, it->titulo, w - 16), x + 8, y + h - 27);
    if (prog >= 0) {  // barra vermelha de progresso (Volte a assistir)
        rect(r, x, y + h - 4, w, 4, 90, 90, 90, 220);
        rect(r, x, y + h - 4, (int)(w * prog), 4, 229, 9, 20, 255);
    }
    if (sel) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        for (int i = 0; i < 3; i++) {
            SDL_Rect b{x - i, y - i, w + 2 * i, h + 2 * i};
            SDL_SetRenderDrawColor(r, 255, 255, 255, (Uint8)(255 * g_a));
            SDL_RenderDrawRect(r, &b);
        }
    }
}

// ---------- capas: baixa so o que aparece, decodifica so o visivel ----------
static void cover_worker(std::vector<Item*>* items) {
    while (!g_quit) {
        if (!g_items_ready) { SDL_Delay(100); continue; }
        if (g_play) { SDL_Delay(300); continue; }  // durante o video, CPU e rede ficam para o player
        Item* job = nullptr;
        {
            std::lock_guard<std::mutex> l(g_mx);
            for (int i : g_want)
                if (i >= 0 && i < (int)items->size() && (*items)[i]->st == 0) { job = (*items)[i]; break; }
        }
        if (!job) { SDL_Delay(120); continue; }
        job->st = 1;
        job->st = http_get_file(job->capa, job->cpath, 4u << 20) ? 2 : 3;  // capa maior que 4 MB e ignorada
    }
}
static SDL_Surface* shrink(SDL_Surface* s, int maxw, int maxh) {
    if (s->w <= maxw && s->h <= maxh) return s;
    double k = std::min((double)maxw / s->w, (double)maxh / s->h);
    int nw = std::max(1, (int)(s->w * k)), nh = std::max(1, (int)(s->h * k));
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, nw, nh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) return s;
    SDL_BlitScaled(s, nullptr, d, nullptr);
    SDL_FreeSurface(s);
    return d;
}
// Libera as texturas dos cards fora da tela e cria no maximo 1 por vez. Devolve true se algo mudou.
static bool manage_tex(SDL_Renderer* r, std::vector<Item*>& items, const std::vector<char>& vis) {
    for (size_t i = 0; i < items.size(); i++)
        if (!vis[i] && items[i]->tex) { SDL_DestroyTexture(items[i]->tex); items[i]->tex = nullptr; }
    for (size_t i = 0; i < items.size(); i++) {
        Item* it = items[i];
        if (!vis[i] || it->tex || it->st != 2) continue;
        SDL_Surface* sf = IMG_Load(it->cpath.c_str());
        if (!sf) { remove(it->cpath.c_str()); it->st = 3; continue; }
        sf = shrink(sf, 800, 450);
        it->tex = SDL_CreateTextureFromSurface(r, sf);
        SDL_FreeSurface(sf);
        if (!it->tex) it->st = 3;
        return true;  // uma capa por quadro
    }
    return false;
}

// ---------- player (mpv) ----------
struct PlayGuard { PlayGuard() { g_play = true; } ~PlayGuard() { g_play = false; } };

// Quebra o texto em linhas que cabem na tela
static std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > width) { out.push_back(line); line.clear(); }
        while (word.size() > width) { out.push_back(word.substr(0, width)); word.erase(0, width); }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == '\n') { flush_word(); out.push_back(line); line.clear(); }
        else if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) out.push_back(line);
    return out;
}
// Tela de aviso/erro. Fecha com A/B ou depois de "ms".
static void show_msg(SDL_Renderer* ren, PadState& pad, const std::string& title, const std::string& msg, Uint32 ms = 12000) {
    auto lines = wrap(msg, 62);
    Uint32 t0 = SDL_GetTicks();
    while (appletMainLoop() && SDL_GetTicks() - t0 < ms) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B)) break;
        SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
        SDL_RenderClear(ren);
        txt(ren, 1, title, 48, 120, RED);
        int y = 180;
        for (auto& l : lines) { txt(ren, 1, l, 48, y); y += 34; if (y > 640) break; }
        txt(ren, 0, "A / B  voltar", 48, 670, GRAY);
        SDL_RenderPresent(ren);
        SDL_Delay(50);
    }
}
static bool is_http(const std::string& u) { return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0; }
static bool is_hls(const std::string& u) { return cs::lower(u).find(".m3u8") != std::string::npos; }
// extensao do arquivo na URL (ajuda o mpv a escolher o demuxer)
static std::string url_ext(const std::string& u) {
    std::string p = u.substr(0, u.find_first_of("?#"));
    size_t sl = p.find_last_of('/'), dot = p.find_last_of('.');
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl) || p.size() - dot > 6) return "";
    for (size_t i = dot + 1; i < p.size(); i++) if (!isalnum((unsigned char)p[i])) return "";
    return cs::lower(p.substr(dot));
}

// Clock da CPU em 1785 MHz durante o video (o padrao de homebrew e 1020 MHz e a decodificacao e por software).
// Se o build reclamar de pcv/clkrst, compile com -DNO_CPU_BOOST.
#ifndef NO_CPU_BOOST
struct CpuBoost {
    static constexpr u32 HZ = 1785000000u;
    bool ok = false, clk = false;
    ClkrstSession cs{};
    u32 old = 1020000000u;
    CpuBoost() {
        u32 cur = 0;
        if (hosversionAtLeast(8, 0, 0)) {
            if (R_SUCCEEDED(clkrstInitialize())) {
                if (R_SUCCEEDED(clkrstOpenSession(&cs, PcvModuleId_CpuBus, 3))) {
                    if (R_SUCCEEDED(clkrstGetClockRate(&cs, &cur)) && cur) old = cur;
                    if (R_SUCCEEDED(clkrstSetClockRate(&cs, HZ))) ok = clk = true;
                    else clkrstCloseSession(&cs);
                }
                if (!ok) clkrstExit();
            }
        } else if (R_SUCCEEDED(pcvInitialize())) {
            if (R_SUCCEEDED(pcvGetClockRate(PcvModule_CpuBus, &cur)) && cur) old = cur;
            if (R_SUCCEEDED(pcvSetClockRate(PcvModule_CpuBus, HZ))) ok = true;
            else pcvExit();
        }
    }
    ~CpuBoost() {
        if (!ok) return;
        if (clk) { clkrstSetClockRate(&cs, old); clkrstCloseSession(&cs); clkrstExit(); }
        else { pcvSetClockRate(PcvModule_CpuBus, old); pcvExit(); }
    }
};
#else
struct CpuBoost {};
#endif

// Devolve true se o video terminou sozinho (para passar ao proximo episodio).
static bool play(SDL_Renderer* ren, PadState& pad, const std::string& title, const std::string& itemUrl, const PlayCtx& ctx) {
    PlayGuard guard;
    g_a = 1.f;
    CpuBoost boost;
    bool ended = false;
    FILE* lg = fopen(BASE "log.txt", "w");  // diagnostico: sdmc:/switch/netflix-player/log.txt
    size_t logBytes = 0;
    auto log = [&](const std::string& s, bool flush = true) {
        if (!lg || logBytes > 200000) return;  // o log nao cresce sem limite durante um filme longo
        logBytes += s.size() + 1;
        fputs(s.c_str(), lg);
        fputc('\n', lg);
        if (flush) fflush(lg);
    };
    struct LogClose { FILE*& f; ~LogClose() { if (f) fclose(f); } } logClose{lg};
    auto status = [&](const std::string&) { spin_frame(ren); };  // so o circulo vermelho, sem texto
    log("titulo: " + title);

    std::string playUrl = itemUrl, driveErr;
    cs::Probe pr;
    std::string curlErr;
    bool useCurl = false, probed = false, driveOk = true;
    {   // prepara o link (Drive / teste do curl) em segundo plano; na tela, so o circulo vermelho girando
        std::atomic<bool> done{false};
        std::thread prep([&] {
            if (playUrl.find("drive.usercontent.google.com") != std::string::npos) {
                std::string r = resolve_drive(playUrl, driveErr);
                if (r.empty()) driveOk = false; else playUrl = r;
            }
            if (driveOk && is_http(playUrl) && !is_hls(playUrl)) {
                probed = true;
                if (cs::probe(playUrl, pr)) useCurl = true; else curlErr = pr.err;
            }
            done = true;
        });
        while (!done && appletMainLoop()) spin_frame(ren);
        prep.join();
    }
    if (!driveOk) {
        log("drive: " + driveErr);
        show_msg(ren, pad, "Erro ao reproduzir", driveErr);
        return false;
    }
    if (probed) {
        if (useCurl)
            log("curl ok: http " + std::to_string(pr.code) + ", tamanho " + std::to_string((long long)pr.size) +
                (pr.seekable ? ", seek ok" : ", sem seek"));
        else
            log("curl falhou: " + pr.err + " (tentando modo nativo)");
    }

    mpv_handle* mpv = mpv_create();
    if (!mpv) { show_msg(ren, pad, "Erro no player", "mpv_create falhou (memoria insuficiente?)"); return false; }
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "no");
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "32MiB");      // buffer a frente (RAM)
    mpv_set_option_string(mpv, "demuxer-max-back-bytes", "4MiB");  // buffer para voltar
    mpv_set_option_string(mpv, "demuxer-readahead-secs", "20");
    // menos trabalho para a CPU
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "load-scripts", "no");
    mpv_set_option_string(mpv, "sid", "no");                // sem legendas embutidas (nao renderiza texto)
    mpv_set_option_string(mpv, "audio-channels", "stereo"); // mixa 5.1 para estereo
    mpv_set_option_string(mpv, "vd-lavc-threads", "3");     // 3 nucleos livres no Switch
    mpv_set_option_string(mpv, "vd-lavc-fast", "yes");
    mpv_set_option_string(mpv, "vd-lavc-skiploopfilter", "nonref");
    mpv_set_option_string(mpv, "framedrop", "vo");          // descarta quadros em vez de travar
    mpv_set_option_string(mpv, "sws-fast", "yes");
    mpv_set_option_string(mpv, "sws-scaler", "fast-bilinear");
    mpv_set_option_string(mpv, "cache-pause-wait", "4");
    mpv_set_option_string(mpv, "network-timeout", "15");
    mpv_set_option_string(mpv, "user-agent", "Mozilla/5.0 NetflixPlayerNX/0.2");
    mpv_set_option_string(mpv, "tls-verify", "no");
    mpv_set_option_string(mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    mpv_set_option_string(mpv, "terminal", "no");
    double resume = 0;  // volta ao ponto de saida (3 s antes, para situar a cena)
    if (const Prog* pp = hist_find(ctx.key))
        if (pp->pos > 15 && (!useCurl || pr.seekable)) resume = std::max(0.0, pp->pos - 3);
    if (resume > 0) mpv_set_option_string(mpv, "start", std::to_string((int)resume).c_str());
    int ir = mpv_initialize(mpv);
    if (ir < 0) {
        log(std::string("mpv_initialize: ") + mpv_error_string(ir));
        mpv_terminate_destroy(mpv);
        show_msg(ren, pad, "Erro no player", std::string("mpv_initialize falhou: ") + mpv_error_string(ir));
        return false;
    }
    mpv_request_log_messages(mpv, "info");

    mpv_render_param ip[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
                             {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rc = nullptr;
    int rr = mpv_render_context_create(&rc, mpv, ip);
    if (rr < 0) {
        log(std::string("render_context_create: ") + mpv_error_string(rr));
        mpv_terminate_destroy(mpv);
        show_msg(ren, pad, "Erro no player", std::string("Render do mpv falhou: ") + mpv_error_string(rr));
        return false;
    }

    const int W = 960, H = 540;  // renderiza em 540p e o SDL escala para 720p
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_UpdateTexture(tex, nullptr, px.data(), W * 4);

    auto load = [&](const std::string& target) {
        const char* cmd[] = {"loadfile", target.c_str(), nullptr};
        int r = mpv_command(mpv, cmd);
        if (r < 0) log(std::string("loadfile: ") + mpv_error_string(r));
    };
    if (useCurl) {
        int r = mpv_stream_cb_add_ro(mpv, "nxcurl", &pr, cs::cb_open);
        if (r < 0) { log(std::string("stream_cb_add_ro: ") + mpv_error_string(r)); useCurl = false; }
    }
    if (useCurl) load("nxcurl://video" + url_ext(playUrl));
    else load(playUrl);
    log(useCurl ? "modo: curl" : "modo: nativo");

    double lastPos = 0, lastDur = 0;
    Uint32 lastSave = SDL_GetTicks(), toast = 0;
    auto save = [&](bool finished) {  // grava o ponto de saida (terminou = volta ao inicio na proxima vez)
        if (ctx.key.empty()) return;
        bool fin = finished || (lastDur > 60 && lastPos > lastDur - 45);
        if (fin) { if (hist_find(ctx.key) || lastPos > 10) hist_put(ctx.key, ctx.item, ctx.ss, ctx.ep, 0, lastDur); else return; }
        else if (lastPos > 10) hist_put(ctx.key, ctx.item, ctx.ss, ctx.ep, lastPos, lastDur);
        else return;
        hist_save();
    };
    bool run = true, started = false, fellBack = !useCurl;  // fellBack: ja esta no modo nativo
    std::string erro, lastLog;
    Uint32 ui = SDL_GetTicks(), t0 = SDL_GetTicks();
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
            if (e->event_id == MPV_EVENT_FILE_LOADED || e->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                if (!started) {
                    mpv_request_log_messages(mpv, "warn");  // tocando: so avisos/erros
                    if (resume > 0) toast = SDL_GetTicks() + 3500;
                }
                started = true;
            }
            if (e->event_id == MPV_EVENT_LOG_MESSAGE) {
                auto* m = (mpv_event_log_message*)e->data;
                if (m && m->text) {
                    log(std::string("[") + m->level + "] " + m->prefix + ": " + m->text, !started);
                    if (!strcmp(m->level, "error") || !strcmp(m->level, "fatal")) {
                        lastLog = m->text;
                        while (!lastLog.empty() && (lastLog.back() == '\n' || lastLog.back() == '\r')) lastLog.pop_back();
                    }
                }
            }
            if (e->event_id == MPV_EVENT_END_FILE) {
                auto* ef = (mpv_event_end_file*)e->data;
                int reason = ef ? ef->reason : MPV_END_FILE_REASON_EOF;
                if (reason == MPV_END_FILE_REASON_REDIRECT) continue;
                std::string why;
                if (reason == MPV_END_FILE_REASON_ERROR) why = mpv_error_string(ef->error);
                else if (reason == MPV_END_FILE_REASON_EOF && !started) why = "o servidor nao enviou dados do video";
                log("fim: " + (why.empty() ? std::string("normal") : why));
                if (reason == MPV_END_FILE_REASON_EOF && started) ended = true;
                if (!why.empty() && !fellBack) {
                    // o modo curl falhou: tenta uma vez com o ffmpeg nativo do mpv
                    fellBack = true;
                    curlErr = why;
                    started = false;
                    lastLog.clear();
                    status("Tentando modo alternativo...");
                    load(playUrl);
                    t0 = SDL_GetTicks();
                    log("modo: nativo (fallback)");
                    continue;
                }
                if (!why.empty()) erro = (curlErr.empty() || curlErr == why) ? why : (why + " (curl: " + curlErr + ")");
                run = false;
            }
        }
        if (!started && !erro.size() && SDL_GetTicks() - t0 > 60000) { erro = "tempo esgotado ao abrir o video"; run = false; }

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

        if (pos > 0) lastPos = pos;
        if (dur > 0) lastDur = dur;
        if (started && !pause && lastPos > 10 && SDL_GetTicks() - lastSave > 30000) { save(false); lastSave = SDL_GetTicks(); }

        if (!started || wait) draw_spinner(ren, 640, 360, 96);  // conectando / buffer: so o circulo (B cancela)
        if (SDL_GetTicks() < toast) {
            rect(ren, 48, 36, 360, 44, 0, 0, 0, 170);
            txt(ren, 0, "Continuando de " + fmt(resume), 64, 46);
        }
        if (pause || SDL_GetTicks() - ui < 3500) {
            rect(ren, 0, 600, 1280, 120, 0, 0, 0, 180);
            txt(ren, 1, fit(1, title, 1184), 48, 612);
            rect(ren, 48, 668, 1184, 6, 90, 90, 90, 255);
            if (dur > 0) rect(ren, 48, 668, (int)(1184 * std::min(1.0, pos / dur)), 6, 229, 9, 20, 255);
            txt(ren, 0, fmt(pos) + " / " + fmt(dur) + (pause ? "   PAUSADO" : ""), 48, 682, GRAY);
        }
        SDL_RenderPresent(ren);
    }

    save(ended);
    mpv_render_context_free(rc);
    mpv_terminate_destroy(mpv);
    SDL_DestroyTexture(tex);

    if (!erro.empty()) {
        std::string m = erro;
        if (!lastLog.empty() && lastLog != erro) m += "\n\nmpv: " + lastLog;
        m += "\n\nDetalhes em sdmc:/switch/netflix-player/log.txt";
        show_msg(ren, pad, "Erro ao reproduzir", m);
    }
    return ended;
}


// ---------- abertura: video da Netflix (romfs:/intro.mp4) ----------
// Copia o video do romfs para o SD (1 vez) e toca com o mpv. Qualquer falha = pula a abertura em silencio.
static bool prep_intro(std::string& path) {
    struct stat sb;
    if (stat(BASE "intro.mp4", &sb) == 0 && sb.st_size > 0) { path = BASE "intro.mp4"; return true; }  // trocar o video: coloque o seu aqui
    FILE* in = fopen("romfs:/intro.mp4", "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    const char* cp = BASE "cache/intro.mp4";
    if (stat(cp, &sb) == 0 && sb.st_size == sz) { fclose(in); path = cp; return true; }
    FILE* out = fopen(cp, "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(65536);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    fclose(in);
    fclose(out);
    if (!ok) { remove(cp); return false; }
    path = cp;
    return true;
}
static void play_intro(SDL_Renderer* ren, PadState& pad) {
    g_a = 1.f;
    std::string path;
    if (!prep_intro(path)) return;
    std::vector<std::string> cand{path};
    if (path.rfind("sdmc:", 0) == 0) cand.push_back(path.substr(5));  // alternativa: caminho sem "sdmc:"
    CpuBoost boost;
    mpv_handle* mpv = mpv_create();
    if (!mpv) return;
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "no");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "load-scripts", "no");
    mpv_set_option_string(mpv, "sid", "no");
    mpv_set_option_string(mpv, "audio-channels", "stereo");
    mpv_set_option_string(mpv, "vd-lavc-threads", "3");
    mpv_set_option_string(mpv, "framedrop", "vo");
    mpv_set_option_string(mpv, "terminal", "no");
    if (mpv_initialize(mpv) < 0) { mpv_terminate_destroy(mpv); return; }
    mpv_render_param ip[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rc = nullptr;
    if (mpv_render_context_create(&rc, mpv, ip) < 0) { mpv_terminate_destroy(mpv); return; }

    const int W = 1280, H = 720;  // o video e 720p: sem reescala
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_UpdateTexture(tex, nullptr, px.data(), W * 4);
    size_t ci = 0;
    auto load = [&]() { const char* cmd[] = {"loadfile", cand[ci].c_str(), nullptr}; mpv_command(mpv, cmd); };
    load();

    bool run = true, started = false;
    Uint32 tStart = SDL_GetTicks(), t0 = tStart;
    while (run && appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_A | HidNpadButton_B)) break;  // A/B pula a abertura
        for (;;) {
            mpv_event* e = mpv_wait_event(mpv, 0);
            if (e->event_id == MPV_EVENT_NONE) break;
            if (e->event_id == MPV_EVENT_SHUTDOWN) run = false;
            if (e->event_id == MPV_EVENT_FILE_LOADED || e->event_id == MPV_EVENT_PLAYBACK_RESTART) started = true;
            if (e->event_id == MPV_EVENT_END_FILE) {
                auto* ef = (mpv_event_end_file*)e->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_REDIRECT) continue;
                if (!started && ++ci < cand.size()) { load(); t0 = SDL_GetTicks(); continue; }
                run = false;
            }
        }
        if ((!started && SDL_GetTicks() - t0 > 8000) || SDL_GetTicks() - tStart > 20000) break;  // seguranca
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
        SDL_RenderPresent(ren);
    }
    mpv_render_context_free(rc);
    mpv_terminate_destroy(mpv);
    SDL_DestroyTexture(tex);
}

// ---------- tela de detalhes (sinopse, temporadas e episodios) ----------
static void detail(SDL_Renderer* ren, PadState& pad, Item* it) {
    const int ROWS = 5;
    int ss = 0, ep = 0, top = 0;
    auto syn = wrap(it->sinopse.empty() ? "Sem descricao." : it->sinopse, 60);
    std::string meta;
    for (const std::string* p : {&it->ano, &it->genero, &it->duracao})
        if (!p->empty()) meta += (meta.empty() ? "" : "  |  ") + *p;
    if (it->serie())
        meta += (meta.empty() ? "" : "  |  ") + std::to_string(it->seasons.size()) + (it->seasons.size() == 1 ? " temporada" : " temporadas");
    const SDL_Color LIGHT{200, 200, 200, 255}, BLACK{0, 0, 0, 255};
    g_a = 1.f;
    if (it->serie()) {  // ja assistiu? abre no ultimo episodio visto
        const Prog* lp = last_prog(it->titulo);
        if (lp && lp->ss >= 0 && lp->ss < (int)it->seasons.size() && lp->ep >= 0 && lp->ep < (int)it->seasons[lp->ss].eps.size()) {
            ss = lp->ss; ep = lp->ep; top = std::max(0, ep - ROWS + 1);
        }
    }
    bool dirty = true;
    Uint32 last = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_B) return;
        if (d) dirty = true;

        if (!it->serie()) {
            if (d & HidNpadButton_A) { play(ren, pad, it->titulo, it->url, PlayCtx{it->url, it->titulo, -1, -1}); dirty = true; }
        } else {
            int ns = (int)it->seasons.size();
            if ((d & HidNpadButton_Left) && ss > 0) { ss--; ep = 0; top = 0; }
            if ((d & HidNpadButton_Right) && ss + 1 < ns) { ss++; ep = 0; top = 0; }
            int ne = (int)it->seasons[ss].eps.size();
            if ((d & HidNpadButton_Up) && ep > 0) ep--;
            if ((d & HidNpadButton_Down) && ep + 1 < ne) ep++;
            if (ep < top) top = ep;
            if (ep >= top + ROWS) top = ep - ROWS + 1;
            if ((d & HidNpadButton_A) && ne > 0) {
                for (;;) {  // ao terminar o episodio, passa para o proximo (inclusive na proxima temporada)
                    auto& S = it->seasons[ss];
                    std::string t = it->titulo + "  -  T" + std::to_string(ss + 1) + " E" + std::to_string(ep + 1) + "  " + S.eps[ep].titulo;
                    if (!play(ren, pad, t, S.eps[ep].url, PlayCtx{S.eps[ep].url, it->titulo, ss, ep})) break;
                    if (ep + 1 < (int)S.eps.size()) ep++;
                    else if (ss + 1 < ns && !it->seasons[ss + 1].eps.empty()) { ss++; ep = 0; }
                    else break;
                }
                ne = (int)it->seasons[ss].eps.size();
                if (ep < top) top = ep;
                if (ep >= top + ROWS) top = ep - ROWS + 1;
                dirty = true;
            }
        }

        if (SDL_GetTicks() - last > 1000) dirty = true;  // refresh de seguranca (volta do menu HOME)
        if (!dirty) { SDL_Delay(30); continue; }
        dirty = false;
        last = SDL_GetTicks();

        SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
        SDL_RenderClear(ren);
        if (it->tex) { SDL_Rect dr{480, 0, 800, 360}; SDL_RenderCopy(ren, it->tex, nullptr, &dr); }
        if (OV) { SDL_Rect o{0, 0, 1280, 360}; SDL_RenderCopy(ren, OV, nullptr, &o); }
        else rect(ren, 0, 0, 480, 360, 20, 20, 20, 255);

        draw_logo(ren, 48, 8, 44);
        draw_title(ren, it->titulo, 48, 62, 54, 44, 700);
        txt(ren, 0, meta, 48, 146, GRAY);
        int y = 180;
        for (size_t i = 0; i < syn.size() && i < 7; i++, y += 24) txt(ren, 0, syn[i], 48, y, LIGHT);

        if (!it->serie()) {
            rect(ren, 48, 400, 220, 52, 255, 255, 255, 255);
            txt(ren, 1, item_prog(it) >= 0 ? "A  Continuar" : "A  Assistir", 68, 410, BLACK);
            txt(ren, 0, "B  Voltar", 48, 690, GRAY);
        } else {
            int ns = (int)it->seasons.size();
            int tfirst = std::max(0, std::min(ss - 2, ns - 5));  // abas de temporada (5 por vez)
            for (int i = tfirst; i < std::min(ns, tfirst + 5); i++) {
                int x = 48 + (i - tfirst) * 236;
                bool sel = i == ss;
                rect(ren, x, 372, 228, 40, sel ? 255 : 55, sel ? 255 : 55, sel ? 255 : 55, 255);
                SDL_Rect c{x, 372, 228, 40};
                SDL_RenderSetClipRect(ren, &c);
                txt(ren, 1, fit(1, it->seasons[i].nome, 204), x + 12, 378, sel ? BLACK : WHITE);
                SDL_RenderSetClipRect(ren, nullptr);
            }
            auto& S = it->seasons[ss];
            if (S.eps.empty()) txt(ren, 1, "Nenhum episodio nesta temporada", 48, 440, GRAY);
            for (int i = top; i < std::min((int)S.eps.size(), top + ROWS); i++) {
                int ry = 424 + (i - top) * 52;
                bool sel = i == ep;
                rect(ren, 48, ry, 1184, 48, sel ? 255 : 40, sel ? 255 : 40, sel ? 255 : 40, 255);
                SDL_Rect c{48, ry, 1184, 48};
                SDL_RenderSetClipRect(ren, &c);
                txt(ren, 1, fit(1, std::to_string(i + 1) + "   " + S.eps[i].titulo, 1120), 64, ry + 10, sel ? BLACK : WHITE);
                SDL_RenderSetClipRect(ren, nullptr);
            }
            txt(ren, 0, "Cima/Baixo episodio   Esq/Dir temporada   A assistir   B voltar", 48, 692, GRAY);
        }
        SDL_RenderPresent(ren);
    }
}

// ---------- main ----------
int main() {
    socketInitializeDefault();
    sslInitialize(0x3);  // servico SSL do Switch: o ffmpeg do mpv usa isto para HTTPS no modo nativo
    romfsInit();         // intro.mp4 e logo.png vao dentro do .nro
    plInitialize(PlServiceType_User);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    SDL_Init(SDL_INIT_VIDEO);
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    TTF_Init();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    mkdir("sdmc:/switch/netflix-player", 0777);
    mkdir("sdmc:/switch/netflix-player/cache", 0777);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* win = SDL_CreateWindow("player", 0, 0, 1280, 720, SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    OV = make_overlay(ren);
    SPIN = make_spinner(ren);
    LOGO = load_logo(ren);

    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);
    int sizes[4] = {18, 26, 56, 40};
    for (int i = 0; i < 4; i++) F[i] = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 0, sizes[i]);

    // Abertura: enquanto o video da Netflix roda, o JSON e as primeiras capas carregam por tras.
    std::vector<Item*> items;
    std::string err;
    std::atomic<int> boot{0};
    std::thread th(cover_worker, &items);  // espera g_items_ready
    std::thread bt([&] {
        bool o = load_items(items, err);
        if (o) {
            hist_load();
            std::vector<int> want = recent_idx(items);
            for (int i = 0; i < (int)items.size() && i < 10; i++) want.push_back(i);
            { std::lock_guard<std::mutex> l(g_mx); g_want = want; }
            g_items_ready = true;
        }
        boot = o ? 1 : 2;
    });
    play_intro(ren, pad);
    bool quit = false;
    while (boot == 0 && appletMainLoop()) {  // se o JSON ainda nao chegou: so o circulo vermelho
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) { quit = true; break; }
        spin_frame(ren);
    }
    bt.join();
    bool ok = boot == 1;

    std::vector<Row> rows;
    int row = 0, col = 0, vtop = 0, first[3] = {0, 0, 0};
    std::vector<char> vis;
    std::vector<int> want;
    if (ok && !quit) {
        rows = build_rows(items);
        Uint32 w0 = SDL_GetTicks();  // espera (atras do circulo) as capas da primeira tela: sem "pop-in" na animacao
        for (;;) {
            compute_vis(rows, row, col, vtop, first, items.size(), vis, want);
            { std::lock_guard<std::mutex> l(g_mx); g_want = want; }
            bool made = manage_tex(ren, items, vis), pending = false;
            for (size_t i = 0; i < items.size(); i++) if (vis[i] && items[i]->st < 2) pending = true;
            if (!made && (!pending || SDL_GetTicks() - w0 > 2500 || !appletMainLoop())) break;
            spin_frame(ren);
        }
    }

    auto ez = [](float x) { x = std::max(0.f, std::min(1.f, x)); return 1.f - (1.f - x) * (1.f - x) * (1.f - x); };
    Uint32 rev0 = SDL_GetTicks();  // inicio da animacao de entrada dos icones
    bool dirty = true;             // so redesenha quando algo muda (menos CPU/GPU parado no menu)
    Uint32 lastDraw = 0;
    while (!quit && appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_Plus) break;
        if (d) dirty = true;

        int n = (int)items.size(), nr = (int)rows.size();
        if (ok) {
            if (d & HidNpadButton_Up) row = std::max(0, row - 1);
            if (d & HidNpadButton_Down) row = std::min(nr - 1, row + 1);
            if (d & HidNpadButton_Left) col--;
            if (d & HidNpadButton_Right) col++;
            col = std::max(0, std::min(col, (int)rows[row].idx.size() - 1));
            if (row < vtop) vtop = row;           // 2 fileiras visiveis por vez
            if (row > vtop + 1) vtop = row - 1;
            if (col < first[row]) first[row] = col;
            if (col > first[row] + 4) first[row] = col - 4;
            if (d & HidNpadButton_A) {
                int ver = g_hist_ver;
                detail(ren, pad, items[rows[row].idx[col]]);
                rows = build_rows(items);
                nr = (int)rows.size();
                if (g_hist_ver != ver) { row = 0; col = 0; vtop = 0; first[0] = first[1] = first[2] = 0; }  // o que acabou de ver vai para o inicio
                row = std::min(row, nr - 1);
                col = std::max(0, std::min(col, (int)rows[row].idx.size() - 1));
                dirty = true;
            }
            compute_vis(rows, row, col, vtop, first, n, vis, want);
            { std::lock_guard<std::mutex> l(g_mx); g_want = want; }
            if (manage_tex(ren, items, vis)) dirty = true;
        }

        Uint32 T = SDL_GetTicks() - rev0;
        bool rev = ok && T < 1500;
        if (rev) dirty = true;
        if (SDL_GetTicks() - lastDraw > 1000) dirty = true;  // refresh de seguranca (ex.: voltou do menu HOME)
        if (!dirty) { SDL_Delay(30); continue; }
        dirty = false;
        lastDraw = SDL_GetTicks();

        SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
        SDL_RenderClear(ren);

        if (!ok) {
            g_a = 1.f;
            txt(ren, 1, err, 48, 320);
            txt(ren, 0, "Coloque videos.json (ou videos_url.txt) em sdmc:/switch/netflix-player/  |  + sai", 48, 370, GRAY);
            SDL_RenderPresent(ren);
            continue;
        }

        Item* cur = items[rows[row].idx[col]];
        g_a = rev ? ez(T / 600.f) : 1.f;  // destaque aparece suave...
        draw_hero(ren, cur, item_prog(cur) >= 0);
        int ly[2] = {366, 538}, cy[2] = {402, 572};
        for (int sl = 0; sl < 2 && vtop + sl < nr; sl++) {  // ...depois as fileiras e os cards, um a um, subindo
            int r = vtop + sl;
            const Row& R = rows[r];
            g_a = rev ? ez((T - 250.f - sl * 150) / 400.f) : 1.f;
            txt(ren, 1, R.label, 48, ly[sl]);
            for (int i = first[r], k = 0; i < std::min((int)R.idx.size(), first[r] + 5); i++, k++) {
                float p = rev ? ez((T - 350.f - sl * 150 - k * 80) / 500.f) : 1.f;
                g_a = p;
                Item* it = items[R.idx[i]];
                draw_card(ren, it, 48 + k * 232, cy[sl] + (int)((1.f - p) * 28), r == row && i == col, R.cont ? item_prog(it) : -1.f);
            }
        }
        g_a = 1.f;
        SDL_RenderPresent(ren);
    }

    g_quit = true;
    th.join();
    curl_global_cleanup();
    TTF_Quit(); IMG_Quit(); SDL_Quit();
    plExit(); romfsExit(); sslExit(); socketExit();
    return 0;
}
