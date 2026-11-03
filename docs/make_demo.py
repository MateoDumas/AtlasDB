"""Records a real AtlasDB CLI session and renders it as an animated GIF.

Every statement is executed by the real atlasdb binary; the GIF only adds the
typing animation and colors. Requires Pillow and a monospace font (Consolas on
Windows, DejaVu Sans Mono elsewhere).

Usage: python docs/make_demo.py build/atlasdb docs/demo.gif
"""
import os, re, shutil, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

EXE, OUT = os.path.abspath(sys.argv[1]), sys.argv[2]

# Each step: list of display lines (joined with spaces to execute).
# A list of steps grouped in a tuple runs inside one process (transactions).
SESSION = [
    ["CREATE TABLE authors (id INT PRIMARY KEY,",
     "  name TEXT NOT NULL UNIQUE, country TEXT);"],
    ["CREATE TABLE books (id INT PRIMARY KEY, title TEXT NOT NULL,",
     "  author_id INT REFERENCES authors(id), year INT CHECK (year > 0));"],
    ["CREATE INDEX books_year ON books (year);"],
    ["INSERT INTO authors VALUES (1, 'Borges', 'AR'),",
     "  (2, 'Cortázar', 'AR'), (3, 'Ocampo', 'AR');"],
    ["INSERT INTO books VALUES (1, 'Ficciones', 1, 1944), (2, 'El Aleph', 1, 1949),",
     "  (3, 'Bestiario', 2, 1951), (4, 'Rayuela', 2, 1963);"],
    ["SELECT a.name, COUNT(b.id) AS books, MIN(b.year) AS first",
     "  FROM authors a LEFT JOIN books b ON b.author_id = a.id",
     "  GROUP BY a.name ORDER BY books DESC, a.name;"],
    ["EXPLAIN SELECT title FROM books WHERE year BETWEEN 1940 AND 1950;"],
    ["INSERT INTO books VALUES (5, 'Ghost', 99, 2000);"],
    (["BEGIN;"],
     ["DELETE FROM books WHERE year < 1950;"],
     ["ROLLBACK;"],
     ["SELECT COUNT(*) AS books FROM books;"]),
]

work = tempfile.mkdtemp()
os.chdir(work)


def run(statements):
    path = os.path.join(work, "step.sql")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(statements) + "\n")
    r = subprocess.run([EXE, "library", "-f", path], capture_output=True)
    return (r.stdout + r.stderr).decode("utf-8").replace("\r\n", "\n")


# (prompt shows an open transaction, display lines, output text)
steps = []
for step in SESSION:
    if isinstance(step, tuple):
        # Outputs of a group are recovered by diffing the output of each prefix.
        prev, sqls = "", []
        for i, sub in enumerate(step):
            sqls.append(" ".join(sub))
            out = run(sqls)
            assert out.startswith(prev)
            steps.append((False, sub, out[len(prev):]))
            prev = out
    else:
        steps.append((False, step, run([" ".join(step)])))
# Inside BEGIN..ROLLBACK the prompt shows the open transaction.
txn = False
fixed = []
for _, lines, out in steps:
    fixed.append((txn, lines, out))
    stmt = " ".join(lines).strip().upper()
    if stmt.startswith("BEGIN"): txn = True
    if stmt.startswith(("ROLLBACK", "COMMIT")): txn = False
steps = fixed

shutil.rmtree(work, ignore_errors=True)

# ---------------------------------------------------------------- rendering
COLS, ROWS = 110, 27


def load_font(names):
    for name in names:
        try:
            return ImageFont.truetype(name, 15)
        except OSError:
            pass
    raise SystemExit(f"none of these fonts was found: {names}")


FONT = load_font(["C:/Windows/Fonts/consola.ttf", "DejaVuSansMono.ttf"])
FONT_B = load_font(["C:/Windows/Fonts/consolab.ttf", "DejaVuSansMono-Bold.ttf"])
CW = FONT.getlength("M")
LH = 20
PAD, BAR = 18, 34
W = int(PAD * 2 + CW * COLS)
H = BAR + PAD + LH * ROWS + PAD - 6

