import logging, os, subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from functools import partial
from typing import Callable

import util

mlog = logging.Logger(__name__)


@dataclass(frozen=True)
class OperationMetadata:
    num_operands: int
    min_field_width: int = 1
    max_field_width: int | Callable | None = None
    bitblock: bool = False
    bit_immed: bool = False
    field_immed: bool = False
    fixed_immed: int | None = None

    @property
    def takes_immediate(self) -> bool:
        assert self.bit_immed + self.field_immed + (self.fixed_immed
                                                    is not None) <= 1
        return self.bit_immed or self.field_immed or (self.fixed_immed
                                                      is not None)

    def field_width_range(self, bitblock_width: int, lane_width: int):
        if self.bitblock:
            return (lane_width, lane_width)
        elif self.max_field_width is None:
            return (self.min_field_width, bitblock_width)
        elif isinstance(self.max_field_width, int):
            return (self.min_field_width, self.max_field_width)
        else:
            return (self.min_field_width,
                    self.max_field_width(bitblock_width, lane_width))

    def immediate_range(self, bitblock_width: int, field_width: int):
        if self.fixed_immed is not None:
            assert not self.bit_immed and not self.field_immed
            return self.fixed_immed
        if self.bitblock:
            assert not self.field_immed
            if self.bit_immed:
                return bitblock_width
            else:
                return 0
        elif self.bit_immed:
            assert not self.field_immed
            assert field_width != 0
            return field_width
        elif self.field_immed:
            assert field_width != 0
            assert bitblock_width >= field_width
            assert (bitblock_width % field_width) == 0
            return bitblock_width // field_width
        else:
            return 0


unary = partial(OperationMetadata, 1)
unary_bitblock = partial(OperationMetadata, 1, bitblock=True)
unary_bitblock_immed = partial(OperationMetadata,
                               1,
                               bitblock=True,
                               bit_immed=True)
unary_bit_immed = partial(OperationMetadata, 1, bit_immed=True)
unary_field_immed = partial(OperationMetadata, 1, field_immed=True)
binary = partial(OperationMetadata, 2)
binary_bitblock = partial(OperationMetadata, 2, bitblock=True)
binary_bitblock_immed = partial(OperationMetadata,
                                2,
                                bitblock=True,
                                bit_immed=True)
binary_field_immed = partial(OperationMetadata, 2, field_immed=True)
ternary = partial(OperationMetadata, 3)
ternary_bitblock = partial(OperationMetadata, 3, bitblock=True)
ternary_bitblock_immed = partial(OperationMetadata,
                                 3,
                                 bitblock=True,
                                 bit_immed=True)

