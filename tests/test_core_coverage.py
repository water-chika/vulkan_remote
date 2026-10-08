#!/usr/bin/env python3
"""Keep Vulkan 1.0 capability claims aligned with implemented entry points."""

import argparse
import ctypes
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET


def core_commands(registry, version="1.0"):
    root = ET.parse(registry).getroot()
    names = set()
    for feature in root.findall("feature"):
        if feature.get("number") != version:
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
    registry = os.path.join(source, "registry", "vk.xml")
    core = core_commands(registry)
    core11 = core_commands(registry, "1.1")
    core12 = core_commands(registry, "1.2")
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
    }
    missing_server = sorted(core - local_only - server)
    if len(core) != 137:
        print("FAIL: expected 137 Vulkan 1.0 commands, found {}".format(len(core)))
        return 1
    if len(core11) != 28:
        print("FAIL: expected 28 Vulkan 1.1 commands, found {}".format(len(core11)))
        return 1
    if len(core12) != 13:
        print("FAIL: expected 13 Vulkan 1.2 commands, found {}".format(len(core12)))
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
    local11 = {"vkEnumerateInstanceVersion", "vkEnumeratePhysicalDeviceGroups",
               "vkGetPhysicalDeviceExternalBufferProperties",
               "vkGetPhysicalDeviceExternalFenceProperties",
               "vkGetPhysicalDeviceExternalSemaphoreProperties"}
    missing11_client = sorted(core11 - client)
    missing11_server = sorted(core11 - local11 - server)
    print("Vulkan 1.1 core: 28")
    print("client entries: {} / 28".format(len(core11 & client)))
    print("server handlers/local implementations: {} / 28".format(28 - len(missing11_server)))
    if missing11_client or missing11_server:
        print("FAIL: Vulkan 1.1 client gap: " + ", ".join(missing11_client))
        print("FAIL: Vulkan 1.1 server gap: " + ", ".join(missing11_server))
        return 1
    missing12_client = sorted(core12 - client)
    missing12_server = sorted(core12 - server)
    print("Vulkan 1.2 core: 13")
    print("client entries: {} / 13".format(len(core12 & client)))
    print("server handlers/local implementations: {} / 13".format(13 - len(missing12_server)))
    if missing12_client or missing12_server:
        print("FAIL: Vulkan 1.2 client gap: " + ", ".join(missing12_client))
        print("FAIL: Vulkan 1.2 server gap: " + ", ".join(missing12_server))
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
        runtime11_missing = sorted(name for name in core11 if not get_proc(None, name.encode()))
        if runtime11_missing:
            print("FAIL: compiled ICD Vulkan 1.1 gap: " + ", ".join(runtime11_missing))
            return 1
        runtime12_missing = sorted(name for name in core12 if not get_proc(None, name.encode()))
        if runtime12_missing:
            print("FAIL: compiled ICD Vulkan 1.2 gap: " + ", ".join(runtime12_missing))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
