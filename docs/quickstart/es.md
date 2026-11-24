> Esta página es una traducción de las guías rápidas en inglés, hecha con un modelo de IA y revisada por un segundo modelo de IA. Ningún hablante nativo la ha revisado todavía.

# Inicio rápido: Midi Fadester y Visual Sidechain

## Midi Fadester

Midi Fadester convierte los faders y potenciómetros de un controlador MIDI en controles en directo para OBS Studio: fundir entre escenas con un crossfader de verdad, mezclar dos capas, fundir una cámara, llevar un volumen, recorrer cualquier ajuste de un filtro. Está hecho para el Akai APC40 mkII y funciona con otros controladores mediante MIDI Learn.

### Inicio rápido

<img src="../../docs/quick-start.svg" alt="Midi Fadester en tres pasos: 1, ejecuta el instalador de la última versión e inicia OBS; 2, conecta el controlador (un APC40 mkII se configura solo); 3, activa el Modo Estudio y mueve el crossfader: Programa funde a Vista previa." width="900">

1. **Instálalo.** Desde la [última versión](https://github.com/jbrick2070/midi-fadester/releases/latest), ejecuta `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) o `midi-fadester-<version>-macos-universal.pkg` (Mac) e inicia OBS Studio 33 o posterior. Otros archivos y avisos: [Install](../../README.md#install) (en inglés).
2. **Conecta el controlador.** Un APC40 mkII se configura solo y se abre el panel **Fadester Desk**. Otro controlador: abre **Herramientas → Midi Fadester** (barra de menús de OBS, arriba) y haz clic en **Cambiar controlador** en la pestaña **Tiras**.
3. **Mueve el crossfader.** Haz clic en el botón **Modo Estudio** de OBS (abajo a la derecha) y lleva el crossfader hasta el otro extremo: Programa funde a lo que había en Vista previa.

Eso es todo. Para dar otra tarea a un fader, haz clic en **Configurar** debajo de su tira en **Herramientas → Midi Fadester**.

**[Guía completa (en inglés)](../../README.md)**

## Visual Sidechain

Visual Sidechain hace que tu vídeo se mueva con tu música en OBS Studio 33 en Windows y macOS. Forma parte de Midi Fadester desde la 0.9.0: instala Midi Fadester y ya lo tienes.

### Inicio rápido

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain en tres pasos: 1, instala Midi Fadester (Visual Sidechain viene incluido); 2, en los Filtros de una fuente pulsa + y añade Visual Sidechain: Filtro de shader; 3, pon música y la imagen se mueve con el sonido." width="900">

1. **Instala Midi Fadester** (ver [Install](../../README.md#install), en inglés) e inicia OBS Studio 33.
2. **Añade el filtro.** Haz clic derecho en una fuente o escena, elige **Filtros**, pulsa **`+`** debajo de los filtros de efectos y añade **`Visual Sidechain: Filtro de shader`**.
3. **Pon música.** La imagen se mueve con el sonido de **Pista 1 (mezcla principal)**.

Eso es todo. Elige otro aspecto en **Efecto**, en la ventana del filtro.

> **Antes de emitir, asigna una tecla de pánico:** **Ajustes → Atajos → `Visual Sidechain: Pánico (apaga todos los efectos)`**. Una pulsación apaga todos los efectos de Visual Sidechain en todas las escenas.

**[Guía completa (en inglés)](../VISUAL-SIDECHAIN.md)**
