cmake_minimum_required(VERSION 3.20)
project(telegram_relay_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
)

FetchContent_MakeAvailable(nlohmann_json httplib)

add_executable(telegram_relay_server src/main.cpp)
target_include_directories(telegram_relay_server PRIVATE include)
target_link_libraries(telegram_relay_server PRIVATE httplib::httplib nlohmann_json::nlohmann_json)



