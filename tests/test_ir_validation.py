import copy

import pytest

from calcium.frontend.analysis import analyze
from calcium.frontend.loader import load
from calcium.ir.builder import build
from calcium.ir.validator import validate
from calcium.models import CalciumInternalError


def test_invalid_unpack_access_and_signature_are_rejected(tmp_path):
    source = tmp_path / "source.py"
    source.write_text("a, b = [1, 2]\ndef f(a=1):\n    return a\n")
    program = build(analyze(load(source)).tree)
    invalid = copy.deepcopy(program)
    op = next(
        op
        for block in invalid.functions[0].blocks
        for op in block.instructions
        if op.op == "tuple_item"
    )
    op.data = 2
    with pytest.raises(CalciumInternalError, match="out of bounds"):
        validate(invalid)
    invalid = copy.deepcopy(program)
    invalid.functions[1].posonly = 2
    with pytest.raises(CalciumInternalError, match="signature"):
        validate(invalid)
    invalid = copy.deepcopy(program)
    invalid.functions[0].blocks[0].name = "not a C label"
    with pytest.raises(CalciumInternalError, match="label"):
        validate(invalid)
