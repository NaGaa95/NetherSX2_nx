import argparse
import hashlib
from pathlib import Path
import re
import struct

def core_tables(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError(f"Not an AArch64 ELF: {path}")
    offset = struct.unpack_from("<Q", data, 40)[0]
    stride, count = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * stride) for i in range(count)]
    relocations = {}
    for section in sections:
        if section[1] == 4:
            for position in range(section[4], section[4] + section[5], section[9]):
                where, info, addend = struct.unpack_from("<QQq", data, position)
                if info & 0xffffffff == 1027:
                    relocations[where] = addend

    def pointer(address):
        if address in relocations:
            return relocations[address]
        for section in sections:
            if section[1] != 8 and section[3] <= address and address + 8 <= section[3] + section[5]:
                return struct.unpack_from("<Q", data, section[4] + address - section[3])[0]
        raise ValueError(f"Unmapped vtable pointer: {address:x}")

    return data, pointer

def verify_profiles(workspace, cores):
    text = (workspace / "source/deko3d/dk_profiles.h").read_text()
    # Strip comments before parsing. The address regexes below match any 0x...
    # literal, so a hex value mentioned in a comment inside a CoreProfile or
    # vtable body would be read as an address and silently shift every slot.
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    for version in ("4248", "3668"):
        profile = re.search(rf"CoreProfile Profile{version}\{{(.*?)\}};", text, re.S)[1]
        expected_hash = re.findall(r'"([0-9a-f]{64})"', profile)[0]
        addresses = [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", profile)]
        data, pointer = core_tables(cores / f"NetherSX2-v2.2n-{version}/lib/arm64-v8a/libemucore.so")
        if hashlib.sha256(data).hexdigest() != expected_hash:
            raise ValueError(f"Unsupported core hash: {version}")
        for name, address in zip(("Device", "Host"), addresses):
            entries = re.search(rf"{name}{version}\[\]\{{(.*?)\}};", text, re.S)[1]
            expected = [int(value, 16) for value in re.findall(r"0x[0-9a-f]+", entries)]
            for slot, method in enumerate(expected):
                if pointer(address + slot * 8) != method:
                    raise ValueError(f"{version} {name} vtable mismatch at slot {slot}")
        print(f"Core {version}: hash and every GSDevice/HostDisplay vtable entry verified")

def verify_link(path):
    text = path.read_text()
    forbidden = re.findall(r"lib(?:EGL|GLESv2|glapi|vulkan(?:_\w+)?|nvk|drm_nouveau|nir|nouveau_ws|nak_rs)\.a", text)
    if forbidden:
        raise ValueError(f"Graphics driver archives in native link: {sorted(set(forbidden))}")
    if not re.search(r"libdeko3dd?\.a", text) or "UamCompileGlsl" not in text:
        raise ValueError("Native link is missing Deko3D or the isolated UAM compiler")
    print(f"{path.name}: Deko3D + UAM linked; no GL/Vulkan driver archives")

def romfs_files(nro):
    data = nro.read_bytes()
    if data[16:20] != b"NRO0":
        raise ValueError(f"Not an NRO: {nro}")
    asset = struct.unpack_from("<I", data, 24)[0]
    if data[asset:asset + 4] != b"ASET":
        raise ValueError("NRO has no bundled assets")
    romfs_offset, romfs_size = struct.unpack_from("<QQ", data, asset + 40)
    base = asset + romfs_offset
    if base + romfs_size > len(data):
        raise ValueError("Truncated NRO RomFS")
    header = struct.unpack_from("<10Q", data, base)
    directories, files, contents = base + header[3], base + header[7], base + header[9]
    result = {}

    def visit(entry, parent):
        _, _, child, file, _, length = struct.unpack_from("<6I", data, directories + entry)
        name = data[directories + entry + 24:directories + entry + 24 + length].decode()
        folder = parent + name + "/" if name else parent
        while file != 0xffffffff:
            _, sibling, offset, size, _, length = struct.unpack_from("<IIQQII", data, files + file)
            name = data[files + file + 32:files + file + 32 + length].decode()
            result[folder + name] = memoryview(data)[contents + offset:contents + offset + size]
            file = sibling
        while child != 0xffffffff:
            visit(child, folder)
            child = struct.unpack_from("<I", data, directories + child + 4)[0]

    visit(0, "")
    return result

def verify_bundle(nro, workspace, cores):
    files = romfs_files(nro)
    # Vulkan-only: the bundle ships just the NVK emulator. The dk and gl targets
    # are still buildable from the Makefile, but they are no longer bundled, so
    # requiring them here would fail every bundle built after the switch.
    expected = {"emu/NetherSX2_nx_vk.nro": workspace / "NetherSX2_nx_vk.nro"}
    for version in ("4248", "3668"):
        expected[f"cores/emucore_{version}.so"] = cores / f"NetherSX2-v2.2n-{version}/lib/arm64-v8a/libemucore.so"
        expected[f"res/{version}/shaders/opengl/tfx_fs.glsl"] = cores / f"NetherSX2-v2.2n-{version}/assets/shaders/opengl/tfx_fs.glsl"
    for name, source in expected.items():
        if name not in files or hashlib.sha256(files[name]).digest() != hashlib.sha256(source.read_bytes()).digest():
            raise ValueError(f"Missing or outdated bundled artifact: {name}")
    print(f"{nro.name}: both cores, the Vulkan (NVK) NRO and per-core GS shader resources verified")

def main():
    workspace = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cores-dir", type=Path, default=workspace / "out/cores")
    parser.add_argument("--map", type=Path, default=workspace / "build-dk/NetherSX2_nx_dk.map",
                        help="link map of the native Deko3D target; checked only when it exists")
    parser.add_argument("--bundle", type=Path)
    args = parser.parse_args()
    verify_profiles(workspace, args.cores_dir)
    # The Deko3D target is no longer part of the shipped bundle, so its map is
    # optional: verify it when a build produced one, skip quietly otherwise.
    if args.map.is_file():
        verify_link(args.map)
    else:
        print(f"{args.map}: not built, skipping the Deko3D link check")
    if args.bundle:
        verify_bundle(args.bundle, workspace, args.cores_dir)

if __name__ == "__main__":
    main()
