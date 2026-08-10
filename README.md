# ESP32-S3-Touch-LCD-1.85B 大狗叫

原生 ESP-IDF 版「大狗大狗叫叫叫」核心玩法，目标板为 Waveshare
ESP32-S3-Touch-LCD-1.85B。网页原始素材保留在 `tmp/`，构建时会从中提取
大、狗、叫三个 32kHz 音效并嵌入固件。

## 功能

- 360×360 触摸屏：左、中、右三列分别触发「大」「狗」「叫」原始音效。
- 仅点按触发；声音直接进入音频队列，不使用节拍量化、滑动音符或长按延音。
- 大狗开闭嘴画面、板载扬声器输出。
- 背景音乐（BGM）：首次点按后自动启动，软件实时合成网页版同款 BGM（BPM 128
  电子舞曲循环，C-G-Am-F 四和弦 + 鼓组），之后无限循环，无需外部音频文件。
- BGM 音量独立（固定增益），不受 VOL 调节影响；BOOT 单击切换全局静音（BGM 一并静音）。
- 右上角设置音量（VOL 仅控制狗叫音效响度）。

## 构建

本项目引用本机的微雪 BSP：
`/Users/nemo/github/ESP32-S3-Touch-LCD-1.85B`。先载入 ESP-IDF v5.5.3：

```zsh
load_idf
idf.py set-target esp32s3
idf.py build
```

`load_idf` 是本机 `~/.zshrc` 中的别名。它依赖 pyenv 的 Python 3.13 shim；若在
非交互 shell 中运行，请先执行
`export PATH="$HOME/.pyenv/shims:$PATH"`，再运行 `load_idf`。

通过 USB 连接开发板后使用 `idf.py -p <port> flash monitor`。
