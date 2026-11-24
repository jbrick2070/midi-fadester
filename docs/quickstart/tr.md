> Bu sayfa İngilizce hızlı başlangıçların bir çevirisidir; bir yapay zekâ modeliyle hazırlandı ve ikinci bir yapay zekâ modeli tarafından denetlendi. Henüz anadili Türkçe olan biri tarafından gözden geçirilmedi.

# Hızlı başlangıç: Midi Fadester ve Visual Sidechain

## Midi Fadester

Midi Fadester, bir MIDI kontrolcüsündeki fader'ları ve düğmeleri OBS Studio için canlı denetimlere dönüştürür: gerçek bir crossfader ile sahneler arasında geçiş yapın, iki katmanı karıştırın, bir kamerayı yavaşça açıp kapatın, bir ses düzeyini ayarlayın, herhangi bir filtre ayarını değiştirin. Akai APC40 mkII için yapılmıştır ve diğer kontrolcülerle MIDI Learn üzerinden çalışır.

### Hızlı başlangıç

<img src="../../docs/quick-start.svg" alt="Üç adımda Midi Fadester: 1, son sürümdeki yükleyiciyi çalıştırın ve OBS'i başlatın; 2, kontrolcüyü takın (APC40 mkII kendini ayarlar); 3, Stüdyo Modunu açın ve crossfader'ı itin: Program, Önizleme'ye geçer." width="900">

1. **Yükleyin.** [Son sürümden](https://github.com/jbrick2070/midi-fadester/releases/latest) `midi-fadester-<version>-windows-x64-Installer.exe` (Windows) veya `midi-fadester-<version>-macos-universal.pkg` (Mac) dosyasını çalıştırın, ardından OBS Studio 33 veya daha yenisini başlatın. Diğer dosyalar ve uyarılar: [Install](../../README.md#install) (İngilizce).
2. **Kontrolcüyü takın.** APC40 mkII kendini ayarlar ve **Fadester Desk** paneli açılır. Başka bir kontrolcü için: **Araçlar → Midi Fadester**'ı açın (OBS'in üstteki menü çubuğu) ve **Şeritler** sekmesinde **Kontrolcüyü değiştir**'e tıklayın.
3. **Crossfader'ı itin.** OBS'te **Stüdyo Modu** düğmesine tıklayın (sağ altta), ardından crossfader'ı diğer uca kadar itin: Program, Önizleme'de olana geçer.

Bu kadar. Bir fader'a başka bir görev vermek için **Araçlar → Midi Fadester** içinde şeridinin altındaki **Ayarla**'ya tıklayın.

**[Tam kılavuz (İngilizce)](../../README.md)**

## Visual Sidechain

Visual Sidechain, Windows ve macOS'ta OBS Studio 33 içinde videonuzun müziğinizle birlikte hareket etmesini sağlar. 0.9.0'dan beri Midi Fadester'ın bir parçasıdır: Midi Fadester'ı yükleyin, o da gelir.

### Hızlı başlangıç

<img src="../../docs/visual-sidechain-steps.svg" alt="Üç adımda Visual Sidechain: 1, Midi Fadester'ı yükleyin (Visual Sidechain onunla gelir); 2, bir kaynağın Filtreler penceresinde + düğmesine basın ve Visual Sidechain: Shader Filtresi ekleyin; 3, müzik çalın, görüntü sesle birlikte hareket eder." width="900">

1. **Midi Fadester'ı yükleyin** (bkz. [Install](../../README.md#install), İngilizce) ve OBS Studio 33'ü başlatın.
2. **Filtreyi ekleyin.** Bir kaynağa veya sahneye sağ tıklayın, **Filtreler**'i seçin, efekt filtrelerinin altındaki **`+`** düğmesine basın ve **`Visual Sidechain: Shader Filtresi`** ekleyin.
3. **Müzik çalın.** Görüntü **Ses İzi 1 (Ana Miks)** üzerindeki sesle birlikte hareket eder.

Bu kadar. Filtrenin penceresinde **Efekt** altından başka bir görünüm seçin.

> **Canlı yayına çıkmadan önce bir Panik tuşu atayın:** **Ayarlar → Kısayollar → `Visual Sidechain: Panik (tüm efektleri kapatır)`**. Tek basış, tüm sahnelerdeki her Visual Sidechain efektini kapatır.

**[Tam kılavuz (İngilizce)](../VISUAL-SIDECHAIN.md)**
