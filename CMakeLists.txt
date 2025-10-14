cmake_minimum_required(VERSION 3.15)
project(ssl_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.75 REQUIRED COMPONENTS system thread json)
find_package(OpenSSL REQUIRED)

if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found")
endif()

add_executable(ssl_server ssl_server.cpp)

target_include_directories(ssl_server PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(ssl_server PRIVATE
    Boost::system
    Boost::thread
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
)

