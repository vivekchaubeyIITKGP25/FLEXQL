#!/usr/bin/env python3

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
DIAGRAM_DIR = ROOT / "diagrams"
PDF_PATH = ROOT / "FLEXQL_DESIGN_DOC.pdf"

WIDTH = 1600
HEIGHT = 900
BG = (248, 246, 240)
TEXT = (31, 34, 39)
ACCENT = (29, 78, 137)
ACCENT_2 = (48, 125, 96)
ACCENT_3 = (176, 96, 26)
BOX = (255, 255, 255)
BOX_ALT = (239, 247, 255)
LINE = (91, 104, 117)


def load_font(size, bold=False):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf" if bold else
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    ]
    for candidate in candidates:
        path = Path(candidate)
        if path.exists():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


TITLE_FONT = load_font(42, bold=True)
SUBTITLE_FONT = load_font(24, bold=True)
BODY_FONT = load_font(24)
SMALL_FONT = load_font(20)


def text_wrap(draw, text, font, max_width):
    words = text.split()
    lines = []
    current = []
    for word in words:
        trial = " ".join(current + [word])
        if draw.textlength(trial, font=font) <= max_width:
            current.append(word)
        else:
            if current:
                lines.append(" ".join(current))
            current = [word]
    if current:
        lines.append(" ".join(current))
    return lines


def draw_text_block(draw, text, x, y, width, font, fill=TEXT, spacing=10):
    lines = text_wrap(draw, text, font, width)
    for line in lines:
        draw.text((x, y), line, font=font, fill=fill)
        y += font.size + spacing
    return y


def box(draw, xy, label, fill=BOX, outline=ACCENT):
    x1, y1, x2, y2 = xy
    draw.rounded_rectangle(xy, radius=24, fill=fill, outline=outline, width=4)
    lines = text_wrap(draw, label, SUBTITLE_FONT, x2 - x1 - 40)
    total_height = len(lines) * (SUBTITLE_FONT.size + 8)
    y = y1 + ((y2 - y1) - total_height) / 2
    for line in lines:
        w = draw.textlength(line, font=SUBTITLE_FONT)
        draw.text((x1 + (x2 - x1 - w) / 2, y), line, font=SUBTITLE_FONT, fill=TEXT)
        y += SUBTITLE_FONT.size + 8


def arrow(draw, start, end, fill=LINE, width=6):
    draw.line([start, end], fill=fill, width=width)
    ex, ey = end
    sx, sy = start
    if abs(ex - sx) > abs(ey - sy):
        direction = 1 if ex >= sx else -1
        points = [(ex, ey), (ex - 18 * direction, ey - 10), (ex - 18 * direction, ey + 10)]
    else:
        direction = 1 if ey >= sy else -1
        points = [(ex, ey), (ex - 10, ey - 18 * direction), (ex + 10, ey - 18 * direction)]
    draw.polygon(points, fill=fill)


def canvas(title, subtitle):
    img = Image.new("RGB", (WIDTH, HEIGHT), BG)
    draw = ImageDraw.Draw(img)
    draw.text((80, 60), title, font=TITLE_FONT, fill=TEXT)
    draw.text((80, 120), subtitle, font=BODY_FONT, fill=LINE)
    return img, draw


def save_system_architecture():
    img, draw = canvas(
        "FlexQL System Architecture",
        "Client requests move through the network stack into parsing, execution, caching, and storage.",
    )
    box(draw, (90, 260, 330, 400), "Client / REPL", fill=BOX_ALT)
    box(draw, (410, 260, 650, 400), "TCP Server", fill=BOX_ALT)
    box(draw, (730, 180, 970, 320), "Parser")
    box(draw, (730, 420, 970, 560), "Executor")
    box(draw, (1070, 120, 1390, 260), "LRU Cache", fill=(242, 252, 245), outline=ACCENT_2)
    box(draw, (1070, 330, 1390, 470), "Durable Log", fill=(255, 245, 232), outline=ACCENT_3)
    box(draw, (1070, 540, 1390, 680), "Table Storage + PK Index", fill=BOX_ALT)

    arrow(draw, (330, 330), (410, 330))
    arrow(draw, (650, 300), (730, 250))
    arrow(draw, (650, 360), (730, 490))
    arrow(draw, (970, 250), (1070, 190))
    arrow(draw, (970, 490), (1070, 610))
    arrow(draw, (930, 420), (1120, 470))
    arrow(draw, (1230, 260), (1230, 330))
    arrow(draw, (1230, 470), (1230, 540))

    img.save(DIAGRAM_DIR / "system_architecture.png")


