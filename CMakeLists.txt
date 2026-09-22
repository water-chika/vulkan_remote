cmake_minimum_required(VERSION 3.21)

# Vulkan remoting: execute Vulkan calls on a machine that owns a GPU, from a
# machine that does not.
#
# Three pieces, deliberately separated because they run in different places:
#   common/  wire format and generated opcodes, shared by both ends
#   server/  runs where the GPU is, links a real Vulkan loader
#   client/  runs where the GPU is not, and IS a Vulkan ICD
#   tools/   a direct client used to measure the link
project(vulkan_remoting CXX)

find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Both peers must be generated from one registry or their opcode numbering
# disagrees. The generated digest makes a mismatch fail at connect time
# instead of silently invoking the wrong command.
if(NOT DEFINED vulkan_registry_xml)
    find_file(vulkan_registry_xml
        NAMES vk.xml
        PATHS
            /usr/share/vulkan/registry
            ${Vulkan_INCLUDE_DIR}/../share/vulkan/registry
        DOC "Vulkan registry vk.xml used to generate wire opcodes"
    )
endif()
if(NOT vulkan_registry_xml)
    message(FATAL_ERROR "vk.xml not found; pass -Dvulkan_registry_xml=/path/to/vk.xml")
endif()
message(STATUS "vulkan_remoting: registry ${vulkan_registry_xml}")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/remoting_commands.inl
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/common/generate_remoting.py
            ${vulkan_registry_xml}
            --output ${CMAKE_CURRENT_BINARY_DIR}/remoting_commands.inl
    DEPENDS ${vulkan_registry_xml} ${CMAKE_CURRENT_SOURCE_DIR}/common/generate_remoting.py
    COMMENT "Generating Vulkan remoting opcodes"
)

add_library(vulkan_remoting_wire STATIC
    common/wire.cpp
    common/wire.hpp
    common/marshal.cpp
    common/marshal.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/remoting_commands.inl
)
target_include_directories(vulkan_remoting_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_features(vulkan_remoting_wire PUBLIC cxx_std_17)
set_target_properties(vulkan_remoting_wire PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(vulkan_remoting_server
    server/main.cpp
    server/session.cpp
    server/handlers_instance.cpp
    server/handlers_device.cpp
    server/handlers_memory.cpp
    server/handlers_command.cpp
    server/handlers_wsi.cpp
    server/own_window.cpp
)
target_link_libraries(vulkan_remoting_server PRIVATE vulkan_remoting_wire Vulkan::Vulkan)

# The client links Vulkan::Headers, never the loader: it is the driver the
# loader talks to, so linking the loader would be circular.
add_library(vulkan_remoting_icd SHARED
    client/icd.cpp
    client/physical_device.cpp
    client/device.cpp
    client/memory.cpp
    client/commands.cpp
    client/wsi.cpp
    client/entry_table.cpp
    client/remote_objects.cpp
)
target_link_libraries(vulkan_remoting_icd PRIVATE vulkan_remoting_wire Vulkan::Headers)
if(WIN32)
    # The client speaks Winsock instead of BSD sockets on Windows (see
    # common/wire.cpp); ws2_32 is where WSAStartup/getaddrinfo/socket/etc.
    # live there.
    target_link_libraries(vulkan_remoting_icd PRIVATE ws2_32)
    # VK_USE_PLATFORM_WIN32_KHR itself is not defined here: client/icd.cpp and
    # client/wsi.cpp already define it themselves under #if defined(_WIN32),
    # the same way they define VK_USE_PLATFORM_WAYLAND_KHR for a Linux build,
    # so doing it again from CMake would just be a duplicate-definition
    # warning waiting to happen.

    # Static runtime, deliberately. A GCC-built DLL imports libstdc++-6.dll,
    # libgcc_s_seh-1.dll and libwinpthread-1.dll, none of which ship with
    # Windows; without them LoadLibrary fails with error 126, which names
    # only the module it was asked for and never the dependency that was
    # actually missing. That cost a tester a debugging session already.
    #
    # Shipping those three DLLs alongside is the other option and is worse
    # here: an ICD is loaded into somebody else's process, so the search
    # happens on the *host* program's terms, and a host that already loaded
    # a different libstdc++-6.dll gives an ABI mismatch instead of an honest
    # failure. 2.7 MB against 438 KB buys a DLL that cannot fail this way.
    # What remains imported is the UCRT (api-ms-win-crt-*), which is part of
    # the OS from Windows 10 on.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(vulkan_remoting_icd PRIVATE -static)
    endif()
endif()

# The probe links no Vulkan at all, which keeps it honest about what actually
# has to cross the socket.
add_executable(vulkan_remoting_probe tools/probe.cpp)
target_link_libraries(vulkan_remoting_probe PRIVATE vulkan_remoting_wire)

# Manifest telling the Vulkan loader where the ICD is. Written to the build
# directory because it must carry an absolute path to the built library.
# $<TARGET_FILE:...> already resolves to a .dll here on a Windows configure,
# so the JSON itself needs no platform-specific handling; what differs on
# Windows is how the loader is told to look at it at all. On Linux the
# loader finds this file through VK_ICD_FILENAMES or the standard
# /usr/share/vulkan/icd.d search path, but on Windows it instead reads a
# REG_DWORD value (data 0) named after this JSON's path under the registry key
# HKLM\SOFTWARE\Khronos\Vulkan\Drivers (or the HKCU equivalent) - creating
# that value is a one-time step for whoever installs this driver, and CMake
# has no business writing to the registry to do it.
set(icd_library $<TARGET_FILE:vulkan_remoting_icd>)
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vulkan_remoting_icd.json
    CONTENT "{
    \"file_format_version\": \"1.0.0\",
    \"ICD\": {
        \"library_path\": \"${icd_library}\",
        \"api_version\": \"1.0.0\"
    }
}
"
)

