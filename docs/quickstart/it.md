> Questa pagina è una traduzione delle guide rapide in inglese, fatta con un modello di IA e controllata da un secondo modello di IA. Nessun madrelingua l'ha ancora rivista.

# Guida rapida: Midi Fadester e Visual Sidechain

## Midi Fadester

Midi Fadester trasforma i fader e le manopole di un controller MIDI in comandi dal vivo per OBS Studio: sfumare tra le scene con un vero crossfader, miscelare due livelli, far apparire una telecamera in dissolvenza, regolare un volume, muovere qualsiasi impostazione di un filtro. È pensato per l'Akai APC40 mkII e funziona con altri controller tramite MIDI Learn.

### Guida rapida

<img src="../../docs/quick-start.svg" alt="Midi Fadester in tre passi: 1, avvia il programma di installazione dell'ultima versione e apri OBS; 2, collega il controller (un APC40 mkII si configura da solo); 3, attiva la modalità studio e sposta il crossfader: il Programma sfuma nell'Anteprima." width="900">

1. **Installalo.** Dall'[ultima versione](https://github.com/jbrick2070/midi-fadester/releases/latest), avvia `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) o `midi-fadester-<version>-macos-universal.pkg` (Mac), poi apri OBS Studio 33 o successivo. Altri file e avvisi: [Install](../../README.md#install) (in inglese).
2. **Collega il controller.** Un APC40 mkII si configura da solo e si apre il pannello **Fadester Desk**. Un altro controller: apri **Strumenti → Midi Fadester** (barra dei menu di OBS, in alto) e clicca **Cambia controller** nella scheda **Strisce**.
3. **Sposta il crossfader.** Clicca il pulsante **Modalità studio** di OBS (in basso a destra), poi porta il crossfader all'altro estremo: il Programma sfuma in quello che era nell'Anteprima.

Fatto. Per dare un altro compito a un fader, clicca **Configura** sotto la sua striscia in **Strumenti → Midi Fadester**.

**[Guida completa (in inglese)](../../README.md)**

## Visual Sidechain

Visual Sidechain fa muovere il tuo video con la tua musica in OBS Studio 33 su Windows e macOS. Fa parte di Midi Fadester dalla 0.9.0: installa Midi Fadester e ce l'hai.

### Guida rapida

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain in tre passi: 1, installa Midi Fadester (Visual Sidechain è incluso); 2, nei Filtri di una fonte premi + e aggiungi Visual Sidechain: Filtro shader; 3, metti la musica e l'immagine si muove con il suono." width="900">

1. **Installa Midi Fadester** (vedi [Install](../../README.md#install), in inglese) e apri OBS Studio 33.
2. **Aggiungi il filtro.** Clicca col tasto destro una fonte o una scena, scegli **Filtri**, premi **`+`** sotto i filtri effetto e aggiungi **`Visual Sidechain: Filtro shader`**.
3. **Metti la musica.** L'immagine si muove con il suono di **Traccia 1 (mix principale)**.

Fatto. Scegli un altro aspetto in **Effetto**, nella finestra del filtro.

> **Prima di andare in onda, assegna un tasto di panico:** **Impostazioni → Scorciatoie → `Visual Sidechain: Panico (spegne tutti gli effetti)`**. Una pressione spegne tutti gli effetti di Visual Sidechain in tutte le scene.

**[Guida completa (in inglese)](../VISUAL-SIDECHAIN.md)**
