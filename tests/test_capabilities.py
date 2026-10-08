#!/usr/bin/env python3
"""Runtime capability checks for the built remoting ICD."""

import argparse
import ctypes
import os
import re
import sys
import xml.etree.ElementTree as ET


VK_SUCCESS = 0
VK_ERROR_LAYER_NOT_PRESENT = -6
VK_ERROR_EXTENSION_NOT_PRESENT = -7
VK_ERROR_FEATURE_NOT_PRESENT = -8
VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3
VK_MAX_EXTENSION_NAME_SIZE = 256


class VkExtensionProperties(ctypes.Structure):
    _fields_ = [("extensionName", ctypes.c_char * VK_MAX_EXTENSION_NAME_SIZE),
                ("specVersion", ctypes.c_uint32)]


class VkApplicationInfo(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int32), ("pNext", ctypes.c_void_p),
                ("pApplicationName", ctypes.c_char_p), ("applicationVersion", ctypes.c_uint32),
                ("pEngineName", ctypes.c_char_p), ("engineVersion", ctypes.c_uint32),
                ("apiVersion", ctypes.c_uint32)]


class VkInstanceCreateInfo(ctypes.Structure):
    _fields_ = [("sType", ctypes.c_int32), ("pNext", ctypes.c_void_p),
                ("flags", ctypes.c_uint32), ("pApplicationInfo", ctypes.POINTER(VkApplicationInfo)),
                ("enabledLayerCount", ctypes.c_uint32),
                ("ppEnabledLayerNames", ctypes.POINTER(ctypes.c_char_p)),
                ("enabledExtensionCount", ctypes.c_uint32),
                ("ppEnabledExtensionNames", ctypes.POINTER(ctypes.c_char_p))]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--icd", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()
    library = ctypes.CDLL(os.path.abspath(args.icd))

    get_proc = library.vk_icdGetInstanceProcAddr
    get_proc.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    get_proc.restype = ctypes.c_void_p

    enumerate_version_pointer = get_proc(None, b"vkEnumerateInstanceVersion")
    check(enumerate_version_pointer, "vkEnumerateInstanceVersion is not exposed")
    enumerate_version = ctypes.CFUNCTYPE(ctypes.c_int32,
                                         ctypes.POINTER(ctypes.c_uint32))(enumerate_version_pointer)
    api_version = ctypes.c_uint32()
    check(enumerate_version(ctypes.byref(api_version)) == VK_SUCCESS and
          api_version.value == ((1 << 22) | (2 << 12)),
          "instance version must be Vulkan 1.2")

    enumerate_type = ctypes.CFUNCTYPE(
        ctypes.c_int32, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(VkExtensionProperties))
    enumerate_pointer = get_proc(None, b"vkEnumerateInstanceExtensionProperties")
    check(enumerate_pointer, "vkEnumerateInstanceExtensionProperties is not exposed")
    enumerate_extensions = enumerate_type(enumerate_pointer)
    count = ctypes.c_uint32()
    check(enumerate_extensions(None, ctypes.byref(count), None) == VK_SUCCESS,
          "instance extension count failed")
    props = (VkExtensionProperties * count.value)()
    check(enumerate_extensions(None, ctypes.byref(count), props) == VK_SUCCESS,
          "instance extension enumeration failed")
    names = {bytes(item.extensionName).split(b"\0", 1)[0].decode() for item in props}
    check("VK_KHR_get_physical_device_properties2" in names,
          "complete properties2 extension is not advertised")

    source_dir = os.path.abspath(args.source_dir)
    contract_path = os.path.join(source_dir, "common", "extension_contract.hpp")
    with open(contract_path, encoding="utf-8") as handle:
        contract_source = handle.read()
    contract_macros = re.findall(r"\{(VK_KHR_[A-Z0-9_]+_EXTENSION_NAME),", contract_source)
    macro_to_name = {}
    for header in ("vulkan_core.h", "vulkan.h"):
        header_path = os.path.join("/usr/include/vulkan", header)
        if os.path.exists(header_path):
            with open(header_path, encoding="utf-8") as handle:
                for macro, value in re.findall(
                        r'#define\s+(VK_KHR_[A-Z0-9_]+_EXTENSION_NAME)\s+"([^"]+)"',
                        handle.read()):
                    macro_to_name[macro] = value
    contract_names = [macro_to_name[macro] for macro in contract_macros]
    expected = [
        "VK_KHR_multiview", "VK_KHR_maintenance2", "VK_KHR_image_format_list",
        "VK_KHR_create_renderpass2", "VK_KHR_fragment_shading_rate",
        "VK_KHR_imageless_framebuffer",
    ]
    check(contract_names == expected, "reviewed device extension inventory drifted")
    check("VK_KHR_dynamic_rendering" not in contract_source,
          "dynamic rendering entered the reviewed contract")

    registry = ET.parse(os.path.join(source_dir, "registry", "vk.xml")).getroot()
    registry_names = {extension.get("name") for extension in registry.findall("./extensions/extension")}
    check(set(contract_names) <= registry_names, "contract contains a non-registry extension")
    command_nodes = registry.findall("./commands/command")
    aliases = {command.get("name"): command.get("alias") for command in command_nodes
               if command.get("alias")}

    renderpass2 = registry.find("./extensions/extension[@name='VK_KHR_create_renderpass2']")
    renderpass_commands = {command.get("name") for command in renderpass2.findall("./require/command")}
    check(renderpass_commands == {
        "vkCreateRenderPass2KHR", "vkCmdBeginRenderPass2KHR",
        "vkCmdNextSubpass2KHR", "vkCmdEndRenderPass2KHR"},
        "registry create-renderpass2 family changed")
    renderpass_aliases = {name: aliases.get(name) for name in renderpass_commands}
    check(renderpass_aliases == {
        "vkCreateRenderPass2KHR": "vkCreateRenderPass2",
        "vkCmdBeginRenderPass2KHR": "vkCmdBeginRenderPass2",
        "vkCmdNextSubpass2KHR": "vkCmdNextSubpass2",
        "vkCmdEndRenderPass2KHR": "vkCmdEndRenderPass2"},
        "registry create-renderpass2 aliases changed")
    fsr = registry.find("./extensions/extension[@name='VK_KHR_fragment_shading_rate']")
    fsr_commands = {command.get("name") for command in fsr.findall("./require/command")}
    check(fsr_commands == {"vkGetPhysicalDeviceFragmentShadingRatesKHR",
                           "vkCmdSetFragmentShadingRateKHR"},
          "registry fragment-shading-rate family changed")

    entry_sources = "\n".join(
        open(os.path.join(source_dir, path), encoding="utf-8").read()
        for path in ("client/device.cpp", "client/commands.cpp", "client/physical_device.cpp"))
    for command in renderpass_commands | fsr_commands:
        check(command in entry_sources, "{} is missing from client entry tables".format(command))
    for alias in renderpass_aliases.values():
        check(alias in entry_sources, "{} core alias is missing from client entry tables".format(alias))
    check("vkCmdBeginRenderingKHR" not in entry_sources and
          "vkCmdEndRenderingKHR" not in entry_sources,
          "dynamic-rendering entry points entered the client tables")

    layers = ctypes.c_uint32(1)
    create_type = ctypes.CFUNCTYPE(
        ctypes.c_int32, ctypes.POINTER(VkInstanceCreateInfo), ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p))
    create_pointer = get_proc(None, b"vkCreateInstance")
    check(create_pointer, "vkCreateInstance is not exposed")
    create = create_type(create_pointer)
    app = VkApplicationInfo(VK_STRUCTURE_TYPE_APPLICATION_INFO, None, b"capability-test", 0,
                            b"capability-test", 0, 1 << 22)
    layer_names = (ctypes.c_char_p * 1)(b"VK_LAYER_DOES_NOT_EXIST")
    info = VkInstanceCreateInfo(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, None, 0,
                                ctypes.pointer(app), 1, layer_names, 0, None)
    instance = ctypes.c_void_p()
    check(create(ctypes.byref(info), None, ctypes.byref(instance)) == VK_ERROR_LAYER_NOT_PRESENT,
          "unknown instance layer was not rejected before connection")
    extension_names = (ctypes.c_char_p * 1)(b"VK_EXT_does_not_exist")
    info.enabledLayerCount = 0
    info.ppEnabledLayerNames = None
    info.enabledExtensionCount = 1
    info.ppEnabledExtensionNames = extension_names
    check(create(ctypes.byref(info), None, ctypes.byref(instance)) ==
          VK_ERROR_EXTENSION_NOT_PRESENT,
          "unknown instance extension was not rejected before connection")

    properties2_commands = [
        b"vkGetPhysicalDeviceFeatures2", b"vkGetPhysicalDeviceFeatures2KHR",
        b"vkGetPhysicalDeviceProperties2", b"vkGetPhysicalDeviceProperties2KHR",
        b"vkGetPhysicalDeviceFormatProperties2", b"vkGetPhysicalDeviceFormatProperties2KHR",
        b"vkGetPhysicalDeviceImageFormatProperties2", b"vkGetPhysicalDeviceImageFormatProperties2KHR",
        b"vkGetPhysicalDeviceQueueFamilyProperties2", b"vkGetPhysicalDeviceQueueFamilyProperties2KHR",
        b"vkGetPhysicalDeviceMemoryProperties2", b"vkGetPhysicalDeviceMemoryProperties2KHR",
        b"vkGetPhysicalDeviceSparseImageFormatProperties2",
        b"vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
    ]
    for command in properties2_commands:
        check(get_proc(None, command), "{} is not exported".format(command.decode()))

    manifest_path = os.path.join(os.path.dirname(os.path.abspath(args.icd)),
                                 "vulkan_remoting_icd.json")
    if os.path.exists(manifest_path):
        import json
        with open(manifest_path, encoding="utf-8") as handle:
            manifest = json.load(handle)
        check(manifest["ICD"]["api_version"] == "1.2.0",
              "generated manifest must advertise Vulkan 1.2")

    promoted_aliases = {
        b"vkBindBufferMemory2": b"vkBindBufferMemory2KHR",
        b"vkBindImageMemory2": b"vkBindImageMemory2KHR",
        b"vkGetBufferMemoryRequirements2": b"vkGetBufferMemoryRequirements2KHR",
        b"vkGetImageMemoryRequirements2": b"vkGetImageMemoryRequirements2KHR",
        b"vkGetImageSparseMemoryRequirements2": b"vkGetImageSparseMemoryRequirements2KHR",
        b"vkGetDeviceGroupPeerMemoryFeatures": b"vkGetDeviceGroupPeerMemoryFeaturesKHR",
        b"vkCmdSetDeviceMask": b"vkCmdSetDeviceMaskKHR",
        b"vkCmdDispatchBase": b"vkCmdDispatchBaseKHR",
        b"vkTrimCommandPool": b"vkTrimCommandPoolKHR",
        b"vkCreateDescriptorUpdateTemplate": b"vkCreateDescriptorUpdateTemplateKHR",
        b"vkDestroyDescriptorUpdateTemplate": b"vkDestroyDescriptorUpdateTemplateKHR",
        b"vkUpdateDescriptorSetWithTemplate": b"vkUpdateDescriptorSetWithTemplateKHR",
        b"vkGetDescriptorSetLayoutSupport": b"vkGetDescriptorSetLayoutSupportKHR",
        b"vkCreateSamplerYcbcrConversion": b"vkCreateSamplerYcbcrConversionKHR",
        b"vkDestroySamplerYcbcrConversion": b"vkDestroySamplerYcbcrConversionKHR",
    }
    for core_name, alias_name in promoted_aliases.items():
        core_proc, alias_proc = get_proc(None, core_name), get_proc(None, alias_name)
        check(core_proc and alias_proc and core_proc == alias_proc,
              "{} does not share its promoted alias".format(core_name.decode()))

    promoted12_aliases = {
        b"vkCreateRenderPass2": b"vkCreateRenderPass2KHR",
        b"vkCmdBeginRenderPass2": b"vkCmdBeginRenderPass2KHR",
        b"vkCmdNextSubpass2": b"vkCmdNextSubpass2KHR",
        b"vkCmdEndRenderPass2": b"vkCmdEndRenderPass2KHR",
        b"vkGetSemaphoreCounterValue": b"vkGetSemaphoreCounterValueKHR",
        b"vkWaitSemaphores": b"vkWaitSemaphoresKHR",
        b"vkSignalSemaphore": b"vkSignalSemaphoreKHR",
        b"vkGetBufferDeviceAddress": b"vkGetBufferDeviceAddressKHR",
        b"vkGetBufferOpaqueCaptureAddress": b"vkGetBufferOpaqueCaptureAddressKHR",
        b"vkGetDeviceMemoryOpaqueCaptureAddress": b"vkGetDeviceMemoryOpaqueCaptureAddressKHR",
        b"vkCmdDrawIndirectCount": b"vkCmdDrawIndirectCountKHR",
        b"vkCmdDrawIndexedIndirectCount": b"vkCmdDrawIndexedIndirectCountKHR",
    }
    for core_name, alias_name in promoted12_aliases.items():
        core_proc, alias_proc = get_proc(None, core_name), get_proc(None, alias_name)
        check(core_proc and alias_proc and core_proc == alias_proc,
              "{} does not share its Vulkan 1.2 alias".format(core_name.decode()))

    print("capability claims: registry/source/runtime Vulkan 1.2 contract passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print("FAIL: {}".format(error))
        sys.exit(1)
