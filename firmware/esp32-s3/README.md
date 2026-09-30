# 慢慢说：ESP32-S3 硬件验证固件

这是 `zhengchen/1.54tft-wifi`（ESP32-S3 N16R8）的第一版硬件验证程序。

## 当前功能

- 保持整机供电（GPIO 2）
- 初始化 1.54 英寸 240×240 ST7789 屏幕
- 显示 `MANMANSHUO / HARDWARE / READY`
- 实时检测三个按键；按下时屏幕底部对应区域变色
  - 顶部中间按键：`MAIN`（GPIO 0）
  - 音量加：`UP`（GPIO 10）
  - 音量减：`DOWN`（GPIO 39）

本版本不会连接网络、不会录音，也不会读取或上传设备里的原有信息。

当前诊断版本会每秒轮换红、绿、蓝、白四种纯色，用来验证屏幕 SPI 通信。该测试已在第一台样机上通过。

## 构建与烧录

需要 ESP-IDF 6.x：

```text
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

烧录前已经把设备原厂 16MB Flash 完整备份到项目目录之外。未经明确操作，不要把备份提交到 GitHub。

## 硬件依据

引脚、显示方向和颜色设置依据上游开源板卡配置：

- <https://github.com/78/xiaozhi-esp32/tree/main/main/boards/zhengchen/1.54tft-wifi>

原厂 Flash 备份中的程序标识也确认为 `ZHENGCHEN_CUBE_1_54TFT_WIFI`。此前按
`xingzhi-cube-1.54tft-wifi` 配置时误将屏幕 CS（GPIO 21）作为电源保持脚，导致屏幕只有背光、没有画面。
