#!/usr/bin/env python3
# mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
"""telegraph_assets.py - builds the client models / textures and the SQL of the ground telegraph marks.

Every mark is a WoW 3.3.5a model (MD20 v264 .m2 + 00.skin) made from scratch here: three quads (area, rim, fill), each
drawn by the client as a PROJECTED texture (skin batch flag 0x4), so the mark lies on terrain and building floors like
the spell targeting circle. A projected batch paints the whole box of each triangle, so the shape lives in the texture
alpha (self-made, white, tinted by the model's colour) and every texture keeps a clear margin (the texture clamp repeats
its border texels outside the quad). Scale 1 = 1 yd (nova radius, donut outer radius, cone length, line WIDTH); the
server scales each mark to the exact size. The fill grows from the origin to the border in the variant's duration (bone
scale); a donut's fill fades in instead (a growing ring would paint its safe middle).

Variants = shapes x 2 colours (hostile orange-red, friendly green) x fill durations; edit the grid below.

Usage:
    pip install pillow
    python3 telegraph_assets.py --base <client GameObjectDisplayInfo.dbc> --out out/
Writes:
    out/Spells/Telegraph/*                     models + textures  -> into your client patch MPQ under Spells\\Telegraph\\
    out/DBFilesClient/GameObjectDisplayInfo.dbc  base + our rows  -> into the same MPQ under DBFilesClient\\
    out/ground_telegraph_variants.sql          gameobject_template, ground_telegraph_variant, gameobjectdisplayinfo_dbc
                                               -> data/sql/db-world/ of the module (the server side needs no DBC file)
The base DBC must be the GameObjectDisplayInfo.dbc your clients use now (stock, or the one from a patch you already
ship); the client loads one whole file, so the newest patch with this file must carry everyone's rows.
Deterministic: the same input gives the same bytes; running on its own output changes nothing.
"""
import argparse
import hashlib
import math
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blp import dxt5_blp          # noqa: E402
from wdbc import DBC, DBCWriter   # noqa: E402

FOLDER = "Spells\\Telegraph"      # client path of the art

# ---------------------------------------------------------------- the variant grid (edit here, then re-run)
DURATIONS = [2000, 2500, 3000, 3500, 4000, 5000]  # fill time in ms; the server picks the nearest
SHAPES = (
    [("nova", 0)]
    + [("donut", r) for r in (0.25, 0.33, 0.4, 0.5, 0.53, 0.57, 0.6, 0.67, 0.75)]   # inner / outer radius
    + [("cone", a) for a in (15, 30, 45, 60, 90, 120, 180)]                          # full angle, degrees
    + [("line", r) for r in (5, 10, 20, 40)]                                         # length / width (2 x 40 = 20)
)
COLOURS = {"h": (1.0, 0.42, 0.12),   # hostile: orange-red
           "f": (0.25, 1.0, 0.3)}    # friendly: WoW green
SHAPE_ID = {"line": 1, "cone": 2, "nova": 3, "donut": 4}
EDGE = {"nova": 0.035, "donut": 0.035, "cone": 0.03, "line": 0.12}   # rim width, model units
PAD = 0.06   # clear margin around the shape in the texture / quad, share of the box span per axis
SS = 4       # supersamples per texel side

# ---------------------------------------------------------------- shapes


def shape_box(shape, ratio):
    """-> fill start scale (sx, sy) and the shape's box (x0, y0, x1, y1) in model units (+X = facing, origin = the GO)."""
    if shape == "nova":
        return (0.001, 0.001), (-1, -1, 1, 1)
    if shape == "donut":
        return (1.0, 1.0), (-1, -1, 1, 1)        # the donut's fill fades in, it does not grow
    if shape == "cone":
        h = math.radians(ratio) / 2.0
        xs = [0.0] + [math.cos(-h + 2 * h * i / 32) for i in range(33)]
        ys = [0.0] + [math.sin(-h + 2 * h * i / 32) for i in range(33)]
        return (0.001, 0.001), (min(xs), min(ys), max(xs), max(ys))
    L = float(ratio)
    return (0.001 / L, 1.0), (0.0, -0.5, L, 0.5)


