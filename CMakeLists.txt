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
    ${CMAKE_CURRENT_BINARY_DIR}/remoting_commands.inl
)
target_include_directories(vulkan_remoting_wire PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_features(vulkan_remoting_wire PUBLIC cxx_std_17)
set_target_properties(vulkan_remoting_wire PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(vulkan_remoting_server server/server.cpp)
target_link_libraries(vulkan_remoting_server PRIVATE vulkan_remoting_wire Vulkan::Vulkan)

# The client links Vulkan::Headers, never the loader: it is the driver the
# loader talks to, so linking the loader would be circular.
add_library(vulkan_remoting_icd SHARED client/icd.cpp)
target_link_libraries(vulkan_remoting_icd PRIVATE vulkan_remoting_wire Vulkan::Headers)

# The probe links no Vulkan at all, which keeps it honest about what actually
# has to cross the socket.
add_executable(vulkan_remoting_probe tools/probe.cpp)
target_link_libraries(vulkan_remoting_probe PRIVATE vulkan_remoting_wire)

# Manifest telling the Vulkan loader where the ICD is. Written to the build
# directory because it must carry an absolute path to the built library.
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
