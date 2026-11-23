# mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
"""Minimal WDBC (WoW 3.3.5a .dbc) reader / writer: records of 4-byte fields + a string block (offset 0 = "").
DBCWriter keeps untouched records byte-identical and appends new strings."""
import struct


class DBC:
    def __init__(self, path, kind=None):
        data = open(path, "rb").read()
        magic, self.nrec, self.nfield, self.recsize, self.strsize = struct.unpack_from("<4s4I", data, 0)
        assert magic == b"WDBC", f"{path}: not a WDBC file"
        assert self.recsize == self.nfield * 4, "non-4-byte fields not supported"
        self.path = path
        self.kind = kind
        off = 20
        self.raw_records = [list(struct.unpack_from(f"<{self.nfield}I", data, off + i * self.recsize))
                            for i in range(self.nrec)]
        self.strings = data[off + self.nrec * self.recsize: off + self.nrec * self.recsize + self.strsize]
        names, floats, strs = ([f"f{i}" for i in range(self.nfield)], set(), set())
        if len(names) != self.nfield:
            names = [f"f{i}" for i in range(self.nfield)]
        self.names, self.floats, self.strfields = names, floats, strs
        self.by_id = {r[0]: r for r in self.raw_records}

    def string(self, offset):
        if offset == 0 or offset >= len(self.strings):
            return ""
        end = self.strings.index(b"\0", offset)
        return self.strings[offset:end].decode("utf-8", "replace")

    def value(self, rec, i):
        v = rec[i]
        if i in self.floats:
            return round(struct.unpack("<f", struct.pack("<I", v))[0], 4)
        if i in self.strfields:
            return self.string(v)
        if v >= 0x80000000:
            return v - 0x100000000  # show as signed
        return v

    def row(self, rec_or_id):
        rec = self.by_id[rec_or_id] if isinstance(rec_or_id, int) else rec_or_id
        return {self.names[i]: self.value(rec, i) for i in range(self.nfield)}


def _f2u(x):
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


def _i2u(x):
    return int(x) & 0xFFFFFFFF


class DBCWriter:
    """Edits a loaded DBC in place and writes it back out.

    Existing strings stay where they are; new strings are appended to the
    string block, so untouched records are byte-identical to the input."""

    def __init__(self, dbc):
        self.dbc = dbc
        self.strings = bytearray(dbc.strings)
        self._index = {}

    def add_string(self, text):
        if text == "":
            return 0
        raw = text.encode("utf-8")
        if raw in self._index:
            return self._index[raw]
        # reuse an identical existing string if present
        pos = self.strings.find(b"\0" + raw + b"\0")
        off = pos + 1 if pos >= 0 else None
        if off is None:
            off = len(self.strings)
            self.strings += raw + b"\0"
        self._index[raw] = off
        return off

    def set(self, rec_id, field, value):
        d = self.dbc
        i = d.names.index(field) if isinstance(field, str) else field
        rec = d.by_id[rec_id]
        if i in d.strfields:
            rec[i] = self.add_string(value)
        elif i in d.floats:
            rec[i] = _f2u(value)
        else:
            rec[i] = _i2u(value)

    def copy_record(self, src_id, dst_id):
        """Overwrite record dst_id with a copy of src_id (keeps dst ID)."""
        d = self.dbc
        src = d.by_id[src_id]
        dst = d.by_id[dst_id]
        dst[:] = list(src)
        dst[0] = dst_id

    def add_record(self, dst_id, src_id=None):
        """Create record dst_id (a copy of src_id, or all zeros) if it doesn't
        exist yet. Records stay sorted by ID. Returns True if it was created."""
        d = self.dbc
        if dst_id in d.by_id:
            if src_id is not None:
                self.copy_record(src_id, dst_id)
            return False
        rec = list(d.by_id[src_id]) if src_id is not None else [0] * d.nfield
        rec[0] = dst_id
        pos = len(d.raw_records)
        while pos > 0 and d.raw_records[pos - 1][0] > dst_id:
            pos -= 1
        d.raw_records.insert(pos, rec)
        d.by_id[dst_id] = rec
        return True

    def write(self, path):
        d = self.dbc
        out = bytearray(struct.pack("<4s4I", b"WDBC", len(d.raw_records), d.nfield, d.recsize, len(self.strings)))
        for rec in d.raw_records:
            out += struct.pack(f"<{d.nfield}I", *rec)
        out += self.strings
        with open(path, "wb") as fh:
            fh.write(out)
