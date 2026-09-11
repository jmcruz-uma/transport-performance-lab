add_executable(tcpserver_tls_framed server.cpp)
target_compile_features(tcpserver_tls_framed PRIVATE cxx_std_23)
target_include_directories(tcpserver_tls_framed PRIVATE ${TLS_COMMON_DIR})
target_link_libraries(tcpserver_tls_framed
    PRIVATE
        Boost::corosio_openssl   # the OpenSSL TLS backend; links Boost::corosio + OpenSSL transitively
        OpenSSL::Crypto          # for OpenSSL_version() called directly
)
