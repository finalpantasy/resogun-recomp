#!/usr/bin/env python3
"""Texture packs from the player's own game (see gpu/shadps4/video_core/texture_cache/texture_pack.h).

  python tools/texture_pack.py convert DUMP_DIR PNG_DIR
      RESOGUN_TEXTURE_DUMP's .dds files to PNGs (same names), for viewing or editing.
  python tools/texture_pack.py upscale PNG_DIR PACK_DIR [--scale 2] [--max-side 4096] [--esrgan EXE]
      Upscales every PNG into PACK_DIR (point RESOGUN_TEXTURE_PACK there). With --esrgan, the
      Real-ESRGAN ncnn-vulkan executable does the color textures (models next to it); without it,
      Lanczos and a light unsharp mask. Normal maps and channel masks are data, not pictures:
      they always get plain Lanczos (Real-ESRGAN shifted their values by up to 10 levels).
      A Real-ESRGAN result keeps the texture's own tone (each channel's mean and spread) and is
      used only if it keeps --min-detail of the edge detail; it smooths grass-like noise away.
      Results are cached in PACK_DIR-esrgan, so a rerun does not redo the GPU work.
      Alpha is scaled separately with Lanczos. --limit N does at most N textures per run; a
      later run skips the ones already in PACK_DIR.

A pack PNG only has to start with the 16-digit key of the texture it replaces; it may have any
size. Packs are made from a dump of your own copy: never share or commit them.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy
from PIL import Image, ImageFilter, ImageStat

BCN = {71: 1, 72: 1, 74: 2, 75: 2, 77: 3, 78: 3, 98: 7, 99: 7}
RAW = {28: "RGBA", 29: "RGBA", 87: "BGRA", 91: "BGRA"}


def read_dds(path):
    data = path.read_bytes()
    if data[:4] != b"DDS " or data[84:88] != b"DX10":
        raise ValueError(f"{path.name}: not a resogun dump")
    height, width = struct.unpack_from("<II", data, 12)
    dxgi = struct.unpack_from("<I", data, 128)[0]
    pixels = data[148:]
    if dxgi in BCN:
        return Image.frombytes("RGBA", (width, height), pixels, "bcn", BCN[dxgi])
    if dxgi in RAW:
        return Image.frombytes("RGBA", (width, height), pixels, "raw", RAW[dxgi])
    raise ValueError(f"{path.name}: DXGI format {dxgi} is not handled")


def convert(args):
    out = pathlib.Path(args.png_dir)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for dds in sorted(pathlib.Path(args.dump_dir).glob("*.dds")):
        target = out / (dds.stem + ".png")
        if target.exists():
            continue
        try:
            read_dds(dds).save(target)
            count += 1
        except (ValueError, OSError) as error:
            print(error, file=sys.stderr)
    print(f"{count} textures converted to {out}")


def lanczos(image, size):
    return image.resize(size, Image.LANCZOS)


def texture_kind(image):
    """'normal' (tangent-space normal map: blue high, red and green around 128), 'mask' (no blue
    anywhere: channels packed with material data) or 'color'."""
    stat = ImageStat.Stat(image.convert("RGB").resize((64, 64)))
    r, g, b = stat.mean
    if b > 200 and abs(r - 128) < 30 and abs(g - 128) < 30:
        return "normal"
    if stat.extrema[2][1] < 12 and max(r, g) > 20:
        return "mask"
    return "color"


def upscale(args):
    src = sorted(pathlib.Path(args.png_dir).glob("*.png"))
    out = pathlib.Path(args.pack_dir)
    out.mkdir(parents=True, exist_ok=True)
    todo, color = [], set()
    for png in src:
        target = out / png.name
        if target.exists():
            continue
        with Image.open(png) as image:
            if min(image.size) < args.min or max(image.size) >= args.max_side:
                continue
            # Screen-sized 8-bit RGBA textures are frame captures: new contents every time.
            if "_rgba8" in png.name and image.width >= 1280:
                continue
            if texture_kind(image) == "color":
                color.add(png.name)
        todo.append(png)
        if args.limit and len(todo) >= args.limit:
            break
    cache = out.parent / (out.name + "-esrgan")
    if args.esrgan and color:
        # Real-ESRGAN's results, scaled to the pack size, are kept next to the pack: a later run
        # (other limits or checks) does not redo the GPU work. One batch run over a folder: the
        # model loads once. The 4x intermediates are large, so they go next to the pack too and
        # are deleted as soon as they are scaled down.
        cache.mkdir(exist_ok=True)
        missing = [png for png in todo if png.name in color and not (cache / png.name).exists()]
        if missing:
            work = pathlib.Path(tempfile.mkdtemp(prefix="texture-pack-", dir=out.parent))
            try:
                (work / "in").mkdir()
                (work / "out").mkdir()
                for png in missing:
                    with Image.open(png) as image:
                        image.convert("RGB").save(work / "in" / png.name)
                exe = pathlib.Path(args.esrgan)
                subprocess.run([str(exe), "-i", str(work / "in"), "-o", str(work / "out"), "-n",
                                args.model, "-s", "4", "-f", "png"], check=True, cwd=exe.parent)
                for png in missing:
                    big_path = work / "out" / png.name
                    if not big_path.exists():
                        continue
                    with Image.open(png) as source, Image.open(big_path) as big:
                        lanczos(big.convert("RGB"), target_size(source, args)).save(cache / png.name)
                    big_path.unlink()
            finally:
                shutil.rmtree(work, ignore_errors=True)
    used = fallback = 0
    for png in todo:
        with Image.open(png) as source:
            source = source.convert("RGBA")
            size = target_size(source, args)
            plain = lanczos(source.convert("RGB"), size)
            ai = cache / png.name
            if png.name in color and args.esrgan and ai.exists():
                with Image.open(ai) as image:
                    rgb = keep_tone(image.convert("RGB"), plain)
                # Real-ESRGAN smooths noise-like detail (grass, gravel) away: such a texture
                # keeps the Lanczos result.
                if detail(lanczos(rgb, source.size)) < args.min_detail * detail(source.convert("RGB")):
                    rgb, fallback = sharpen(plain), fallback + 1
                else:
                    used += 1
            elif png.name in color:
                rgb = sharpen(plain)
            else:
                rgb = plain
            alpha = lanczos(source.getchannel("A"), size)
            rgb.putalpha(alpha)
            rgb.save(out / png.name)
    print(f"{len(todo)} textures upscaled {args.scale}x into {out} ({len(color)} color, "
          f"{len(todo) - len(color)} normal maps and masks; Real-ESRGAN kept for {used}, "
          f"Lanczos for {fallback} that lost detail)")


def target_size(image, args):
    scale = min(args.scale, args.max_side / max(image.size))
    return (round(image.width * scale), round(image.height * scale))


def sharpen(image):
    return image.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def detail(image):
    """Mean edge strength of an RGB image: how much fine detail it holds."""
    return ImageStat.Stat(image.convert("L").filter(ImageFilter.FIND_EDGES)).mean[0]


def keep_tone(image, reference):
    """`image` with each channel's mean and spread moved to `reference`'s (same size): Real-ESRGAN
    darkens and adds contrast to some textures."""
    a = numpy.asarray(image, dtype=numpy.float32)
    r = numpy.asarray(reference, dtype=numpy.float32)
    mean_a, std_a = a.mean(axis=(0, 1)), a.std(axis=(0, 1))
    mean_r, std_r = r.mean(axis=(0, 1)), r.std(axis=(0, 1))
    a = (a - mean_a) * (std_r / numpy.maximum(std_a, 1e-3)) + mean_r
    return Image.fromarray(numpy.clip(a + 0.5, 0, 255).astype(numpy.uint8), "RGB")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("convert")
    c.add_argument("dump_dir")
    c.add_argument("png_dir")
    c.set_defaults(run=convert)
    u = sub.add_parser("upscale")
    u.add_argument("png_dir")
    u.add_argument("pack_dir")
    u.add_argument("--scale", type=int, default=2)
    u.add_argument("--min", type=int, default=64, help="skip textures smaller than this")
    u.add_argument("--max-side", type=int, default=4096,
                   help="largest output side; textures this large already are skipped")
    u.add_argument("--esrgan", help="realesrgan-ncnn-vulkan executable")
    u.add_argument("--model", default="realesrgan-x4plus")
    u.add_argument("--limit", type=int, default=0, help="at most this many textures per run")
    u.add_argument("--min-detail", type=float, default=0.75,
                   help="keep Real-ESRGAN's result only if it keeps this share of the edge detail")
    u.set_defaults(run=upscale)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
