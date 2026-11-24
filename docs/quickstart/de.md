> Diese Seite ist eine Übersetzung der englischen Schnellstarts, erstellt mit einem KI-Modell und von einem zweiten KI-Modell geprüft. Ein Muttersprachler hat sie noch nicht durchgesehen.

# Schnellstart: Midi Fadester und Visual Sidechain

## Midi Fadester

Midi Fadester macht die Fader und Regler eines MIDI-Controllers zu Live-Steuerungen für OBS Studio: mit einem echten Crossfader zwischen Szenen überblenden, zwei Ebenen mischen, eine Kamera ein- und ausblenden, eine Lautstärke regeln, jede Filtereinstellung verändern. Es ist für den Akai APC40 mkII gemacht und funktioniert mit anderen Controllern über MIDI Learn.

### Schnellstart

<img src="../../docs/quick-start.svg" alt="Midi Fadester in drei Schritten: 1, Installationsprogramm aus der neuesten Version ausführen und OBS starten; 2, Controller anschließen (ein APC40 mkII richtet sich selbst ein); 3, Studio-Modus einschalten und den Crossfader schieben: Programm blendet auf Vorschau über." width="900">

1. **Installieren.** Führen Sie aus der [neuesten Version](https://github.com/jbrick2070/midi-fadester/releases/latest) `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) oder `midi-fadester-<version>-macos-universal.pkg` (Mac) aus und starten Sie dann OBS Studio 33 oder neuer. Weitere Dateien und Warnhinweise: [Install](../../README.md#install) (Englisch).
2. **Controller anschließen.** Ein APC40 mkII richtet sich selbst ein, und das Dock **Fadester Desk** öffnet sich. Ein anderer Controller: Öffnen Sie **Werkzeuge → Midi Fadester** (Menüleiste von OBS, oben) und klicken Sie auf der Registerkarte **Kanalzüge** auf **Controller wechseln**.
3. **Crossfader schieben.** Klicken Sie in OBS auf die Schaltfläche **Studio-Modus** (unten rechts) und schieben Sie den Crossfader ganz ans andere Ende: Programm blendet auf das über, was in der Vorschau war.

Das war's. Um einem Fader eine andere Aufgabe zu geben, klicken Sie unter seinem Kanalzug in **Werkzeuge → Midi Fadester** auf **Einrichten**.

**[Vollständige Anleitung (Englisch)](../../README.md)**

## Visual Sidechain

Visual Sidechain lässt Ihr Video in OBS Studio 33 unter Windows und macOS mit Ihrer Musik mitgehen. Seit 0.9.0 gehört es zu Midi Fadester: Installieren Sie Midi Fadester, und Sie haben es.

### Schnellstart

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain in drei Schritten: 1, Midi Fadester installieren (Visual Sidechain ist dabei); 2, in den Filtern einer Quelle auf + drücken und Visual Sidechain: Shader-Filter hinzufügen; 3, Musik abspielen, und das Bild bewegt sich mit dem Klang." width="900">

1. **Installieren Sie Midi Fadester** (siehe [Install](../../README.md#install), Englisch) und starten Sie OBS Studio 33.
2. **Filter hinzufügen.** Klicken Sie mit der rechten Maustaste auf eine Quelle oder Szene, wählen Sie **Filter**, drücken Sie **`+`** unter den Effektfiltern und fügen Sie **`Visual Sidechain: Shader-Filter`** hinzu.
3. **Musik abspielen.** Das Bild bewegt sich mit dem Klang auf **Spur 1 (Hauptmix)**.

Das war's. Einen anderen Look wählen Sie im Fenster des Filters unter **Effekt**.

> **Bevor Sie live gehen, legen Sie eine Panik-Taste fest:** **Einstellungen → Hotkeys → `Visual Sidechain: Panik (alle Effekte aus)`**. Ein Druck schaltet jeden Effekt von Visual Sidechain in allen Szenen aus.

**[Vollständige Anleitung (Englisch)](../VISUAL-SIDECHAIN.md)**
