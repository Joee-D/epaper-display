# epaper-display

Waveshare 4.2 英寸 V2 墨水屏的 ESP32 固件。设备通过配网页面获取 Wi-Fi 信息，每次唤醒时
从服务器拉取预渲染的 400×300 黑白位图并显示，然后等待下一次刷新。设备端不绘制图表。

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
Wi-Fi 账号密码、图片服务器的 IP 或主机名，以及刷新间隔（1 分钟到 7 天）。固件会访问
`http://<服务器>:35000/epaper-display/image`；不需要填写协议、端口或路径。开机时按住
ESP32 的 BOOT 键 3 秒可强制重新进入配网页面。

配置（Wi-Fi 凭据、刷新间隔等）保存在 NVS 里，断电、按复位键或程序崩溃都不会丢失，
所以正常情况下只有首次上电才会出现配网页面。想改配置时按住 BOOT 键 3 秒开机即可重新
进入配网页面；需要彻底恢复出厂时，用 `pio run --target erase` 擦掉整片 Flash 再重新
烧录。

屏幕装反了就把 **Rotate display 180 degrees** 设为 `1`。
设备插在 USB/市电上时，把 **Keep WiFi on between refreshes for USB power** 设为 `1`：
固件会在两次刷新之间保持唤醒（WiFi modem sleep），这样 1 分钟刷新就不用重新连接。
电池供电时保持 `0`（深度睡眠，刷新之间关闭 WiFi）。

## 服务器图片接口

固件请求时会带上 `w=400`、`h=300` 和 `id=<URL 编码后的 MAC 地址>`。接口必须返回
HTTP 200 和恰好 15,000 字节：1 位、MSB 在前、按行排列、行间无填充的位图。响应可以使用
`Content-Length`，也可以使用 HTTP 分块传输编码。下载或校验失败时，面板保留上一张图。

仓库根目录的 `main.py` 可以生成当前交易时段的图表，并通过该接口提供给设备：

```sh
python main.py --host 0.0.0.0 --port 35000
```

服务运行在与设备同一网络的机器上，然后把配网页面中的服务器地址填写为这台机器的
局域网 IP。为减轻墨水屏残影，面板每 5 次局部刷新后会做一次全刷。

交易时段按各市场所在时区判断，冬令时和夏令时切换会自动生效；横轴标签默认换算为
`Asia/Shanghai` 时间。需要显示其他时区时，启动服务前设置 `DISPLAY_TIMEZONE`，例如：

```sh
DISPLAY_TIMEZONE=Asia/Tokyo python main.py --port 35000
```

设备请求本身不会逐条写入日志。需要排查请求或缓存问题时，可以临时启用调试日志：

```sh
LOG_LEVEL=DEBUG python main.py --port 35000
```
