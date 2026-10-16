# Host-only devkit harness + CI build

Dvě věci, které patří k sobě:

1. **`../../.github/workflows/build.yml`** — skutečný build `.nro` na GitHub
   Actions. Runner má plný internet, takže použije obraz `devkitpro/devkita64`
   a postaví vše, co nepotřebuje vstupy mimo repozitář.
2. **Tenhle adresář** — co se dá dělat v sandboxu, který devkitA64 nemá
   (audit požadavků, host validace, sysroot z hlaviček).

## Proč build běží v CI a ne v sandboxu

Allowlist sandboxu je jen `github.com`, `codeload.github.com`, `api.github.com`,
`pypi.org`, `files.pythonhosted.org`, `registry.npmjs.org`. Změřeno, vše `000`:
`pkg.devkitpro.org`, `apt.devkitpro.org`, `downloads.devkitpro.org`,
`deb.debian.org`, `release-assets.githubusercontent.com`,
`results-receiver.actions.githubusercontent.com`, `registry-1.docker.io`,
`ghcr.io`. Žádný container runtime (`docker`/`podman`/`nerdctl`/`buildah`/
`skopeo` chybí, `/var/run/docker.sock` neexistuje, PID 1 je `systemd`).

devkitPro nedrží binární toolchain v gitu (`devkitPro/pacman-packages` má 0
release, repo `devkitPro/devkitA64` neexistuje) a `devkitPro/buildscripts`
stahuje gcc/binutils/newlib z `downloads.devkitpro.org`, takže ani build ze
zdroje není možný.

**Důsledek pro CI:** logy runu ze sandboxu nestáhneš. Kroky proto píší
`::notice::` / `::warning::` / `::error::`, což jde přečíst přes annotations API:

```sh
gh api repos/<owner>/<repo>/actions/runs/<run_id>/jobs --jq '.jobs[].id'
gh api repos/<owner>/<repo>/check-runs/<job_id>/annotations --jq '.[]|"[\(.annotation_level)] \(.message)"'
```

## Co obraz `devkitpro/devkita64:latest` skutečně obsahuje

Změřeno během runu 38939048512 (2026-10-16), job `switch`:

| Položka | Stav |
|---|---|
| `devkitA64/base_rules` | ✅ |
| `libnx/lib/libnx.a` | ✅ |
| `cmake/Switch.cmake` (devkitpro-cmake) | ✅ |
| switch-tools (`elf2nro`, `elf2nso`, `elf2kip`, `nacptool`, `npdmtool`, `build_pfs0`) | ✅ |
| portlibs: **112 archivů**, 90+ `.pc` | ✅ |
| z portlibs konkrétně | `sdl2`, `SDL2_ttf`, `SDL2_image`, `libcurl`, `libturbojpeg`, `zlib`, `zstd`, `libpng`, `egl`, `glesv2`, `glapi`, `libdrm_nouveau`, `libntfs-3g`, `expat`, `liblz4`, `libxml-2.0`, `freetype2`, `libjpeg` |
| pacman | ⚠️ **`pacman` není v PATH, je tam `dkp-pacman`** |
| bison / flex | ❌ → `apt-get install bison flex` (doplní 3.8.2 / 2.6.4) |
| cmake / ninja | ❌ → `apt-get` (doplní cmake 3.31.6) |
| `switch-zstd` jako balíček | ❌ neexistuje, zstd už v obraze je |

Balíčky, které workflow stejně doinstalovává přes `dkp-pacman`, protože obraz je
nemusí mít všechny: `switch-sdl2`, `switch-sdl2_ttf`, `switch-sdl2_image`,
`switch-curl`, `switch-libpng`, `switch-ntfs-3g`, `switch-mesa`,
`switch-libdrm_nouveau`, `deko3d`, `switch-zlib`, `switch-libjpeg-turbo`.

## Výsledek buildu

| Cíl | Stav | Velikost |
|---|---|---|
| `NetherSX2_nx_dk.nro` (native Deko3D) | ✅ | 5 118 851 B |
| `NetherSX2_nx_gl.nro` (OpenGL) | ✅ | 7 105 411 B |
| `launcher/NetherSX2.nro` + `launcher/fwd` | ✅ | v artefaktu |
| `NetherSX2_nx_vk.nro` (Mesa NVK) | ⏭️ přeskočeno | potřebuje `vulkan/lib/*.a` |
| finální bundle `NetherSX2.nro` | ⏭️ přeskočeno | potřebuje APK cores |
| `out/deko3d-deps` (UAM + SPIRV-Cross + LSFG) | ✅ | — |
| `launcher/dependencies/build` (libsmb2 + libusbhsfs) | ✅ | — |

