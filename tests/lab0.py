#!/usr/bin/env python3
"""Check four-core boot output and BSS initialization using QEMU and GDB."""

import argparse
import re
import socket
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QEMU = [
    "qemu-system-aarch64", "-machine", "virt,gic-version=3",
    "-cpu", "cortex-a72", "-smp", "4", "-m", "4096",
    "-nographic", "-monitor", "none", "-serial", "mon:stdio",
    "-global", "virtio-mmio.force-legacy=false",
    "-kernel", str(ROOT / "build/src/kernel8.elf"),
]


def check_hello(output):
    cores = re.findall(r"^Hello, world! \(Core (\d+)\)\r?$", output, re.M)
    assert len(cores) == 4 and cores[0] == "0" and sorted(cores) == list("0123"), output
    return " ".join(cores)


def wait_for_hello(process, log):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        output = log.read_text(errors="replace")
        if output.count("Hello, world!") >= 4:
            time.sleep(0.2)
            return check_hello(log.read_text(errors="replace"))
        if process.poll() is not None:
            raise AssertionError(f"QEMU exited early: {output}")
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for four cores: {output}")


def stop(process):
    if process.poll() is None:
        try:
            process.communicate(input=b"\x01x", timeout=5)
        except (subprocess.TimeoutExpired, BrokenPipeError):
            process.kill()
            process.wait()


def check_bss(directory):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    script = directory / "bss.gdb"
    script.write_text(f"""set pagination off
set confirm off
set remotetimeout 10
file {ROOT / 'build/src/kernel8.elf'}
target remote 127.0.0.1:{port}
thbreak main
continue
python
inferior = gdb.selected_inferior()
begin = int(gdb.parse_and_eval('(unsigned long long)edata'))
end = int(gdb.parse_and_eval('(unsigned long long)end'))
size = end - begin
assert size > 0, 'BSS range is empty'
before = bytes(inferior.read_memory(begin - 16, 16))
after = bytes(inferior.read_memory(end, 16))
inferior.write_memory(begin, bytes([0xa5]) * size)
inferior.write_memory(end, bytes([0x5a]) * 16)
assert bytes(inferior.read_memory(begin, size)) == bytes([0xa5]) * size
print('Injected 0xa5 into [0x%x, 0x%x): %d bytes' % (begin, end, size))
end
thbreak smp_init
continue
python
assert bytes(inferior.read_memory(begin, size)) == bytes(size), 'BSS was not cleared before smp_init'
assert bytes(inferior.read_memory(begin - 16, 16)) == before, 'Data before BSS was modified'
assert bytes(inferior.read_memory(end, 16)) == bytes([0x5a]) * 16, 'BSS clear wrote past end'
inferior.write_memory(end, after)
print('PASS: BSS is all zero before smp_init; both boundary guards are intact.')
end
detach
quit
""")
    log = directory / "bss-qemu.log"
    with log.open("wb") as output:
        process = subprocess.Popen(QEMU + ["-S", "-gdb", f"tcp:127.0.0.1:{port}"],
                                   stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT)
        try:
            # QEMU opens its GDB listener before this short startup grace period ends.
            time.sleep(1)
            assert process.poll() is None, log.read_text(errors="replace")
            result = subprocess.run(["gdb-multiarch", "--nx", "--batch", "-x", str(script)],
                                    capture_output=True, text=True, timeout=30)
            print(result.stdout, end="")
            assert result.returncode == 0, result.stdout + result.stderr
            assert "PASS: BSS" in result.stdout, result.stdout + result.stderr
            print("PASS: boot after BSS injection:", wait_for_hello(process, log))
        finally:
            stop(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    orders = set()
    with tempfile.TemporaryDirectory(prefix="lab0-test-") as temporary:
        directory = Path(temporary)
        for index in range(args.runs):
            log = directory / f"boot-{index}.log"
            with log.open("wb") as output:
                process = subprocess.Popen(QEMU, stdin=subprocess.PIPE, stdout=output,
                                           stderr=subprocess.STDOUT)
                try:
                    order = wait_for_hello(process, log)
                    orders.add(order)
                    print(f"PASS: boot {index + 1}/{args.runs}: {order}")
                finally:
                    stop(process)
        check_bss(directory)
    print(f"PASS: {args.runs} boots; observed orders: {', '.join(sorted(orders))}")


if __name__ == "__main__":
    main()
