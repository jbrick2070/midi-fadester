> Cette page est une traduction des démarrages rapides anglais, réalisée avec un modèle d'IA et vérifiée par un second modèle d'IA. Aucun locuteur natif ne l'a encore relue.

# Démarrage rapide : Midi Fadester et Visual Sidechain

## Midi Fadester

Midi Fadester transforme les faders et les boutons d'un contrôleur MIDI en commandes en direct pour OBS Studio : passer d'une scène à l'autre en fondu avec un vrai crossfader, mélanger deux calques, faire apparaître une caméra en fondu, doser un volume, balayer n'importe quel réglage d'un filtre. Il est conçu pour l'Akai APC40 mkII et fonctionne avec d'autres contrôleurs grâce à MIDI Learn.

### Démarrage rapide

<img src="../../docs/quick-start.svg" alt="Midi Fadester en trois étapes : 1, lancez l'installateur de la dernière version et démarrez OBS ; 2, branchez le contrôleur (un APC40 mkII se configure tout seul) ; 3, activez le mode studio et poussez le crossfader : le Programme passe en fondu à l'Aperçu." width="900">

1. **Installez-le.** Depuis la [dernière version](https://github.com/jbrick2070/midi-fadester/releases/latest), lancez `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) ou `midi-fadester-<version>-macos-universal.pkg` (Mac), puis démarrez OBS Studio 33 ou plus récent. Autres fichiers et avertissements : [Install](../../README.md#install) (en anglais).
2. **Branchez le contrôleur.** Un APC40 mkII se configure tout seul et le dock **Fadester Desk** s'ouvre. Un autre contrôleur : ouvrez **Outils → Midi Fadester** (barre de menus d'OBS, en haut) et cliquez sur **Changer de contrôleur** dans l'onglet **Tranches**.
3. **Poussez le crossfader.** Cliquez sur le bouton **Mode studio** d'OBS (en bas à droite), puis poussez le crossfader jusqu'à l'autre bout : le Programme passe en fondu à ce qui était dans l'Aperçu.

C'est tout. Pour donner un autre rôle à un fader, cliquez sur **Configurer** sous sa tranche dans **Outils → Midi Fadester**.

**[Guide complet (en anglais)](../../README.md)**

## Visual Sidechain

Visual Sidechain fait bouger votre vidéo avec votre musique dans OBS Studio 33 sous Windows et macOS. Il fait partie de Midi Fadester depuis la 0.9.0 : installez Midi Fadester et vous l'avez.

### Démarrage rapide

<img src="../../docs/visual-sidechain-steps.svg" alt="Visual Sidechain en trois étapes : 1, installez Midi Fadester (Visual Sidechain est inclus) ; 2, dans les Filtres d'une source, appuyez sur + et ajoutez Visual Sidechain: Filtre de shader ; 3, lancez la musique et l'image bouge avec le son." width="900">

1. **Installez Midi Fadester** (voir [Install](../../README.md#install), en anglais) et démarrez OBS Studio 33.
2. **Ajoutez le filtre.** Faites un clic droit sur une source ou une scène, choisissez **Filtres**, appuyez sur **`+`** sous les filtres d'effets et ajoutez **`Visual Sidechain: Filtre de shader`**.
3. **Lancez la musique.** L'image bouge avec le son de **Piste 1 (mix principal)**.

C'est tout. Choisissez un autre rendu sous **Effet** dans la fenêtre du filtre.

> **Avant de passer en direct, définissez une touche de panique :** **Paramètres → Raccourcis clavier → `Visual Sidechain: Panique (coupe tous les effets)`**. Un appui coupe tous les effets de Visual Sidechain dans toutes les scènes.

**[Guide complet (en anglais)](../VISUAL-SIDECHAIN.md)**
