# Netflix Player (Switch .nro)

<img width="656" height="304" alt="images" src="https://github.com/user-attachments/assets/a0709690-7b9d-472b-88c1-1ff2c4c660e8" />


Player de vídeo em streaming (buffer em RAM, sem baixar o arquivo inteiro) com visual estilo Netflix e capas.

## Como usar
1. Suba esta pasta num repositório GitHub (branch `main`).
2. Aba **Actions** > *Build NRO* > baixe o artefato `netflix-player`.
3. Copie a pasta `switch/` para a raiz do cartão SD.
4. Edite `sdmc:/switch/netflix-player/videos.json` (formato abaixo).
   - Alternativa: crie `videos_url.txt` com uma única linha contendo a URL de um JSON online (ex.: GitHub raw). Ele tem prioridade sobre o local.

## videos.json
```json
{ "videos": [ { "titulo": "Nome", "url": "https://.../video.mp4", "capa": "https://.../capa.jpg" } ] }
```
`capa` é opcional. `url` precisa ser link direto (mp4, mkv, m3u8).

## Filmes e séries (tela de detalhes)
Ao apertar **A** num título abre a tela de detalhes (sinopse, ano, gênero, duração). Campos extras, todos opcionais:
```json
{ "titulo": "Filme", "url": "gdrive:ID", "capa": "...", "sinopse": "...", "ano": 2015, "genero": "Romance", "duracao": "1h 52min" }
```
Série: troque `url` por `temporadas` (o editor-videos.html monta isso para você):
```json
{ "titulo": "Minha Série", "sinopse": "...",
  "temporadas": [ { "nome": "Temporada 1", "episodios": [ { "titulo": "Piloto", "url": "gdrive:ID" } ] } ] }
```
Na série: **↑/↓** episódio, **←/→** temporada, **A** assiste, **B** volta. Ao acabar um episódio o próximo começa sozinho.

## Google Drive
- No Drive: botão direito no arquivo > Compartilhar > **"Qualquer pessoa com o link"**.
- Cole no JSON o link de compartilhamento normal (`.../file/d/ID/view`) ou só `gdrive:ID`. O app converte sozinho, para vídeo e para capa (capa = imagem .jpg/.png no Drive).
- O Drive tem cota de acessos: se um arquivo muito acessado der erro, tire uma cópia para o seu Drive e use a cópia.
- Pastas inteiras não são suportadas: um link por arquivo.

## Abertura, retomar e "Volte a assistir" (v0.3)
- **Abertura:** ao abrir o app toca o vídeo da Netflix (`romfs/intro.mp4`, dentro do próprio `.nro`). Enquanto ele roda, o JSON e as primeiras capas já carregam por trás; depois os ícones aparecem com animação suave. **A/B** pula a abertura. Para usar outro vídeo, coloque `sdmc:/switch/netflix-player/intro.mp4` no SD (ele vence o embutido).
- **Loading:** só o círculo vermelho girando, sem textos.
- **Retomar:** ao sair de um filme/episódio (B, ou fechando o app) o ponto fica salvo em `sdmc:/switch/netflix-player/progress.json` (também a cada 30 s). Ao abrir de novo ele volta de onde parou (3 s antes) e mostra "Continuando de ...". Terminou o filme = próxima vez começa do início.
- **Volte a assistir:** fileira no topo com os **últimos 5 títulos** vistos, com barra vermelha de progresso. Em série, abre já no último episódio visto. O botão vira **A Continuar** quando há ponto salvo.
- **Títulos:** nunca mais cortam letra no meio: se não couberem, viram "..." (no banner e na tela de detalhes quebram em até 2 linhas).
- **Logo:** o "N" no canto vem de `romfs/logo.png` (recortado do vídeo de abertura). Para trocar, substitua o arquivo ou coloque `logo.png` em `sdmc:/switch/netflix-player/`.

## Controles
- Menu: D-pad navega, **A** assiste, **+** sai
- Player: **A** pausa, **←/→** ±10 s, **ZL/ZR** ±1 min, **B** volta

## Ajustes (source/main.cpp, função `play`)
- `demuxer-max-bytes` = tamanho do buffer à frente (padrão 50MiB)
- `demuxer-readahead-secs` = segundos pré-carregados
- Renderização em 960x540 por software; aumente `W`/`H` se o aparelho aguentar.

## Se o vídeo não tocar
- O app agora mostra o motivo na tela e grava `sdmc:/switch/netflix-player/log.txt` (últimas linhas do mpv). Mande esse arquivo se continuar falhando.
- Vídeos http/https são baixados pelo libcurl (mesmo caminho das capas) e entregues ao mpv; `.m3u8` usa o ffmpeg nativo do mpv.

## Lentidão depois de alguns minutos
Corrigido: o download reconectava só 6 vezes por filme; agora reconecta sempre que o servidor fechar a conexão. O clock da CPU sobe para 1785 MHz durante o vídeo (se o build der erro em `pcv`/`clkrst`, adicione `-DNO_CPU_BOOST` em `CFLAGS` no Makefile).

## Atalho / ícone
O `.nro` agora leva `icon.jpg` (256x256) e o NACP embutidos. Troque o `icon.jpg` pelo seu se quiser.

## Observações
- Decodificação é por software (`hwdec=no`): prefira H.264 até 720p.
- Se o job falhar no passo de dependências, confira o nome do pacote da libmpv no devkitPro (`dkp-pacman -Ss mpv`).

## Tela inicial (v0.4)
Visual de plataforma de streaming: fundo escuro, vermelho só nas ações principais, imagens grandes.

- **Cabeçalho**: logo + abas **Início, Filmes, Séries, Minha lista, Pesquisar**. Aperte **Cima** na primeira fileira para focar o cabeçalho; **Esquerda/Direita** (ou L/R) trocam de aba, **Baixo/A** voltam para os títulos.
- **Destaque (banner)**: muda sozinho conforme o título em foco (capa grande, título, ano/gênero/duração, sinopse, botão vermelho e botão da lista).
- **Fileiras por categoria**: Volte a assistir, Novidades, uma fileira por gênero (lidos do campo `genero` do JSON) e Todos os títulos. Mostram **4 cards grandes** por vez.
- **L / R** (ou ZL / ZR) trocam de **página** da fileira; o indicador `[L] ● ○ ○ [R]` e as setas nas bordas mostram que há mais títulos. O direcional anda de um em um e vira a página sozinho.
- **Minha lista**: **X** adiciona/remove o título em foco (salvo em `sdmc:/switch/netflix-player/lista.json`).
- **Pesquisa**: **Y** (ou aba Pesquisar + A) abre o teclado do Switch e busca em título, gênero e sinopse.
- **A** abre a tela de detalhes. `netflix-nx-simulador.html` mostra o mesmo layout no navegador (teclas: setas, `[` `]` = L/R, Enter = A, X, Y, Esc = B, P = +).
