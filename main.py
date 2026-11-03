import io
import argparse
import logging
import os
import threading
import time
from bisect import bisect_left
from datetime import datetime, time as datetime_time, timedelta

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger("epaper")

from flask import Flask, Response, jsonify, request
from PIL import Image
import matplotlib

# This script is normally run on a headless server.
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import requests
import yfinance as yf
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.path import Path
from matplotlib.patches import PathPatch
import pytz

# ==================== 配置 ====================
# 推送模式的凭证应通过环境变量提供，避免把设备密钥提交到代码库。
API_KEY = os.getenv("ZECTRIX_API_KEY", "")
DEVICE_ID = os.getenv("ZECTRIX_DEVICE_ID", "")
PUSH_URL = (
    f"https://cloud.zectrix.com/open/v1/devices/{DEVICE_ID}/display/image"
    if DEVICE_ID
    else ""
)

EPD_WIDTH = 400
EPD_HEIGHT = 300
EPD_IMAGE_BYTES = EPD_WIDTH * EPD_HEIGHT // 8
IMAGE_CACHE_TTL_SECONDS = 60
PUSH_REQUEST_TIMEOUT_SECONDS = 30


def env_positive_interval(name, default):
    """Read an optional positive interval from the environment."""
    try:
        value = float(os.getenv(name, str(default)))
        return value if value > 0 else default
    except ValueError:
        return default


DEFAULT_PUSH_INTERVAL_MINUTES = env_positive_interval("PUSH_INTERVAL_MINUTES", 15)

app = Flask(__name__)
_image_cache = {"payload": None, "created_at": None, "segment": None}
_image_lock = threading.Lock()
_render_lock = threading.Lock()

ET = pytz.timezone("America/New_York")
DISPLAY_TZ = pytz.timezone(os.getenv("DISPLAY_TIMEZONE", "Asia/Shanghai"))
BAR_INTERVAL = timedelta(minutes=2)

TIME_SEGMENTS = [
    {
        "name": "Premarket",
        "ticker": "NQ=F",
        "display_name": "NASDAQ FUTURES",
        "market_timezone": "America/New_York",
        "active_windows": [("00:00", "09:30")],
        "data_windows": [("00:00", "09:30")],
        "axis_times": ["00:00", "05:00", "09:30"],
        "prepost": True,
        "display_priority": 1,
        "output_file": "nq_trend.png",
    },
    {
        "name": "Regular",
        "ticker": "^ndx",
        "display_name": "NASDAQ 100",
        "market_timezone": "America/New_York",
        # Keep the last regular-session chart visible briefly after the close.
        "active_windows": [("09:30", "17:40")],
        "data_windows": [("09:30", "16:00")],
        "axis_times": ["09:30", "13:00", "16:00"],
        "prepost": False,
        "display_priority": 2,
        "output_file": "ndx_trend.png",
    },
    {
        "name": "Evening",
        "ticker": "930955.SS",
        "display_name": "CSI Dividend Low Volatility 100",
        "market_timezone": "Asia/Shanghai",
        "active_windows": [("09:30", "11:30"), ("13:00", "15:00")],
        "data_windows": [("09:30", "11:30"), ("13:00", "15:00")],
        "axis_times": ["09:30", "13:00", "15:00"],
        "prepost": False,
        # A-share afternoon overlaps the U.S. premarket in Beijing time. Prefer
        # the local market so both markets still get a visible update each day.
        "display_priority": 3,
        "output_file": "sse_trend.png",
    },
]


# ==================== 工具函数 ====================
def parse_clock(value):
    hour, minute = (int(part) for part in value.split(":", 1))
    return datetime_time(hour=hour, minute=minute)


def is_time_in_window(now_time, start_time, end_time):
    if start_time <= end_time:
        return start_time <= now_time < end_time
    # Cross-midnight window, e.g. 21:30 -> 00:00.
    return now_time >= start_time or now_time < end_time


def is_trading_now(config, now=None):
    """Check the config's trading windows in the market's own timezone."""
    if now is None:
        now = datetime.now(pytz.UTC)
    market_tz = pytz.timezone(config["market_timezone"])
    if now.tzinfo is None:
        now = market_tz.localize(now)
    now_market = now.astimezone(market_tz)
    #if now.weekday() >= 5:  # 周六(5)、周日(6) 跳过
    #    return False
    return any(
        is_time_in_window(now_market.time(), parse_clock(start), parse_clock(end))
        for start, end in config["active_windows"]
    )


