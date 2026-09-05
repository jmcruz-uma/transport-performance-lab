find_package(Threads REQUIRED)

add_executable(tcpserver server.cpp)
target_compile_features(tcpserver PRIVATE cxx_std_23)

target_compile_definitions(tcpserver
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(tcpserver
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(tcpserver
    PRIVATE
        Threads::Threads
)