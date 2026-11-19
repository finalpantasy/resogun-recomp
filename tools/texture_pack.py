#!/usr/bin/env python3
"""Texture packs from the player's own game (see gpu/shadps4/video_core/texture_cache/texture_pack.h).

  python tools/texture_pack.py convert DUMP_DIR PNG_DIR
      RESOGUN_TEXTURE_DUMP's .dds files to PNGs (same names), for viewing or editing.
  python tools/texture_pack.py upscale PNG_DIR PACK_DIR [--scale 2] [--max-side 4096] [--esrgan EXE]
      Upscales every PNG into PACK_DIR (point RESOGUN_TEXTURE_PACK there). With --esrgan, the
      Real-ESRGAN ncnn-vulkan executable does the color textures (models next to it); without it,
      Lanczos and a light unsharp mask. Normal maps and channel masks are data, not pictures:
      they always get plain Lanczos (Real-ESRGAN shifted their values by up to 10 levels).
      Only a Real-ESRGAN result's brightness detail is used, moved to the texture's own mean and
      spread; color comes from the Lanczos result (matching each color channel tinted gray
      textures lavender). It is used only if it keeps --min-detail of the edge detail (it smooths
      grass-like noise away) and sharpens the texture evenly: --max-uneven is the most the
      spread of its detail gain over an 8x8 grid may be (on blurry noise it sharpens some
      patches and invents lichen in others). Real-ESRGAN works in --tile pixel tiles; 528 holds
      a whole padded 512 texture (an RTX 4090's driver fails above about 540).
      Results are cached in PACK_DIR-esrgan, so a rerun does not redo the GPU work.
      Alpha is scaled separately with Lanczos. --limit N does at most N textures per run; a
      later run skips the ones already in PACK_DIR. Real-ESRGAN's input is padded by 8 pixels,
      wrapped around for textures whose edges meet (tiling ones) and mirrored for the rest, so
      it leaves no seam where a tiling texture repeats.
      --format dds writes block-compressed DDS files with their whole mip chain instead of PNGs:
      BC1 for opaque textures, BC3 for ones with alpha and BC7 where the game used BC7. They take
      a quarter to an eighth of a PNG pack's GPU memory (the game uploads PNGs uncompressed), so
      --scale 4 fits on the GPU. Needs the etcpak module. --jobs N encodes N at a time.

A pack PNG only has to start with the 16-digit key of the texture it replaces; it may have any
size. Packs are made from a dump of your own copy: never share or commit them.
"""
import argparse
import concurrent.futures
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
    suffix = ".dds" if args.format == "dds" else ".png"
    for png in src:
        target = out / (png.stem + suffix)
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
                        pad(image.convert("RGB")).save(work / "in" / png.name)
                exe = pathlib.Path(args.esrgan)
                subprocess.run([str(exe), "-i", str(work / "in"), "-o", str(work / "out"), "-n",
                                args.model, "-s", "4", "-f", "png", "-t", str(args.tile)],
                               check=True, cwd=exe.parent)
                for png in missing:
                    big_path = work / "out" / png.name
                    if not big_path.exists():
                        continue
                    with Image.open(png) as source, Image.open(big_path) as big:
                        p = PAD * 4
                        big = big.convert("RGB").crop((p, p, p + source.width * 4,
                                                       p + source.height * 4))
                        lanczos(big, target_size(source, args)).save(cache / png.name)
                    big_path.unlink()
            finally:
                shutil.rmtree(work, ignore_errors=True)
    jobs = [(png, png.name in color, cache, args) for png in todo]
    with concurrent.futures.ProcessPoolExecutor(max(1, args.jobs)) as pool:
        results = list(pool.map(finish, *zip(*jobs))) if jobs else []
    used, fallback = results.count("ai"), results.count("fallback")
    print(f"{len(todo)} textures upscaled {args.scale}x into {out} ({len(color)} color, "
          f"{len(todo) - len(color)} normal maps and masks; Real-ESRGAN kept for {used}, "
          f"Lanczos for {fallback} that lost detail)")


def finish(png, is_color, cache, args):
    """Writes one pack texture; returns "ai" (Real-ESRGAN kept), "fallback" or "plain"."""
    out = pathlib.Path(args.pack_dir)
    result = "plain"
    with Image.open(png) as source:
        source = source.convert("RGBA")
        size = target_size(source, args)
        plain = lanczos(source.convert("RGB"), size)
        ai = cache / png.name
        if is_color and args.esrgan and ai.exists():
            with Image.open(ai) as image:
                rgb = luma_detail(image.convert("RGB"), plain)
            # Real-ESRGAN smooths noise-like detail (grass, gravel) away, and on blurry noise it
            # sharpens some patches and not others: such a texture keeps the Lanczos result.
            small, original = lanczos(rgb, source.size), source.convert("RGB")
            if (detail(small) < args.min_detail * detail(original)
                    or unevenness(original, small) > args.max_uneven):
                rgb, result = sharpen(plain), "fallback"
            else:
                result = "ai"
        elif is_color:
            rgb = sharpen(plain)
        else:
            rgb = plain
        alpha = lanczos(source.getchannel("A"), size)
        rgb.putalpha(alpha)
        if args.format == "dds":
            write_dds(out / (png.stem + ".dds"), rgb, png.name)
        else:
            rgb.save(out / png.name)
    return result


