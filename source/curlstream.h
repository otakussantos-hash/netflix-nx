// Stream HTTP(S) para o mpv usando libcurl (o mesmo que ja baixa o JSON e as capas).
//
// Por que existe: o ffmpeg do devkitPro faz HTTPS pelo servico "ssl" do Switch,
// que precisa de sslInitialize() e nao passa pelo curl/mbedtls. Quando isso falha o
// mpv nao abre o video, mesmo com o curl funcionando para as capas. Aqui o mpv so
// recebe bytes ja baixados pelo curl (protocolo "nxcurl://"), com suporte a seek
// (HTTP Range) e reconexao automatica.
#pragma once
#include <curl/curl.h>
#include <mpv/stream_cb.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cs {

static const size_t RING_MAX = 4u << 20;  // bytes baixados a frente do que o mpv leu (o mpv tem o proprio cache)

// Resultado do teste inicial de conexao (feito antes de abrir o video)
struct Probe {
    std::string url;
    int64_t size = -1;       // tamanho total, -1 se desconhecido
    bool seekable = false;   // servidor aceita Range
    long code = 0;
    std::string err;         // preenchido se falhar
};

struct Stream {
    Probe info;
    std::mutex m;
    std::condition_variable cv_data, cv_space, cv_req;
    std::deque<std::vector<uint8_t>> q;
    size_t front_off = 0, buffered = 0;
    int64_t pos = 0;          // posicao que o mpv vai ler em seguida
    uint64_t gen = 0;         // muda a cada seek: transferencias antigas se cancelam
    int64_t req_start = -1;   // pedido pendente para a thread de download
    bool eof = false, err = false, quit = false, cancel = false;
    std::thread th;
};

// ---------- teste de conexao ----------
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
struct ProbeAcc { long code = 0; int64_t total = -1, clen = -1; };
static size_t probe_hdr(char* b, size_t s, size_t n, void* u) {
    auto* p = (ProbeAcc*)u;
    std::string l(b, s * n), lo = lower(l);
    if (lo.compare(0, 5, "http/") == 0) {  // novo salto (redirect) -> recomeca
        *p = ProbeAcc();
        long c = 0;
        size_t sp = l.find(' ');
        if (sp != std::string::npos) c = atol(l.c_str() + sp + 1);
        p->code = c;
    } else if (lo.compare(0, 14, "content-range:") == 0) {
        size_t sl = lo.find('/');
        if (sl != std::string::npos && sl + 1 < lo.size() && isdigit((unsigned char)lo[sl + 1]))
            p->total = atoll(lo.c_str() + sl + 1);
    } else if (lo.compare(0, 15, "content-length:") == 0) {
        p->clen = atoll(lo.c_str() + 15);
    }
    return s * n;
}
static size_t probe_wr(char*, size_t s, size_t n, void* u) {
    auto* p = (ProbeAcc*)u;
    return p->code == 206 ? s * n : 0;  // 200 = servidor ignorou o Range: nao baixa o arquivo todo so para testar
}

static void apply_common(CURL* c, const std::string& url) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 NetflixPlayerNX/0.2");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
}

// Pede 1 byte (Range 0-0) para descobrir tamanho e se aceita seek.
static bool probe(const std::string& url, Probe& out) {
    out = Probe();
    out.url = url;
    CURL* c = curl_easy_init();
    if (!c) { out.err = "curl indisponivel"; return false; }
    ProbeAcc acc;
    apply_common(c, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &acc);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &acc);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (code == 0) code = acc.code;
    out.code = code;

    bool got_headers = code != 0;
    if (!got_headers) {
        out.err = std::string("Sem conexao: ") + curl_easy_strerror(rc);
        return false;
    }
    if (code >= 400) {
        out.err = "Servidor respondeu HTTP " + std::to_string(code);
        return false;
    }
    if (code == 206 && acc.total > 0) { out.seekable = true; out.size = acc.total; }
    else { out.seekable = false; out.size = acc.clen > 0 ? acc.clen : -1; }
    return true;
}

