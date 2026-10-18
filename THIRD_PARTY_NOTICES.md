# Third-party notices

The engine's own code, shaders, tools, documentation and assets (the courtyard scene, its textures,
the Blender scripts) are under the MIT license in [LICENSE](LICENSE). The textures were generated
for this project (Gemini image models via `tools/imagegen/gen.py`, then processed by
`tools/textures/`) or procedurally; the scene was built from scratch by `assets/blender/scripts/`.

Vendored libraries in `third_party/`, each under its own license (full text next to the code):

| Library | Version | License | File |
|---|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf) | v1.15 | MIT | `third_party/cgltf/LICENSE` |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.93.0 | MIT | `third_party/imgui/LICENSE.txt` |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | v1.3 | MIT | `third_party/meshoptimizer/LICENSE.md` |
| [SMAA](https://github.com/iryoku/smaa) | 71c806a | MIT-style (see file) | `third_party/smaa/LICENSE.txt` |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | 2c980bb | MIT or public domain (Unlicense), at your choice | end of each header |

Optional tool, not included: `texconv.exe` from [DirectXTex](https://github.com/microsoft/DirectXTex)
(MIT) builds the BC7 texture set (`tools/textures/compress_bc.py`).
