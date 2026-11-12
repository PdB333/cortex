#!/usr/bin/env python3
"""Builds the capture, firmware, document and shader fixtures of
tests/fixtures/format-more/media and their reference values:

  sample.pcap, sample.pcapng   dpkt writers; the TLS ClientHello is a real
                               one from Python's ssl module (MemoryBIO), the
                               DNS query from dpkt.dns; expected values are
                               read back with dpkt
  image.hex                    intelhex (segments, start address)
  image.srec                   bincopy (segments, header, entry point)
  image.uf2                    built to the UF2 specification
  board.dtb                    fdt, read back with fdt.parse_dtb
  sample.pdf, encrypted.pdf    pypdf, read back with PdfReader
  target.lnk                   pylnk3, read back with LnkParse3
  compute.spv, fragment.spv    glslang (Khronos), via @webgpu/glslang in Node

Needs `pip install dpkt intelhex bincopy fdt pypdf pylnk3 LnkParse3` and,
for SPIR-V, `npm install @webgpu/glslang@0.0.15` in GLSLANG_DIR.
"""
import io
import json
import os
import ssl
import struct
import subprocess
import sys

import dpkt

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "media")
os.makedirs(OUT, exist_ok=True)
expected = {}

# ---------------------------------------------------------------- captures ---
ctx = ssl.create_default_context()
incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
tls = ctx.wrap_bio(incoming, outgoing, server_hostname="cortex.example")
try:
    tls.do_handshake()
except ssl.SSLWantReadError:
    pass
client_hello = outgoing.read()

dns_query = dpkt.dns.DNS(id=0x1234, qd=[dpkt.dns.DNS.Q(name="owned.cortex.test", type=dpkt.dns.DNS_A)])
http = b"GET /index.html HTTP/1.1\r\nHost: cortex.test\r\nUser-Agent: fixture\r\n\r\n"

def eth(payload, kind):
    return dpkt.ethernet.Ethernet(src=b"\x02\x00\x00\x00\x00\x01", dst=b"\x02\x00\x00\x00\x00\x02", type=kind, data=payload)

def ip4(src, dst, payload, proto):
    return dpkt.ip.IP(src=bytes(map(int, src.split("."))), dst=bytes(map(int, dst.split("."))), p=proto, data=payload, ttl=64)

packets = [
    eth(ip4("10.0.0.2", "93.184.216.34", dpkt.tcp.TCP(sport=51515, dport=443, flags=dpkt.tcp.TH_PUSH | dpkt.tcp.TH_ACK, data=client_hello), 6), 0x0800),
    eth(ip4("10.0.0.2", "8.8.8.8", dpkt.udp.UDP(sport=40000, dport=53, data=bytes(dns_query)), 17), 0x0800),
    eth(dpkt.ip6.IP6(src=bytes.fromhex("fd000000000000000000000000000002"), dst=bytes.fromhex("fd000000000000000000000000000001"),
                     nxt=6, hlim=64, data=dpkt.tcp.TCP(sport=40001, dport=80, flags=dpkt.tcp.TH_PUSH | dpkt.tcp.TH_ACK, data=http)), 0x86DD),
    eth(dpkt.arp.ARP(), 0x0806),
    eth(ip4("10.0.0.2", "10.0.0.1", dpkt.icmp.ICMP(type=8, data=dpkt.icmp.ICMP.Echo(id=1, seq=1, data=b"ping")), 1), 0x0800),
]
for p in packets:  # let dpkt compute lengths and checksums
    if isinstance(p.data, dpkt.ip.IP):
        p.data.len = len(bytes(p.data))
    elif isinstance(p.data, dpkt.ip6.IP6):
        p.data.plen = len(bytes(p.data.data))
times = [1700000000.25 + i for i in range(len(packets))]
with open(os.path.join(OUT, "sample.pcap"), "wb") as f:
    w = dpkt.pcap.Writer(f, linktype=dpkt.pcap.DLT_EN10MB)
    for ts, p in zip(times, packets):
        w.writepkt(bytes(p), ts=ts)
with open(os.path.join(OUT, "sample.pcapng"), "wb") as f:
    w = dpkt.pcapng.Writer(f, linktype=dpkt.pcap.DLT_EN10MB)
    for ts, p in zip(times, packets):
        w.writepkt(bytes(p), ts=ts)

