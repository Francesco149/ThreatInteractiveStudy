"""Compresses a glTF scene's PNG textures to BC7 DDS files (with full mip chains) for the engine.

    python -I tools/textures/compress_bc.py assets/scene/courtyard.gltf

Writes <gltf dir>/dds/<image stem>.dds. The engine loads those instead of the PNGs when present
(`--no-bc` forces the PNGs, for the A/B). Colour textures (base colour, emissive) become
BC7_UNORM_SRGB with mips filtered in linear space; alpha-tested base colours keep their alpha-test
coverage per mip (texconv --keep-coverage, the same idea as the engine's PNG path); everything else
(normal, ORM) is linear BC7_UNORM with sRGB metadata ignored. Uses Microsoft's texconv
(DirectXTex, MIT): .tools/texconv/texconv.exe.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEXCONV = os.path.join(ROOT, ".tools", "texconv", "texconv.exe")


def main():
    gltf_path = os.path.abspath(sys.argv[1])
    base = os.path.dirname(gltf_path)
    gltf = json.load(open(gltf_path, encoding="utf-8"))
    images = gltf.get("images", [])
    textures = gltf.get("textures", [])

    def image_of(texinfo):
        if not texinfo:
            return None
        return textures[texinfo["index"]].get("source")

    srgb, cutoff = set(), {}
    for m in gltf.get("materials", []):
        pbr = m.get("pbrMetallicRoughness", {})
        a = image_of(pbr.get("baseColorTexture"))
        if a is not None:
            srgb.add(a)
            if m.get("alphaMode") == "MASK":
                cutoff[a] = m.get("alphaCutoff", 0.5)
        e = image_of(m.get("emissiveTexture"))
        if e is not None:
            srgb.add(e)

    out_dir = os.path.join(base, "dds")
    os.makedirs(out_dir, exist_ok=True)
    total_in = total_out = 0
    for i, img in enumerate(images):
        uri = img.get("uri")
        if not uri:
            continue
        src = os.path.join(base, uri.replace("%20", " "))
        args = [TEXCONV, "-nologo", "-y", "-m", "0", "-o", out_dir]
        if i in srgb:
            args += ["-f", "BC7_UNORM_SRGB", "-srgb"]
            if i in cutoff:
                args += ["--keep-coverage", str(cutoff[i])]
        else:
            args += ["-f", "BC7_UNORM", "--ignore-srgb"]
        args.append(src)
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        out = os.path.join(out_dir, os.path.splitext(os.path.basename(src))[0] + ".dds")
        total_in += os.path.getsize(src)
        total_out += os.path.getsize(out)
        print(f"{os.path.basename(src):40s} {'sRGB' if i in srgb else 'linear':6s}"
              f"{' coverage ' + str(cutoff[i]) if i in cutoff else ''} -> {os.path.getsize(out) / 1e6:.2f} MB")
    print(f"{len(images)} images, PNG {total_in / 1e6:.1f} MB -> BC7 DDS {total_out / 1e6:.1f} MB (with mips)")


if __name__ == "__main__":
    main()
