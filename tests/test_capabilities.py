#!/usr/bin/env python3
"""Runtime capability checks for the built remoting ICD."""

import argparse
import ctypes
import os
import sys


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
    args = parser.parse_args()
    library = ctypes.CDLL(os.path.abspath(args.icd))

    get_proc = library.vk_icdGetInstanceProcAddr
    get_proc.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    get_proc.restype = ctypes.c_void_p

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
    check("VK_KHR_get_physical_device_properties2" not in names,
          "unimplemented properties2 extension is advertised")

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

    print("capability claims: runtime Vulkan 1.0 phase-0 gate passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print("FAIL: {}".format(error))
        sys.exit(1)
