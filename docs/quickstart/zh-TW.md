> 本頁是英文快速入門的翻譯，由一個 AI 模型完成，並由第二個 AI 模型檢查，尚未經過母語人士審閱。

# 快速入門：Midi Fadester 與 Visual Sidechain

## Midi Fadester

Midi Fadester 把 MIDI 控制器上的推桿與旋鈕變成 OBS Studio 的即時控制：用真正的交叉推桿在場景之間淡變、混合兩個圖層、讓攝影機淡入淡出、調整音量、控制任何濾鏡設定。它專為 Akai APC40 mkII 打造，也能透過 MIDI Learn 搭配其他控制器使用。

### 快速入門

<img src="../../docs/quick-start.svg" alt="三步使用 Midi Fadester：1，執行最新版本中的安裝程式並啟動 OBS；2，接上控制器（APC40 mkII 會自動設定）；3，開啟工作室模式並推動交叉推桿：展示淡變為預覽。" width="900">

1. **安裝。** 從[最新版本](https://github.com/jbrick2070/midi-fadester/releases/latest)執行 `midi-fadester-<version>-windows-x64-Installer.exe`（Windows）或 `midi-fadester-<version>-macos-universal.pkg`（Mac），然後啟動 OBS Studio 33 或更新版本。其他檔案與注意事項：[Install](../../README.md#install)（英文）。
2. **接上控制器。** APC40 mkII 會自動設定，並開啟 **Fadester Desk** 停駐視窗。其他控制器：開啟 **工具 → Midi Fadester**（OBS 上方的選單列），在 **控制條** 分頁上按一下 **變更控制器**。
3. **推動交叉推桿。** 按一下 OBS 的 **工作室模式** 按鈕（右下方），然後把交叉推桿推到另一端：展示淡變為預覽中的畫面。

就這樣。要讓推桿改做其他工作，在 **工具 → Midi Fadester** 中按一下該控制條下方的 **設定**。

**[完整指南（英文）](../../README.md)**

## Visual Sidechain

Visual Sidechain 讓您的影片在 Windows 與 macOS 上的 OBS Studio 33 中隨音樂而動。自 0.9.0 起它是 Midi Fadester 的一部分：安裝 Midi Fadester 就能使用它。

### 快速入門

<img src="../../docs/visual-sidechain-steps.svg" alt="三步使用 Visual Sidechain：1，安裝 Midi Fadester（內含 Visual Sidechain）；2，在來源的濾鏡中按 + 並加入 Visual Sidechain: 著色器濾鏡；3，播放音樂，畫面隨聲音而動。" width="900">

1. **安裝 Midi Fadester**（見 [Install](../../README.md#install)，英文）並啟動 OBS Studio 33。
2. **加入濾鏡。** 對來源或場景按右鍵，選擇 **濾鏡**，在特效濾鏡下方按 **`+`**，加入 **`Visual Sidechain: 著色器濾鏡`**。
3. **播放音樂。** 畫面隨 **音軌 1 (主混音)** 上的聲音而動。

就這樣。在濾鏡視窗的 **效果** 中可以選擇其他效果。

> **開播前，請設定緊急停止鍵：** **設定 → 快捷鍵 → `Visual Sidechain: 緊急停止 (關閉所有效果)`**。按一下即可關閉所有場景中的所有 Visual Sidechain 效果。

**[完整指南（英文）](../VISUAL-SIDECHAIN.md)**
