find_package(Threads REQUIRED)

add_executable(tcpserver_tls server.cpp)
target_compile_features(tcpserver_tls PRIVATE cxx_std_23)

target_compile_definitions(tcpserver_tls
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(tcpserver_tls
    PRIVATE
        ${ASIO_INCLUDE_DIR}
        ${TLS_COMMON_DIR}
)

target_link_libraries(tcpserver_tls
    PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)
