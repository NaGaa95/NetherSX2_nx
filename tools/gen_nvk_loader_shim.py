#!/usr/bin/env python3
"""Generate source/vk_loader_shim.c for the ICD-only NXVK portlib package.

The nxvk package (berlint-hub/nxvk, switch-vulkan.yml) ships libnvk.a +
libnvk_support.a which define exactly one public symbol,
vk_icdGetInstanceProcAddr -- there is no Vulkan loader on Horizon. The older
23-archive layout in vulkan/lib did export the public entry points
(libvulkan_lite_instance/libvulkan_lite_runtime), which is why vendored code
calling them directly linked there.

source/hooks/vk.c -- the layer facing libemucore.so -- calls the driver directly
and keeps raw-symbol fallbacks for vkGetDeviceQueue{,2}, which are link-time
references. third_party/lsfg-vk does not need this: it dispatches through its
own vk.df()/vk.if() tables. This generates one forwarder per referenced entry
point that resolves lazily through vk_icdGetInstanceProcAddr.

Prototypes are parsed out of the package headers so the definitions cannot drift
from the declarations. Re-run after changing which VK sources are compiled:

    python3 tools/gen_nvk_loader_shim.py \\
        --vulkan-include "$DEVKITPRO/portlibs/switch/include/vulkan"

The output is guarded by NETHERSX2_NVK_ICD_ONLY, which the Makefile defines only
for the pkg-config (portlib) link path, so the vendored build is untouched.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

# Directories compiled into RENDERER=VK, per the Makefile.
VK_SOURCE_DIRS = [
    "source",
    "source/hooks",
    "source/switch",
    "source/lsfg",
    "third_party/lsfg-vk/lsfg-vk-common/src/helpers",
    "third_party/lsfg-vk/lsfg-vk-common/src/vulkan",
    "third_party/lsfg-vk/lsfg-vk-backend/src",
    "third_party/lsfg-vk/lsfg-vk-backend/src/extraction",
    "third_party/lsfg-vk/lsfg-vk-backend/src/helpers",
    "third_party/lsfg-vk/lsfg-vk-backend/src/shaderchains",
]

# Supplied by the shim in source/hooks/vk.c; defining it twice would collide.
PROVIDED_ELSEWHERE = {"vkGetInstanceProcAddr"}

# Repo-internal helpers, not public entry points.
INTERNAL_SUFFIX = ("_shim", "_hook", "_native", "_impl", "_real")

# Global commands: no instance exists yet, so query them with VK_NULL_HANDLE.
GLOBAL_COMMANDS = {
    "vkCreateInstance",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateInstanceVersion",
}

# Any vk* identifier, not only calls: hooks/vk.c also takes the address of an
# entry point (real_get_device_queue ? real_get_device_queue : vkGetDeviceQueue),
# and a bare reference links exactly the same. PFN_vkFoo typedefs are excluded
# for free because \b does not fire after the underscore.
IDENT = re.compile(r"\b(vk[A-Z][A-Za-z0-9]*)\b")
RAW_STRING = re.compile(r'R"([^(]*)\(')
PROTO = re.compile(
    r"VKAPI_ATTR\s+(?P<ret>[^()]*?)\s+VKAPI_CALL\s+(?P<name>vk\w+)\s*\((?P<params>.*?)\)\s*;",
    re.S,
)


def strip_noise(text: str) -> str:
    """Blank out comments and string/char literals in a single left-to-right pass.

    One regex per construct is not enough: stripping // before string literals
    lets a URL inside a string eat the rest of its line, and an unterminated /*
    left by a C++ raw string swallows whole files. Comments and literals cannot
    nest, so a scanner that takes whichever starts first is correct.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j  # leave the newline itself for line numbers
            out.append(" ")
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        raw = RAW_STRING.match(text, i)
        if raw:
            delim = raw.group(1)
            end = text.find(")" + delim + '"', raw.end())
            i = n if end < 0 else end + len(delim) + 2
            out.append(' "" ')
            continue
        if c in "\"'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == c:
                    break
                j += 1
            i = min(j + 1, n)
            out.append(' "" ')
            continue
        out.append(c)
        i += 1
    return "".join(out)


def referenced_entry_points(root: Path) -> list[str]:
    names: set[str] = set()
    for rel in VK_SOURCE_DIRS:
        d = root / rel
        if not d.is_dir():
            continue
        for path in sorted(list(d.glob("*.c")) + list(d.glob("*.cpp"))):
            for m in IDENT.finditer(strip_noise(path.read_text(errors="ignore"))):
                names.add(m.group(1))
    return [n for n in sorted(names)
            if n not in PROVIDED_ELSEWHERE and not n.endswith(INTERNAL_SUFFIX)]


def split_params(params: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return [p for p in out if p and p != "void"]


def param_name(decl: str) -> str:
    """'const VkSubmitInfo* pSubmits' -> 'pSubmits'; 'VkCommandBuffer b[]' -> 'b'."""
    d = decl.replace("VKAPI_PTR", "").strip()
    d = re.sub(r"\[\s*\]", "", d)
    return d.split()[-1].lstrip("*")


def prototypes(vulkan_include: Path) -> dict[str, tuple[str, list[str]]]:
    protos: dict[str, tuple[str, list[str]]] = {}
    for header in sorted(vulkan_include.glob("*.h")):
        for m in PROTO.finditer(header.read_text(errors="ignore")):
            protos.setdefault(m.group("name"),
                              (m.group("ret").strip(), split_params(m.group("params"))))
    return protos


def fallback(ret: str) -> str:
    if ret == "void":
        return "return;"
    if ret == "VkResult":
        return "return VK_ERROR_INITIALIZATION_FAILED;"
    return f"return ({ret})0;"


def emit(name: str, ret: str, params: list[str]) -> str:
    decl = ",\n    ".join(re.sub(r"\s+", " ", p) for p in params) or "void"
    args = ", ".join(param_name(p) for p in params)
    handle = "VK_NULL_HANDLE" if name in GLOBAL_COMMANDS else "nvk_loader_instance"
    call = f"fn({args})" if params else "fn()"

    body = [
        f"VKAPI_ATTR {ret} VKAPI_CALL {name}(",
        f"    {decl}) {{",
        f"  static PFN_{name} fn;",
        f"  if (!fn)",
        f'    fn = (PFN_{name})vk_icdGetInstanceProcAddr({handle}, "{name}");',
        f"  if (!fn)",
        f"    {fallback(ret)}",
    ]
    if name == "vkCreateInstance":
        # The only place a real instance is born; record it so every later
        # forwarder can dispatch through it without hooks/vk.c having to care.
        body += [
            f"  VkResult res = {call};",
            "  if (res == VK_SUCCESS && pInstance)",
            "    nvk_loader_instance = *pInstance;",
            "  return res;",
        ]
    elif ret == "void":
        body += [f"  {call};"]
    else:
        body += [f"  return {call};"]
    body.append("}\n")
    return "\n".join(body)


HEADER = '''/* vk_loader_shim.c -- minimal Vulkan loader for the ICD-only NXVK package.
 *
 * GENERATED FILE -- do not edit by hand. Regenerate with:
 *   python3 tools/gen_nvk_loader_shim.py \\
 *     --vulkan-include "$DEVKITPRO/portlibs/switch/include/vulkan"
 *
 * libnvk.a in the nxvk portlib package defines exactly one public symbol,
 * vk_icdGetInstanceProcAddr: Horizon has no Vulkan loader, so nothing supplies
 * the public vk* entry points. The vendored 23-archive layout used to
 * (libvulkan_lite_instance/libvulkan_lite_runtime), which is why third-party
 * code calling them directly linked there.
 *
 * source/hooks/vk.c is the layer that faces libemucore.so: it calls the driver
 * directly (vkCreateInstance, vkCreateViSurfaceNN, vkGetPhysicalDevice*,
 * vkEnumerate*) and keeps raw-symbol fallbacks such as
 *   real_get_device_queue ? real_get_device_queue : vkGetDeviceQueue
 * which is a link-time reference even though the primary path resolves the same
 * function at runtime. third_party/lsfg-vk does NOT need this -- it dispatches
 * through its own vk.df()/vk.if() tables -- so the set below is exactly what
 * hooks/vk.c names. Each one gets a forwarder that resolves lazily through
 * vk_icdGetInstanceProcAddr and caches the result in a function-local static.
 *
 * Dispatch handles: global commands (vkCreateInstance, vkEnumerateInstance*) are
 * queried with VK_NULL_HANDLE because no instance exists yet. Everything else
 * uses the instance recorded by the vkCreateInstance forwarder -- Mesa's ICD
 * answers instance, device and queue level queries from an instance handle,
 * which is exactly what a loader does.
 *
 * Compiled only for the pkg-config link path: NETHERSX2_NVK_ICD_ONLY is defined
 * by the Makefile when nxvk.pc is present, never for the vendored vulkan/lib
 * tree, whose libvulkan_lite_* would collide with these definitions.
 */