def reference(path, reader):
    with open(path, "rb") as f:
        r = reader(f)
        protocols, flows, sni, http_lines, dns_names, stamps = {}, set(), set(), set(), set(), []
        for ts, buf in r:
            stamps.append(ts)
            e = dpkt.ethernet.Ethernet(buf)
            ip = e.data
            if isinstance(ip, dpkt.arp.ARP):
                protocols["arp"] = protocols.get("arp", 0) + 1
                continue
            fam = "ipv4" if isinstance(ip, dpkt.ip.IP) else "ipv6"
            protocols[fam] = protocols.get(fam, 0) + 1
            l4 = ip.data
            if isinstance(l4, dpkt.tcp.TCP):
                protocols["tcp"] = protocols.get("tcp", 0) + 1
                flows.add(("tcp/" + fam, l4.sport, l4.dport))
                if l4.data[:1] == b"\x16":
                    rec = dpkt.ssl.TLSRecord(l4.data)
                    hello = dpkt.ssl.TLSHandshake(rec.data).data
                    for ext_type, ext_data in hello.extensions:
                        if ext_type == 0:
                            sni.add(ext_data[5:5 + struct.unpack(">H", ext_data[3:5])[0]].decode())
                elif l4.data.startswith(b"GET"):
                    req = dpkt.http.Request(l4.data)
                    http_lines.add(f"{req.method} {req.uri} HTTP/{req.version} [host {req.headers['host']}]")
            elif isinstance(l4, dpkt.udp.UDP):
                protocols["udp"] = protocols.get("udp", 0) + 1
                flows.add(("udp/" + fam, l4.sport, l4.dport))
                if 53 in (l4.sport, l4.dport):
                    dns_names.update(q.name for q in dpkt.dns.DNS(l4.data).qd)
            elif isinstance(l4, dpkt.icmp.ICMP):
                protocols["icmp"] = protocols.get("icmp", 0) + 1
    return {"packets": len(stamps), "protocols": protocols, "flows": sorted(list(x) for x in flows),
            "sni": sorted(sni), "http": sorted(http_lines), "dns": sorted(dns_names),
            "first": min(stamps), "last": max(stamps)}

expected["pcap"] = reference(os.path.join(OUT, "sample.pcap"), dpkt.pcap.Reader)
expected["pcapng"] = reference(os.path.join(OUT, "sample.pcapng"), dpkt.pcapng.Reader)

# ---------------------------------------------------------------- firmware ---
from intelhex import IntelHex
ih = IntelHex()
ih.puts(0x0000, bytes(range(32)))
ih.puts(0x10000, b"cortex owned firmware")
ih.start_addr = {"EIP": 0x10000}
ih.write_hex_file(os.path.join(OUT, "image.hex"))
expected["ihex"] = {"segments": [list(s) for s in ih.segments()], "start": 0x10000, "bytes": len(ih)}

import bincopy
bf = bincopy.BinFile()
bf.add_binary(bytes(range(64)), address=0x08000000)
bf.add_binary(b"cortex", address=0x08001000)
bf.header = "cortex"
bf.execution_start_address = 0x08000000
with open(os.path.join(OUT, "image.srec"), "w") as f:
    f.write(bf.as_srec())
check = bincopy.BinFile(os.path.join(OUT, "image.srec"))
expected["srec"] = {"segments": [[s.minimum_address, s.maximum_address] for s in check.segments],
                    "start": check.execution_start_address, "header": check.header}

# UF2: 1024 bytes for an RP2040 at 0x10000000, 256-byte payloads.
payload = bytes((i * 7) & 0xFF for i in range(1024))
blocks = []
n = len(payload) // 256
for i in range(n):
    data = payload[i * 256:(i + 1) * 256].ljust(476, b"\0")
    blocks.append(struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x2000, 0x10000000 + i * 256, 256, i, n, 0xE48BFF56)
                  + data + struct.pack("<I", 0x0AB16F30))
with open(os.path.join(OUT, "image.uf2"), "wb") as f:
    f.write(b"".join(blocks))
expected["uf2"] = {"blocks": n, "family": "RP2040", "start": 0x10000000, "bytes": len(payload)}

import fdt
tree = fdt.FDT()
tree.set_property("model", "Cortex Fixture Board")
tree.set_property("compatible", ["cortex,fixture", "cortex,board"])
tree.set_property("#address-cells", 1)
tree.set_property("#size-cells", 1)
tree.set_property("device_type", "cpu", path="/cpus/cpu@0")
tree.set_property("reg", 0, path="/cpus/cpu@0")
tree.set_property("device_type", "memory", path="/memory@80000000")
tree.set_property("reg", [0x80000000, 0x10000000], path="/memory@80000000")
tree.set_property("bootargs", "console=ttyS0 owned", path="/chosen")
with open(os.path.join(OUT, "board.dtb"), "wb") as f:
    f.write(tree.to_dtb(version=17))
with open(os.path.join(OUT, "board.dtb"), "rb") as f:
    parsed = fdt.parse_dtb(f.read())
paths = []
for path, nodes, props in parsed.walk():
    paths.append(path)
