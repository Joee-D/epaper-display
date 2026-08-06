import io
import argparse
import logging
import os
import threading
import time
from datetime import datetime

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

TIME_SEGMENTS = [
    {
        "name": "Premarket",
        "ticker": "NQ=F",
        "display_name": "NASDAQ FUTURES",
        "start_time": "00:00",
        "end_time": "09:30",
        "xticks": [0, 150, 285],
        "xticklabels": [" 12:00", "17:00", "21:30 "],
        "output_file": "nq_trend.png",
    },
    {
        "name": "Regular",
        "ticker": "^ndx",
        "display_name": "NASDAQ 100",
        "start_time": "9:30",
        "end_time": "17:40",
        "xticks": [0, 105, 195],
        "xticklabels": [" 21:30", "01:00", "04:00 "],
        "output_file": "ndx_trend.png",
    },
    {
        "name": "Evening",
        "ticker": "930955.SS",
        "display_name": "CSI Dividend Low Volatility 100",
        "start_time": "21:30",
        "end_time": "00:00",
        "xticks": [0, 60, 119],
        "xticklabels": [" 09:30", "13:00", "15:00 "],
        "output_file": "sse_trend.png",
    },
]


# ==================== 工具函数 ====================
def is_trading_now(config):
    now = datetime.now(ET)
    #if now.weekday() >= 5:  # 周六(5)、周日(6) 跳过
    #    return False
    t = now.time()
    start = datetime.strptime(config["start_time"], "%H:%M").time()
    end = datetime.strptime(config["end_time"], "%H:%M").time()
    # 跨午夜时段（如 21:30 → 00:00）
    if start > end:
        return t >= start or t < end
    return start <= t < end


def fetch_prices(ticker):
    """获取当日价格、前收和最新报价；失败时返回三个 None。"""
    try:
        obj = yf.Ticker(ticker)
        df = obj.history(period="1d", interval="2m")
        if df.empty:
            logger.warning("数据获取失败 (%s): 未返回行情数据", ticker)
            return None, None, None
        # 用轻量的 fast_info 获取前收和最新价，避免加载 obj.info 的大量公司元数据。
        try:
            prev_close = obj.fast_info.previous_close
            latest_price = obj.fast_info.last_price
        except Exception:
            prev_close = None
            latest_price = None
        if not prev_close or prev_close <= 0:
            prev_close = df["Open"].iloc[0]
        prices = df["Close"].dropna().values
        if len(prices) == 0 or prev_close is None or prev_close <= 0:
            logger.warning("数据获取失败 (%s): 行情数据不完整", ticker)
            return None, None, None
        return prices, float(prev_close), latest_price
    except Exception as e:
        logger.warning("数据获取失败 (%s): %s", ticker, e)
        return None, None, None


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


def create_chart(config, prices, prev_close, latest_price):
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

    ax_chart.set_xticks(config["xticks"])
    ax_chart.set_xticklabels(config["xticklabels"], fontsize=7, fontweight="bold", color="black")
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


def render_segment_png(config, log_prefix):
    """获取行情并渲染 PNG；任何数据问题都返回 None。"""
    prices, prev_close, latest_price = fetch_prices(config["ticker"])
    if prices is None:
        return None

    max_len = config["xticks"][-1] + 1
    prices = prices[:max_len]
    if len(prices) == 0:
        return None
    if latest_price is None:
        latest_price = prices[-1]
    if not np.isfinite(latest_price):
        logger.warning("%s图表生成失败: %s 最新价格无效", log_prefix, config["name"])
        return None

    # Flask 可并发处理请求，pyplot 则需要串行使用。
    with _render_lock:
        fig, current, pct = create_chart(config, prices, prev_close, latest_price)
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


def render_segment_bitmap(config):
    """获取一个时段并渲染为设备可直接显示的位图。"""
    png_bytes = render_segment_png(config, "服务")
    return to_epaper_bitmap(png_bytes) if png_bytes else None


def current_bitmap():
    """Return a fresh image when possible, otherwise the last good image."""
    with _image_lock:
        now = datetime.now()
        cached = _image_cache["payload"]
        created_at = _image_cache["created_at"]
        if cached and (now - created_at).total_seconds() < IMAGE_CACHE_TTL_SECONDS:
            return cached

        active = [segment for segment in TIME_SEGMENTS if is_trading_now(segment)]
        if active:
            payload = render_segment_bitmap(active[0])
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
def process_segment(config):
    png_bytes = render_segment_png(config, "推送")
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
    now_local = datetime.now()
    now_et = datetime.now(ET)

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

    active = [config for config in TIME_SEGMENTS if is_trading_now(config)]
    if active:
        for config in active:
            process_segment(config)
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