Job `switch` trvá ~3 minuty, `host-tests` ~25 s. Artefakt `nethersx2-switch`
je ~12,4 MB a jde stáhnout ze stránky runu (ze sandboxu ne).

Chybějící dva vstupy jsou přesně ty, které nelze distribuovat z repa:
`vulkan/lib` (23 archivů Mesa NVK, viz `-l:lib*.a` v `Makefile`) a rozbalené
APK cores `NetherSX2-v2.2n-4248` / `-3668`. Workflow je detekuje kroky
`Vulkan (Mesa NVK)` a `Cores` a příslušné cíle odstaví.

## Skripty

```sh
. tools/host_devkit/env.sh                          # DEVKITPRO + PATH
bash tools/host_devkit/setup_host_devkit.sh         # ~14 s, idempotentní
bash tools/host_devkit/run_host_tests.sh            # 7/8 testů
bash tools/host_devkit/audit_build_requirements.sh  # co chce build a co máme
```

**Sandbox mezi turny maže vše, co není v gitu.** `/home/user/devkitpro` a
`/home/user/toolchain` zmizí; `setup_host_devkit.sh` je obnoví. Proto skripty
bydlí v repu a sysroot mimo něj.

`setup_host_devkit.sh` dá dohromady:

* `cmake` 4.4.4, `ninja` 1.13.2, `Mako` 1.4.3 — PyPI wheels
* libnx hlavičky (180) + `switch_rules`/`switch.specs`/`switch.ld` — `switchbrew/libnx`
* `deko3d.h`/`.hpp` — `devkitPro/deko3d`
* **všech 8 switch-tools postavených host gcc** — `switchbrew/switch-tools`
  (`elf2nro`, `elf2nso`, `elf2kip`, `nacptool`, `npdmtool`, `build_pfs0`,
  `build_romfs`) + `devkitPro/general-tools` (`bin2s`, potřebuje
  `-DPACKAGE_STRING`). `elf2nro` vyžaduje lz4, jehož prebuilt verze je release
  asset, takže se staví i `liblz4.a` ze `lz4/lz4`
* EGL/GLES2 **host stuby** — jen typedefy, které odkazuje `source/hooks.h`,
  aby šel zkompilovat `tests/patches`. Pro opravdový Switch build patří
  `switch-mesa`, ne tohle

`$DEVKITPRO/{devkitA64/bin,libnx/lib,portlibs/switch/lib}` jsou prázdné — to je
část, kterou sandbox dodat neumí. `audit_build_requirements.sh` to vypíše po
položkách; cesty parsuje z `Makefile`/`CMakeLists`, takže nezastará.

## Testy: 7 z 8

| Test | Jak | Stav |
|---|---|---|
| `tests/patches` | CMake + Ninja + ctest | PASS, 15 core patch checks |
| `tests/network` | CMake + Ninja + ctest | PASS, 4 network checks |
| `tests/deko3d/transfer_validation` | `g++ -std=c++20` | PASS, 17 DMA případů |
| `tests/deko3d/render_target_validation` | idem | PASS, 25 případů |
| `tests/deko3d/vertex_format_validation` | idem | PASS, 16 atributů |
| `tests/deko3d/render_state_validation` | idem | PASS, 59 regresí |
| `tests/deko3d/presentation_validation` | idem + `dk_presentation.cpp` | PASS, 255 kontrol |
| `tests/deko3d/shader_validation` | — | SKIP: linkuje `uam`, vyžaduje bison + flex |

Deko3D testy se kompilují přímo, protože
`source/deko3d/compiler/CMakeLists.txt` includuje `Uam.cmake` bezpodmínečně a to
bez bison/flex spadne na `FATAL_ERROR`. `build_all.sh` je stejně vypíná
(`-DDEKO3D_SHADER_TESTS=OFF`). V CI běží obojí: `host-tests` bez devkitA64 a
`shader_validation` nepřímo jako součást DK buildu.
