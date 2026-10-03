#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""
bl31_oc.py - inspect and (optionally) patch the frequency tables inside the Rockchip RK3562 BL31 ELF.
DATA ONLY: no code is touched.  Tested on rk3562_bl31 v1.20, v1.21, v1.22, v1.23 (v1.19 is refused).

BL31 holds, for CPU / GPU / NPU:
  * a PVTPLL table of 12-byte records {u32 rate_hz, u32 ring, u32 length}.  SCMI set_rate looks the rate
    up by EXACT match.  length==0: normal PLL through a divider; length!=0: PVTPLL ring oscillator
    (shorter ring = faster at a given voltage).  Lookup loops are bounded by a hard-coded slot count
    that includes one spare all-zero record, so one new entry fits without touching code.
  * an SCMI rate list (u64 array).  The kernel registers [first, last] of it as the clock's allowed range
    (clk_hw_set_rate_range), so a rate above the last element never reaches BL31.  The last element is
    stored as rate_hz + 63 (low 6 bits of a request carry flags).

usage:
  bl31_oc.py rk3562_bl31_v1.21.elf --show
  bl31_oc.py rk3562_bl31_v1.21.elf -o bl31_oc.elf --cpu 2112:4:4 --cpu 2208:4:3 --gpu 1000:1:11
    --cpu/--gpu/--npu MHZ:RING:LEN   add or update an entry
    --recycle-cpu MHZ (likewise gpu/npu)  allow overwriting an existing entry (CPU default: 1896, 1704)
