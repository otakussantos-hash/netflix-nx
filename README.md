# Netflix Player (Switch .nro)

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
