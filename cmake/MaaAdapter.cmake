option(WCO_BUILD_MAA_ADAPTER "Build the MaaFramework Custom recognition adapter" OFF)
if(NOT WCO_BUILD_MAA_ADAPTER)
    return()
endif()

find_package(MaaFramework CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS regex)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs)

add_library(WeChatOCRMaaAdapter STATIC "${CMAKE_CURRENT_LIST_DIR}/../src/maa_adapter.cpp")
add_library(WeChatOCR::MaaAdapter ALIAS WeChatOCRMaaAdapter)
set_target_properties(WeChatOCRMaaAdapter PROPERTIES
    EXPORT_NAME MaaAdapter
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(WeChatOCRMaaAdapter PUBLIC cxx_std_20)
target_include_directories(WeChatOCRMaaAdapter PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(WeChatOCRMaaAdapter SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(WeChatOCRMaaAdapter PUBLIC WeChatOCR MaaFramework::MaaFramework
    PRIVATE nlohmann_json::nlohmann_json Boost::regex ${OpenCV_LIBS})

include(GNUInstallDirs)
install(TARGETS WeChatOCRMaaAdapter EXPORT WeChatOCRMaaAdapterTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES "${CMAKE_CURRENT_LIST_DIR}/../include/WeChatOCR/MaaAdapter.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WeChatOCR)
install(EXPORT WeChatOCRMaaAdapterTargets NAMESPACE WeChatOCR::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WeChatOCR)

option(WCO_BUILD_MAA_EXAMPLE "Build the MaaFramework adapter example" OFF)
if(WCO_BUILD_MAA_EXAMPLE)
    add_executable(wco_maa_example "${CMAKE_CURRENT_LIST_DIR}/../examples/maa/main.cpp")
    target_link_libraries(wco_maa_example PRIVATE WeChatOCR::MaaAdapter)
endif()
