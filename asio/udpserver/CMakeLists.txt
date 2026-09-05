find_package(Threads REQUIRED)

add_executable(udpserver server.cpp)
target_compile_features(udpserver PRIVATE cxx_std_23)

target_compile_definitions(udpserver
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(udpserver
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(udpserver
    PRIVATE
        Threads::Threads
)
