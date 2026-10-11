"""Reject split firmware import tables; complements the unchanged ME verifier."""
from pathlib import Path
import argparse
import json
import struct
from verify_artifacts import sections


def verify(build, output):
    path = build / "psp-dualcore-actions"
    if not path.exists():
        path = build / "psp-dualcore-actions.elf"
    data = path.read_bytes()
    sec = sections(data)
    entries = []
    table = sec[".lib.stub"]
    cursor = table["offset"]
    while cursor < table["offset"] + table["size"]:
        name, flags, words, variables, count, nids, funcs = struct.unpack_from("<IIBBHII", data, cursor)
        assert words >= 5 and not variables and count, "Unexpected import record"
        assert cursor + words * 4 <= table["offset"] + table["size"]
        entries.append({"functions": count, "nids": nids, "stubs": funcs})
        cursor += words * 4
    for section_name, key, stride in ((".rodata.sceNid", "nids", 4), (".sceStub.text", "stubs", 8)):
        region = sec[section_name]
        expected = region["addr"]
        for entry in sorted(entries, key=lambda value: value[key]):
            assert entry[key] == expected, f"Split/overlapping import table in {section_name}"
            expected += entry["functions"] * stride
        assert expected == region["addr"] + region["size"], f"Unclaimed imports in {section_name}"
    result = {"status": "PASS: contiguous firmware import NID/stub ranges",
              "libraries": len(entries), "functions": sum(entry["functions"] for entry in entries)}
    if output:
        output.mkdir(parents=True, exist_ok=True)
        (output / "import-layout.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(result["status"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    verify(args.build, args.output)
