#!/usr/bin/env python3
"""Build native sanitizer tests and run an unchanged iceprog against the firmware.

Pass --iceprog-source /path/to/icestorm/iceprog, or fetch the pinned upstream
files into the ignored build directory. No Pico or FPGA is accessed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "host-tests"
REVISION = "f31c39cc2eadd0ab7f29f34becba1348ae9f8721"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iceprog-source", type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    upstream = args.iceprog_source
    if upstream is None:
        upstream = BUILD / "icestorm" / REVISION
        upstream.mkdir(parents=True, exist_ok=True)
        for name in ("iceprog.c", "mpsse.c", "mpsse.h"):
            path = upstream / name
            if not path.exists():
                url = f"https://raw.githubusercontent.com/YosysHQ/icestorm/{REVISION}/iceprog/{name}"
                path.write_bytes(urllib.request.urlopen(url).read())
    upstream = upstream.resolve()
    print(f"Testing iceprog source in {upstream}", flush=True)
    for name in ("iceprog.c", "mpsse.c", "mpsse.h"):
        print(f"  {name}: {hashlib.sha256((upstream / name).read_bytes()).hexdigest()}")
    common = [args.cc, "-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined",
              "-fno-omit-frame-pointer", "-Itests/stubs", "-Itests",
              "tests/firmware_host.c", "tests/model.c"]
    subprocess.run(common + ["-DTEST_PROTOCOL", "-o", str(BUILD / "protocol")], cwd=ROOT, check=True)
    subprocess.run([BUILD / "protocol"], check=True, timeout=30)
    subprocess.run(common + [str(upstream / "iceprog.c"), str(upstream / "mpsse.c"),
                            "-o", str(BUILD / "iceprog")], cwd=ROOT, check=True)

    count = 0
    with tempfile.TemporaryDirectory(prefix="pico-iceprog-tests-") as temp:
        work = Path(temp)
        flash, stats, sram = (work / name for name in ("flash.bin", "stats.json", "sram.bin"))
        image = bytes((i*73 + i//7 + 19) & 255 for i in range(8195))
        (work / "image.bin").write_bytes(image)
        (work / "different.bin").write_bytes(bytes(b ^ 1 for b in image))
        (work / "empty.bin").write_bytes(b"")

        def run(options, *, extra=None, input=None, expected=0):
            nonlocal count
            env = dict(os.environ, TEST_FLASH=str(flash), TEST_STATS=str(stats), TEST_SRAM=str(sram))
            env.update(extra or {})
            result = subprocess.run([BUILD / "iceprog", *options], env=env, cwd=work,
                                    input=input, capture_output=True, timeout=30)
            if result.returncode != expected:
                raise AssertionError(f"iceprog {' '.join(options)}: {result.returncode}, expected {expected}\n"
                                     + result.stderr.decode(errors="replace"))
            assert b"runtime error" not in result.stderr and b"AddressSanitizer" not in result.stderr
            count += 1
            return json.loads(stats.read_text()), result

        # All programming paths on both channels, at both supported speeds.
        for port in ("A", "B"):
            for slow in (False, True):
                base = ["-I", port] + (["-s"] if slow else [])
                flash.unlink(missing_ok=True)
                data, result = run(base + ["-t"])
                assert b"0xEF 0x40 0x15" in result.stderr and data["reset_bits"] == 1
                assert data["div_a" if port == "A" else "div_b"] == (600 if slow else 5)
                for erase in ("4", "32", "64"):
                    # Offset 255 crosses a flash page immediately and is not sector aligned.
                    flash.write_bytes(bytes(2*1024*1024))
                    data, result = run(base + ["-i", erase, "-o", "255", "image.bin"])
                    assert b"VERIFY OK" in result.stderr
                    mem = flash.read_bytes()
                    assert mem[255:255+len(image)] == image
                    end = ((255+len(image)+int(erase)*1024-1)//(int(erase)*1024))*int(erase)*1024
                    assert mem[:255] == b"\xff"*255 and mem[255+len(image):end] == b"\xff"*(end-255-len(image))
                    assert mem[end:] == bytes(len(mem)-end)
                    run(base + ["-c", "-o", "255", "image.bin"])
                    run(base + ["-c", "-o", "255", "different.bin"], expected=3)
                    run(base + ["-R", str(len(image)), "-o", "255", "read.bin"])
                    assert (work / "read.bin").read_bytes() == image
                run(base + ["-b", "image.bin"])
                assert flash.read_bytes()[:len(image)] == image
                run(base + ["-b"])
                assert flash.read_bytes() == b"\xff"*(2*1024*1024)
                run(base + ["-n", "-X", "-k", "image.bin"])
                assert flash.read_bytes()[:len(image)] == image
                assert json.loads(stats.read_text())["power_down"] == 0
                run(base + ["-r", "read.bin"])
                assert (work / "read.bin").read_bytes() == flash.read_bytes()[:256*1024]
                for erase in ("4", "32", "64"):
                    flash.write_bytes(bytes(2*1024*1024))
                    run(base + ["-i", erase, "-o", "1k", "-e", "1k"])
                    assert flash.read_bytes()[:int(erase)*1024] == b"\xff"*(int(erase)*1024)
                    assert flash.read_bytes()[int(erase)*1024:] == bytes(2*1024*1024-int(erase)*1024)
                data, result = run(base + ["-p", "-v", "image.bin"], extra={"TEST_PROTECTED": "1"})
                assert data["sr1"] == 0 and b"VERIFY OK" in result.stderr
                data, _ = run(base + ["-Q"])
                assert data["sr2"] & 2
                data, _ = run(base + ["-S", "image.bin"])
                assert data["sram_bits"] == len(image)*8+49
                assert sram.read_bytes()[:len(image)] == image
                run(base + ["empty.bin"])

        # Short USB packets, stdin/stdout and alternate device selection.
        for packet in (1, 7, 31, 63, 64):
            env = {"TEST_PACKET_SIZE": str(packet)}
            run(["-d", "i:0x0403:0x6010", "-"], input=image, extra=env)
            _, result = run(["-R", str(len(image)), "-"], extra=env)
            assert result.stdout == image
            data, _ = run(["-S", "-"], input=image, extra=env)
            assert data["sram_bits"] == len(image)*8+49 and sram.read_bytes()[:len(image)] == image
        # Verify-only stdin and protection-only operation.
        run(["-c", "-"], input=image)
        data, _ = run(["-p"], extra={"TEST_PROTECTED": "1"})
        assert data["sr1"] == 0
    print(f"PASS: protocol suite and {count} unchanged-iceprog invocations under ASan/UBSan", flush=True)


if __name__ == "__main__":
    main()
