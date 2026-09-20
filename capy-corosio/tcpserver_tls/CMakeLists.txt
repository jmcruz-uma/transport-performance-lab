add_executable(tcpserver_tls server.cpp)
target_compile_features(tcpserver_tls PRIVATE cxx_std_23)
target_include_directories(tcpserver_tls PRIVATE ${TLS_COMMON_DIR})
target_link_libraries(tcpserver_tls
    PRIVATE
        Boost::corosio_openssl   # the OpenSSL TLS backend; links Boost::corosio + OpenSSL transitively
        OpenSSL::Crypto          # for OpenSSL_version() called directly
        Threads::Threads
)
