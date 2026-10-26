#!/usr/bin/env python3
"""Print Cap'n Proto struct layouts (data/pointer sizes, field offsets).

Used to derive the layout constants in src/cft_rpc.c.
Run: uv run --with pycapnp tools/capnp_layout.py SCHEMA_DIR
"""

import sys

import capnp

STRUCTS = {
    "rpc.capnp": [
        "Message", "Bootstrap", "Call", "Return", "Finish", "Release",
        "MessageTarget", "Payload", "CapDescriptor", "PromisedAnswer",
        "Exception", "Resolve", "Disembargo",
    ],
    "tunnelrpc.capnp": [
        "TunnelAuth", "ClientInfo", "ConnectionOptions", "ConnectionResponse",
        "ConnectionError", "ConnectionDetails",
    ],
}

INTERFACES = {
    "tunnelrpc.capnp": ["RegistrationServer"],
}


def slot_desc(field):
    which = field.proto.which()
    if which == "group":
        return "group"
    slot = field.proto.slot
    kind = slot.type.which()
    if kind in ("text", "data", "list", "struct", "interface", "anyPointer"):
        return f"ptr[{slot.offset}] {kind}"
    sizes = {"bool": 1, "int8": 8, "uint8": 8, "int16": 16, "uint16": 16,
             "int32": 32, "uint32": 32, "int64": 64, "uint64": 64,
             "float32": 32, "float64": 64, "enum": 16, "void": 0}
    bits = sizes[kind]
    if bits == 0:
        return "void"
    return f"data bit {slot.offset * bits} ({kind}, offset {slot.offset} in {bits}-bit units)"


def dump_struct(name, schema, indent=""):
    node = schema.node
    st = node.struct
    print(f"{indent}{name}: data_words={st.dataWordCount} ptrs={st.pointerCount}"
          + (f" discriminant@data bit {st.discriminantOffset * 16}" if st.discriminantCount else ""))
    for field in schema.as_struct().fields_list if hasattr(schema, "fields_list") else []:
        pass
    for f in node.struct.fields:
        disc = f" [union tag {f.discriminantValue}]" if f.discriminantValue != 65535 else ""
        if f.which() == "group":
            print(f"{indent}  {f.name}{disc}: group")
            continue
        fake = type("F", (), {"proto": f})
        print(f"{indent}  {f.name}{disc}: {slot_desc(fake)}")


def main():
    schema_dir = sys.argv[1]
    for fname, names in STRUCTS.items():
        mod = capnp.load(f"{schema_dir}/{fname}", imports=[schema_dir])
        print(f"### {fname} id={mod.schema.node.id:#x}" if hasattr(mod, "schema") else f"### {fname}")
        for n in names:
            dump_struct(n, getattr(mod, n).schema)
            # Unions inside groups are reported via nested group nodes.
            for f in getattr(mod, n).schema.node.struct.fields:
                if f.which() == "group":
                    sub = getattr(mod, n).schema.fields[f.name].schema
                    dump_struct(f"{n}.{f.name}", sub, "  ")
        for n in INTERFACES.get(fname, []):
            iface = getattr(mod, n).schema
            print(f"{n}: interface id={iface.node.id:#x}")
            for i, m in enumerate(iface.node.interface.methods):
                print(f"  @{i} {m.name} params={m.paramStructType:#x} results={m.resultStructType:#x}")
                ps = iface.methods[m.name].param_type
                dump_struct(f"  {m.name}$Params", ps, "  ")
                rs = iface.methods[m.name].result_type
                dump_struct(f"  {m.name}$Results", rs, "  ")


if __name__ == "__main__":
    main()