def padded(box):
    x0, y0, x1, y1 = box
    px, py = (x1 - x0) * PAD, (y1 - y0) * PAD
    return (x0 - px, y0 - py, x1 + px, y1 + py)


def tex_size(shape):
    return (512, 64) if shape == "line" else (256, 256)


def shape_inside(shape, ratio, x, y):
    """-> (inside, distance to the border in model units) for a point in model space."""
    if shape == "nova":
        r = math.hypot(x, y)
        return r <= 1.0, 1.0 - r
    if shape == "donut":
        r = math.hypot(x, y)
        return ratio <= r <= 1.0, min(1.0 - r, r - ratio)
    if shape == "cone":
        r, a, h = math.hypot(x, y), math.atan2(y, x), math.radians(ratio) / 2.0
        side = r * math.sin(h - abs(a)) if abs(a) <= math.pi / 2 else -1.0
        return r <= 1.0 and abs(a) <= h, min(1.0 - r, side)
    L = float(ratio)
    return 0.0 <= x <= L and abs(y) <= 0.5, min(x, L - x, 0.5 - abs(y))


def shape_textures(shape, ratio, box, size):
    """area (faint), rim (bright border band) and fill (solid) textures of one shape: white + alpha, u = x, v = y."""
    x0, y0, x1, y1 = box
    e = EDGE[shape]
    sw, sh = size
    area, rim, fill = (Image.new("RGBA", (sw, sh)) for _ in range(3))
    for j in range(sh):
        for i in range(sw):
            if i < 2 or j < 2 or i >= sw - 2 or j >= sh - 2:
                continue   # the clamp repeats the border texels outside the quad: keep them clear
            aa = ar = af = 0.0
            for sj in range(SS):
                for si in range(SS):
                    x = x0 + (i + (si + 0.5) / SS) / sw * (x1 - x0)
                    y = y0 + (j + (sj + 0.5) / SS) / sh * (y1 - y0)
                    ins, d = shape_inside(shape, ratio, x, y)
                    if not ins:
                        continue
                    u = max(0.0, 1.0 - d / e)          # 1 on the border, 0 at the band's inner side
                    aa += 0.2
                    ar += min(1.0, 0.15 * u ** 2.2 + 0.9 * math.exp(-((u - 0.86) / 0.08) ** 2)) if d < e else 0.0
                    af += 0.38
            n = SS * SS
            area.putpixel((i, j), (255, 255, 255, int(255 * aa / n + 0.5)))
            rim.putpixel((i, j), (255, 255, 255, int(255 * ar / n + 0.5)))
            fill.putpixel((i, j), (255, 255, 255, int(255 * af / n + 0.5)))
    return area, rim, fill


def quads(box):
    """area / rim / fill = one quad each over the box, uv = position in the box; the fill is on bone 1 (it grows)."""
    x0, y0, x1, y1 = box
    out = {}
    for key, bone in (("area", 0), ("edge", 0), ("fill", 1)):
        v = [(x0, y0, bone, 0.0, 0.0), (x1, y0, bone, 1.0, 0.0), (x1, y1, bone, 1.0, 1.0), (x0, y1, bone, 0.0, 1.0)]
        out[key] = (v, [(0, 1, 2), (0, 2, 3)])
    return out

# ---------------------------------------------------------------- M2 writer (MD20 v264)
HEADER = 0x130
PROJECTED = 0x4   # skin batch flag: projected texture


class Blob:
    def __init__(self):
        self.b = bytearray(HEADER)

    def put(self, data, align=16):
        while len(self.b) % align:
            self.b.append(0)
        off = len(self.b)
        self.b += data
        return off

    def arr(self, data, count):
        return (count, self.put(data) if count else 0)


