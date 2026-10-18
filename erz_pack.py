"""ERZ (= RNC ProPack, magic renamed) packer front-end.

Runs the patched rnc_propack_source binary (see tools/rnc_propack/) in a
temporary directory and swaps the `RNC` magic for `ERZ`. Method 1, dictionary
0x8000 (the tool's default for method 1) reproduces Edge of Reality's bytes.

usage: python -I tools/erz_pack.py RNC_BIN IN.raw OUT.erz [method]
"""
import os
import subprocess
import sys
import tempfile


def pack(rnc_bin, data: bytes, method: int = 1) -> bytes:
    with tempfile.TemporaryDirectory() as td:
        src, dst = os.path.join(td, "in.bin"), os.path.join(td, "out.bin")
        open(src, "wb").write(data)
        # relative names + cwd: the tool treats any argv starting with '/' as a flag
        r = subprocess.run([os.path.abspath(rnc_bin), "p", "in.bin", "out.bin",
                            f"-m={method}"], cwd=td, check=True,
                       capture_output=True, text=True)
        if not os.path.exists(dst):
            raise RuntimeError("packer failed: " + r.stdout[-300:])
        out = open(dst, "rb").read()
    if out[:3] != b"RNC":
        raise RuntimeError("packer did not emit an RNC header")
    return b"ERZ" + out[3:]


if __name__ == "__main__":
    rnc, src, dst = sys.argv[1:4]
    m = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    open(dst, "wb").write(pack(rnc, open(src, "rb").read(), m))