def save_request_lifecycle():
    img, draw = canvas(
        "Request Lifecycle",
        "Each SQL request is framed, parsed, executed, and streamed back row by row.",
    )
    steps = [
        ("Client sends QUERY frame", 120, ACCENT),
        ("Server reads length-prefixed payload", 260, ACCENT),
        ("Parser builds the query object", 400, ACCENT_2),
        ("Executor chooses scan or index path", 540, ACCENT_2),
        ("Rows stream through callbacks", 680, ACCENT_3),
    ]
    for label, y, outline in steps:
        box(draw, (250, y, 1350, y + 90), label, fill=BOX, outline=outline)
    for _, y, _ in steps[:-1]:
        arrow(draw, (800, y + 90), (800, y + 140))
    img.save(DIAGRAM_DIR / "request_lifecycle.png")


def save_index_structure():
    img, draw = canvas(
        "Primary-Key Index Structure",
        "FlexQL keeps a resident hash index for hot lookups and spills to disk-backed lookup files when needed.",
    )
    box(draw, (120, 260, 520, 420), "Query WHERE ID = literal", fill=BOX_ALT)
    box(draw, (620, 180, 1020, 340), "Resident Hash Index", fill=(242, 252, 245), outline=ACCENT_2)
    box(draw, (620, 420, 1020, 580), "Disk Hash Overflow Index", fill=(255, 245, 232), outline=ACCENT_3)
    box(draw, (1120, 260, 1480, 420), "Row Offset -> Table File", fill=BOX)
    arrow(draw, (520, 340), (620, 260))
    arrow(draw, (520, 340), (620, 500))
    arrow(draw, (1020, 260), (1120, 340))
    arrow(draw, (1020, 500), (1120, 340))
    img.save(DIAGRAM_DIR / "index_structure.png")


def save_wal_recovery():
    img, draw = canvas(
        "WAL Recovery Flow",
        "Prepared records become durable only after the matching commit marker is flushed.",
    )
    box(draw, (130, 260, 470, 420), "Validate Statement", fill=BOX_ALT)
    box(draw, (570, 260, 910, 420), "Append PREPARE Record", fill=(255, 245, 232), outline=ACCENT_3)
    box(draw, (1010, 260, 1350, 420), "Apply Local Change", fill=BOX)
    box(draw, (350, 560, 790, 720), "Append COMMIT + Flush WAL", fill=(242, 252, 245), outline=ACCENT_2)
    box(draw, (910, 560, 1450, 720), "Replay Only Committed Records on Restart", fill=BOX_ALT)
    arrow(draw, (470, 340), (570, 340))
    arrow(draw, (910, 340), (1010, 340))
    arrow(draw, (1180, 420), (1180, 560))
    arrow(draw, (790, 640), (910, 640))
    img.save(DIAGRAM_DIR / "wal_recovery.png")


def save_concurrency_model():
    img, draw = canvas(
        "Concurrency Model",
        "Reads proceed under shared locks while mutations pass through the serialized executor mutation path.",
    )
    box(draw, (120, 260, 480, 420), "Client Sessions", fill=BOX_ALT)
    box(draw, (560, 180, 980, 320), "Thread Pool", fill=BOX)
    box(draw, (560, 380, 980, 520), "Executor", fill=BOX)
    box(draw, (1080, 110, 1480, 250), "Catalog Shared Mutex", fill=(242, 252, 245), outline=ACCENT_2)
    box(draw, (1080, 300, 1480, 440), "Table Shared Mutex", fill=(242, 252, 245), outline=ACCENT_2)
    box(draw, (1080, 490, 1480, 630), "Mutation Mutex", fill=(255, 245, 232), outline=ACCENT_3)
    arrow(draw, (480, 340), (560, 250))
    arrow(draw, (480, 340), (560, 450))
    arrow(draw, (980, 250), (1080, 180))
    arrow(draw, (980, 450), (1080, 370))
    arrow(draw, (980, 450), (1080, 560))
    img.save(DIAGRAM_DIR / "concurrency_model.png")