def session_bounds(market_tz, now_market, start_text, end_text):
    """Return timezone-aware bounds for the session containing or ending today."""
    start_clock = parse_clock(start_text)
    end_clock = parse_clock(end_text)
    day = now_market.date()

    if start_clock > end_clock and now_market.time() < end_clock:
        day -= timedelta(days=1)

    start = market_tz.localize(datetime.combine(day, start_clock))
    end_day = day + timedelta(days=1) if start_clock > end_clock else day
    end = market_tz.localize(datetime.combine(end_day, end_clock))
    return start, end


def select_session_rows(config, frame, now):
    """Keep only bars inside the configured market sessions."""
    market_tz = pytz.timezone(config["market_timezone"])
    if now.tzinfo is None:
        now = market_tz.localize(now)
    now_market = now.astimezone(market_tz)

    if frame.index.tz is None:
        frame.index = frame.index.tz_localize(market_tz)
    else:
        frame.index = frame.index.tz_convert(market_tz)

    selected = None
    for start_text, end_text in config["data_windows"]:
        start, end = session_bounds(market_tz, now_market, start_text, end_text)
        mask = (frame.index >= start) & (frame.index <= end)
        selected = mask if selected is None else selected | mask
    return frame[selected].copy() if selected is not None else frame.iloc[0:0]


def fetch_prices(config, now=None):
    """获取当前交易时段的价格、时间戳、前收和最新报价。"""
    try:
        obj = yf.Ticker(config["ticker"])
        history = obj.history(
            period="5d",
            interval="2m",
            prepost=config.get("prepost", False),
        )
        if history.empty:
            logger.warning("数据获取失败 (%s): 未返回行情数据", config["ticker"])
            return None, None, None, None

        frame = select_session_rows(config, history, now or datetime.now(pytz.UTC))
        closes = frame["Close"].dropna()
        if closes.empty:
            logger.warning("数据获取失败 (%s): 当前时段没有行情数据", config["ticker"])
            return None, None, None, None
        prices = closes.to_numpy(dtype=float)
        timestamps = list(closes.index.to_pydatetime())

        # 最新价用轻量的 fast_info，避免加载 obj.get_info() 的大量公司元数据。
        try:
            latest_price = obj.fast_info.last_price
        except Exception:
            latest_price = None
        # 前收必须取自报价接口的 regularMarketPreviousClose，它才等于 Yahoo 网页上的
        # "Previous Close"。fast_info.regular_market_previous_close 取的是日线倒数第二
        # 根 bar 的收盘价，对近 24 小时交易的期货（NQ=F）用的是另一种日切分点，比网页
        # 的结算价低约 1%，会把涨跌幅放大 1 个百分点。
        try:
            quote_info = obj.get_info()
            prev_close = (
                quote_info.get("regularMarketPreviousClose")
                or quote_info.get("previousClose")
            )
        except Exception:
            prev_close = None
        if not prev_close or prev_close <= 0:
            # 报价接口不可用时的退路；对期货仍可能有约 1% 的偏差。
            try:
                prev_close = obj.fast_info.regular_market_previous_close
            except Exception:
                prev_close = None
        if not prev_close or prev_close <= 0:
            opens = frame["Open"].dropna()
            prev_close = opens.iloc[0] if not opens.empty else None
        if prev_close is None or prev_close <= 0:
            logger.warning("数据获取失败 (%s): 行情数据不完整", config["ticker"])
            return None, None, None, None
        return prices, timestamps, float(prev_close), latest_price
    except Exception as e:
        logger.warning("数据获取失败 (%s): %s", config["ticker"], e)
        return None, None, None, None


def build_axis_ticks(config, timestamps):
    """Map market-local clock times to data indices and display-time labels."""
    if not timestamps:
        return [], []

    market_tz = pytz.timezone(config["market_timezone"])
    first_market = timestamps[0].astimezone(market_tz)
    last_timestamp = timestamps[-1]
    timestamp_seconds = [timestamp.timestamp() for timestamp in timestamps]
    ticks = []
    labels = []

    for value in config["axis_times"]:
        target = market_tz.localize(
            datetime.combine(first_market.date(), parse_clock(value))
        )
        # Tolerate the half/final bar ending just before the nominal close.
        if target > last_timestamp + BAR_INTERVAL:
            continue

        target_seconds = target.timestamp()
        position = bisect_left(timestamp_seconds, target_seconds)
        if position > 0 and (
            position == len(timestamp_seconds)
            or abs(timestamp_seconds[position - 1] - target_seconds)
            < abs(timestamp_seconds[position] - target_seconds)
        ):
            position -= 1
        if position >= len(timestamp_seconds) or position in ticks:
            continue
        ticks.append(position)
        labels.append(target.astimezone(DISPLAY_TZ).strftime("%H:%M"))

    if not ticks:
        ticks.append(0)
        labels.append(timestamps[0].astimezone(DISPLAY_TZ).strftime("%H:%M"))
    return ticks, labels