def track(blob, interp, times, values, vfmt):
    """M2Track (20 bytes) with one sub-array per sequence (one sequence here). Empty times = static, no keys."""
    if not times:
        return struct.pack("<hhIIII", 0, -1, 0, 0, 0, 0)
    t_off = blob.put(struct.pack("<%dI" % len(times), *times))
    v_off = blob.put(b"".join(struct.pack(vfmt, *v) if isinstance(v, tuple) else struct.pack(vfmt, v) for v in values))
    ts = blob.put(struct.pack("<II", len(times), t_off))
    vs = blob.put(struct.pack("<II", len(values), v_off))
    return struct.pack("<hhIIII", interp, -1, 1, ts, 1, vs)


def model(name, layers, start, box, colour, duration, texnames, fade=False):
    """One mark model + its skin. fade: the fill's transparency goes 0 -> 1 over `duration` instead of growing."""
    blob = Blob()
    hold = 2000
    total = duration + hold
    x0, y0, x1, y1 = box
    z0, z1 = -0.1, 0.1
    radius = math.sqrt((x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2) / 2.0
    bounds = struct.pack("<6ff", x0, y0, z0, x1, y1, z1, radius)

    order = ["area", "edge", "fill"]
    verts, sections, tris = [], [], []
    for key in order:
        mv, mt = layers[key]
        vstart, istart = len(verts), len(tris) * 3
        verts += mv
        tris += [(vstart + a, vstart + b, vstart + c) for (a, b, c) in mt]
        sections.append((vstart, len(mv), istart, len(mt) * 3))
    vdata = b"".join(struct.pack("<3f4B4B3f2f2f", x, y, 0.0, 255, 0, 0, 0, bone, 0, 0, 0, 0.0, 0.0, 1.0, u, v, 0.0, 0.0)
                     for (x, y, bone, u, v) in verts)

    name_b = name.encode() + b"\0"
    h = {}
    h["name"] = blob.arr(name_b, len(name_b))
    # one sequence: Stand (id 0), stored in the model (flag 0x20), loops; the fill holds after `duration`
    seq = struct.pack("<HHIfIhHIII", 0, 0, total, 0.0, 0x20, 0x7FFF, 0, 0, 0, 150) + bounds + struct.pack("<hH", -1, 0)
    h["sequences"] = blob.arr(seq, 1)
    # hash table (slot = id % 3, 0xFFFF empty): it must never be full or the client hangs
    h["sequence_lookup"] = blob.arr(struct.pack("<3H", 0, 0xFFFF, 0xFFFF), 3)
    # bones: 0 static root, 1 the fill (scale track; flag 0x200 = animated)
    empty = track(blob, 0, [], [], "")
    sx, sy = start
    scale = track(blob, 1, [0, duration, total], [(sx, sy, 1.0), (1.0, 1.0, 1.0), (1.0, 1.0, 1.0)], "<3f")
    bones = b""
    for flags, sc in ((0, empty), (0x200, scale)):
        bones += struct.pack("<iIhHHH", -1, flags, -1, 0, 0, 0) + empty + empty + sc + struct.pack("<3f", 0, 0, 0)
    h["bones"] = blob.arr(bones, 2)
    h["key_bone_lookup"] = blob.arr(struct.pack("<h", -1), 1)
    h["vertices"] = blob.arr(vdata, len(verts))
    colour_tr = track(blob, 0, [0], [tuple(colour)], "<3f")
    alpha_tr = track(blob, 0, [0], [32767], "<h")
    h["colors"] = blob.arr(colour_tr + alpha_tr, 1)
    texrecs = b""
    for key in order:
        fn = ("%s\\%s" % (FOLDER, texnames[key])).encode() + b"\0"
        texrecs += struct.pack("<IIII", 0, 0, len(fn), blob.put(fn))
    h["textures"] = blob.arr(texrecs, 3)
    weights = track(blob, 0, [0], [32767], "<h")
    if fade:
        weights += track(blob, 1, [0, duration, total], [0, 32767, 32767], "<h")
    h["texture_weights"] = blob.arr(weights, 2 if fade else 1)
    h["replaceable_lookup"] = blob.arr(struct.pack("<h", -1), 1)
    # material: unlit 0x1, unfogged 0x2, two-sided 0x4, no depth write 0x10; blend 2 = alpha
    h["materials"] = blob.arr(struct.pack("<HH", 0x17, 2), 1)
    h["bone_lookup"] = blob.arr(struct.pack("<2H", 0, 1), 2)
    h["texture_lookup"] = blob.arr(struct.pack("<3H", 0, 1, 2), 3)
    h["texunit_lookup"] = blob.arr(struct.pack("<H", 0), 1)
    h["weight_lookup"] = blob.arr(struct.pack("<2H", 0, 1), 2) if fade else blob.arr(struct.pack("<H", 0), 1)
    h["transform_lookup"] = blob.arr(struct.pack("<h", -1), 1)

    def a(key):
        return struct.pack("<II", *h.get(key, (0, 0)))
    head = b"MD20" + struct.pack("<I", 264) + a("name") + struct.pack("<I", 0) + a("global_loops") + a("sequences") \
        + a("sequence_lookup") + a("bones") + a("key_bone_lookup") + a("vertices") + struct.pack("<I", 1) \
        + a("colors") + a("textures") + a("texture_weights") + a("texture_transforms") + a("replaceable_lookup") \
        + a("materials") + a("bone_lookup") + a("texture_lookup") + a("texunit_lookup") + a("weight_lookup") \
        + a("transform_lookup") + bounds + struct.pack("<6ff", 0, 0, 0, 0, 0, 0, 0) \
        + b"".join(a(k) for k in ("collision_indices", "collision_positions", "collision_normals", "attachments",
                                   "attachment_lookup", "events", "lights", "cameras", "camera_lookup", "ribbons",
                                   "particles"))
    assert len(head) == HEADER, len(head)
    blob.b[:HEADER] = head

    s = bytearray(48)

    def sput(data):
        while len(s) % 4:
            s.append(0)
        off = len(s)
        s.extend(data)
        return off
    n = len(verts)
    idx = struct.pack("<%dH" % n, *range(n))
    tri = struct.pack("<%dH" % (len(tris) * 3), *[i for t in tris for i in t])
    props = b"".join(struct.pack("<4B", bone, 0, 0, 0) for (_, _, bone, _, _) in verts)
    subs, batches = b"", b""
    for k, (vs, vc, is_, ic) in enumerate(sections):
        subs += struct.pack("<10H", 0, 0, vs, vc, is_, ic, 2, 0, 1, 0) + struct.pack("<3f3ff", 0, 0, 0, 0, 0, 0, radius)
        # flags (projected), priority plane (area 0, rim 2 above the fill 1), shader 0, section, geoset, colour 0,
        # material 0, layer 0, 1 texture, texture combo k, uv combo 0, weight combo (fill fade: 1), transform combo 0
        plane = {0: 0, 1: 2, 2: 1}[k]
        weight = 1 if (fade and k == 2) else 0
        batches += struct.pack("<BbHHHHHHHHHHH", PROJECTED, plane, 0, k, k, 0, 0, 0, 1, k, 0, weight, 0)
    o_idx, o_tri, o_props = sput(idx), sput(tri), sput(props)
    o_subs, o_batch = sput(subs), sput(batches)
    s[:48] = b"SKIN" + struct.pack("<10I", n, o_idx, len(tris) * 3, o_tri, n, o_props, len(sections), o_subs,
                                   len(sections), o_batch) + struct.pack("<I", 2)
    return bytes(blob.b), bytes(s)

# ---------------------------------------------------------------- variants, DBC, SQL


def tag_of(shape, ratio):
    return {"nova": "nova", "donut": "donut%d" % round(ratio * 100), "cone": "cone%d" % ratio,
            "line": "line%d" % ratio}[shape]


def variants(display_base, go_base):
    out, disp, go = [], display_base, go_base
    for shape, ratio in SHAPES:
        for col in ("h", "f"):
            for dur in DURATIONS:
                out.append(dict(shape=shape, ratio=ratio, col=col, dur=dur, display=disp, go=go,
                                name="tg_%s_%s_%d" % (tag_of(shape, ratio), col, dur // 100)))
                disp += 1
                go += 1
    return out


def sql(vs, boxes, display_base, display_last, go_base, go_last):
    q = lambda s: "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"
    lines = ["-- mod-ground-telegraphs: mark variants. GENERATED by tools/telegraph_assets.py; do not edit by hand.",
             "-- gameobject_template %d-%d, GameObjectDisplayInfo %d-%d (client: the same rows in the patch's DBC)."
             % (go_base, go_last, display_base, display_last),
             "CREATE TABLE IF NOT EXISTS `ground_telegraph_variant` (`entry` INT UNSIGNED NOT NULL, `shape` TINYINT UNSIGNED NOT NULL,"
             " `ratio` FLOAT NOT NULL, `friendly` TINYINT UNSIGNED NOT NULL, `duration_ms` INT UNSIGNED NOT NULL,"
             " PRIMARY KEY (`entry`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
             "DELETE FROM `ground_telegraph_variant`;",
             "DELETE FROM `gameobject_template` WHERE `entry` BETWEEN %d AND %d;" % (go_base, go_last),
             "DELETE FROM `gameobject_template_addon` WHERE `entry` BETWEEN %d AND %d;" % (go_base, go_last),
             "DELETE FROM `gameobjectdisplayinfo_dbc` WHERE `ID` BETWEEN %d AND %d;" % (display_base, display_last)]
    rows, addon, vrows, drows = [], [], [], []
    for v in vs:
        rows.append("(%d,5,%d,'Telegraph %s','','','',1,0,0,'','',0)" % (v["go"], v["display"], v["name"]))
        addon.append("(%d,0,16,0,0,0,0,0,0)" % v["go"])     # flags 16 = GO_FLAG_NOT_SELECTABLE
        vrows.append("(%d,%d,%g,%d,%d)" % (v["go"], SHAPE_ID[v["shape"]], v["ratio"], v["col"] == "f", v["dur"]))
        x0, y0, x1, y1 = boxes[v["name"]]
        drows.append("(%d,%s,%r,%r,-0.1,%r,%r,0.1)" % (v["display"], q("%s\\%s.mdx" % (FOLDER, v["name"])), x0, y0, x1, y1))
    lines.append("INSERT INTO `gameobject_template` (`entry`,`type`,`displayId`,`name`,`IconName`,`castBarCaption`,`unk1`,"
                 "`size`,`Data1`,`Data6`,`AIName`,`ScriptName`,`VerifiedBuild`) VALUES\n" + ",\n".join(rows) + ";")
    lines.append("INSERT INTO `gameobject_template_addon` (`entry`,`faction`,`flags`,`mingold`,`maxgold`,`artkit0`,`artkit1`,"
                 "`artkit2`,`artkit3`) VALUES\n" + ",\n".join(addon) + ";")
    lines.append("INSERT INTO `ground_telegraph_variant` (`entry`,`shape`,`ratio`,`friendly`,`duration_ms`) VALUES\n" + ",\n".join(vrows) + ";")
    lines.append("INSERT INTO `gameobjectdisplayinfo_dbc` (`ID`,`ModelName`,`GeoBoxMinX`,`GeoBoxMinY`,`GeoBoxMinZ`,`GeoBoxMaxX`,"
                 "`GeoBoxMaxY`,`GeoBoxMaxZ`) VALUES\n" + ",\n".join(drows) + ";")
    return "\n".join(lines) + "\n"


def dbc_rows(base_path, vs, boxes, display_base, display_last):
    d = DBC(base_path)
    assert d.nfield == 19, "%s: %d fields, not a 3.3.5a GameObjectDisplayInfo.dbc" % (base_path, d.nfield)
    w = DBCWriter(d)
    for i in range(display_base, display_last + 1):      # the block is ours: drop what an earlier run left
        if i in d.by_id:
            d.raw_records.remove(d.by_id.pop(i))
    for v in vs:
        w.add_record(v["display"])
        d.by_id[v["display"]][1] = w.add_string("%s\\%s.mdx" % (FOLDER, v["name"]))
        x0, y0, x1, y1 = boxes[v["name"]]
        for k, val in enumerate((x0, y0, -0.1, x1, y1, 0.1)):
            d.by_id[v["display"]][12 + k] = struct.unpack("<I", struct.pack("<f", val))[0]
    return w


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="out", help="output folder (default ./out)")
    ap.add_argument("--base", help="the client's current GameObjectDisplayInfo.dbc (without it no DBC is written)")
    ap.add_argument("--display-base", type=int, default=9700, help="first GameObjectDisplayInfo id (default 9700; stock max 9624)")
    ap.add_argument("--display-last", type=int, default=9999, help="last id of the display block (rows in it are replaced)")
    ap.add_argument("--go-base", type=int, default=9530000, help="first gameobject_template entry (default 9530000)")
    ap.add_argument("--go-last", type=int, default=9530899, help="last entry of the gameobject block (default 9530899)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    vs = variants(args.display_base, args.go_base)
    assert vs[-1]["display"] <= args.display_last and vs[-1]["go"] <= args.go_last, "the grid is larger than the ID blocks"
    files, boxes, cache = {}, {}, {}
    for v in vs:
        key = (v["shape"], v["ratio"])
        if key not in cache:
            start, box = shape_box(*key)
            pbox = padded(box)
            tex = {}
            for k, img in zip(("area", "edge", "fill"), shape_textures(v["shape"], v["ratio"], pbox, tex_size(v["shape"]))):
                tex[k] = "tgp_%s_%s.blp" % (tag_of(*key), k)
                files[tex[k]] = dxt5_blp(img)
            cache[key] = (quads(pbox), start, pbox, tex)
        layers, start, box, tex = cache[key]
        m2, skin = model(v["name"], layers, start, box, COLOURS[v["col"]], v["dur"], tex, fade=v["shape"] == "donut")
        files[v["name"] + ".m2"] = m2
        files[v["name"] + "00.skin"] = skin
        boxes[v["name"]] = box
    text = sql(vs, boxes, args.display_base, args.display_last, args.go_base, args.go_last)
    print("variants %d (displays %d-%d, gameobjects %d-%d), files %d" % (len(vs), vs[0]["display"], vs[-1]["display"],
                                                                           vs[0]["go"], vs[-1]["go"], len(files)))
    if args.dry_run:
        return
    art = os.path.join(args.out, "Spells", "Telegraph")
    os.makedirs(art, exist_ok=True)
    for fn in os.listdir(art):                         # the folder is ours alone
        os.remove(os.path.join(art, fn))
    for fn, data in sorted(files.items()):
        with open(os.path.join(art, fn), "wb") as fh:
            fh.write(data)
    sqlp = os.path.join(args.out, "ground_telegraph_variants.sql")
    with open(sqlp, "w") as fh:
        fh.write(text)
    outs = [sqlp]
    if args.base:
        dbcp = os.path.join(args.out, "DBFilesClient", "GameObjectDisplayInfo.dbc")
        os.makedirs(os.path.dirname(dbcp), exist_ok=True)
        dbc_rows(args.base, vs, boxes, args.display_base, args.display_last).write(dbcp)
        outs.append(dbcp)
    hsh = hashlib.sha256()
    for fn in sorted(files):
        hsh.update(fn.encode() + files[fn])
    for p in outs:
        print("%s  %s" % (hashlib.sha256(open(p, "rb").read()).hexdigest()[:16], p))
    print("%s  art (%d files) %s" % (hsh.hexdigest()[:16], len(files), art))


if __name__ == "__main__":
    main()
