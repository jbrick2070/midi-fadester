> 本页是英文快速入门的翻译，由一个 AI 模型完成，并由第二个 AI 模型检查，尚未经过母语人士审阅。

# 快速入门：Midi Fadester 与 Visual Sidechain

## Midi Fadester

Midi Fadester 把 MIDI 控制器上的推子和旋钮变成 OBS Studio 的实时控制：用真正的交叉推子在场景之间渐变，混合两个图层，让摄像头渐入渐出，调节音量，推动任意滤镜设置。它为 Akai APC40 mkII 打造，也能通过 MIDI Learn 配合其他控制器使用。

### 快速入门

<img src="../../docs/quick-start.svg" alt="三步使用 Midi Fadester：1，运行最新版本中的安装程序并启动 OBS；2，连接控制器（APC40 mkII 会自动设置）；3，开启工作室模式并推动交叉推子：输出渐变为预览。" width="900">

1. **安装。** 从[最新版本](https://github.com/jbrick2070/midi-fadester/releases/latest)运行 `midi-fadester-<version>-windows-x64-Installer.exe`（Windows）或 `midi-fadester-<version>-macos-universal.pkg`（Mac），然后启动 OBS Studio 33 或更新版本。其他文件和注意事项：[Install](../../README.md#install)（英文）。
2. **连接控制器。** APC40 mkII 会自动设置，并打开 **Fadester Desk** 停靠窗口。其他控制器：打开 **工具 → Midi Fadester**（OBS 顶部菜单栏），在 **通道条** 选项卡上点击 **更换控制器**。
3. **推动交叉推子。** 点击 OBS 的 **工作室模式** 按钮（右下方），然后把交叉推子推到另一端：输出渐变为预览中的画面。

就这么简单。要给推子换一个任务，在 **工具 → Midi Fadester** 中点击该通道条下方的 **配置**。

**[完整指南（英文）](../../README.md)**

## Visual Sidechain

Visual Sidechain 让您的视频在 Windows 和 macOS 上的 OBS Studio 33 中随音乐而动。自 0.9.0 起它是 Midi Fadester 的一部分：安装 Midi Fadester 就能使用它。

### 快速入门

<img src="../../docs/visual-sidechain-steps.svg" alt="三步使用 Visual Sidechain：1，安装 Midi Fadester（自带 Visual Sidechain）；2，在源的滤镜中按 + 并添加 Visual Sidechain: 着色器滤镜；3，播放音乐，画面随声音而动。" width="900">

1. **安装 Midi Fadester**（见 [Install](../../README.md#install)，英文）并启动 OBS Studio 33。
2. **添加滤镜。** 右键点击源或场景，选择 **滤镜**，在效果滤镜下方按 **`+`**，添加 **`Visual Sidechain: 着色器滤镜`**。
3. **播放音乐。** 画面随 **音轨 1（主混音）** 上的声音而动。

就这么简单。在滤镜窗口的 **效果** 中可以选择其他效果。

> **开播前，请设置紧急停止键：** **设置 → 快捷键 → `Visual Sidechain: 紧急停止（关闭所有效果）`**。按一下即可关闭所有场景中的全部 Visual Sidechain 效果。

**[完整指南（英文）](../VISUAL-SIDECHAIN.md)**
