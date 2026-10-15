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

## Observações
- Decodificação é por software (`hwdec=no`): prefira H.264 até 720p.
- Se o job falhar no passo de dependências, confira o nome do pacote da libmpv no devkitPro (`dkp-pacman -Ss mpv`).
