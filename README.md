# epaper-display

Waveshare 4.2 英寸 V2 墨水屏的 ESP32 固件。设备直接请求 Yahoo Finance，获取当前交易
时段的行情并在本地绘制 400×300 黑白图表，不需要后端服务器。

<img width="1920" height="1080" alt="IMG_3356" src="https://github.com/user-attachments/assets/dac1687e-7988-471f-b88c-2f2b705c35dc" />

## 编译与烧录

安装 [PlatformIO](https://platformio.org/)，接上 ESP32-WROOM-32E 开发板，然后执行：

```sh
pio run --target upload
pio device monitor
```

默认接线和目标板卡见 `platformio.ini` 与 `src/display.h`。如果你的屏幕接线不同，先改
`src/display.h` 里的引脚定义再烧录。

## 固件配置

固件不再提供配网页面。先复制配置模板并填写本地参数：

```sh
cp src/firmware_config.example.h src/firmware_config.h
```

然后编辑 `src/firmware_config.h`：

```cpp
#define EPAPER_WIFI_SSID "YOUR_WIFI_SSID"
#define EPAPER_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define EPAPER_REFRESH_INTERVAL_MIN 15
#define EPAPER_WIFI_CONNECT_TIMEOUT_MS 15000
```

- `EPAPER_REFRESH_INTERVAL_MIN`：刷新周期，范围为 1 分钟到 7 天。
- `EPAPER_WIFI_CONNECT_TIMEOUT_MS`：Wi-Fi 连接超时时间。

Wi-Fi 密码会编译进固件，修改配置后需要重新编译并烧录。不要把包含真实密码的配置文件
发布到公开仓库。`firmware_config.h` 已加入 `.gitignore`，仓库只保留示例文件。

设备按固定供电设计，刷新之间保持 WiFi 在线，只启用 modem sleep 降低无线功耗；墨水屏
会进入休眠等待下一次刷新。

## 行情与绘图

设备联网后通过 NTP 校时，并按各市场本地时区选择当前交易时段：

| 时段 | Yahoo 代码 | 交易时段 | 图表标题 |
|------|------------|----------|----------|
| Evening | `930955.SS` | 北京时间 09:30–11:30、13:00–15:00 | CSI Dividend Low Volatility 100 |
| Regular | `^ndx` | 美东时间 09:30–17:40 | NASDAQ 100 |
| Premarket | `NQ=F` | 美东时间 00:00–09:30 | NASDAQ FUTURES |

美东时段会自动应用夏令时和冬令时，A 股午休不会被计入行情。固件直接请求
`query1.finance.yahoo.com` 的 Chart API，在 ESP32 内完成图表绘制和黑白抖动，因此不需要
服务器。为减轻墨水屏残影，面板每 60 分钟强制全刷一次，与行情刷新周期无关。

固件内置 2026–2030 年 Nasdaq、CME 和上交所休市日表；超出该范围前需要更新
`src/market.cpp` 中的休市日列表。NTP 时间每 6 小时重新校对一次。

固件不校验证书，适合通过正常互联网连接 Yahoo 的公开行情接口。
