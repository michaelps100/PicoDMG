"""Run independent SingleStepTests vectors against the actual cpu.c.

Use --download to cache the first --count vectors/opcode, or run offline with
an existing cache. No game ROMs are downloaded; the source repository is archived.
"""
import argparse
import ctypes
import json
from pathlib import Path
import urllib.request
from concurrent.futures import ThreadPoolExecutor

OFFICIAL = """00 01 05 06 08 09 0a 0d 0e 10 11 15 16 18 19 1d 1e
20 21 24 25 26 28 29 2a 2c 2d 2e 30 31 35 36 38 39 3d 3e
40 41 45 46 48 49 4a 4c 4d 4e 50 51 55 56 58 59 5d 5e
60 61 65 66 68 69 6a 6c 6d 6e 70 71 75 76 78 79 7d 7e
81 84 85 86 88 8a 8c 8d 8e 90 91 94 95 96 98 99 9a 9d
a0 a1 a2 a4 a5 a6 a8 a9 aa ac ad ae b0 b1 b4 b5 b6 b8 b9 ba bc bd be
c0 c1 c4 c5 c6 c8 c9 ca cc cd ce d0 d1 d5 d6 d8 d9 dd de
e0 e1 e4 e5 e6 e8 e9 ea ec ed ee f0 f1 f5 f6 f8 f9 fd fe""".split()

def download_prefix(op, cache, count):
    path = cache / f"{op}.json"
    if path.exists() and len(json.loads(path.read_text())) >= count:
        return
    url = f"https://raw.githubusercontent.com/SingleStepTests/ProcessorTests/main/6502/v1/{op}.json"
    decoder = json.JSONDecoder()
    data, result = "", []
    with urllib.request.urlopen(url, timeout=30) as response:
        while len(result) < count:
            chunk = response.read(8192).decode()
            if not chunk:
                raise RuntimeError(f"incomplete vectors for {op}")
            data += chunk
            while len(result) < count:
                data = data.lstrip(" \r\n\t,[")
                try:
                    item, end = decoder.raw_decode(data)
                except json.JSONDecodeError:
                    break
                result.append(item)
                data = data[end:]
    path.write_text(json.dumps(result))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    parser.add_argument("cache", type=Path)
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--count", type=int, default=64)
    parser.add_argument("--opcodes", default=" ".join(OFFICIAL))
    args = parser.parse_args()
    ops = args.opcodes.split()
    args.cache.mkdir(parents=True, exist_ok=True)
    if args.download:
        with ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(lambda op: download_prefix(op, args.cache, args.count), ops))
    lib = ctypes.CDLL(str(args.library.resolve()))
    total, failures, details = 0, 0, []
    for op in ops:
        vectors = json.loads((args.cache / f"{op}.json").read_text())
        for vector in vectors[:args.count]:
            initial, final = vector["initial"], vector["final"]
            lib.vector_init(*(initial[k] for k in ("pc", "a", "x", "y", "s", "p")))
            for address, value in initial["ram"]:
                lib.vector_poke(address, value)
            cycles = lib.vector_run()
            errors = []
            for i, key in enumerate(("pc", "a", "x", "y", "s", "p")):
                mask = 0xcf if key == "p" else 0xffff
                actual = lib.vector_register(i) & mask
                if actual != final[key] & mask:
                    errors.append(f"{key}: {actual:04x} != {final[key] & mask:04x}")
            for address, value in final["ram"]:
                if lib.vector_peek(address) != value:
                    errors.append(f"RAM {address:04x}")
            if cycles != len(vector["cycles"]):
                errors.append(f"cycles {cycles} != {len(vector['cycles'])}")
            for i, (address, value, kind) in enumerate(vector["cycles"]):
                expected = (address, value, int(kind == "write"))
                actual = tuple(lib.vector_trace(i, f) for f in range(3))
                if actual != expected:
                    errors.append(f"bus {i}: {actual} != {expected}")
            total += 1
            if errors:
                failures += 1
                if len(details) < 24:
                    details.append(f"{op} {vector['name']}: {'; '.join(errors)}")
    print("\n".join(details))
    print(f"SingleStepTests: {total} vectors, {len(ops)} opcodes, {failures} failures")
    return int(failures != 0)

if __name__ == "__main__":
    raise SystemExit(main())
