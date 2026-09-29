cmake_minimum_required(VERSION 3.25)

project(Ultraviolent
    VERSION 0.0.0
    DESCRIPTION "Clean-room virtual hardware platform for SGI-class systems"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Ccache)
include(Warnings)
include(Sanitizers)

add_library(ultraviolent_core
    src/core/address_space.cpp
    src/core/interrupt.cpp
    src/core/invariant.cpp
    src/core/scheduler.cpp
    src/core/state_image.cpp
    src/core/trace.cpp
    src/core/virtual_time.cpp
)

target_include_directories(ultraviolent_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

ultraviolent_enable_warnings(ultraviolent_core)
ultraviolent_enable_sanitizers(ultraviolent_core)

add_library(ultraviolent_mips
    src/arch/mips/cache_arrays.cpp
    src/arch/mips/cpu.cpp
    src/arch/mips/fpu.cpp
    src/arch/mips/interpreter.cpp
    src/arch/mips/semantics.cpp
    src/arch/mips/tlb.cpp
)

target_link_libraries(ultraviolent_mips PUBLIC ultraviolent_core)
ultraviolent_enable_warnings(ultraviolent_mips)
ultraviolent_enable_sanitizers(ultraviolent_mips)

# FPU arithmetic runs under the guest rounding mode and reads host IEEE exception flags, so
# the compiler must not assume the default floating-point environment or contract a*b+c.
set_source_files_properties(src/arch/mips/fpu.cpp
    PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-contract=off"
)

add_library(ultraviolent_devices
    src/devices/am29f080.cpp
    src/devices/dp83840.cpp
    src/devices/ioc3.cpp
    src/devices/ioc3_ethernet.cpp
    src/devices/isp1020.cpp
    src/devices/m48t35.cpp
    src/pci/pci.cpp
    src/devices/bridge.cpp
    src/devices/elsc.cpp
    src/devices/i2c.cpp
    src/devices/i2c_eeprom.cpp
    src/devices/one_wire.cpp
    src/devices/pcf8584.cpp
    src/devices/uart16550.cpp
    src/devices/xbow.cpp
    src/scsi/cdrom.cpp
    src/scsi/disk.cpp
)

target_link_libraries(ultraviolent_devices PUBLIC ultraviolent_core)
ultraviolent_enable_warnings(ultraviolent_devices)
ultraviolent_enable_sanitizers(ultraviolent_devices)

add_library(ultraviolent_ip27
    src/machines/ip27/directory_memory.cpp
    src/machines/ip27/hub.cpp
    src/machines/ip27/ip27_machine.cpp
    src/machines/ip27/prom_image.cpp
)

target_link_libraries(ultraviolent_ip27 PUBLIC ultraviolent_mips ultraviolent_devices)
ultraviolent_enable_warnings(ultraviolent_ip27)
ultraviolent_enable_sanitizers(ultraviolent_ip27)

add_library(ultraviolent_backends
    src/backends/console.cpp
    src/backends/file_block_store.cpp
    src/backends/stream_serial_sink.cpp
    src/backends/stream_trace_sink.cpp
    src/backends/pcap_link.cpp
    src/backends/tap_link.cpp
    src/backends/terminal_input.cpp
)

target_link_libraries(ultraviolent_backends PUBLIC ultraviolent_core ultraviolent_devices)
ultraviolent_enable_warnings(ultraviolent_backends)
ultraviolent_enable_sanitizers(ultraviolent_backends)

add_executable(ultraviolent
    src/main.cpp
)

target_link_libraries(ultraviolent PRIVATE ultraviolent_ip27 ultraviolent_backends)
ultraviolent_enable_warnings(ultraviolent)
ultraviolent_enable_sanitizers(ultraviolent)

include(CTest)

if(BUILD_TESTING)
    add_library(ultraviolent_test_support STATIC
        tests/support/test.cpp
    )
    target_include_directories(ultraviolent_test_support
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    ultraviolent_enable_warnings(ultraviolent_test_support)
    ultraviolent_enable_sanitizers(ultraviolent_test_support)

    add_executable(core_tests
        tests/core/address_space_test.cpp
        tests/core/address_test.cpp
        tests/core/byte_order_test.cpp
        tests/core/interrupt_test.cpp
        tests/core/scheduler_test.cpp
        tests/core/state_image_test.cpp
        tests/core/trace_test.cpp
        tests/core/virtual_time_test.cpp
    )
    target_link_libraries(core_tests PRIVATE ultraviolent_core ultraviolent_test_support)
    ultraviolent_enable_warnings(core_tests)
    ultraviolent_enable_sanitizers(core_tests)
    add_test(NAME core COMMAND core_tests)

    add_executable(mips_tests
        tests/arch/mips/boundary_test.cpp
        tests/arch/mips/cache_test.cpp
        tests/arch/mips/differential_test.cpp
        tests/arch/mips/integer_test.cpp
        tests/arch/mips/memory_test.cpp
        tests/arch/mips/host_page_test.cpp
        tests/arch/mips/exception_test.cpp
        tests/arch/mips/fpu_test.cpp
        tests/arch/mips/tlb_test.cpp
    )
    target_link_libraries(mips_tests PRIVATE ultraviolent_mips ultraviolent_test_support)
    ultraviolent_enable_warnings(mips_tests)
    ultraviolent_enable_sanitizers(mips_tests)
    add_test(NAME mips COMMAND mips_tests)

    add_executable(device_tests
        tests/devices/am29f080_test.cpp
        tests/devices/bridge_test.cpp
        tests/devices/uart_test.cpp
        tests/devices/elsc_test.cpp
        tests/devices/i2c_eeprom_test.cpp
        tests/devices/ioc3_test.cpp
        tests/devices/ioc3_ethernet_test.cpp
        tests/devices/isp1020_test.cpp
        tests/devices/m48t35_test.cpp
        tests/devices/one_wire_test.cpp
        tests/devices/pcf8584_test.cpp
        tests/devices/xbow_test.cpp
        tests/scsi/scsi_test.cpp
        tests/backends/console_test.cpp
        tests/backends/terminal_input_test.cpp
    )
    target_link_libraries(device_tests PRIVATE ultraviolent_devices ultraviolent_backends
                                               ultraviolent_test_support)
    ultraviolent_enable_warnings(device_tests)
    ultraviolent_enable_sanitizers(device_tests)
    add_test(NAME devices COMMAND device_tests)

    add_executable(machine_tests
        tests/machines/directory_memory_test.cpp
        tests/machines/hub_test.cpp
        tests/machines/ip27_test.cpp
    )
    target_link_libraries(machine_tests PRIVATE ultraviolent_ip27 ultraviolent_test_support)
    ultraviolent_enable_warnings(machine_tests)
    ultraviolent_enable_sanitizers(machine_tests)
    add_test(NAME machines COMMAND machine_tests)

    add_executable(core_test tests/core/core_test.cpp)
    target_link_libraries(core_test PRIVATE ultraviolent_core)
    ultraviolent_enable_warnings(core_test)
    ultraviolent_enable_sanitizers(core_test)
    add_test(NAME core_test COMMAND core_test)
endif()
