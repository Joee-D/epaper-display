# epaper-display

Waveshare 4.2 英寸 V2 墨水屏的 ESP32 固件。设备通过配网页面获取 Wi-Fi 信息，每次唤醒时
自己抓取行情并在设备上绘制走势图（也可以改成从你自己的图片服务器拉取预渲染的 400×300
1 位位图），然后深度睡眠等待下一次刷新。

<img width="1920" height="1080" alt="IMG_3356" src="https://github.com/user-attachments/assets/dac1687e-7988-471f-b88c-2f2b705c35dc" />

## 编译与烧录

安装 [PlatformIO](https://platformio.org/)，接上 ESP32-WROOM-32E 开发板，然后执行：

```sh
pio run --target upload
pio device monitor
```

默认接线和目标板卡见 `platformio.ini` 与 `src/display.h`。如果你的屏幕接线不同，先改
`src/display.h` 里的引脚定义再烧录。

## 首次配置

首次启动时连接 `epaper-display-Setup` 这个 Wi-Fi，然后打开 `http://192.168.4.1`。填写
Wi-Fi 账号密码、刷新间隔（1 分钟到 7 天）和图表来源。**Chart source** 填 `1` 表示由设备
自己抓行情、自己画图（默认值，不需要任何服务端）；填 `0` 表示从你的图片服务器拉取渲染
好的位图，这时还要填服务器 IP，固件会自动补上 `:35000/epaper-display/image`。开机时
按住 ESP32 的 BOOT 键 3 秒可强制重新进入配网页面。

注意：设备每次冷启动都会恢复出厂设置——真正的断电上电（拔插电源）、按 EN/RESET 键或
程序崩溃。此时配置和已保存的 Wi-Fi 凭据都会被清除，配网页面会重新打开，所以每次上电
都要重新填写一次。电池供电下的深度睡眠唤醒不算冷启动，不会触发重置；只有真正断电才会。

屏幕装反了就把 **Rotate display 180 degrees** 设为 `1`。
设备插在 USB/市电上时，把 **Keep WiFi on between refreshes for USB power** 设为 `1`：
固件会在两次刷新之间保持唤醒（WiFi modem sleep），这样 1 分钟刷新就不用重新连接。
电池供电时保持 `0`（深度睡眠，刷新之间关闭 WiFi）。

服务端模式下，固件请求时会带上 `w=400`、`h=300` 和 `id=<MAC 地址>`。接口必须返回 HTTP
200 和恰好 15,000 字节：1 位、MSB 在前、按行排列、行间无填充的位图。响应可以用
`Content-Length`，也可以用 HTTP 分块传输编码。这一段只适用于服务端模式。

为减轻墨水屏残影，面板每 5 次局部刷新后会做一次全刷。

## 设备端绘图模式

**Chart source = 1**（默认）时完全不依赖后端。每次唤醒固件会：

1. 通过 NTP 对时，并应用 `America/New_York` 时区；
2. 选出当前正在开盘的时段，时间窗口与 `src/main.py` 完全一致：

   | 时段 | 代码 | 美东时间窗口 | 图表标题 |
   |------|------|--------------|----------|
   | Premarket | `NQ=F` | 00:00–09:30 | NASDAQ FUTURES |
   | Regular | `^ndx` | 09:30–17:40 | NASDAQ 100 |
   | Evening | `930955.SS` | 21:30–00:00 | CSI Dividend Low Volatility 100 |

3. 从 Yahoo Finance 下载该时段的行情——
   `https://query1.finance.yahoo.com/v8/finance/chart/<symbol>?range=1d&interval=2m`，
   一次 HTTPS GET 约 13–17 KB——最新价和官方前收直接取自响应里的 `meta` 字段，
   所以涨跌幅与 Yahoo 网页一致；
4. 把图表渲染到 400×300 的 1 位帧缓冲（`src/chart.cpp`）并推送到面板。

所有市场都休市，或者取行情 / 渲染失败时，面板会保留上一张图，而不是显示错误页——
这也是原来服务端模式的行为。

说明：

- TLS **不做证书校验**（`setInsecure()`）：行情是公开数据，跳过校验可以让固件在 CDN
  更换 CA 证书时无需重新烧录。
- 版式复刻 `src/main.py` 渲染出的 matplotlib 图，渐变用有序（Bayer）抖动实现。文字使用
  内置的 FreeSansBold GLCD 字体，因此不支持中文字形。
- PlatformIO 报告约 1.09 MB Flash（占默认 1.25 MB app 分区的 83%）和约 64 KB 静态
  RAM；绘制时还需要额外的 15 KB 堆缓冲。

## 图表服务端模式（可选）

**Chart source = 0** 时设备改为拉取渲染好的位图。`src/main.py` 用 matplotlib 画图，
支持两种运行方式：

- **服务模式**：暴露 400×300 原始位图接口，由固件按自己的刷新周期来拉取：

  ```sh
  python src/main.py --mode service --host 0.0.0.0 --port 35000
  ```

- **推送模式**：生成当前时段的图表并反复推送到 Zectrix 设备。凭据通过环境变量提供，
  推送周期用 `--interval`（单位：分钟）设置：

  ```sh
  export ZECTRIX_API_KEY='your-api-key'
  export ZECTRIX_DEVICE_ID='AA:BB:CC:DD:EE:FF'
  python src/main.py --mode push --interval 15
  ```

  默认周期 15 分钟，也可以通过 `PUSH_INTERVAL_MINUTES` 设置。如果要用 cron 之类的外部
  调度器执行单次推送，加上 `--once`。
