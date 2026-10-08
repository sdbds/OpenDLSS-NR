"""Tensor-shape and native-arithmetic variants for the Direct3D backend."""
import sys
from pathlib import Path

import gemm2_e4m3
import mlp_e4m3
import global_attention_e4m3
import global_attention_stream_e4m3


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    kernels = [
        # Its register-only cooperative-matrix cast is unsaturated and carries
        # NaN; only the final output publication canonicalizes it to zero.
        mlp_e4m3.generate(64, HIDDEN=256, NOUT=64, broadcast=False, raw_hidden=True),
        # The native split-512 FFN uses F2FP.SATFINITE at this register boundary.
        mlp_e4m3.generate(64, HIDDEN=256, NOUT=64, broadcast=False, native_hidden=True),
        gemm2_e4m3.generate(64, gemm2_e4m3.F_F16, valid_n=32),
    ]
    for name, ptx in kernels:
        (output / (name + ".ptx")).write_text(ptx, encoding="ascii", newline="\n")
        print(name)
    native = [global_attention_stream_e4m3.generate_normalize(fused_norm=True)]
    native += [global_attention_e4m3.generate(padded, fused_norm=True) for padded in (64, 128, 192, 256)]
    for name, ptx, _ in native:
        (output / (name + "_native.ptx")).write_text(ptx, encoding="ascii", newline="\n")
        print(name + "_native")


if __name__ == "__main__":
    main()
