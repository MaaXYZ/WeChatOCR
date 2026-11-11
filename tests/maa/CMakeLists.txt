cmake_minimum_required(VERSION 3.24)
project(WeChatOCRMaaAdapterTests LANGUAGES CXX)

find_package(MaaFramework CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS regex)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs)

# A mock SDK isolates the adapter contract; MaaFramework and its buffers/tasker
# are real, so these checks exercise registration, callbacks and result parsing.
add_executable(wco_maa_adapter_test ../maa_adapter_test.cpp ../../src/maa_adapter.cpp)
target_compile_features(wco_maa_adapter_test PRIVATE cxx_std_20)
target_compile_definitions(wco_maa_adapter_test PRIVATE WCO_BUILD)
if(MSVC)
    target_compile_options(wco_maa_adapter_test PRIVATE /utf-8)
endif()
target_include_directories(wco_maa_adapter_test PRIVATE ../../include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(wco_maa_adapter_test PRIVATE MaaFramework::MaaFramework
    nlohmann_json::nlohmann_json Boost::regex ${OpenCV_LIBS})
enable_testing()
add_test(NAME maa_adapter_contract COMMAND wco_maa_adapter_test)