BG, CHROME, FG = (13, 17, 23), (22, 27, 34), (201, 209, 217)
GREEN, BLUE, ORANGE, PURPLE = (63, 185, 80), (121, 192, 255), (255, 166, 87), (210, 168, 255)
GRAY, DIM, RED, YELLOW = (110, 118, 129), (139, 148, 158), (248, 81, 73), (210, 153, 34)

KEYWORDS = set("""CREATE TABLE INT TEXT PRIMARY KEY NOT NULL UNIQUE REFERENCES CHECK INDEX ON
INSERT INTO VALUES SELECT AS COUNT MIN FROM LEFT JOIN GROUP BY ORDER DESC EXPLAIN WHERE
BETWEEN AND BEGIN DELETE ROLLBACK COMMIT""".split())
TOKEN = re.compile(r"'[^']*'?|\d+|[A-Za-z_]+|.", re.S)


def highlight(sql):
    spans = []
    for m in TOKEN.finditer(sql):
        t = m.group()
        if t.startswith("'"): c = ORANGE
        elif t.isdigit(): c = PURPLE
        elif t.upper() in KEYWORDS: c = BLUE
        else: c = FG
        spans.append((t, c, False))
    return spans


def color_output(line):
    if line.startswith("ERROR"):
        return [(line, RED, False)]
    if line.startswith("+") or line.startswith("("):
        return [(line, GRAY, False)]
    if line.startswith("|"):
        return [(p, GRAY if p == "|" else FG, False) for p in re.split(r"(\|)", line) if p]
    return [(line, DIM, False)]


def wrap(spans):
    """Wraps a list of colored spans at COLS characters."""
    out, cur, n = [], [], 0
    for text, c, b in spans:
        while text:
            take = text[: COLS - n]
            cur.append((take, c, b)); n += len(take); text = text[len(take):]
            if n >= COLS:
                out.append(cur); cur, n = [], 0
    out.append(cur)
    return out


def draw(screen, cursor):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, BAR], fill=CHROME)
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse([14 + i * 20, 11, 26 + i * 20, 23], fill=c)
    title = "atlasdb — library"
    d.text(((W - FONT.getlength(title)) / 2, 9), title, font=FONT, fill=DIM)
    lines = screen[-ROWS:]
    y = BAR + PAD
    for r, line in enumerate(lines):
        x = PAD
        for text, c, b in line:
            d.text((x, y), text, font=FONT_B if b else FONT, fill=c)
            x += CW * len(text)
        if cursor and r == len(lines) - 1:
            d.rectangle([x + 1, y + 1, x + CW - 1, y + LH - 3], fill=FG)
        y += LH
    return img


frames, durations, screen = [], [], [[]]


def snap(ms, cursor=True):
    frames.append(draw(screen, cursor)); durations.append(ms)


def type_text(spans, cps=3, ms=40):
    """Types the spans a few characters per frame, regardless of token boundaries."""
    chars = [(ch, c, b) for text, c, b in spans for ch in text]
    for i in range(0, len(chars), cps):
        screen[-1].extend(chars[i:i + cps])
        snap(ms)


def emit(spans_lines):
    for sl in spans_lines:
        for w in wrap(sl):
            screen.append(w)


snap(600)
type_text([("$ ", GREEN, True)], ms=1)
type_text([("atlasdb library", FG, False)], ms=55)
snap(500)
screen.append([("AtlasDB 1.0.0 - connected to local database 'library'", DIM, False)])
screen.append([("Type .help for help, .quit to exit.", DIM, False)])
screen.append([])
snap(900)

for txn, lines, out in steps:
    for i, line in enumerate(lines):
        if i == 0:
            screen[-1] = [("atlas*> " if txn else "atlas> ", YELLOW if txn else GREEN, True)]
        else:
            screen.append([("   ...> ", GRAY, True)])
        snap(80)
        type_text(highlight(line))
        if i < len(lines) - 1:
            snap(120)
    snap(200)
    body = out.rstrip("\n").split("\n")
    emit([color_output(l) for l in body])
    screen.append([])
    big = len(body) > 2
    snap(1800 if big else 450)

screen[-1] = [("atlas> ", GREEN, True)]
snap(3000)

frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=durations,
               loop=0, optimize=True, disposal=1)
print(f"{len(frames)} frames, {sum(durations) / 1000:.1f}s, {os.path.getsize(OUT) / 1e6:.2f} MB, {W}x{H}")
