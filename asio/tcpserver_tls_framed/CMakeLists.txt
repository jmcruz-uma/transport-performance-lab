find_package(Threads REQUIRED)

add_executable(tcpserver_tls_framed server.cpp)
target_compile_features(tcpserver_tls_framed PRIVATE cxx_std_23)

target_compile_definitions(tcpserver_tls_framed
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(tcpserver_tls_framed
    PRIVATE
        ${ASIO_INCLUDE_DIR}
        ${TLS_COMMON_DIR}
)

target_link_libraries(tcpserver_tls_framed
    PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)
