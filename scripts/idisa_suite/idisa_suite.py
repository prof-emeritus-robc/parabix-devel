#!/usr/bin/python3

import argparse, logging, math, sys, yaml
from typing import Callable
from dataclasses import dataclass
from functools import reduce, partial
from operator import xor
from pathlib import Path

import idisa_exerciser
import util

mlog = logging.Logger(__name__)


def default_immediate_order(imm_range: int):
    assert (imm_range > 0)
    if imm_range == 1:
        return [0]
    bits = (imm_range - 1).bit_length()
    basis = [(1 << (bits - i)) - 1 for i in range(bits)]
    swizzle_basis: list[int] = [
        basis[bits - (i >> 1) - 1] if i & 1 else basis[i >> 1]
        for i in range(bits)
    ]
    imms = [
        reduce(xor, (swizzle_basis[j] * ((i >> j) & 1) for j in range(bits)))
        for i in range(1 << bits)
    ]
    return [imm for imm in imms if imm < imm_range]


@dataclass(frozen=True)
class OperationConfig:
    operation: str
    field_width: int
    immediate: int | None = None


@dataclass(frozen=True)
class ExerciserProcedure:
    name: str
    exerciser_path: Path | str
    extra_args: list[Path | str]
    block_width: int
    default_test_operand_vectors: list[Path | str]
    default_timing_operand_vectors: list[Path | str]
    specialized_test_operand_vectors: dict[str, list[Path | str]]
    specialized_timing_operand_vectors: dict[str, list[Path | str]]
    lane_width = 32  # should be configured?
    timing_warmup: int = 2
    timing_repeat: int = 500
    timing_drop_worst: int = 0
    timing_drop_best: int = 0
    timing_capture_log_path: Callable[..., Path | str
                                      | None] = lambda proc, cfg: None
    test_capture_ir_path: Callable[...,
                                   Path | str | None] = lambda proc, cfg: None
    test_capture_unopt_ir_path: Callable[..., Path | str
                                         | None] = lambda proc, cfg: None
    test_capture_asm_path: Callable[...,
                                    Path | str | None] = lambda proc, cfg: None
    test_capture_output_path: Callable[..., Path | str
                                       | None] = lambda proc, cfg: None
    test_capture_log_path: Callable[..., Path | str
                                    | None] = lambda proc, cfg: None

    def test_run(self, cfg: OperationConfig, **extra_kwargs) -> Callable:
        meta = idisa_exerciser.all_ops[cfg.operation]
        operands = self.specialized_test_operand_vectors.get(
            cfg.operation, self.default_test_operand_vectors)
        immeds = [str(cfg.immediate)] if cfg.immediate is not None else []

        ir_path = self.prep_file(cfg, self.test_capture_ir_path)
        unopt_ir_path = self.prep_file(cfg, self.test_capture_unopt_ir_path)
        asm_path = self.prep_file(cfg, self.test_capture_asm_path)
        output_path = self.prep_file(cfg, self.test_capture_output_path)
        log_path = self.prep_file(cfg, self.test_capture_log_path)
        return idisa_exerciser.run_idisa_exerciser(
            util.project_dir / self.exerciser_path,
            self.extra_args,
            cfg.operation,
            cfg.field_width,
            operands[:meta.num_operands] + immeds,
            output_path=output_path,
            ir_path=ir_path,
            unopt_ir_path=unopt_ir_path,
            asm_path=asm_path,
            log_path=log_path,
            **extra_kwargs)

    def timing_run(self, cfg: OperationConfig, **extra_kwargs) -> Callable:
        meta = idisa_exerciser.all_ops[cfg.operation]
        operands = self.specialized_timing_operand_vectors.get(
            cfg.operation, self.default_timing_operand_vectors)
        immeds = [str(cfg.immediate)] if cfg.immediate is not None else []
        log_path = self.prep_file(cfg, self.test_capture_log_path)
        return idisa_exerciser.run_idisa_exerciser(
            util.project_dir / self.exerciser_path,
            self.extra_args,
            cfg.operation,
            cfg.field_width,
            operands[:meta.num_operands] + immeds,
            disable_checks=True,
            warmup=self.timing_warmup,
            repeat=self.timing_repeat,
            drop_worst=self.timing_drop_worst,
            drop_best=self.timing_drop_best,
            log_path=log_path,
            **extra_kwargs)

    def prep_file(self, cfg, path_f):
        path = path_f(self, cfg)
        if path is not None:
            path = util.project_dir / path
            path.parent.mkdir(parents=True, exist_ok=True)
        return path


