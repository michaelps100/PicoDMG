"""Verify diagnostic retention against the current app and HW16 loader maps."""
import re
import sys
from pathlib import Path


def section(text, name):
    match = re.search(r"^" + re.escape(name) + r"\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", text, re.M)
    assert match, f"missing section {name}"
    return tuple(int(s, 16) for s in match.groups())


def overlaps(a, b):
    return a[0] < b[0] + b[1] and b[0] < a[0] + a[1]


app = Path(sys.argv[1]).read_text(errors="replace")
loader = Path(sys.argv[2]).read_text(errors="replace")
record = section(app, ".pce_diag_retained")
reserve = section(app, "PCE_DIAG_RAM")
assert reserve == (0x2007AF00, 256)
assert reserve[0] <= record[0] and record[0] + record[1] <= sum(reserve)
assert not overlaps(record, section(app, ".stack_dummy"))
assert not overlaps(record, section(app, ".stack1_dummy"))
heap = re.search(r"(0x[0-9a-f]+)\s+__HeapLimit =", app)
assert heap and int(heap[1], 16) <= record[0]
for name in (".data", ".bss", ".stack_dummy", ".stack1_dummy"):
    assert not overlaps(record, section(loader, name)), f"loader {name} overlaps report"
assert overlaps((0x20000110, 64), section(loader, ".data")), "old overlap no longer reproduced"
print(f"PASS linker: {record[1]}-byte NOLOAD report at {record[0]:08X}, outside app heap/stacks and loader data/bss/stacks")
print("PASS regression: old low-SRAM report overlapped loader .data")