def page(title, paragraphs):
    img = Image.new("RGB", (1654, 2339), (255, 255, 255))
    draw = ImageDraw.Draw(img)
    draw.text((110, 90), title, font=load_font(40, bold=True), fill=TEXT)
    y = 180
    for paragraph in paragraphs:
        if isinstance(paragraph, tuple):
            heading, body = paragraph
            draw.text((110, y), heading, font=load_font(28, bold=True), fill=ACCENT)
            y += 50
            y = draw_text_block(draw, body, 110, y, 1430, load_font(22), spacing=8) + 24
        else:
            y = draw_text_block(draw, paragraph, 110, y, 1430, load_font(22), spacing=8) + 24
    return img


def save_pdf():
    pages = [
        page(
            "FlexQL Design Report",
            [
                "Submission for Roll No. 25CS60R64",
                ("Overview", "I implemented FlexQL as a compact SQL-like database server in C++17. The project includes parsing, execution, TCP networking, durable storage, primary-key indexing, LRU caching, tests, and benchmarks."),
                ("Supported SQL", "The current implementation supports CREATE TABLE, INSERT, SELECT, projected queries, a single WHERE condition, INNER JOIN, ORDER BY, DELETE FROM, and row expiry through EXPIRES timestamps."),
                ("Architecture", "A client sends a length-prefixed request to the TCP server. The server parses the SQL, executes it, consults the cache when valid, uses the primary-key index for equality lookup when possible, and streams rows back incrementally."),
            ],
        ),
        page(
            "Storage, Recovery, and Concurrency",
            [
                ("Storage Model", "Rows are stored in disk-backed row files. Table metadata, row counts, and primary-key information are maintained separately. Large tables are not required to remain fully resident in memory."),
                ("Durability", "Mutating statements are written to the durable log before commit. FlexQL replays only committed records on restart and ignores incomplete log tails, which keeps recovery behavior easy to reason about."),
                ("Concurrency", "Client sessions run through a worker pool. Catalog and table data use shared locking for concurrent reads, while mutations pass through a serialized executor path so recovery and correctness stay simple."),
            ],
        ),
        page(
            "Testing and Benchmark Result",
            [
                ("Tests", "The local submission keeps two test binaries, flexql-test and flexql-test-all. Together they cover parser behavior, execution paths, joins, ordering, cache invalidation, TTL handling, durable-log replay, and concurrency smoke checks."),
                ("Submitted Benchmark", "On April 7, 2026 I recorded the final benchmark with ./bin/flexql-benchmark 10000000. The run inserted 10,000,000 rows in 9,209 ms, reached 1,085,894 rows per second, finished the full-table SELECT benchmark in 193,780 ms, finished the primary-key WHERE benchmark in 26,198 ms, and completed the included unit checks with 23 out of 23 tests passing."),
                ("Included Assets", "The submission package includes README.md, DESIGN.md, design_doc.tex, benchmark.out, the generated report PDF, and the diagrams directory so the implementation and results can be reviewed without rebuilding the documentation from scratch."),
            ],
        ),
    ]
    first, *rest = [page.convert("RGB") for page in pages]
    first.save(PDF_PATH, save_all=True, append_images=rest)


def main():
    DIAGRAM_DIR.mkdir(exist_ok=True)
    save_system_architecture()
    save_request_lifecycle()
    save_index_structure()
    save_wal_recovery()
    save_concurrency_model()
    save_pdf()


if __name__ == "__main__":
    main()