def make_configs(
    proc: ExerciserProcedure,
    operations: str | list[str],
    field_widths: int | list[int],
    immediates: None | int | float | list[int] = None,
):
    # regularize parameters
    if isinstance(operations, str):
        operation = [operations]
    if isinstance(field_widths, int):
        field_widths = [field_widths]

    # generate combinations
    for operation in operations:
        assert operation in idisa_exerciser.all_ops
        meta = idisa_exerciser.all_ops[operation]

        available_fws = []
        fw_min, fw_max = meta.field_width_range(proc.block_width, proc.lane_width)
        assert(fw_min <= fw_max)
        for fw in field_widths:
            assert type(fw) == int
            fw = min(max(fw, fw_min), fw_max)
            if fw not in available_fws:
                available_fws.append(fw)

        for fw in available_fws:
            this_imms = [None]
            if meta.takes_immediate:
                imm_range = meta.immediate_range(proc.block_width, fw)

                # Pick a list of immediates to work from
                if isinstance(immediates, list):
                    this_imms = [imm for imm in immediates if imm < imm_range]
                else:
                    this_imms = default_immediate_order(imm_range)
                    # Restrict the default list
                    if immediates is None:
                        this_imms = this_imms[:3]
                    elif immediates > 0 and immediates < 1:
                        this_imms = this_imms[:math.ceil(
                            len(this_imms) * immediates)]
                    else:
                        this_imms = this_imms[:round(immediates)]

                for imm in this_imms:
                    yield OperationConfig(operation, fw, imm)
            else:
                yield OperationConfig(operation, fw)


