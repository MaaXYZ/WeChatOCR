cmake_minimum_required(VERSION 3.20)
project(WeChatOCRConsumer LANGUAGES C)
find_package(WeChatOCR CONFIG REQUIRED)
add_executable(package_consumer main.c)
target_link_libraries(package_consumer PRIVATE WeChatOCR::WeChatOCR)
enable_testing()
add_test(NAME installed_package COMMAND package_consumer)
if(WIN32)
    add_custom_command(TARGET package_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:WeChatOCR::WeChatOCR>" "$<TARGET_FILE_DIR:package_consumer>")
endif()
