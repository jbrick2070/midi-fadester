> Deze pagina is een vertaling van de Engelse snelstarts, gemaakt met een AI-model en gecontroleerd door een tweede AI-model. Een moedertaalspreker heeft haar nog niet nagekeken.

# Snelstart: Midi Fadester en Visual Sidechain

## Midi Fadester

Midi Fadester maakt van de faders en knoppen op een MIDI-controller live bediening voor OBS Studio: tussen scènes faden met een echte crossfader, twee lagen mixen, een camera in- en uitfaden, een volume regelen, elke filterinstelling verschuiven. Het is gemaakt voor de Akai APC40 mkII en werkt met andere controllers via MIDI Learn.

### Snelstart

<img src="../../docs/quick-start.svg" alt="Midi Fadester in drie stappen: 1, voer het installatieprogramma uit de nieuwste release uit en start OBS; 2, sluit de controller aan (een APC40 mkII stelt zichzelf in); 3, zet Studiomodus aan en schuif de crossfader: Programma fadet naar Voorvertoning." width="900">

1. **Installeer het.** Voer uit de [nieuwste release](https://github.com/jbrick2070/midi-fadester/releases/latest) `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) of `midi-fadester-<version>-macos-universal.pkg` (Mac) uit en start dan OBS Studio 33 of nieuwer. Andere bestanden en waarschuwingen: [Install](../../README.md#install) (Engels).
2. **Sluit de controller aan.** Een APC40 mkII stelt zichzelf in en het dock **Fadester Desk** gaat open. Een andere controller: open **Hulpmiddelen → Midi Fadester** (menubalk van OBS, boven) en klik op het tabblad **Strips** op **Controller wijzigen**.
3. **Schuif de crossfader.** Klik in OBS op de knop **Studiomodus** (rechtsonder) en schuif de crossfader helemaal naar het andere eind: Programma fadet naar wat in Voorvertoning stond.

Klaar. Om een fader een andere taak te geven, klik je in **Hulpmiddelen → Midi Fadester** op **Instellen** onder zijn strip.

**[Volledige handleiding (Engels)](../../README.md)**

## Visual Sidechain

Visual Sidechain laat je video meebewegen met je muziek in OBS Studio 33 op Windows en macOS. Het hoort sinds 0.9.0 bij Midi Fadester: installeer Midi Fadester en je hebt het.

### Snelstart

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain in drie stappen: 1, installeer Midi Fadester (Visual Sidechain zit erbij); 2, druk in de Filters van een bron op + en voeg Visual Sidechain: Shaderfilter toe; 3, speel muziek en het beeld beweegt mee met het geluid." width="900">

1. **Installeer Midi Fadester** (zie [Install](../../README.md#install), Engels) en start OBS Studio 33.
2. **Voeg het filter toe.** Klik met rechts op een bron of scène, kies **Filters**, druk op **`+`** onder de effectfilters en voeg **`Visual Sidechain: Shaderfilter`** toe.
3. **Speel muziek.** Het beeld beweegt mee met het geluid op **Spoor 1 (hoofdmix)**.

Klaar. Kies een andere look onder **Effect** in het venster van het filter.

> **Stel voordat je live gaat een paniektoets in:** **Instellingen → Sneltoetsen → `Visual Sidechain: Paniek (alle effecten uit)`**. Eén keer drukken zet elk effect van Visual Sidechain in alle scènes uit.

**[Volledige handleiding (Engels)](../VISUAL-SIDECHAIN.md)**
