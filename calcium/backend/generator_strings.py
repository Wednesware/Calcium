def cstr(text: str | bytes) -> str:
    raw = text if isinstance(text, bytes) else text.encode("utf-8", "surrogatepass")
    return '"' + "".join(f"\\{byte:03o}" for byte in raw) + '"'