add_executable(vulkan_remoting_offscreen tools/offscreen.cpp)
target_link_libraries(vulkan_remoting_offscreen PRIVATE Vulkan::Vulkan)

# wayland/: a waypipe-style protocol proxy, split so the replay half lives
# inside whichever process on machine B also owns the Vulkan driver (see
# wayland/proxy_server.hpp for why that placement is load-bearing, not
# incidental). Kept independent of the Vulkan remoting pieces above other
# than sharing the project's C++17 baseline.
#
# Everything below is Linux-only: wayland-client, the proxy, and the
# server's own-window path only ever make sense on the machine that owns a
# real compositor connection, which per this project's split (see the
# project() comment at the top) is the server side. A Windows configure
# builds only the client half (vulkan_remoting_icd), so none of this needs to
# exist there, and pkg_check_modules(... REQUIRED wayland-client) would fail
# a Windows configure outright if it were not skipped.
if(NOT WIN32)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

if(NOT DEFINED wayland_xml)
    find_file(wayland_xml NAMES wayland.xml PATHS /usr/share/wayland)
endif()
if(NOT DEFINED wayland_xdg_shell_xml)
    find_file(wayland_xdg_shell_xml NAMES xdg-shell.xml
        PATHS /usr/share/wayland-protocols/stable/xdg-shell)
endif()
if(NOT wayland_xml OR NOT wayland_xdg_shell_xml)
    message(FATAL_ERROR "wayland.xml / xdg-shell.xml not found; pass -Dwayland_xml= and "
                         "-Dwayland_xdg_shell_xml=")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/protocol_tables.inl
           ${CMAKE_CURRENT_BINARY_DIR}/wl_protocol_interfaces.inl
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/wayland/generate_protocol.py
            --wayland-xml ${wayland_xml}
            --xdg-shell-xml ${wayland_xdg_shell_xml}
            --wire-output ${CMAKE_CURRENT_BINARY_DIR}/protocol_tables.inl
            --wl-interfaces-output ${CMAKE_CURRENT_BINARY_DIR}/wl_protocol_interfaces.inl
    DEPENDS ${wayland_xml} ${wayland_xdg_shell_xml} ${CMAKE_CURRENT_SOURCE_DIR}/wayland/generate_protocol.py
    COMMENT "Generating Wayland protocol tables"
)
add_custom_target(wayland_protocol_tables DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/protocol_tables.inl
    ${CMAKE_CURRENT_BINARY_DIR}/wl_protocol_interfaces.inl
)

