# epaper-display

Waveshare 4.2 英寸 V2 墨水屏的 ESP32 固件。设备通过配网页面获取 Wi-Fi 信息，每次唤醒时
自己抓取行情、在设备上绘制走势图，然后深度睡眠等待下一次刷新。整个链路不需要任何后端。

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
Wi-Fi 账号密码和刷新间隔（1 分钟到 7 天）即可，不需要配置任何服务器地址。开机时按住
ESP32 的 BOOT 键 3 秒可强制重新进入配网页面。

配置（Wi-Fi 凭据、刷新间隔等）保存在 NVS 里，断电、按复位键或程序崩溃都不会丢失，
所以正常情况下只有首次上电才会出现配网页面。想改配置时按住 BOOT 键 3 秒开机即可重新
进入配网页面；需要彻底恢复出厂时，用 `pio run --target erase` 擦掉整片 Flash 再重新
烧录。

屏幕装反了就把 **Rotate display 180 degrees** 设为 `1`。
设备插在 USB/市电上时，把 **Keep WiFi on between refreshes for USB power** 设为 `1`：
固件会在两次刷新之间保持唤醒（WiFi modem sleep），这样 1 分钟刷新就不用重新连接。
电池供电时保持 `0`（深度睡眠，刷新之间关闭 WiFi）。

为减轻墨水屏残影，面板每 5 次局部刷新后会做一次全刷。

## 行情与绘图

每次唤醒固件会：

1. 通过 NTP 对时，并应用 `America/New_York` 时区；
2. 选出当前正在开盘的时段：

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

所有市场都休市，或者取行情 / 渲染失败时，面板会保留上一张图，而不是显示错误页。

说明：

- TLS **不做证书校验**（`setInsecure()`）：行情是公开数据，跳过校验可以让固件在 CDN
  更换 CA 证书时无需重新烧录。
- 版式沿用 `main.py` 早先渲染 matplotlib 图时的布局，几何按像素对齐，渐变用有序（Bayer）
  抖动实现。文字使用内置的 FreeSansBold GLCD 字体，因此不支持中文字形。
- PlatformIO 报告约 1.08 MB Flash（占默认 1.25 MB app 分区的 83%）和约 64 KB 静态
  RAM；绘制时还需要额外的 15 KB 堆缓冲。

## 附带的 Python 绘图脚本（可选）

仓库根目录的 `main.py` 是早期“服务端画图”时代的脚本，**本固件已不再从服务器拉图**，
它作为独立工具保留：可以用 matplotlib 把同一张图画成 PNG，或者推送到 Zectrix 设备。

- **服务模式**：暴露 400×300 原始位图接口，供自带拉图逻辑的设备使用：

  ```sh
  python main.py --mode service --host 0.0.0.0 --port 35000
  ```

- **推送模式**：生成当前时段的图表并反复推送到 Zectrix 设备。凭据通过环境变量提供，
  推送周期用 `--interval`（单位：分钟）设置：

  ```sh
  export ZECTRIX_API_KEY='your-api-key'
  export ZECTRIX_DEVICE_ID='AA:BB:CC:DD:EE:FF'
  python main.py --mode push --interval 15
  ```

  默认周期 15 分钟，也可以通过 `PUSH_INTERVAL_MINUTES` 设置。如果要用 cron 之类的外部
  调度器执行单次推送，加上 `--once`。
