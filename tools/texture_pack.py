#!/usr/bin/env python3
"""Texture packs from the player's own game (see gpu/shadps4/video_core/texture_cache/texture_pack.h).

  python tools/texture_pack.py convert DUMP_DIR PNG_DIR
      RESOGUN_TEXTURE_DUMP's .dds files to PNGs (same names), for viewing or editing.
  python tools/texture_pack.py upscale PNG_DIR PACK_DIR [--scale 2] [--max-side 4096] [--esrgan EXE]
      Upscales every PNG into PACK_DIR (point RESOGUN_TEXTURE_PACK there). With --esrgan, the
      Real-ESRGAN ncnn-vulkan executable does the work (models next to it); without it, Lanczos
      and a light unsharp mask. Alpha is scaled separately with Lanczos.

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

from PIL import Image, ImageFilter

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


def upscale(args):
    src = sorted(pathlib.Path(args.png_dir).glob("*.png"))
    out = pathlib.Path(args.pack_dir)
    out.mkdir(parents=True, exist_ok=True)
    todo = []
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
        todo.append(png)
    esrgan = {}
    if args.esrgan and todo:
        # One batch run over a folder: the model loads once.
        work = pathlib.Path(tempfile.mkdtemp(prefix="texture-pack-"))
        (work / "in").mkdir()
        (work / "out").mkdir()
        for png in todo:
            with Image.open(png) as image:
                image.convert("RGB").save(work / "in" / png.name)
        exe = pathlib.Path(args.esrgan)
        subprocess.run([str(exe), "-i", str(work / "in"), "-o", str(work / "out"), "-n",
                        args.model, "-s", "4", "-f", "png"], check=True, cwd=exe.parent)
        esrgan = {p.name: p for p in (work / "out").glob("*.png")}
    for png in todo:
        with Image.open(png) as source:
            source = source.convert("RGBA")
            scale = min(args.scale, args.max_side / max(source.size))
            size = (round(source.width * scale), round(source.height * scale))
            if png.name in esrgan:
                with Image.open(esrgan[png.name]) as big:
                    rgb = lanczos(big.convert("RGB"), size)
            else:
                rgb = lanczos(source.convert("RGB"), size).filter(
                    ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))
            alpha = lanczos(source.getchannel("A"), size)
            rgb.putalpha(alpha)
            rgb.save(out / png.name)
    if esrgan:
        shutil.rmtree(next(iter(esrgan.values())).parent.parent, ignore_errors=True)
    print(f"{len(todo)} textures upscaled {args.scale}x into {out}")


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
    u.set_defaults(run=upscale)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
