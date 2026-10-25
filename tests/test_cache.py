import subprocess

from calcium import Calcium


def test_cache_reuse_invalidation_and_corruption(tmp_path):
    source = tmp_path / "main.py"
    helper = tmp_path / "helper.py"
    source.write_text("import helper\nprint(helper.value)")
    helper.write_text("value = 1")
    compiler = Calcium(
        backend="cpython", target="executable", fallback=False, cache_dir=tmp_path / "cache"
    )
    output = tmp_path / "program"
    first = compiler.build(source, output=output)
    assert first.success, first.diagnostics
    assert not any(r["hit"] for r in first.metadata["toolchain"]["cache"])
    second = compiler.build(source, output=output)
    assert second.success, second.diagnostics
    assert all(r["hit"] for r in second.metadata["toolchain"]["cache"])
    helper.write_text("value = 2")
    third = compiler.build(source, output=output)
    assert third.success, third.diagnostics
    assert any(not r["hit"] for r in third.metadata["toolchain"]["cache"])
    assert subprocess.check_output([str(output)]) == b"2\n"
    report = next(
        r for r in third.metadata["toolchain"]["cache"] if r["source"].startswith("module_helper")
    )
    (tmp_path / "cache" / report["key"][:2] / report["key"] / "object.o").write_bytes(b"corrupt")
    repaired = compiler.build(source, output=output)
    assert repaired.success, repaired.diagnostics
    assert subprocess.check_output([str(output)]) == b"2\n"
    assert not next(
        r for r in repaired.metadata["toolchain"]["cache"] if r["source"] == report["source"]
    )["hit"]
    reused = compiler.build(source, output=output)
    assert reused.success
    assert all(r["hit"] for r in reused.metadata["toolchain"]["cache"])
    changed_level = Calcium(
        backend="cpython", target="executable", optimization=1, cache_dir=tmp_path / "cache"
    ).build(source, output=output)
    assert changed_level.success
    assert not any(r["hit"] for r in changed_level.metadata["toolchain"]["cache"])


def test_disabled_debug_and_unwritable_cache(tmp_path):
    source = tmp_path / "main.py"
    source.write_text("print(42)")
    occupied = tmp_path / "not-a-directory"
    occupied.write_text("mine")
    for options in ({"cache": False}, {"debug": True}, {"cache_dir": occupied}):
        result = Calcium(backend="cpython", target="executable", **options).build(
            source, output=tmp_path / "program"
        )
        assert result.success, result.diagnostics
        assert subprocess.check_output([str(result.executable)]) == b"42\n"
