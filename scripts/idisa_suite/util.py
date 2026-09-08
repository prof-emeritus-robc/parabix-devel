import logging, re
from pathlib import Path

mlog = logging.Logger(__name__)


def find_project_dir(start_path):
    cur = start_path
    explored = set()
    while True:
        # print(f"Checking {cur}..")
        if (cur / "scripts").is_dir() and (cur / "scripts" / "idisa_suite" /
                                           "idisa_exerciser.py").is_file():
            return cur
        explored.add(cur)
        parent = cur.parent
        if parent in explored:
            raise Exception(
                "Unable to find project root (reached system root or a cycle while looking)"
            )
        cur = parent


project_dir: Path
idisa_suite_dir: Path
build_dir: Path
idisa_exerciser_path: Path


def macro_subs(s: str, macros: dict[str, str]):
    parts = []
    cur = len(s)
    for m in reversed(list(re.finditer("%[-_0-9A-Za-z]*%", s))):
        macro = m.group()[1:-1]
        b, e = m.span()
        parts.append(s[e:cur])
        if macro == '':
            parts.append('%')
        else:
            parts.append(macros.get(macro, '<BAD MACRO>'))
        cur = b
    parts.append(s[:cur])
    parts.reverse()
    return ''.join(parts)


def init_paths():
    global project_dir
    global idisa_suite_dir

    project_dir = find_project_dir(Path.cwd())
    idisa_suite_dir = project_dir / "scripts" / "idisa_suite"


# def guess_config():
#     global idisa_exerciser_path
#     for d in ["build", "build-vscode"]:
#         build_dir = project_dir / d
#         if build_dir.is_dir() and (build_dir / "CMakeCache.txt").is_file():
#             break
#     else:
#         build_dir = None
#     if build_dir is not None:
#         idisa_exerciser_path = build_dir / "bin" / "idisa_exerciser"
