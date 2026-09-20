add_executable(tcpserver_framed server.cpp)

target_link_libraries(tcpserver_framed
    PRIVATE
        TAPS::static
        Threads::Threads
)

target_compile_features(tcpserver_framed PRIVATE cxx_std_23)
