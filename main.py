import io
import argparse
import logging
import math
import os
import threading
from datetime import datetime, time

LOG_LEVEL = os.getenv("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL, logging.INFO),
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
import yfinance as yf
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.path import Path
from matplotlib.patches import PathPatch
import pytz

EPD_WIDTH = 400
EPD_HEIGHT = 300
EPD_IMAGE_BYTES = EPD_WIDTH * EPD_HEIGHT // 8
IMAGE_CACHE_TTL_SECONDS = 60

app = Flask(__name__)
_image_cache = {"payload": None, "created_at": None, "segment": None}
_image_lock = threading.Lock()
_render_lock = threading.Lock()

DISPLAY_TZ = pytz.timezone(os.getenv("DISPLAY_TIMEZONE", "Asia/Shanghai"))

TIME_SEGMENTS = [
    {
        "name": "Evening",
        "ticker": "930955.SS",
        "display_name": "CSI Dividend Low Volatility 100",
        "market_timezone": "Asia/Shanghai",
        "active_windows": [(time(9, 30), time(11, 30)), (time(13, 0), time(15, 0))],
        "xticks": [0, 60, 119],
        "axis_times": [time(9, 30), time(13, 0), time(15, 0)],
    },
    {
        "name": "Regular",
        "ticker": "^ndx",
        "display_name": "NASDAQ 100",
        "market_timezone": "America/New_York",
        "active_windows": [(time(9, 30), time(17, 40))],
        "xticks": [0, 105, 195],
        "axis_times": [time(9, 30), time(13, 0), time(16, 0)],
    },
    {
        "name": "Premarket",
        "ticker": "NQ=F",
        "display_name": "NASDAQ FUTURES",
        "market_timezone": "America/New_York",
        "active_windows": [(time(0, 0), time(9, 30))],
        "xticks": [0, 150, 285],
        "axis_times": [time(0, 0), time(5, 0), time(9, 30)],
    },
]


# ==================== 工具函数 ====================
def is_trading_now(config, now=None):
    """Check the config's trading windows in the market's own timezone."""
    if now is None:
        now = datetime.now(pytz.UTC)
    market_tz = pytz.timezone(config["market_timezone"])
    if now.tzinfo is None:
        now = market_tz.localize(now)
    now_market = now.astimezone(market_tz)
    now_time = now_market.time()
    return any(
        start <= now_time < end
        for start, end in config["active_windows"]
    )


def positive_price(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) and number > 0 else None


def fetch_prices(ticker):
    """获取当日价格、前收和最新报价；失败时返回三个 None。"""
    try:
        obj = yf.Ticker(ticker)
        df = obj.history(period="1d", interval="2m")
        if df.empty:
            logger.warning("数据获取失败 (%s): 未返回行情数据", ticker)
            return None, None, None
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
                positive_price(quote_info.get("regularMarketPreviousClose"))
                or positive_price(quote_info.get("previousClose"))
            )
        except Exception:
            prev_close = None
        if prev_close is None:
            try:
                prev_close = positive_price(
                    obj.fast_info.regular_market_previous_close
                )
            except Exception:
                prev_close = None
        if prev_close is None:
            prev_close = positive_price(df["Open"].iloc[0])
        prices = df["Close"].dropna().values
        if len(prices) == 0 or prev_close is None:
            logger.warning("数据获取失败 (%s): 行情数据不完整", ticker)
            return None, None, None
        return prices, prev_close, latest_price
    except Exception as e:
        logger.warning("数据获取失败 (%s): %s", ticker, e)
        return None, None, None


def build_axis_labels(config, now=None):
    """Keep the original tick positions but format their times for today."""
    if now is None:
        now = datetime.now(pytz.UTC)
    market_tz = pytz.timezone(config["market_timezone"])
    if now.tzinfo is None:
        now = market_tz.localize(now)
    market_day = now.astimezone(market_tz).date()
    labels = []
    for index, market_time in enumerate(config["axis_times"]):
        market_time = market_tz.localize(datetime.combine(market_day, market_time))
        label = market_time.astimezone(DISPLAY_TZ).strftime("%H:%M")
        if index == 0:
            label = " " + label
        elif index == len(config["axis_times"]) - 1:
            label += " "
        labels.append(label)
    return labels


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


def create_chart(config, prices, prev_close, latest_price, x_tick_labels=None):
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


def render_segment_bitmap(config, now=None):
    """Fetch one market segment and return its device-ready bitmap."""
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
        logger.warning("图表生成失败: %s 最新价格无效", config["name"])
        return None

    x_tick_labels = build_axis_labels(config, now)

    # Flask 可并发处理请求，pyplot 则需要串行使用。
    with _render_lock:
        fig, current, pct = create_chart(
            config,
            prices,
            prev_close,
            latest_price,
            x_tick_labels,
        )
        try:
            buffer = io.BytesIO()
            fig.savefig(buffer, format="png", facecolor="white", edgecolor="none")
            png_bytes = buffer.getvalue()
        finally:
            plt.close(fig)

    logger.info(
        "图表已刷新: %s %s (%+.2f%%)",
        config["name"], f"{current:,.3f}", pct,
    )
    return to_epaper_bitmap(png_bytes)


def current_bitmap():
    """Return a fresh image when possible, otherwise the last good image."""
    with _image_lock:
        now = datetime.now(pytz.UTC)
        cached = _image_cache["payload"]
        created_at = _image_cache["created_at"]
        if cached and (now - created_at).total_seconds() < IMAGE_CACHE_TTL_SECONDS:
            logger.debug("缓存命中: %s", _image_cache["segment"])
            return cached

        segment = next(
            (item for item in TIME_SEGMENTS if is_trading_now(item, now)),
            None,
        )
        if segment:
            payload = render_segment_bitmap(segment, now)
            if payload:
                _image_cache.update(
                    payload=payload, created_at=now, segment=segment["name"]
                )
                return payload

        # A temporary market-data failure or an inactive market must not cause
        # a replacement error page on the e-paper display.
        if cached:
            logger.debug("刷新失败或无活动市场，继续返回缓存")
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
    logger.debug("图片请求: %s (%s 字节)", device_id, len(payload))
    return Response(payload, mimetype="application/octet-stream")


@app.get("/healthz")
def healthz():
    return jsonify(
        status="ok",
        cached=bool(_image_cache["payload"]),
        segment=_image_cache["segment"],
        expected_bytes=EPD_IMAGE_BYTES,
    )


def main():
    parser = argparse.ArgumentParser(description="Serve e-paper chart images")
    parser.add_argument("--host", default="0.0.0.0", help="server bind address")
    parser.add_argument("--port", type=int, default=35000, help="server port")
    args = parser.parse_args()

    logging.getLogger("werkzeug").setLevel(logging.WARNING)
    logger.info(
        "图片服务启动: http://%s:%s/epaper-display/image",
        args.host,
        args.port,
    )
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
