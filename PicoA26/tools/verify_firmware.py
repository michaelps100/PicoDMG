"""Validate generated UF2s, SRAM margin and save/load stack; write a manifest."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parent.parent

def verify(build, artifact):
    folder = ROOT / build
    data = (ROOT / artifact).read_bytes()
    assert data == (folder / "PicoA26.uf2").read_bytes(), "root UF2 differs from build"
    assert len(data) % 512 == 0
    targets = []
    numbers = set()
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        magic0, magic1, flags, address, length, number, total, family = struct.unpack_from("<8I", block)
        assert (magic0, magic1) == (0x0A324655, 0x9E5D5157)
        assert struct.unpack_from("<I", block, 508)[0] == 0x0AB16F30
        assert total == len(data) // 512 and number not in numbers and length <= 476
        numbers.add(number)
        if not flags & 1:
            targets.append(address)
            assert 0x10080000 <= address < 0x11000000
    assert min(targets) == 0x10080000, "wrong bootloader partition"
    mapping = (folder / "PicoA26.elf.map").read_text(errors="replace")
    def symbol(name):
        return int(re.search(r"0x([0-9a-fA-F]+)\s+" + re.escape(name) + r"\s*=", mapping)[1], 16)
    heap = symbol("__HeapLimit") - symbol("__end__")
    assert heap >= 50 * 1024, f"insufficient SRAM margin: {heap}"
    stack = {}
    for path in (folder / "CMakeFiles/PicoA26.dir").rglob("*.su"):
        for line in path.read_text(errors="replace").splitlines():
            for name in ("atari_save_state", "atari_load_state", "a26_core_save_state", "a26_core_load_state"):
                if re.search(r"\b" + name + r"\b", line):
                    stack[name] = int(line.split("\t")[1])
    assert len(stack) == 4 and max(stack.values()) <= 256, stack
    return {
        "artifact": artifact,
        "build_id": "1.1-timing-v4",
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "partition": "0x10080000",
        "linker_heap_margin_bytes": heap,
        "save_load_stack_bytes_per_function": stack,
        "hardware_validated": False,
    }

def main():
    result = {
        "date": "2026-10-19",
        "sdk": "2.3.1",
        "compiler": "Arm GNU 15.2.Rel1",
        "note": "Linker margin precedes runtime allocations; stack values exclude callees.",
        "targets": [verify("build_bootloader", "PicoA26.uf2"), verify("build_hw16", "PicoA26_hw16.uf2")],
    }
    path = ROOT / "docs/build-validation.json"
    path.parent.mkdir(exist_ok=True)
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