// ---------- download (thread) ----------
struct Xfer {
    Stream* S;
    uint64_t g;
    CURL* c = nullptr;
    int64_t start = 0, received = 0;
    bool checked = false, bad = false;
    long code = 0;
};
static size_t xf_write(char* p, size_t s, size_t n, void* u) {
    auto* x = (Xfer*)u;
    Stream* S = x->S;
    size_t len = s * n;
    if (!len) return 0;
    if (!x->checked) {
        x->checked = true;
        curl_easy_getinfo(x->c, CURLINFO_RESPONSE_CODE, &x->code);
        if (x->code >= 400 || (x->start > 0 && x->code != 206)) { x->bad = true; return 0; }
    }
    std::unique_lock<std::mutex> lk(S->m);
    S->cv_space.wait(lk, [&] { return S->gen != x->g || S->quit || S->buffered < RING_MAX; });
    if (S->gen != x->g || S->quit) return 0;
    S->q.emplace_back((uint8_t*)p, (uint8_t*)p + len);
    S->buffered += len;
    x->received += (int64_t)len;
    S->cv_data.notify_all();
    return len;
}
static int xf_prog(void* u, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* x = (Xfer*)u;
    std::lock_guard<std::mutex> lk(x->S->m);
    return (x->S->gen != x->g || x->S->quit) ? 1 : 0;
}
static bool stale(Stream* S, uint64_t g) {
    std::lock_guard<std::mutex> lk(S->m);
    return S->gen != g || S->quit;
}
// Baixa a partir de "start"; se a conexao cair, retoma de onde parou.
// So conta como "falha" a conexao que nao trouxe nenhum byte novo: o servidor fecha conexoes
// paradas (buffer cheio) de tempos em tempos e isso e normal, reconecta na hora sem limite.
static bool transfer(Stream* S, int64_t start, uint64_t g) {
    Xfer x;
    x.S = S;
    x.g = g;
    int fails = 0;
    for (;;) {
        int64_t from = start + x.received;
        if (S->info.size > 0 && from >= S->info.size) return true;
        int64_t before = x.received;
        x.start = from;
        x.checked = x.bad = false;
        CURL* c = curl_easy_init();
        if (!c) return false;
        x.c = c;
        apply_common(c, S->info.url);
        curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 131072L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, xf_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xf_prog);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
        if (from > 0) {
            char r[40];
            snprintf(r, sizeof r, "%lld-", (long long)from);
            curl_easy_setopt(c, CURLOPT_RANGE, r);
        }
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (stale(S, g)) return false;
        if (rc == CURLE_OK && !(S->info.size > 0 && start + x.received < S->info.size)) return true;
        if (x.bad) return false;                                // HTTP 4xx/5xx: repetir nao adianta
        if (!S->info.seekable && x.received > 0) return false;  // sem Range nao da para retomar
        if (x.received > before) { fails = 0; continue; }       // estava saudavel: reconecta ja
        if (++fails > 5) return false;
        for (int i = 0; i < 10 * fails; i++) {                  // espera 1s, 2s, 3s... sem travar o cancelamento
            if (stale(S, g)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
static void worker(Stream* S) {
    std::unique_lock<std::mutex> lk(S->m);
    for (;;) {
        S->cv_req.wait(lk, [&] { return S->quit || S->req_start >= 0; });
        if (S->quit) return;
        int64_t start = S->req_start;
        S->req_start = -1;
        uint64_t g = S->gen;
        lk.unlock();
        bool ok = transfer(S, start, g);
        lk.lock();
        if (S->gen == g && !S->quit) {
            if (ok) S->eof = true; else S->err = true;
            S->cv_data.notify_all();
        }
    }
}

// ---------- callbacks do mpv ----------
static int64_t cb_read(void* ck, char* buf, uint64_t n) {
    auto* S = (Stream*)ck;
    std::unique_lock<std::mutex> lk(S->m);
    S->cv_data.wait(lk, [&] { return S->buffered > 0 || S->eof || S->err || S->quit || S->cancel; });
    if (S->buffered == 0) return S->eof ? 0 : -1;
    size_t got = 0;
    while (got < n && !S->q.empty()) {
        auto& f = S->q.front();
        size_t t = std::min(f.size() - S->front_off, (size_t)(n - got));
        memcpy(buf + got, f.data() + S->front_off, t);
        got += t;
        S->front_off += t;
        if (S->front_off == f.size()) { S->q.pop_front(); S->front_off = 0; }
    }
    S->buffered -= got;
    S->pos += (int64_t)got;
    S->cv_space.notify_all();
    return (int64_t)got;
}
static void drop_bytes(Stream* S, size_t n) {  // chamar com o mutex
    while (n > 0 && !S->q.empty()) {
        auto& f = S->q.front();
        size_t t = std::min(f.size() - S->front_off, n);
        S->front_off += t;
        n -= t;
        S->buffered -= t;
        if (S->front_off == f.size()) { S->q.pop_front(); S->front_off = 0; }
    }
}
static int64_t cb_seek(void* ck, int64_t off) {
    auto* S = (Stream*)ck;
    std::lock_guard<std::mutex> lk(S->m);
    if (off < 0 || (S->info.size >= 0 && off > S->info.size)) return MPV_ERROR_GENERIC;
    if (off == S->pos) return off;
    if (off > S->pos && (uint64_t)(off - S->pos) <= S->buffered) {  // pulo curto: ja esta no buffer
        drop_bytes(S, (size_t)(off - S->pos));
        S->pos = off;
        S->cv_space.notify_all();
        return off;
    }
    S->gen++;                      // cancela a transferencia atual
    S->q.clear();
    S->front_off = 0;
    S->buffered = 0;
    S->err = false;
    S->pos = off;
    if (S->info.size >= 0 && off >= S->info.size) {  // seek para o fim: nada a baixar
        S->eof = true;
        S->req_start = -1;
    } else {
        S->eof = false;
        S->req_start = off;
        S->cv_req.notify_one();
    }
    S->cv_space.notify_all();
    S->cv_data.notify_all();
    return off;
}
static int64_t cb_size(void* ck) {
    auto* S = (Stream*)ck;
    return S->info.size >= 0 ? S->info.size : MPV_ERROR_UNSUPPORTED;
}
static void cb_cancel(void* ck) {
    auto* S = (Stream*)ck;
    std::lock_guard<std::mutex> lk(S->m);
    S->cancel = true;
    S->cv_data.notify_all();
    S->cv_space.notify_all();
}
static void cb_close(void* ck) {
    auto* S = (Stream*)ck;
    {
        std::lock_guard<std::mutex> lk(S->m);
        S->quit = true;
        S->cv_data.notify_all();
        S->cv_space.notify_all();
        S->cv_req.notify_all();
    }
    if (S->th.joinable()) S->th.join();
    delete S;
}

// O mpv chama isto ao abrir "nxcurl://...". user_data = Probe* feito antes (play()).
static int cb_open(void* user_data, char* /*uri*/, mpv_stream_cb_info* info) {
    auto* pr = (Probe*)user_data;
    auto* S = new Stream;
    S->info = *pr;
    S->gen = 1;
    S->req_start = 0;
    S->th = std::thread(worker, S);
    info->cookie = S;
    info->read_fn = cb_read;
    info->seek_fn = S->info.seekable ? cb_seek : nullptr;
    info->size_fn = S->info.size >= 0 ? cb_size : nullptr;
    info->close_fn = cb_close;
    info->cancel_fn = cb_cancel;
    return 0;
}

}  // namespace cs