# No libwayland dependency on purpose: proxy_client (machine A) only ever
# decodes/re-encodes bytes, so it should not need to link the library that
# only makes sense where a real compositor connection exists.
add_library(vulkan_remoting_wayland_wire STATIC wayland/wire.cpp wayland/wire.hpp)
add_dependencies(vulkan_remoting_wayland_wire wayland_protocol_tables)
target_include_directories(vulkan_remoting_wayland_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/wayland
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_features(vulkan_remoting_wayland_wire PUBLIC cxx_std_17)

add_executable(vulkan_remoting_wayland_proxy wayland/proxy_client.cpp)
target_link_libraries(vulkan_remoting_wayland_proxy PRIVATE vulkan_remoting_wayland_wire)

add_library(vulkan_remoting_wayland_proxy_server STATIC
    wayland/proxy_server.hpp
    wayland/proxy_server.cpp
)
add_dependencies(vulkan_remoting_wayland_proxy_server wayland_protocol_tables)
target_include_directories(vulkan_remoting_wayland_proxy_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/wayland
    ${CMAKE_CURRENT_BINARY_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_link_libraries(vulkan_remoting_wayland_proxy_server PUBLIC
    vulkan_remoting_wayland_wire
    ${WAYLAND_CLIENT_LIBRARIES}
)
target_compile_features(vulkan_remoting_wayland_proxy_server PUBLIC cxx_std_17)

# The server embeds WaylandProxy directly (see server/server.cpp's --wayland
# flag) and pumps it on its own thread, so it needs both the proxy library
# and threading support that the earlier vulkan_remoting_server target
# definition (above) did not yet have available.
find_package(Threads REQUIRED)
target_link_libraries(vulkan_remoting_server PRIVATE
    vulkan_remoting_wayland_proxy_server
    Threads::Threads
)

# The client-side ICD only needs wl_proxy_get_id to name the app's surface
# (see client/icd.cpp); it never opens its own Wayland connection.
target_link_libraries(vulkan_remoting_icd PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
target_include_directories(vulkan_remoting_icd PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})

add_executable(vulkan_remoting_wayland_test_harness wayland/test_harness.cpp)
target_link_libraries(vulkan_remoting_wayland_test_harness PRIVATE
    vulkan_remoting_wayland_proxy_server
    ${WAYLAND_CLIENT_LIBRARIES}
)

# xdg-shell client bindings for the throwaway verification client only — the
# proxy itself never depends on scanner output (see generate_protocol.py).
enable_language(C)  # wayland-scanner's generated code relies on C's default
                     # external linkage for const globals; C++ would make
                     # them internal-linkage and break cross-file symbols.
find_program(wayland_scanner_exe NAMES wayland-scanner)
if(NOT wayland_scanner_exe)
    message(FATAL_ERROR "wayland-scanner not found, needed for wayland/test_client.cpp")
endif()
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
    COMMAND ${wayland_scanner_exe} client-header ${wayland_xdg_shell_xml}
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
    DEPENDS ${wayland_xdg_shell_xml}
)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
    COMMAND ${wayland_scanner_exe} private-code ${wayland_xdg_shell_xml}
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
    DEPENDS ${wayland_xdg_shell_xml}
)

add_executable(vulkan_remoting_wayland_test_client
    wayland/test_client.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
)
target_include_directories(vulkan_remoting_wayland_test_client PRIVATE
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(vulkan_remoting_wayland_test_client PRIVATE ${WAYLAND_CLIENT_LIBRARIES})

# server/own_window.cpp needs the same xdg-shell client bindings as the
# throwaway verification client above (server/main.cpp's --wayland proxy path
# never depends on scanner output, but this server-owned-window path is a
# real libwayland client and does). Only the generated header is compiled
# in here, never xdg-shell-protocol.c: that .c file's xdg_*_interface
# globals would collide at link time with the equivalent globals
# vulkan_remoting_wayland_proxy_server already defines from the very same
# xdg-shell.xml (see wayland/generate_protocol.py), so own_window.cpp's
# calls resolve against that library's copies instead of a second one.
target_sources(vulkan_remoting_server PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
)
target_include_directories(vulkan_remoting_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()  # NOT WIN32