all_ops: dict[str, OperationMetadata] = {
    "simd_select_hi": unary(min_field_width=2),
    "simd_select_lo": unary(min_field_width=2),
    "simd_fill": unary(),
    "simd_any": unary(),
    "simd_popcount": unary(),
    "simd_cttz": unary(),
    "simd_bitreverse": unary(),
    # fw=1,2,4 is well defined (though maybe pointless)
    "hsimd_partial_sum": unary(min_field_width=8),
    "esimd_bitspread": unary(),
    "bitblock_any": unary_bitblock(),
    "bitblock_popcount": unary_bitblock(),
    "simd_not": unary_bitblock(),
    "bitblock_mask_from": unary_bitblock(),
    "bitblock_mask_to": unary_bitblock(),
    "bitblock_set_bit": unary_bitblock(),
    "simd_slli": unary_bit_immed(),
    "simd_srli": unary_bit_immed(),
    "simd_srai": unary_bit_immed(),
    "mvmd_extract": unary_field_immed(),
    "mvmd_slli": unary_field_immed(),
    "mvmd_srli": unary_field_immed(),
    "simd_add": binary(),
    "simd_sub": binary(),
    "simd_mult": binary(),
    "simd_eq": binary(),
    "simd_ne": binary(),
    "simd_gt": binary(),
    "simd_ugt": binary(),
    "simd_ge": binary(),
    "simd_uge": binary(),
    "simd_lt": binary(),
    "simd_le": binary(),
    "simd_ult": binary(),
    "simd_ule": binary(),
    "simd_max": binary(),
    "simd_min": binary(),
    "simd_umax": binary(),
    "simd_umin": binary(),
    "simd_sllv": binary(),
    "simd_srlv": binary(),
    "simd_rotl": binary(),
    "simd_rotr": binary(),
    "simd_pext": binary(),
    "simd_pdep": binary(),
    "esimd_mergeh": binary(max_field_width=lambda bw, lw: bw // 2),
    "esimd_mergel": binary(max_field_width=lambda bw, lw: bw // 2),
    "hsimd_packh": binary(min_field_width=2),
    "hsimd_packl": binary(min_field_width=2),
    "hsimd_packus": binary(min_field_width=2),
    "hsimd_packss": binary(min_field_width=2),
    "mvmd_sll": binary(),
    "mvmd_srl": binary(),
    # fw=1,2,4 are well defined, these ops just not implemented for fw<8
    "mvmd_shuffle": binary(min_field_width=8),
    "mvmd_shuffle:over": binary(min_field_width=8),
    "mvmd_shuffle:highbit": binary(min_field_width=8),
    "mvmd_compress": binary(min_field_width=8),
    "mvmd_expand": binary(min_field_width=8),
    "simd_and": binary_bitblock(),
    "simd_or": binary_bitblock(),
    "simd_xor": binary_bitblock(),
    "simd_binary": binary_bitblock(fixed_immed=(1 << (1 << 2))),
    "mvmd_insert": binary_field_immed(),
    "mvmd_dslli": binary_field_immed(),
    "bitblock_advance.shiftout": binary_bitblock_immed(),
    "bitblock_advance.shifted": binary_bitblock_immed(),
    "simd_if": ternary(),
    "bitblock_add_with_carry.sum": ternary(),
    "bitblock_add_with_carry.carry": ternary(),
    "bitblock_subtract_with_borrow.diff": ternary(),
    "bitblock_subtract_with_borrow.borrow": ternary(),
    "simd_ternary": ternary_bitblock(fixed_immed=(1 << (1 << 3))),
    "bitblock_indexed_advance.shiftout": ternary_bitblock_immed(),
    "bitblock_indexed_advance.shifted": ternary_bitblock_immed(),
}


def run_project_cmake(*args):
    was_cur_dir: Path = Path.cwd()
    os.chdir(util.project_dir)
    try:
        return subprocess.call(["cmake"] + args)
    finally:
        try:
            os.chdir(was_cur_dir)
        except:
            pass


cmake_configure_args = ["-DCMAKE_BUILD_TYPE=Debug"]
cmake_build_args = ["-j", 12]


def build_idisa_exerciser(build_dir: Path | str | None = None):
    if build_dir is None:
        build_dir = util.project_dir / "build"
    else:
        build_dir = Path(build_dir)
    if not build_dir.is_dir():
        mlog.info("Build dir not found, reconfiguring project")
        result = run_project_cmake(
            "-B", str(build_dir.relative_to(util.project_dir)), "-S", ".",
            "-G", "Ninja")
        if result != 0:
            raise Exception("cmake failed during configure")
    result = run_project_cmake("--build",
                               str(build_dir.relative_to(util.project_dir)),
                               *cmake_build_args)
    if result != 0:
        raise Exception("cmake failed during build")
    if not util.idisa_exerciser_path.is_file():
        raise Exception("cmake finished but idisa_exerciser not found?")


def run_idisa_exerciser(exerciser_path,
                        extra_args,
                        operation,
                        field_width,
                        op_args,
                        disable_checks=False,
                        warmup=None,
                        repeat=None,
                        drop_worst=None,
                        drop_best=None,
                        output_path=None,
                        ir_path=None,
                        unopt_ir_path=None,
                        asm_path=None,
                        log_path=None,
                        with_valgrind=False):
    cmd = [str(exerciser_path)]
    if with_valgrind:
        cmd = ["valgrind"] + cmd
    if disable_checks:
        cmd.append("--disable-checks")
    if warmup is not None:
        cmd += ["--warmup", str(warmup)]
    if drop_best is not None:
        cmd += ["--drop-best", str(drop_best)]
    if drop_worst is not None:
        cmd += ["--drop-worst", str(drop_worst)]
    if repeat is not None:
        cmd += ["--repeat", str(repeat)]
    if output_path is not None:
        cmd.append(f"--output={str(output_path)}")
    if ir_path is not None:
        cmd.append(f"--ShowIR={str(ir_path)}")
        cmd.append(f"--ToShow=test_{operation}_{field_width}")
    if unopt_ir_path is not None:
        cmd.append(f"--ShowUnoptimizedIR={str(unopt_ir_path)}")
        if ir_path is None:
            cmd.append(f"--ToShow=test_{operation}_{field_width}")
    if asm_path is not None:
        cmd.append(f"--ShowASM={str(asm_path)}")
    cmd += [str(arg) for arg in extra_args]
    cmd += [str(operation), str(field_width)]
    cmd += [str(arg) for arg in op_args]
    mlog.debug("Running %s", " ".join(cmd))
    log_file = None

    shquote = lambda s: s if ' ' not in s else f"'{s.replace('\'', '\'\\\'')}"

    log_header = f"{'-' * 80}\n{datetime.now().isoformat(sep=' ', timespec='seconds')}: executing {' '.join(map(shquote, cmd))}\n"
    if log_path is not None:
        log_file = os.open(log_path, os.O_WRONLY | os.O_APPEND | os.O_CREAT)
        os.write(log_file, log_header.encode())
    result = subprocess.call(cmd, stdout=log_file, stderr=log_file)
    if log_file is not None:
        os.close(log_file)
    return result, log_path
