import pytest


@pytest.fixture(autouse=True)
def isolated_working_directory(tmp_path, monkeypatch):
    """Default build outputs must not collide across tests or Python matrices."""
    monkeypatch.chdir(tmp_path)