PAD = 8


def pad(image):
    """`image` with PAD pixels around it: wrapped if its opposite edges match (a tiling texture),
    mirrored otherwise."""
    a = numpy.asarray(image, dtype=numpy.int16)
    edges = (numpy.abs(a[:, 0] - a[:, -1]).mean() + numpy.abs(a[0] - a[-1]).mean()) / 2
    inner = (numpy.abs(a[:, 0] - a[:, 1]).mean() + numpy.abs(a[0] - a[1]).mean()) / 2
    mode = "wrap" if edges <= 2 * inner + 4 else "reflect"
    padded = numpy.pad(a.astype(numpy.uint8), ((PAD, PAD), (PAD, PAD), (0, 0)), mode=mode)
    return Image.fromarray(padded, "RGB")


def write_dds(path, image, name):
    """`image` (RGBA) as a DX10 DDS with its full mip chain (2x2 box filter, as the game's own
    PNG path does): BC7 if the game's texture was BC7, BC3 if it has alpha, otherwise BC1."""
    import etcpak
    srgb = "_srgb" in name
    if "_bc7" in name:
        dxgi, encode, block = (99 if srgb else 98), etcpak.compress_bc7, 16
    elif image.getchannel("A").getextrema()[0] < 255:
        dxgi, encode, block = (78 if srgb else 77), etcpak.compress_bc3, 16
    else:
        dxgi, encode, block = (72 if srgb else 71), etcpak.compress_bc1, 8
    levels, mip = [], image
    while True:
        a = numpy.asarray(mip)
        h, w = a.shape[:2]
        # Blocks are 4x4: a mip smaller than that is padded by repeating its edge.
        a = numpy.pad(a, ((0, -h % 4), (0, -w % 4), (0, 0)), mode="edge")
        levels.append(encode(numpy.ascontiguousarray(a).tobytes(), a.shape[1], a.shape[0]))
        if w == 1 and h == 1:
            break
        mip = mip.resize((max(w // 2, 1), max(h // 2, 1)), Image.BOX)
    header = bytearray(148)
    struct.pack_into("<4sIIIIIII", header, 0, b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000,
                     image.height, image.width, len(levels[0]), 0, len(levels))
    struct.pack_into("<II4s", header, 76, 32, 0x4, b"DX10")
    struct.pack_into("<I", header, 108, 0x1000 | 0x400000 | 0x8)  # texture, mipmap, complex
    struct.pack_into("<IIIII", header, 128, dxgi, 3, 0, 1, 0)
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes(bytes(header) + b"".join(levels))
    tmp.replace(path)


def target_size(image, args):
    scale = min(args.scale, args.max_side / max(image.size))
    return (round(image.width * scale), round(image.height * scale))


def sharpen(image):
    return image.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def detail(image):
    """Mean edge strength of an RGB image: how much fine detail it holds."""
    return ImageStat.Stat(image.convert("L").filter(ImageFilter.FIND_EDGES)).mean[0]


def luma_detail(image, reference):
    """`reference`'s color with `image`'s brightness, moved to `reference`'s brightness mean and
    spread (same size): Real-ESRGAN darkens and adds contrast to some textures and tints others."""
    a = numpy.asarray(image.convert("YCbCr"), dtype=numpy.float32)
    r = numpy.asarray(reference.convert("YCbCr"), dtype=numpy.float32)
    y = (a[..., 0] - a[..., 0].mean()) * (r[..., 0].std() / max(a[..., 0].std(), 1e-3))
    r[..., 0] = y + r[..., 0].mean()
    out = numpy.clip(r + 0.5, 0, 255).astype(numpy.uint8)
    return Image.fromarray(out, "YCbCr").convert("RGB")


def unevenness(source, result):
    """How unevenly `result` (at `source`'s size) adds detail: the spread of its edge-strength
    gain over `source` across an 8x8 grid, relative to the mean gain."""
    def edges(image):
        return numpy.asarray(image.convert("L").filter(ImageFilter.FIND_EDGES), dtype=numpy.float32)
    s, a = edges(source), edges(result)
    h, w = s.shape
    gains = numpy.array([(a[y * h // 8:(y + 1) * h // 8, x * w // 8:(x + 1) * w // 8].mean() + 1) /
                         (s[y * h // 8:(y + 1) * h // 8, x * w // 8:(x + 1) * w // 8].mean() + 1)
                         for y in range(8) for x in range(8)])
    return float(gains.std() / gains.mean())


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
    u.add_argument("--format", choices=("png", "dds"), default="png")
    u.add_argument("--tile", type=int, default=528, help="Real-ESRGAN tile size")
    u.add_argument("--max-uneven", type=float, default=0.25,
                   help="largest spread of Real-ESRGAN's detail gain over an 8x8 grid")
    u.add_argument("--jobs", type=int, default=4, help="textures encoded at once")
    u.add_argument("--min-detail", type=float, default=0.75,
                   help="keep Real-ESRGAN's result only if it keeps this share of the edge detail")
    u.set_defaults(run=upscale)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
