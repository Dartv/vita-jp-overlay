#!/usr/bin/env python3
"""Generates synthetic Vita-sized game screenshots with Japanese text for
vjo-cli / fixture recording (host/samples/*.jpg). Needs Pillow."""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "host", "samples")
GOTHIC = "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc"
MINCHO = "/System/Library/Fonts/ヒラギノ明朝 ProN.ttc"


def vn_textbox():
    im = Image.new("RGB", (960, 544), (40, 60, 90))
    d = ImageDraw.Draw(im)
    for y in range(0, 544, 8):  # background gradient
        d.rectangle([0, y, 960, y + 8], fill=(40 + y // 8, 60 + y // 10, 90))
    d.text((20, 14), "HP 120/120   LV 7   Score 004500", font=ImageFont.truetype(GOTHIC, 20), fill="white")
    d.rectangle([30, 360, 930, 520], fill=(0, 0, 0), outline=(200, 200, 255), width=3)
    f = ImageFont.truetype(GOTHIC, 28)
    d.text((50, 330), "名雪", font=f, fill=(255, 220, 120))
    d.text((60, 385), "「朝ごはん、もう冷めちゃったよ。", font=f, fill="white")
    d.text((60, 425), "　早く起きないと学校に遅刻するってば！」", font=f, fill="white")
    im.save(os.path.join(OUT, "vn_textbox.jpg"), quality=90)


def vertical():
    im = Image.new("RGB", (960, 544), (245, 240, 225))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(MINCHO, 30)
    cols = ["吾輩は猫である。", "名前はまだ無い。", "どこで生れたかとんと", "見当がつかぬ。"]
    x = 800
    for col in cols:
        y = 60
        for ch in col:
            d.text((x, y), ch, font=f, fill=(20, 20, 20))
            y += 36
        x -= 50
    d.text((20, 500), "MENU  SAVE  LOAD", font=ImageFont.truetype(GOTHIC, 18), fill=(90, 90, 90))
    im.save(os.path.join(OUT, "vertical.jpg"), quality=90)


def small_res():
    im = Image.new("RGB", (720, 408), (10, 10, 10))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(GOTHIC, 22)
    d.text((40, 300), "どうぐ　を　つかいますか？", font=f, fill="white")
    d.text((60, 340), "▶ はい　　いいえ", font=f, fill="white")
    im.save(os.path.join(OUT, "small_res.jpg"), quality=90)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    vn_textbox()
    vertical()
    small_res()
    print("wrote", sorted(os.listdir(OUT)))
