add_executable(tcpserver_tls server.cpp)

target_link_libraries(tcpserver_tls
    PRIVATE
        TAPS::static
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_include_directories(tcpserver_tls
    PRIVATE
        ${TLS_COMMON_DIR}
)

target_compile_features(tcpserver_tls PRIVATE cxx_std_23)
