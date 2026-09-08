add_executable(tcpserver_tls_framed server.cpp)

target_link_libraries(tcpserver_tls_framed
    PRIVATE
        TAPS::static
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_include_directories(tcpserver_tls_framed
    PRIVATE
        ${TLS_COMMON_DIR}
)

target_compile_features(tcpserver_tls_framed PRIVATE cxx_std_23)
