#!/usr/bin/env python3
"""Generate Wayland wire tables from protocol XML.

Both proxy_client (machine A) and proxy_server (machine B) must agree on the
shape of every message so the wire parser can walk a byte stream generically
instead of hand-decoding each interface. This mirrors generate_remoting.py's
role for Vulkan: derive the tables from the registry XML rather than
hand-maintain them.

Two independent things are emitted from the same parse:
  - wire::InterfaceSpec / MessageSpec / ArgSpec tables, used by wire.cpp to
    decode and re-encode raw wire bytes without linking libwayland.
  - `struct wl_interface` definitions (name, signature, types) for the
    protocols libwayland itself does not already export symbols for (the
    core wayland.xml interfaces are already in libwayland-client, so only
    extension protocols such as xdg-shell need generating here). These let
    proxy_server construct new libwayland proxies of these types generically
    via wl_proxy_marshal_array_flags.
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET

# Interfaces libwayland-client already exports a `wl_*_interface` symbol for
# (declared in wayland-client-protocol.h). Anything else we parse gets its
# wl_interface struct generated here instead.
CORE_INTERFACES = {
    'wl_display', 'wl_registry', 'wl_callback', 'wl_compositor', 'wl_shm_pool',
    'wl_shm', 'wl_buffer', 'wl_data_offer', 'wl_data_source', 'wl_data_device',
    'wl_data_device_manager', 'wl_shell', 'wl_shell_surface', 'wl_surface',
    'wl_seat', 'wl_pointer', 'wl_keyboard', 'wl_touch', 'wl_output',
    'wl_region', 'wl_subcompositor', 'wl_subsurface',
}

ARG_TYPE_ENUM = {
    'int': 'Int', 'uint': 'Uint', 'fixed': 'Fixed', 'string': 'String',
    'object': 'Object', 'new_id': 'NewId', 'array': 'Array', 'fd': 'Fd',
}

ARG_TYPE_SIGCHAR = {
    'int': 'i', 'uint': 'u', 'fixed': 'f', 'string': 's',
    'object': 'o', 'new_id': 'n', 'array': 'a', 'fd': 'h',
}


class Arg:
    def __init__(self, xml_arg):
        self.name = xml_arg.get('name')
        raw_type = xml_arg.get('type')
        if raw_type == 'uint' and xml_arg.get('enum'):
            raw_type = 'uint'  # enums travel as uint on the wire
        self.wire_type = raw_type
        self.interface = xml_arg.get('interface')  # None for dynamic new_id


class Message:
    def __init__(self, xml_msg, opcode):
        self.name = xml_msg.get('name')
        self.opcode = opcode
        self.args = [Arg(a) for a in xml_msg.findall('arg')]
        self.is_destructor = xml_msg.get('type') == 'destructor'


class Interface:
    def __init__(self, xml_iface):
        self.name = xml_iface.get('name')
        self.version = int(xml_iface.get('version', '1'))
        self.requests = [Message(m, i) for i, m in enumerate(xml_iface.findall('request'))]
        self.events = [Message(m, i) for i, m in enumerate(xml_iface.findall('event'))]


def parse_protocols(xml_paths):
    interfaces = {}
    for path in xml_paths:
        tree = ET.parse(path)
        for xml_iface in tree.getroot().findall('interface'):
            iface = Interface(xml_iface)
            interfaces[iface.name] = iface  # later files override same-named dupes
    return interfaces


def emit_cstring(s):
    return '"{}"'.format(s.replace('\\', '\\\\').replace('"', '\\"'))


def emit_wire_tables(out, interfaces):
    print('namespace wire {', file=out)
    print('', file=out)

    # Per-message argument arrays, named so requests and events cannot collide.
    for iface in interfaces.values():
        for kind, msgs in (('req', iface.requests), ('ev', iface.events)):
            for msg in msgs:
                sym = 'kArgs_{}_{}_{}'.format(iface.name, kind, msg.name)
                if not msg.args:
                    continue
                print('static const ArgSpec {}[] = {{'.format(sym), file=out)
                for arg in msg.args:
                    type_name = ARG_TYPE_ENUM[arg.wire_type]
                    iface_str = emit_cstring(arg.interface) if arg.interface else 'nullptr'
                    print('    {{ArgType::{}, {}}},'.format(type_name, iface_str), file=out)
                print('};', file=out)

    # Per-interface request/event message arrays.
    for iface in interfaces.values():
        for kind, msgs in (('req', iface.requests), ('ev', iface.events)):
            sym = 'kMsgs_{}_{}'.format(iface.name, kind)
            if not msgs:
                continue
            print('static const MessageSpec {}[] = {{'.format(sym), file=out)
            for msg in msgs:
                args_sym = 'kArgs_{}_{}_{}'.format(iface.name, kind, msg.name)
                args_ref = args_sym if msg.args else 'nullptr'
                destructor = 'true' if msg.is_destructor else 'false'
                print('    {{{}, {}, {}, {}, {}}},'.format(
                    emit_cstring(msg.name), msg.opcode, len(msg.args), args_ref, destructor), file=out)
            print('};', file=out)

    print('static const InterfaceSpec kInterfaceTable[] = {', file=out)
    for iface in interfaces.values():
        req_sym = 'kMsgs_{}_req'.format(iface.name) if iface.requests else 'nullptr'
        ev_sym = 'kMsgs_{}_ev'.format(iface.name) if iface.events else 'nullptr'
        print('    {{{}, {}, {}, {}, {}, {}}},'.format(
            emit_cstring(iface.name), iface.version,
            len(iface.requests), req_sym, len(iface.events), ev_sym), file=out)
    print('};', file=out)
    print('static const size_t kInterfaceTableCount = {};'.format(len(interfaces)), file=out)
    print('', file=out)
    print('}  // namespace wire', file=out)
    print('', file=out)


def emit_wl_interfaces(out, interfaces):
    """wl_interface structs for non-core (extension) protocols only.

    Kept in a separate file from the wire tables because this one needs
    <wayland-client.h> for `struct wl_interface`/`struct wl_message`, and only
    proxy_server (the half that links libwayland-client) should pay for that;
    proxy_client decodes the wire format without ever touching libwayland.
    """
    ext = [i for i in interfaces.values() if i.name not in CORE_INTERFACES]
    if not ext:
        return

    print('// Extension protocol descriptors libwayland does not already', file=out)
    print('// export symbols for (core wayland.xml interfaces come from', file=out)
    print('// wayland-client-protocol.h instead).', file=out)
    for iface in ext:
        print('extern const struct wl_interface {}_interface;'.format(iface.name), file=out)
    print('', file=out)

    def types_symbol(iface, kind, msg):
        return 'kTypes_{}_{}_{}'.format(iface.name, kind, msg.name)

    def interface_ref(name):
        if name is None:
            return 'nullptr'
        return '&{}_interface'.format(name)

    any_types = False
    for iface in ext:
        for kind, msgs in (('req', iface.requests), ('ev', iface.events)):
            for msg in msgs:
                needs_types = any(a.wire_type in ('object', 'new_id') for a in msg.args)
                if not needs_types:
                    continue
                any_types = True
                print('static const struct wl_interface* {}[] = {{'.format(
                    types_symbol(iface, kind, msg)), file=out)
                for arg in msg.args:
                    if arg.wire_type in ('object', 'new_id'):
                        print('    {},'.format(interface_ref(arg.interface)), file=out)
                    else:
                        print('    nullptr,', file=out)
                print('};', file=out)
    if any_types:
        print('', file=out)

    for iface in ext:
        for kind, msgs in (('req', iface.requests), ('ev', iface.events)):
            if not msgs:
                continue
            sym = 'kWlMsgs_{}_{}'.format(iface.name, kind)
            print('static const struct wl_message {}[] = {{'.format(sym), file=out)
            for msg in msgs:
                sig = ''.join(ARG_TYPE_SIGCHAR[a.wire_type] for a in msg.args)
                needs_types = any(a.wire_type in ('object', 'new_id') for a in msg.args)
                types_ref = types_symbol(iface, kind, msg) if needs_types else 'nullptr'
                print('    {{{}, {}, {}}},'.format(emit_cstring(msg.name), emit_cstring(sig), types_ref), file=out)
            print('};', file=out)

    for iface in ext:
        req_sym = 'kWlMsgs_{}_req'.format(iface.name) if iface.requests else 'nullptr'
        ev_sym = 'kWlMsgs_{}_ev'.format(iface.name) if iface.events else 'nullptr'
        print('const struct wl_interface {}_interface = {{'.format(iface.name), file=out)
        print('    {}, {},'.format(emit_cstring(iface.name), iface.version), file=out)
        print('    {}, {},'.format(len(iface.requests), req_sym), file=out)
        print('    {}, {},'.format(len(iface.events), ev_sym), file=out)
        print('};', file=out)
    print('', file=out)

    # name -> descriptor lookup, so proxy_server can resolve wl_registry.bind
    # and any other new_id target by the interface name string on the wire.
    print('static const struct { const char* name; const struct wl_interface* iface; } '
          'kExtensionInterfaces[] = {', file=out)
    for iface in ext:
        print('    {{{}, &{}_interface}},'.format(emit_cstring(iface.name), iface.name), file=out)
    print('};', file=out)
    print('static const size_t kExtensionInterfaceCount = {};'.format(len(ext)), file=out)
    print('', file=out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wayland-xml', default='/usr/share/wayland/wayland.xml')
    parser.add_argument('--xdg-shell-xml',
                         default='/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
    parser.add_argument('--viewporter-xml',
                         default='/usr/share/wayland-protocols/stable/viewporter/viewporter.xml')
    parser.add_argument('--linux-dmabuf-xml',
                         default='/usr/share/wayland-protocols/unstable/linux-dmabuf/'
                                 'linux-dmabuf-unstable-v1.xml')
    parser.add_argument('--xdg-decoration-xml',
                         default='/usr/share/wayland-protocols/unstable/xdg-decoration/'
                                 'xdg-decoration-unstable-v1.xml')
    parser.add_argument('--wire-output', required=True,
                         help='wire-format tables, no libwayland dependency (used by both proxy halves)')
    parser.add_argument('--wl-interfaces-output', required=True,
                         help='struct wl_interface descriptors for extension protocols (proxy_server only)')
    args = parser.parse_args()

    paths = [args.wayland_xml, args.xdg_shell_xml]
    for optional in (args.viewporter_xml, args.linux_dmabuf_xml, args.xdg_decoration_xml):
        if optional and os.path.isfile(optional):
            paths.append(optional)
        elif optional:
            print('generate_protocol.py: skipping missing {}'.format(optional), file=sys.stderr)

    interfaces = parse_protocols(paths)

    with open(args.wire_output, 'w') as out:
        print('// Generated by generate_protocol.py. Do not edit.', file=out)
        print('#pragma once', file=out)
        print('', file=out)
        emit_wire_tables(out, interfaces)

    with open(args.wl_interfaces_output, 'w') as out:
        print('// Generated by generate_protocol.py. Do not edit.', file=out)
        print('#pragma once', file=out)
        print('', file=out)
        emit_wl_interfaces(out, interfaces)

    print('generate_protocol.py: {} interfaces from {}'.format(len(interfaces), ', '.join(paths)))


if __name__ == '__main__':
    main()
