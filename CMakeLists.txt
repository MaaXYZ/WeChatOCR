cmake_minimum_required(VERSION 3.20)
project(WeChatOCR VERSION 0.1.0 LANGUAGES C CXX)

if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "WeChatOCR currently supports Windows and Linux only")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WeChatOCR currently requires a 64-bit toolchain")
endif()
if(CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|x64)$")
    message(FATAL_ERROR "WeChatOCR currently supports the x86-64 architecture only")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(Threads REQUIRED)

function(wco_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(${target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(WeChatOCR SHARED src/sdk.cpp src/process.cpp)
add_library(WeChatOCR::WeChatOCR ALIAS WeChatOCR)
wco_target(WeChatOCR)
target_compile_definitions(WeChatOCR PRIVATE WCO_BUILD)
file(RELATIVE_PATH WCO_WORKER_INSTALL_RELATIVE
    "${CMAKE_INSTALL_FULL_LIBDIR}" "${CMAKE_INSTALL_FULL_BINDIR}/wechatocr-worker")
target_compile_definitions(WeChatOCR PRIVATE WCO_WORKER_INSTALL_RELATIVE="${WCO_WORKER_INSTALL_RELATIVE}")
if(WIN32)
    target_link_libraries(WeChatOCR PRIVATE Advapi32)
endif()
target_include_directories(WeChatOCR PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(WeChatOCR PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_executable(wechatocr-worker src/worker.cpp src/png.cpp src/process.cpp
    src/runtime_backend.cpp src/runtime_protocol.cpp)
wco_target(wechatocr-worker)
if(MSVC)
    # The public boundary owns its allocations; ship the SDK and private worker
    # without requiring a separately installed Visual C++ Redistributable.
    set_property(TARGET WeChatOCR wechatocr-worker PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS WeChatOCR wechatocr-worker EXPORT WeChatOCRTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/WeChatOCR/WeChatOCR.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WeChatOCR)
install(EXPORT WeChatOCRTargets NAMESPACE WeChatOCR:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WeChatOCR)
include(cmake/MaaAdapter.cmake)
configure_package_config_file(cmake/WeChatOCRConfig.cmake.in
    "${PROJECT_BINARY_DIR}/WeChatOCRConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WeChatOCR)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/WeChatOCRConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/WeChatOCRConfig.cmake"
    "${PROJECT_BINARY_DIR}/WeChatOCRConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WeChatOCR)
install(FILES LICENSE README.md README.en.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DATADIR}/WeChatOCR)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/WeChatOCR/docs)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/WeChatOCR/examples)

if(BUILD_TESTING)
    add_executable(wco_protocol_test src/runtime_protocol_test.cpp src/runtime_protocol.cpp)
    wco_target(wco_protocol_test)
    add_test(NAME runtime_protocol COMMAND wco_protocol_test)

    add_executable(wco_process_test tests/process_test.cpp src/process.cpp)
    wco_target(wco_process_test)
    add_test(NAME process_transport COMMAND wco_process_test)

    add_executable(wco_fake_worker tests/fake_worker.cpp)
    wco_target(wco_fake_worker)
    add_executable(wco_sdk_test tests/sdk_test.cpp)
    wco_target(wco_sdk_test)
    target_link_libraries(wco_sdk_test PRIVATE WeChatOCR)
    add_test(NAME sdk_process_boundary COMMAND wco_sdk_test "$<TARGET_FILE:wco_fake_worker>")
    add_library(wco_fake_mmmojo SHARED tests/fake_mmmojo.cpp)
    wco_target(wco_fake_mmmojo)
    add_executable(wco_runtime_worker_test tests/runtime_worker_test.cpp)
    wco_target(wco_runtime_worker_test)
    target_link_libraries(wco_runtime_worker_test PRIVATE WeChatOCR)
    add_test(NAME runtime_worker_boundary COMMAND wco_runtime_worker_test
        "$<TARGET_FILE:wechatocr-worker>" "$<TARGET_FILE:wco_fake_mmmojo>")
    add_executable(wco_c_header_test tests/c_header_test.c)
    target_link_libraries(wco_c_header_test PRIVATE WeChatOCR)
    add_test(NAME c_header COMMAND wco_c_header_test)
    if(WIN32)
        foreach(target wco_c_header_test wco_sdk_test wco_runtime_worker_test)
            if(TARGET ${target})
                add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:WeChatOCR>" "$<TARGET_FILE_DIR:${target}>")
            endif()
        endforeach()
    endif()
    set_tests_properties(runtime_protocol process_transport c_header PROPERTIES TIMEOUT 30)
    if(TEST sdk_process_boundary)
        set_tests_properties(sdk_process_boundary PROPERTIES TIMEOUT 45)
    endif()
    set_tests_properties(runtime_worker_boundary PROPERTIES TIMEOUT 45)
endif()