"""
import argparse, hashlib, struct

REC, PLUS = 12, 63
NAMES = {b"scmi_clk_cpu": "cpu", b"scmi_clk_gpu": "gpu", b"scmi_clk_npu": "npu"}
DEFAULT_RECYCLE = {"cpu": [1896, 1704], "gpu": [], "npu": []}
MAX_CPU_MHZ = 2400   # pclk_dbg divider is 4 bits: ceil(rate/150MHz) must be <= 16


class Elf:
    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 2:
            raise SystemExit("not a 64-bit ELF")
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        self.loads = []
        for i in range(phnum):
            t, _f, off, va, _pa, fsz, _m, _a = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
            if t == 1 and fsz:
                self.loads.append((va, off, fsz))
        self.data = bytearray(data)

    def va2off(self, va, n=1):
        for va0, off0, fsz in self.loads:
            if va0 <= va and va + n <= va0 + fsz:
                return off0 + va - va0

    def off2va(self, off):
        for va0, off0, fsz in self.loads:
            if off0 <= off < off0 + fsz:
                return va0 + off - off0

    def u32(self, o): return struct.unpack_from("<I", self.data, o)[0]
    def u64(self, o): return struct.unpack_from("<Q", self.data, o)[0]


def valid_rec(elf, o):
    if o is None or o < 0 or o + REC > len(elf.data):
        return None
    r, a, b = struct.unpack_from("<III", elf.data, o)
    if r % 1000000 or not (100000000 <= r <= 4000000000) or a > 0xFF or b > 0x7F:
        return None
    return r, a, b


def run_from(elf, off, limit):
    recs, prev, o, zt = [], None, off, False
    while o + REC <= limit:
        if elf.data[o:o + REC] == b"\x00" * REC:
            zt = True
            break
        r = valid_rec(elf, o)
        if r is None or (prev is not None and r[0] >= prev):
            break
        recs.append(r)
        prev, o = r[0], o + REC
    return recs, zt


def find_descriptors(elf):
    """scmi clock descriptor: name[16] @+0, clk id u32 @+0x14, u64 count @+0x18, u64 rates ptr @+0x48"""
    out, d = {}, bytes(elf.data)
    for name, key in NAMES.items():
        pos = d.find(name + b"\x00")
        if pos < 0:
            continue
        count, ptr = elf.u64(pos + 0x18), elf.u64(pos + 0x48)
        poff = elf.va2off(ptr, 8 * count) if 1 <= count <= 32 else None
        if poff is None:
            continue
        out[key] = {"id": elf.u32(pos + 0x14), "count": count, "list_off": poff, "list_va": ptr,
                    "rates": [elf.u64(poff + 8 * i) for i in range(count)]}
    return out


def locate(elf):
    """table start = record whose rate equals the SCMI list maximum; tables lie in the file in list order"""
    descs, res, min_off = find_descriptors(elf), {}, 0
    for key in sorted(descs, key=lambda k: descs[k]["list_va"]):
        ds = descs[key]
        top = ds["rates"][-1]
        top = top - PLUS if top % 1000000 == PLUS else top
        best, pat = None, struct.pack("<I", top)
        for va0, off0, fsz in elf.loads:
            i = elf.data.find(pat, max(off0, min_off), off0 + fsz)
            while i >= 0:
                if (i - off0) % 4 == 0 and valid_rec(elf, i):
                    recs, zt = run_from(elf, i, off0 + fsz)
                    prev = valid_rec(elf, i - REC)
                    if len(recs) >= 5 and not (prev and prev[0] > top) and (best is None or i < best[0]):
                        best = (i, recs, zt)
                i = elf.data.find(pat, i + 1, off0 + fsz)
        if best:
            off, recs, zt = best
            cnt = len(recs) + (1 if zt else 0)
            res[key] = {"table": {"off": off, "recs": recs, "count": cnt, "free": zt}, "desc": ds}
            min_off = off + cnt * REC
    if "cpu" not in res or "gpu" not in res:
        raise SystemExit("cannot locate CPU/GPU tables - unknown BL31 build, refusing to guess")
    return res


def show(elf, info):
    for key in ("cpu", "gpu", "npu"):
        if key not in info:
            continue
        t, ds = info[key]["table"], info[key]["desc"]
        print("== %s (SCMI clk id %d)\n   table @ file+%#x / va %#x, %d slots" % (
            key.upper(), ds["id"], t["off"], elf.off2va(t["off"]), t["count"]))
        for i, (r, a, b) in enumerate(t["recs"]):
            print("     [%2d] %5d MHz  ring=%d len=%-3d %s" % (i, r // 1000000, a, b, "(PLL)" if b == 0 else "(PVTPLL)"))
        print("     [%2d] (free)" % len(t["recs"]) if t["free"] else "     (table is full)")
        print("   SCMI list @ va %#x: %s" % (ds["list_va"], ", ".join(
            "%d%s" % (r // 1000000, "+63Hz" if r % 1000000 == PLUS else "") for r in ds["rates"])))


def apply(elf, info, adds, recycles):
    for key, entries in adds.items():
        if not entries:
            continue
        if key not in info:
            raise SystemExit("no %s tables in this BL31" % key)
        t, ds = info[key]["table"], info[key]["desc"]
        slots = [t["off"] + i * REC for i in range(t["count"])]
        for mhz, ring, length in entries:
            rate = mhz * 1000000
            if key == "cpu" and mhz > MAX_CPU_MHZ:
                raise SystemExit("CPU label %d MHz > %d MHz: pclk_dbg divider would overflow" % (mhz, MAX_CPU_MHZ))
            if not 1 <= length <= 127:
                raise SystemExit("len must be 1..127 (0 selects the normal PLL path)")
            target = next((o for o in slots if elf.u32(o) == rate), None)
            if target is None:
                target = next((o for o in slots if elf.data[o:o + REC] == b"\x00" * REC), None)
            if target is None:
                for m in recycles[key]:
                    target = next((o for o in slots if elf.u32(o) == m * 1000000), None)
                    if target is not None:
                        print("   note: recycling %s %d MHz slot" % (key, m))
                        break
            if target is None:
                raise SystemExit("no free slot in %s table for %d MHz (use --recycle-%s MHZ)" % (key, mhz, key))
            struct.pack_into("<III", elf.data, target, rate, ring, length)
        recs = sorted((struct.unpack_from("<III", elf.data, o) for o in slots
                       if elf.data[o:o + REC] != b"\x00" * REC), key=lambda r: -r[0])
        recs += [(0, 0, 0)] * (len(slots) - len(recs))
        for o, r in zip(slots, recs):
            struct.pack_into("<III", elf.data, o, *r)
        top, lst = recs[0][0], ds["rates"]
        cur = lst[-1] - PLUS if lst[-1] % 1000000 == PLUS else lst[-1]
        if top > cur:
            new = lst[:-1] + [top + PLUS]
            have = {r[0] for r in recs}
            stale = [i for i, v in enumerate(new[:-1]) if v not in have]
            if stale and cur not in new:
                new[stale[-1]] = cur
            for i, v in enumerate(new):
                struct.pack_into("<Q", elf.data, ds["list_off"] + 8 * i, v)
            ds["rates"] = new
            print("   %s SCMI list max %d -> %d MHz" % (key, cur // 1000000, top // 1000000))


def entry(s):
    try:
        r, a, b = s.split(":")
        return int(r), int(a), int(b)
    except Exception:
        raise argparse.ArgumentTypeError("expected MHZ:RING:LEN, e.g. 2112:4:4")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf"); ap.add_argument("-o", "--output"); ap.add_argument("--show", action="store_true")
    for k in ("cpu", "gpu", "npu"):
        ap.add_argument("--" + k, action="append", type=entry, default=[], metavar="MHZ:RING:LEN")
        ap.add_argument("--recycle-" + k, action="append", type=int, default=[], metavar="MHZ")
    a = ap.parse_args()
    raw = open(a.elf, "rb").read()
    elf = Elf(raw)
    info = locate(elf)
    print("input : %s sha256 %s" % (a.elf, hashlib.sha256(raw).hexdigest()[:16]))
    adds = {"cpu": a.cpu, "gpu": a.gpu, "npu": a.npu}
    if a.show or not any(adds.values()):
        show(elf, info)
        if not any(adds.values()):
            return
    if not a.output:
        raise SystemExit("-o OUTPUT is required")
    apply(elf, info, adds, {k: getattr(a, "recycle_" + k) or DEFAULT_RECYCLE[k] for k in adds})
    out = bytes(elf.data)
    open(a.output, "wb").write(out)
    print("output: %s sha256 %s (%d bytes differ)" % (a.output, hashlib.sha256(out).hexdigest()[:16],
                                                      sum(x != y for x, y in zip(raw, out))))
    e2 = Elf(out)
    show(e2, locate(e2))


if __name__ == "__main__":
    main()