def main():
    global mlog

    logging.basicConfig(level=logging.INFO)
    mlog = logging.root

    def macros_from(proc, opcfg):
        macros = {
            'mode': str(proc.name),
            'bw': str(proc.block_width),
            'bw2': '%02d' % (proc.block_width, ),
            'bw3': '%03d' % (proc.block_width, ),
            'bw4': '%04d' % (proc.block_width, ),
            'op': opcfg.operation,
            'fw': 'bb',
            'fw2': 'bb',
            'fw3': 'bbb',
            'imm-': '',
            'imm-i': '',
            'imm': '',
            'imm2': '',
            'imm3': ''
        }
        if opcfg.field_width is not None:
            macros = macros | {
                'fw': str(opcfg.field_width),
                'fw2': '%02d' % (opcfg.field_width, ),
                'fw3': '%03d' % (opcfg.field_width, )
            }
        if opcfg.immediate is not None:
            macros = macros | {
                'imm-': '-',
                'imm-i': '-i',
                'imm': str(opcfg.immediate),
                'imm2': '%02d' % (opcfg.immediate, ),
                'imm3': '%03d' % (opcfg.immediate, )
            }
        return macros

    util.init_paths()
    cfgyaml = yaml.safe_load(open(util.idisa_suite_dir / "config.yaml"))

    # print(repr(cfgyaml))

    test_config = cfgyaml["test"]
    timing_config = cfgyaml["timing"]
    test_vectors = list(
        map(lambda p: util.project_dir / p, test_config["vectors"]))
    timing_vectors = list(
        map(lambda p: util.project_dir / p, timing_config["vectors"]))

    procedures = {}
    for mode in cfgyaml["modes"]:

        def extract_count(cfg, name, default):
            if name not in cfg:
                return default
            return int(cfg[name])

        def extract_path_f(cfg, name):
            if (name not in cfg) or (not cfg[name]):
                return (lambda proc, opcfg: None)
            return (lambda proc, opcfg: util.macro_subs(
                str(cfg[name]), macros_from(proc, opcfg)))

        procedures[mode["name"]] = ExerciserProcedure(
            mode["name"],
            mode["exerciser-path"],
            mode.get("extra-args", []),
            int(mode["block-size"]),
            test_vectors,
            timing_vectors,
            {},
            {},
            timing_warmup=extract_count(timing_config, "warmup", None),
            timing_repeat=extract_count(timing_config, "repeat", 1),
            timing_drop_worst=extract_count(timing_config, "drop-worst", 0),
            timing_drop_best=extract_count(timing_config, "drop-best", 0),
            timing_capture_log_path=extract_path_f(timing_config, "log-path"),
            test_capture_ir_path=extract_path_f(test_config, "ir-path"),
            test_capture_unopt_ir_path=extract_path_f(test_config,
                                                      "unopt-ir-path"),
            test_capture_asm_path=extract_path_f(test_config, "asm-path"),
            test_capture_output_path=extract_path_f(test_config,
                                                    "output-path"),
            test_capture_log_path=extract_path_f(test_config, "log-path"),
        )

    parser = argparse.ArgumentParser(
        sys.argv[0],
        description=
        "Run the idisa_exerciser for different combinations of parameters, "
        "storing outputs to the standard locations",
    )
    parser.add_argument("modes",
                        type=str,
                        nargs="?",
                        default=cfgyaml["default"].get("modes"))
    parser.add_argument("operations",
                        type=str,
                        nargs="?",
                        default=cfgyaml["default"].get("operations"))
    parser.add_argument("field_widths",
                        type=str,
                        nargs="?",
                        default=cfgyaml["default"].get("field-widths"))
    parser.add_argument("immediates",
                        type=str,
                        nargs="?",
                        default=cfgyaml["default"].get("immediates"))
    parser.add_argument("--time", action="store_true", default=False)
    parser.add_argument("--valgrind", action="store_true", default=False)
    args: argparse.Namespace = parser.parse_args()

    if isinstance(args.modes, str):
        do_modes = [o for o in args.modes.split(",") if o]
    else:
        do_modes = args.modes

    if isinstance(args.operations, str):
        do_ops = [o for o in args.operations.split(",") if o]
    else:
        do_ops = args.operations

    if isinstance(args.field_widths, str):
        do_fws = [int(s) for s in args.field_widths.split(",") if s]
    else:
        do_fws = args.field_widths

    if isinstance(args.immediates, str):
        if ',' in args.immediates:
            imms = [int(s.strip()) for s in args.immediates.split(",")]
        elif '.' in args.immediates:
            imms = float(args.immediates)
        else:
            imms = int(args.immediates)
    else:
        imms = args.immediates

    for m in do_modes:
        if m not in procedures:
            raise Exception(
                f"mode {m} not recognized (configured options: {', '.join(procedures.keys())})"
            )

    if args.time:
        for m in do_modes:
            p = procedures[m]
            for c in make_configs(p, do_ops, do_fws, imms):
                p.timing_run(c, with_valgrind=args.valgrind)
    else:
        mlog.info("mode,bw,op,fw,imm,status,log")
        for m in do_modes:
            p = procedures[m]
            for c in make_configs(p, do_ops, do_fws, imms):
                res, logf = p.test_run(c, with_valgrind=args.valgrind)
                mlog.info(f"{p.name},{p.block_width},{c.operation},"
                          f"{c.field_width},"
                          f"{c.immediate if c.immediate is not None else ''},"
                          f"{'pass' if res == 0 else 'FAIL'},{logf}")


if __name__ == "__main__":
    main()