def draw_gradient_fill(ax, x, prices, prev_close, y_lo, y_hi):
    above = prices >= prev_close
    cmap = LinearSegmentedColormap.from_list("fade", ["#fafafa", "#b0b0b0"])

    for is_above in [True, False]:
        mask = above if is_above else ~above
        if not mask.any():
            continue

        if is_above:
            img_data = np.linspace(1, 0, 256).reshape(256, 1)
            extent = [0, len(prices), prev_close, y_hi]
            origin = "upper"
        else:
            img_data = np.linspace(0, 1, 256).reshape(256, 1)
            extent = [0, len(prices), y_lo, prev_close]
            origin = "upper"

        img = ax.imshow(
            img_data,
            aspect="auto",
            extent=extent,
            cmap=cmap,
            origin=origin,
            zorder=2,
            vmin=0,
            vmax=1,
        )

        poly = ax.fill_between(
            x, prices, prev_close,
            where=mask,
            alpha=0,
            zorder=2,
        )
        clip_paths = poly.get_paths()
        if clip_paths:
            combined = Path.make_compound_path(*clip_paths)
            patch = PathPatch(combined, transform=ax.transData, linewidth=0)
            ax.add_patch(patch)
            img.set_clip_path(patch)


def create_chart(
    config, prices, prev_close, latest_price, x_ticks=None, x_tick_labels=None
):
    current = latest_price
    change = current - prev_close
    pct = change / prev_close * 100
    sign = "+" if change >= 0 else ""

    fig, (ax_text, ax_chart) = plt.subplots(
        2, 1, figsize=(4, 3), dpi=100,
        gridspec_kw={"height_ratios": [1, 2.2]}
    )
    fig.patch.set_facecolor("white")

    ax_text.axis("off")
    ax_text.text(0.02, 0.6, config["display_name"], fontsize=11, fontweight="bold")
    ax_text.text(0.02, 0.05, f"{current:,.2f}", fontsize=20, fontweight="bold")
    ax_text.text(
        0.98, 0.15, f" {sign}{pct:.2f}% ", fontsize=11, fontweight="bold",
        color="white", ha="right",
        bbox=dict(facecolor="black", edgecolor="black", boxstyle="round,pad=0.3"),
    )

    x = np.arange(len(prices))
    y_min, y_max = min(prices.min(), prev_close), max(prices.max(), prev_close)
    margin = (y_max - y_min) * 0.1 or 0.001
    y_lo = y_min - margin
    y_hi = y_max + margin
    ax_chart.set_xlim(0, len(prices) - 1)
    ax_chart.set_ylim(y_lo, y_hi)

    ax_chart.axhline(y=prev_close, color="black", linestyle=":", linewidth=1, zorder=1)
    draw_gradient_fill(ax_chart, x, prices, prev_close, y_lo, y_hi)
    ax_chart.plot(x, prices, color="black", linewidth=2, zorder=3)

    if x_ticks:
        ax_chart.set_xticks(x_ticks)
        ax_chart.set_xticklabels(
            x_tick_labels or [],
            fontsize=7,
            fontweight="bold",
            color="black",
        )
    ax_chart.set_yticks([])
    ax_chart.tick_params(length=0)
    for spine in ax_chart.spines.values():
        spine.set_visible(False)

    plt.tight_layout(pad=0.2)
    return fig, current, pct


def push_to_device(img_bytes, filename):
    if not API_KEY or not PUSH_URL:
        logger.error("推送失败: 请设置 ZECTRIX_API_KEY 和 ZECTRIX_DEVICE_ID")
        return False
    try:
        resp = requests.post(
            PUSH_URL,
            headers={"X-API-Key": API_KEY},
            files={"images": (filename, img_bytes, "image/png")},
            data={"dither": "true", "pageId": "1"},
            timeout=PUSH_REQUEST_TIMEOUT_SECONDS,
        )
        resp.raise_for_status()
        logger.info("推送成功: 已发送 %s 到设备 %s", filename, DEVICE_ID)
        return True
    except requests.exceptions.RequestException as e:
        logger.error("推送失败: %s", e)
        return False


