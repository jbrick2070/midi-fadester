> Esta página é uma tradução dos guias rápidos em inglês, feita com um modelo de IA e verificada por um segundo modelo de IA. Nenhum falante nativo a revisou ainda.

# Início rápido: Midi Fadester e Visual Sidechain

## Midi Fadester

O Midi Fadester transforma os faders e botões giratórios de um controlador MIDI em controles ao vivo para o OBS Studio: fazer transição entre cenas com um crossfader de verdade, mixar duas camadas, fazer uma câmera aparecer e sumir, controlar um volume, variar qualquer ajuste de um filtro. Ele foi feito para o Akai APC40 mkII e funciona com outros controladores por meio do MIDI Learn.

### Início rápido

<img src="../../docs/quick-start.svg" alt="Midi Fadester em três passos: 1, execute o instalador da versão mais recente e abra o OBS; 2, conecte o controlador (um APC40 mkII se configura sozinho); 3, ative o Modo estúdio e mova o crossfader: o Programa faz a transição para a Pré-visualização." width="900">

1. **Instale.** Na [versão mais recente](https://github.com/jbrick2070/midi-fadester/releases/latest), execute `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) ou `midi-fadester-<version>-macos-universal.pkg` (Mac) e abra o OBS Studio 33 ou mais recente. Outros arquivos e avisos: [Install](../../README.md#install) (em inglês).
2. **Conecte o controlador.** Um APC40 mkII se configura sozinho e o painel **Fadester Desk** se abre. Outro controlador: abra **Ferramentas → Midi Fadester** (barra de menus do OBS, na parte superior) e clique em **Trocar controlador** na aba **Faixas**.
3. **Mova o crossfader.** Clique no botão **Modo estúdio** do OBS (canto inferior direito) e leve o crossfader até a outra ponta: o Programa faz a transição para o que estava na Pré-visualização.

Pronto. Para dar outra função a um fader, clique em **Configurar** abaixo da faixa dele em **Ferramentas → Midi Fadester**.

**[Guia completo (em inglês)](../../README.md)**

## Visual Sidechain

O Visual Sidechain faz seu vídeo se mexer com a sua música no OBS Studio 33 no Windows e no macOS. Ele faz parte do Midi Fadester desde a 0.9.0: instale o Midi Fadester e você já o tem.

### Início rápido

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain em três passos: 1, instale o Midi Fadester (o Visual Sidechain vem junto); 2, nos Filtros de uma fonte, aperte + e adicione Visual Sidechain: Filtro de shader; 3, toque música e a imagem se mexe com o som." width="900">

1. **Instale o Midi Fadester** (veja [Install](../../README.md#install), em inglês) e abra o OBS Studio 33.
2. **Adicione o filtro.** Clique com o botão direito em uma fonte ou cena, escolha **Filtros**, aperte **`+`** abaixo dos filtros de efeitos e adicione **`Visual Sidechain: Filtro de shader`**.
3. **Toque música.** A imagem se mexe com o som da **Faixa 1 (mix principal)**.

Pronto. Escolha outro visual em **Efeito**, na janela do filtro.

> **Antes de entrar ao vivo, defina uma tecla de pânico:** **Configurações → Teclas de atalho → `Visual Sidechain: Pânico (desliga todos os efeitos)`**. Apertar a tecla uma vez desliga todos os efeitos do Visual Sidechain em todas as cenas.

**[Guia completo (em inglês)](../VISUAL-SIDECHAIN.md)**
