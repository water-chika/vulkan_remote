#!/usr/bin/env python3
"""Keep Vulkan 1.0 capability claims aligned with implemented entry points."""

import argparse
import ctypes
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET


def core_commands(registry):
    root = ET.parse(registry).getroot()
    names = set()
    for feature in root.findall("feature"):
        if feature.get("number") != "1.0":
            continue
        if "vulkan" not in feature.get("api", "vulkan").split(","):
            continue
        for require in feature.findall("require"):
            names.update(command.get("name") for command in require.findall("command"))
    return names


def source_entries(source_dir):
    names = set()
    for path in glob.glob(os.path.join(source_dir, "client", "*.cpp")):
        text = open(path, encoding="utf-8").read()
        names.update("vk" + name for name in re.findall(r"\bD\((\w+)\)", text))
        names.update("vk" + name for name in re.findall(r"\bENTRY\((\w+)\)", text))
    return names


def server_handlers(source_dir):
    names = set()
    for path in glob.glob(os.path.join(source_dir, "server", "*.cpp")):
        text = open(path, encoding="utf-8").read()
        names.update(name for name in re.findall(r"REGISTER_HANDLER\(\s*(vk\w+)", text))
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", default=os.path.dirname(os.path.dirname(__file__)))
    parser.add_argument("--icd", help="built ICD for runtime proc-address verification")
    args = parser.parse_args()
    source = os.path.abspath(args.source_dir)
    core = core_commands(os.path.join(source, "registry", "vk.xml"))
    client = source_entries(source)
    server = server_handlers(source)
    missing_client = sorted(core - client)
    # Loader-owned proc-address commands do not need a server RPC. Instance and
    # layer enumeration are also answered locally by the ICD.
    local_only = {
        "vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
        "vkCreateInstance", "vkDestroyInstance",
        "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
        "vkEnumerateDeviceLayerProperties",
        # These are implemented by the client shadow-memory protocol rather than
        # one same-named Vulkan RPC.
        "vkMapMemory", "vkUnmapMemory", "vkFlushMappedMemoryRanges",
        "vkInvalidateMappedMemoryRanges",
        # A truthful local zero-count implementation while sparse features are
        # not yet part of the remoting contract.
        "vkGetPhysicalDeviceSparseImageFormatProperties",
    }
    missing_server = sorted(core - local_only - server)
    if len(core) != 137:
        print("FAIL: expected 137 Vulkan 1.0 commands, found {}".format(len(core)))
        return 1
    print("Vulkan 1.0 core: 137")
    print("client entries: {} / 137".format(len(core & client)))
    print("server handlers/local implementations: {} / 137".format(
        len(core) - len(missing_server)))
    print("missing client ({}): {}".format(len(missing_client), ", ".join(missing_client)))
    print("missing server ({}): {}".format(len(missing_server), ", ".join(missing_server)))
    # Phase-0 gate: every implemented client RPC has a handler, and the exact
    # gap is explicit. This becomes a 137/137 gate as command families land.
    dangling = sorted((client & core) - local_only - server)
    if dangling:
        print("FAIL: client exposes commands with no server handler: " + ", ".join(dangling))
        return 1
    expected_path = os.path.join(source, "tests", "core_1_0_expected_missing.txt")
    with open(expected_path, encoding="utf-8") as handle:
        expected_missing = sorted(line.strip() for line in handle
                                  if line.strip() and not line.startswith("#"))
    if missing_client != expected_missing:
        print("FAIL: Vulkan 1.0 client gap differs from reviewed inventory")
        print("  added to gap: " + ", ".join(sorted(set(missing_client) - set(expected_missing))))
        print("  removed from gap: " + ", ".join(sorted(set(expected_missing) - set(missing_client))))
        return 1
    if missing_server != expected_missing:
        print("FAIL: Vulkan 1.0 server gap differs from client/reviewed inventory")
        print("  server-only: " + ", ".join(sorted(set(missing_client) - set(missing_server))))
        print("  client-only: " + ", ".join(sorted(set(missing_server) - set(missing_client))))
        return 1
    if args.icd:
        library = ctypes.CDLL(os.path.abspath(args.icd))
        get_proc = library.vk_icdGetInstanceProcAddr
        get_proc.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        get_proc.restype = ctypes.c_void_p
        runtime_exposed = {name for name in core if get_proc(None, name.encode())}
        runtime_missing = sorted(core - runtime_exposed)
        if runtime_missing != expected_missing:
            print("FAIL: compiled ICD proc-address gap differs from reviewed inventory")
            print("  runtime gap: " + ", ".join(runtime_missing))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