def to_epaper_bitmap(png_bytes, width=EPD_WIDTH, height=EPD_HEIGHT):
    """Encode a native-size chart as the firmware's MSB-first 1bpp bitmap.

    GxEPD2 treats a set bit as the requested foreground colour (black), which
    is the inverse of Pillow's usual 1-bit raw-byte convention. Error diffusion
    preserves gradient detail on a physically black-and-white panel; a fixed
    threshold would discard it.
    """
    with Image.open(io.BytesIO(png_bytes)) as image:
        if image.size != (width, height):
            raise ValueError(
                f"Chart must be rendered at {width}x{height}, got {image.size}"
            )
        image = image.convert("L")
        pixels = image.load()
        payload = bytearray(width * height // 8)
        current_error = [0.0] * (width + 2)
        next_error = [0.0] * (width + 2)
        for y in range(height):
            row = y * width // 8
            for x in range(width):
                value = min(255, max(0, pixels[x, y] + current_error[x + 1]))
                black = value < 128
                quantized = 0 if black else 255
                error = value - quantized
                if black:
                    payload[row + x // 8] |= 0x80 >> (x % 8)
                current_error[x + 2] += error * 7 / 16
                next_error[x] += error * 3 / 16
                next_error[x + 1] += error * 5 / 16
                next_error[x + 2] += error / 16
            current_error, next_error = next_error, [0.0] * (width + 2)
    return bytes(payload)


def render_segment_png(config, log_prefix, now=None):
    """获取行情并渲染 PNG；任何数据问题都返回 None。"""
    prices, timestamps, prev_close, latest_price = fetch_prices(config, now)
    if prices is None:
        return None

    if len(prices) == 0:
        return None
    if latest_price is None:
        latest_price = prices[-1]
    if not np.isfinite(latest_price):
        logger.warning("%s图表生成失败: %s 最新价格无效", log_prefix, config["name"])
        return None

    x_ticks, x_tick_labels = build_axis_ticks(config, timestamps)

    # Flask 可并发处理请求，pyplot 则需要串行使用。
    with _render_lock:
        fig, current, pct = create_chart(
            config,
            prices,
            prev_close,
            latest_price,
            x_ticks,
            x_tick_labels,
        )
        try:
            buffer = io.BytesIO()
            fig.savefig(buffer, format="png", facecolor="white", edgecolor="none")
            logger.info(
                "%s图表生成: %s %s (%+.2f%%)",
                log_prefix, config["name"], f"{current:,.3f}", pct,
            )
            return buffer.getvalue()
        finally:
            plt.close(fig)


def render_segment_bitmap(config, now=None):
    """获取一个时段并渲染为设备可直接显示的位图。"""
    png_bytes = render_segment_png(config, "服务", now)
    return to_epaper_bitmap(png_bytes) if png_bytes else None


def current_bitmap():
    """Return a fresh image when possible, otherwise the last good image."""
    with _image_lock:
        now = datetime.now(pytz.UTC)
        cached = _image_cache["payload"]
        created_at = _image_cache["created_at"]
        if cached and (now - created_at).total_seconds() < IMAGE_CACHE_TTL_SECONDS:
            return cached

        active = [segment for segment in TIME_SEGMENTS if is_trading_now(segment, now)]
        if active:
            active.sort(key=lambda segment: segment["display_priority"], reverse=True)
            payload = render_segment_bitmap(active[0], now)
            if payload:
                _image_cache.update(
                    payload=payload, created_at=now, segment=active[0]["name"]
                )
                return payload

        # A temporary market-data failure or an inactive market must not cause
        # a replacement error page on the e-paper display.
        return cached


@app.get("/epaper-display/image")
def epaper_image():
    """Endpoint consumed by the ESP32 firmware's scheduled GET request."""
    try:
        width = int(request.args.get("w", EPD_WIDTH))
        height = int(request.args.get("h", EPD_HEIGHT))
    except ValueError:
        logger.warning("服务请求失败: 图片尺寸参数不是整数")
        return Response("Invalid dimensions", status=400, mimetype="text/plain")

    if (width, height) != (EPD_WIDTH, EPD_HEIGHT):
        logger.warning("服务请求失败: 不支持的图片尺寸 %sx%s", width, height)
        return Response(
            f"This service provides {EPD_WIDTH}x{EPD_HEIGHT} images only",
            status=400,
            mimetype="text/plain",
        )

    payload = current_bitmap()
    if payload is None:
        logger.warning("服务请求失败: 当前没有可用图表，返回 503")
        return Response(
            "No chart is available yet; preserving the display's previous image",
            status=503,
            mimetype="text/plain",
        )

    device_id = request.args.get("id", "未提供设备 ID")
    logger.info("服务请求成功: 向 %s 返回 %s 字节图像", device_id, len(payload))
    return Response(payload, mimetype="application/octet-stream")


@app.get("/healthz")
def healthz():
    return jsonify(
        status="ok",
        cached=bool(_image_cache["payload"]),
        segment=_image_cache["segment"],
        expected_bytes=EPD_IMAGE_BYTES,
    )


# ==================== 主流程 ====================
def process_segment(config, now=None):
    png_bytes = render_segment_png(config, "推送", now)
    if png_bytes is None:
        logger.info("推送跳过: %s 无可用图表", config["name"])
        return

    try:
        with open(config["output_file"], "wb") as output:
            output.write(png_bytes)
    except OSError as error:
        # 本地文件仅用于留档，写入失败不应阻止向设备推送。
        logger.warning("图片留档失败 (%s): %s", config["output_file"], error)

    push_to_device(png_bytes, config["output_file"])


def run_push_cycle():
    """Generate and push the chart for the currently active market segment."""
    now = datetime.now(pytz.UTC)
    now_local = datetime.now()
    now_et = now.astimezone(ET)

    weekdays = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]
    local_weekday = weekdays[now_local.weekday()]
    et_weekday = weekdays[now_et.weekday()]

    logger.info("市场数据推送")
    logger.info(
        "服务器时间: %s %s %s",
        now_local.strftime("%Y-%m-%d"), local_weekday, now_local.strftime("%H:%M:%S"),
    )
    logger.info(
        "美东时间:   %s %s %s",
        now_et.strftime("%Y-%m-%d"), et_weekday, now_et.strftime("%H:%M:%S %Z"),
    )

    active = [config for config in TIME_SEGMENTS if is_trading_now(config, now)]
    if active:
        for config in active:
            process_segment(config, now)
    else:
        logger.info("休市中，跳过更新")


def positive_interval(value):
    try:
        interval = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("周期必须是正数（单位：分钟）") from error
    if interval <= 0:
        raise argparse.ArgumentTypeError("周期必须大于 0（单位：分钟）")
    return interval


def run_push_loop(interval_minutes, once=False):
    """按配置周期执行推送。"""
    try:
        while True:
            run_push_cycle()
            if once:
                return
            time.sleep(interval_minutes * 60)
    except KeyboardInterrupt:
        logger.info("已停止推送模式")


def run_both(host, port, interval_minutes):
    """同时在后台提供服务接口，并在主线程定期推送。"""
    from werkzeug.serving import make_server

    server = make_server(host, port, app, threaded=True)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    logger.info("服务模式已启动（后台线程）")
    logger.info("图片接口地址: http://%s:%s/epaper-display/image", host, port)
    try:
        run_push_loop(interval_minutes)
    finally:
        server.shutdown()
        server_thread.join()


def main():
    parser = argparse.ArgumentParser(description="Generate, push, or serve e-paper charts")
    parser.add_argument(
        "--mode",
        choices=("service", "push", "both"),
        default="push",
        help="运行模式：service 提供接口；push 定期推送；both 同时运行（默认：push）",
    )
    parser.add_argument("--host", default="0.0.0.0", help="server bind address")
    parser.add_argument("--port", type=int, default=35000, help="server port")
    parser.add_argument(
        "--interval",
        type=positive_interval,
        default=DEFAULT_PUSH_INTERVAL_MINUTES,
        metavar="MINUTES",
        help=f"推送间隔（分钟，默认：{DEFAULT_PUSH_INTERVAL_MINUTES}）",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="推送模式仅执行一次，适合由 cron 等外部调度器调用",
    )
    args = parser.parse_args()

    if args.once and args.mode != "push":
        parser.error("--once 仅适用于推送模式")

    if args.mode == "service":
        logger.info("服务模式已启动")
        logger.info("图片接口地址: http://%s:%s/epaper-display/image", args.host, args.port)
        logger.info("服务正在运行，等待设备请求图片...")
        app.run(host=args.host, port=args.port, threaded=True)
        return

    if args.mode == "both":
        run_both(args.host, args.port, args.interval)
        return

    logger.info("推送模式已启动，周期：每 %g 分钟", args.interval)
    run_push_loop(args.interval, args.once)


if __name__ == "__main__":
    main()
