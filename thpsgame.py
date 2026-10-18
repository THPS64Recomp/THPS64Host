"""Per-game parameter table for the EoR THPS N64 ports (decomp/common/games.toml).

Every tool in decomp/common/ selects its game with `--game NAME` (or the
THPS_GAME environment variable) and reads constants from here instead of
hard-coding them.

    import thpsgame
    g = thpsgame.load("thps2")       # or thpsgame.from_argv(argv) -> (g, rest)
    g.main_vram, g.table, g.boot_text, g.main_text ...

A row may say `inherits = "thps1"`: it starts from that row (recursively) and lists only
what differs (a regional release, another revision). `Game.family` is the root of the chain;
`thpsgame.versions("thps1")` lists a family's rows in file order.

Derived values (all in `Game`):
  table          chunk table ROM offset (root_ptr + 4)
  boot_size      root_ptr - boot_rom
  main_blob_off  offset of the main image inside the N64Recomp codeblob
  text_hi        end of the last main_text range
"""
import os
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))


class Game:
    def __init__(self, name, d, family=None):
        self.name = name
        self.family = family or name
        self.__dict__.update(d)
        self.table = self.root_ptr + 4
        self.boot_size = self.root_ptr - self.boot_rom
        self.main_blob_off = self.boot_rom + self.boot_size
        self.boot_text = [tuple(r) for r in self.boot_text]
        self.main_text = [tuple(r) for r in self.main_text]
        self.text_hi = self.main_text[-1][1]
        self.main_end = self.main_vram + self.main_size

    def ucode_ranges(self):
        return [(u["name"], u["vram"], u["size"]) for u in self.ucode]


def _table():
    with open(os.path.join(HERE, "games.toml"), "rb") as f:
        return tomllib.load(f)


def _resolve(t, name):
    """Row `name` with its `inherits` chain merged in (child keys win). Returns (dict, family root)."""
    chain, n = [], name
    while True:
        if n not in t:
            raise SystemExit(f"unknown game {n!r}; known: {', '.join(t)}")
        if n in chain:
            raise SystemExit(f"games.toml: inherits cycle at {n!r}")
        chain.append(n)
        parent = t[n].get("inherits")
        if not parent:
            break
        n = parent
    d = {}
    for n in reversed(chain):
        d.update(t[n])
    d.pop("inherits", None)
    return d, chain[-1]


def load(name):
    t = _table()
    d, family = _resolve(t, name)
    return Game(name, d, family)


def versions(family):
    """Every row whose inherits chain ends at `family` (the root included), in file order."""
    t = _table()
    return [Game(n, *_resolve(t, n)) for n in t if _resolve(t, n)[1] == family]


def from_argv(argv):
    """Strip `--game NAME` from argv (default: $THPS_GAME). Returns (Game, rest)."""
    rest, name, i = [], os.environ.get("THPS_GAME"), 0
    while i < len(argv):
        if argv[i] == "--game":
            name, i = argv[i + 1], i + 2
        elif argv[i].startswith("--game="):
            name, i = argv[i][7:], i + 1
        else:
            rest.append(argv[i])
            i += 1
    if not name:
        raise SystemExit("pass --game thps1|thps2|thps3 or set THPS_GAME")
    return load(name), rest
