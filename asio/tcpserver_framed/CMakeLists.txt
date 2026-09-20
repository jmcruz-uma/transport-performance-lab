find_package(Threads REQUIRED)

add_executable(tcpserver_framed server.cpp)
target_compile_features(tcpserver_framed PRIVATE cxx_std_23)

target_compile_definitions(tcpserver_framed
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(tcpserver_framed
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(tcpserver_framed
    PRIVATE
        Threads::Threads
)