#ifdef NETHERSX2_NVK_ICD_ONLY

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#include <vulkan/vk_icd.h>

static VkInstance nvk_loader_instance = VK_NULL_HANDLE;

'''

FOOTER = "\n#endif /* NETHERSX2_NVK_ICD_ONLY */\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--vulkan-include", type=Path, default=None,
                    help="directory holding vulkan_core.h "
                         "(default: <root>/vulkan/include/vulkan)")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--list", action="store_true",
                    help="print the entry points that would be generated")
    args = ap.parse_args()

    root: Path = args.root
    inc = args.vulkan_include or root / "vulkan/include/vulkan"
    if not (inc / "vulkan_core.h").is_file():
        raise SystemExit(f"vulkan_core.h not found under {inc}")

    wanted = referenced_entry_points(root)
    protos = prototypes(inc)
    missing = [n for n in wanted if n not in protos]
    if missing:
        raise SystemExit("no prototype found for: " + ", ".join(missing))

    if args.list:
        for n in wanted:
            print(n)
        return 0

    out = args.out or root / "source/vk_loader_shim.c"
    body = "".join(emit(n, *protos[n]) + "\n" for n in wanted)
    out.write_text(HEADER + body + FOOTER)
    print(f"wrote {out}: {len(wanted)} forwarders")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
