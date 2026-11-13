"""List the bindings of all SPIR-V modules of an fcshaders run (basis for the pipeline layout).

Usage: python -I spv_bindings.py <run>/spirv
Output: per (set, binding, kind) the number of modules, plus capabilities, execution models and the highest
location of the stage interface.
"""
import collections
import os
import struct
import sys

OP_NAME = {17: "OpCapability", 15: "OpEntryPoint", 71: "OpDecorate", 59: "OpVariable", 32: "OpTypePointer",
           25: "OpTypeImage", 26: "OpTypeSampler", 27: "OpTypeSampledImage", 28: "OpTypeArray",
           29: "OpTypeRuntimeArray", 30: "OpTypeStruct"}
DIM = {0: "1D", 1: "2D", 2: "3D", 3: "Cube", 5: "Buffer"}
CAPS = {1: "Shader", 2: "Geometry", 32: "ClipDistance", 33: "CullDistance", 5302: "RuntimeDescriptorArray",
        5301: "ShaderNonUniform", 5306: "SampledImageArrayNonUniformIndexing",
        5308: "StorageBufferArrayNonUniformIndexing", 34: "SampledCubeArray", 23: "Float16", 11: "Int64",
        10: "Float64", 50: "ImageQuery", 51: "DerivativeControl", 4427: "DrawParameters",
        5305: "UniformBufferArrayNonUniformIndexing", 43: "SampledBuffer", 22: "Int16",
        5307: "StorageBufferArrayDynamicIndexing", 29: "StorageBufferArrayDynamicIndexing",
        28: "UniformBufferArrayDynamicIndexing", 30: "StorageImageArrayDynamicIndexing",
        31: "SampledImageArrayDynamicIndexing", 5312: "SamplerArrayNonUniformIndexing?"}


def parse(path):
    data = open(path, "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    assert words[0] == 0x07230203, path
    version = words[1]
    i = 5
    types = {}
    deco = collections.defaultdict(dict)
    variables = []
    caps = set()
    models = set()
    max_loc = [-1, -1]  # in, out
    locs = {}
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        a = words[i + 1:i + wc]
        if op == 17:
            caps.add(a[0])
        elif op == 15:
            models.add(a[0])
        elif op == 71:
            if a[1] == 34:
                deco[a[0]]["set"] = a[2]
            elif a[1] == 33:
                deco[a[0]]["binding"] = a[2]
            elif a[1] == 30:
                locs[a[0]] = a[2]
            elif a[1] == 2:
                deco[a[0]]["block"] = True
            elif a[1] == 3:
                deco[a[0]]["bufferblock"] = True
        elif op == 25:
            types[a[0]] = ("image", DIM.get(a[2], str(a[2])), a[4])  # dim, arrayed
        elif op == 26:
            types[a[0]] = ("sampler",)
        elif op == 28:
            types[a[0]] = ("array", a[1])
        elif op == 29:
            types[a[0]] = ("rtarray", a[1])
        elif op == 30:
            types[a[0]] = ("struct", a[0])
        elif op == 32:
            types[a[0]] = ("ptr", a[2], a[1])
        elif op == 59:
            variables.append((a[1], a[0], a[2]))  # id, type, storage class
        i += wc
    out = []
    for vid, tid, sc in variables:
        if sc == 1 and vid in locs:
            max_loc[0] = max(max_loc[0], locs[vid])
        if sc == 3 and vid in locs:
            max_loc[1] = max(max_loc[1], locs[vid])
        if "set" not in deco[vid]:
            continue
        t = types[tid]
        t = types[t[1]]
        arr = ""
        while t[0] in ("array", "rtarray"):
            arr = "[]" if t[0] == "rtarray" else "[n]"
            t = types[t[1]]
        if t[0] == "image":
            kind = "image" + t[1] + ("Array" if t[2] else "")
        elif t[0] == "sampler":
            kind = "sampler"
        elif t[0] == "struct":
            d = deco[t[1]]
            kind = {2: "ubo" if not d.get("bufferblock") else "ssbo(old)", 12: "ssbo"}.get(sc, "struct-sc%d" % sc)
        else:
            kind = t[0]
        out.append((deco[vid]["set"], deco[vid].get("binding", -1), kind + arr))
    return version, caps, models, out, max_loc


def main():
    root = sys.argv[1]
    binds = collections.Counter()
    caps = collections.Counter()
    models = collections.Counter()
    versions = collections.Counter()
    mi = mo = -1
    n = 0
    for f in sorted(os.listdir(root)):
        if not f.endswith(".spv"):
            continue
        n += 1
        v, c, m, b, ml = parse(os.path.join(root, f))
        versions[hex(v)] += 1
        for x in c:
            caps[CAPS.get(x, str(x))] += 1
        for x in m:
            models[{0: "Vertex", 4: "Fragment"}.get(x, str(x))] += 1
        for x in set(b):
            binds[x] += 1
        mi, mo = max(mi, ml[0]), max(mo, ml[1])
    print("Modules:", n, "versions:", dict(versions), "models:", dict(models))
    print("Capabilities:", dict(caps))
    print("highest location input/output:", mi, mo)
    for k in sorted(binds):
        print("set %d binding %d %-22s %5d modules" % (k[0], k[1], k[2], binds[k]))


if __name__ == "__main__":
    main()
