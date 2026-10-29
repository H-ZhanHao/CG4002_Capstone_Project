"""Use Windows' existing short path aliases for ESP-IDF's no-spaces build rule."""

import ctypes
import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
if os.name == "nt" and " " in project_dir:
    buffer = ctypes.create_unicode_buffer(32768)
    get_short_path = ctypes.windll.kernel32.GetShortPathNameW
    get_short_path.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
    get_short_path.restype = ctypes.c_uint32
    length = get_short_path(project_dir, buffer, len(buffer))
    if not length or length >= len(buffer) or " " in buffer.value:
        raise RuntimeError(
            "ESP-IDF needs a path without spaces. Windows short names are unavailable; "
            "move this project to a path such as C:/projects/CG4002 and reopen it."
        )
    short_dir = buffer.value
    replacements = {}
    for key in ("PROJECT_DIR", "PROJECT_SRC_DIR", "PROJECT_INCLUDE_DIR",
                "PROJECT_LIB_DIR", "PROJECT_TEST_DIR", "BUILD_DIR"):
        value = env.subst("$" + key)
        if value == project_dir or value.startswith(project_dir + os.sep):
            replacements[key] = short_dir + value[len(project_dir):]
    env.Replace(**replacements)