expected["dtb"] = {"paths": sorted(paths), "model": "Cortex Fixture Board", "compatible": ["cortex,fixture", "cortex,board"],
                   "memory_reg": [0x80000000, 0x10000000], "bootargs": "console=ttyS0 owned"}

# --------------------------------------------------------------- documents ---
from pypdf import PdfReader, PdfWriter
from pypdf.annotations import Link
w = PdfWriter()
for _ in range(3):
    w.add_blank_page(width=200, height=200)
w.add_metadata({"/Title": "Cortex fixture", "/Author": "Owned"})
w.add_attachment("notes.txt", b"owned notes")
# Document-level JavaScript (names tree) that also runs as the open action.
from pypdf.generic import ArrayObject, DictionaryObject, NameObject, TextStringObject
js = DictionaryObject({NameObject("/Type"): NameObject("/Action"), NameObject("/S"): NameObject("/JavaScript"),
                       NameObject("/JS"): TextStringObject("app.alert('cortex fixture');")})
js_ref = w._add_object(js)
root = w._root_object
names = root.get("/Names")
names = names.get_object() if names is not None else DictionaryObject()
names[NameObject("/JavaScript")] = DictionaryObject({NameObject("/Names"): ArrayObject([TextStringObject("cortex"), js_ref])})
root[NameObject("/Names")] = names
root[NameObject("/OpenAction")] = js_ref
w.add_annotation(page_number=0, annotation=Link(rect=(10, 10, 100, 40), url="https://cortex.example/"))
with open(os.path.join(OUT, "sample.pdf"), "wb") as f:
    w.write(f)
w.encrypt(user_password="", owner_password="cortex-owner", algorithm="RC4-128")
with open(os.path.join(OUT, "encrypted.pdf"), "wb") as f:
    w.write(f)
r = PdfReader(os.path.join(OUT, "sample.pdf"))
uris = [a.get_object()["/A"]["/URI"] for p in r.pages for a in (p.get("/Annots") or []) if "/A" in a.get_object()]
expected["pdf"] = {"pages": len(r.pages), "title": r.metadata.title, "author": r.metadata.author,
                   "attachments": sorted(r.attachments.keys()), "javascript": "/JavaScript" in r.trailer["/Root"]["/Names"],
                   "uris": uris, "encrypted": PdfReader(os.path.join(OUT, "encrypted.pdf")).is_encrypted}

import pylnk3
lnk_path = os.path.join(OUT, "target.lnk")
pylnk3.for_file(r"C:\Windows\System32\notepad.exe", lnk_name=lnk_path, arguments="/A owned.txt",
                description="Cortex fixture link", icon_file=r"C:\Windows\System32\shell32.dll", icon_index=3,
                work_dir=r"C:\Users\Public", window_mode="Maximized")
import LnkParse3
with open(lnk_path, "rb") as f:
    parsed = LnkParse3.lnk_file(f).get_json()
volume = next(i["volume_name"] for i in parsed["target"]["items"] if i["class"] == "Volume Item")
files = [i["primary_name"] for i in parsed["target"]["items"] if i["class"] == "File entry"]
rename = {"HasTargetIDList": "HasLinkTargetIDList"}
expected["lnk"] = {"data": parsed["data"], "flags": [rename.get(f, f) for f in parsed["header"]["link_flags"]],
                   "window_style": parsed["header"]["windowstyle"], "icon_index": parsed["header"]["icon_index"],
                   "target_path": volume + "\\".join(files)}

# ------------------------------------------------------------------ SPIR-V ---
glslang = os.environ.get("GLSLANG_DIR")
if glslang:
    script = r'''
const fs = require('fs');
require('@webgpu/glslang')().then(g => {
  delete g.then;
  const compute = `#version 450
layout(local_size_x = 8, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) buffer Data { float values[]; } cortexData;
void main() { cortexData.values[gl_GlobalInvocationID.x] *= 2.0; }`;
  const fragment = `#version 450
layout(location = 0) in vec3 cortexColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(cortexColor, 1.0); }`;
  fs.writeFileSync(process.argv[2], Buffer.from(g.compileGLSL(compute, 'compute', true).buffer));
  fs.writeFileSync(process.argv[3], Buffer.from(g.compileGLSL(fragment, 'fragment', true).buffer));
  process.exit(0);
});
'''
    js = os.path.join(glslang, "make_spirv.js")
    with open(js, "w") as f:
        f.write(script)
    subprocess.run(["node", js, os.path.join(OUT, "compute.spv"), os.path.join(OUT, "fragment.spv")], check=True, cwd=glslang, timeout=120)
expected["spirv"] = {"compute": {"model": "GLCompute", "local_size": [8, 1, 1], "names": ["cortexData", "Data", "values"]},
                     "fragment": {"model": "Fragment", "names": ["cortexColor", "outColor"]}}

with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True, default=str)
print("fixtures written to", OUT)
