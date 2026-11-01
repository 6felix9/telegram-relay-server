cmake_minimum_required(VERSION 3.20)
project(telegram_relay_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# OpenSSL — required so cpp-httplib can make outbound HTTPS calls to
# api.telegram.org. On macOS the Homebrew keg is not on the default search
# path, so seed OPENSSL_ROOT_DIR from the usual locations if the caller
# hasn't set it.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    foreach(_ssl_prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS "${_ssl_prefix}")
            set(OPENSSL_ROOT_DIR "${_ssl_prefix}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL REQUIRED)
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)

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
target_link_libraries(telegram_relay_server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_compile_definitions(telegram_relay_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

find_package(Threads REQUIRED)
target_link_libraries(telegram_relay_server PRIVATE Threads::Threads)




# Unit tests (no framework): `cmake --build build && ctest --test-dir build --output-on-failure`
enable_testing()
add_executable(rate_limiter_test tests/rate_limiter_test.cpp)
target_include_directories(rate_limiter_test PRIVATE include)
target_link_libraries(rate_limiter_test PRIVATE Threads::Threads)
add_test(NAME rate_limiter COMMAND rate_limiter_test)
