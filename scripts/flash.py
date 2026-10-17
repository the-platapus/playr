"""Flash the build output, refusing app images that overflow their partition.

Runs inside the IDF container (scripts/playr flash). idf.py flash only warns
when an app is larger than its partition and then writes it anyway, so the
next image silently overwrites its tail. Here an oversized app is skipped and
its partition erased instead, so the bootloader falls back to a valid app.

usage: python scripts/flash.py <port> <baud>
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "components", "partition_table"))
import gen_esp32part  # noqa: E402

BUILD = "build"


def main():
    port, baud = sys.argv[1], sys.argv[2]

    with open(os.path.join(BUILD, "flasher_args.json")) as f:
        args = json.load(f)
    with open(os.path.join(BUILD, "partition_table", "partition-table.bin"), "rb") as f:
        table = gen_esp32part.PartitionTable.from_binary(f.read())

    apps = {p.offset: p for p in table if p.type == gen_esp32part.APP_TYPE}
    writes, erases = [], []

    for offset, name in sorted((int(k, 16), v) for k, v in args["flash_files"].items()):
        size = os.path.getsize(os.path.join(BUILD, name))
        part = apps.get(offset)
        if part and size > part.size:
            print("playr: SKIPPING %s: %d bytes does not fit partition '%s' (%d bytes, %d over)"
                  % (name, size, part.name, part.size, size - part.size))
            erases.append(part)
            continue
        writes += ["0x%x" % offset, os.path.join(BUILD, name)]

    esptool = ["esptool.py", "-p", port, "-b", baud]

    for part in erases:
        print("playr: erasing partition '%s' so no stale app boots from it" % part.name)
        subprocess.check_call(esptool + ["--after", "no_reset", "erase_region", "0x%x" % part.offset, "0x%x" % part.size])

    subprocess.check_call(esptool + ["--before", "default_reset", "--after", "hard_reset", "write_flash"]
                          + args["write_flash_args"] + writes)


if __name__ == "__main__":
    main()
