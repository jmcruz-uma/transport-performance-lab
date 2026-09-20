add_executable(tcpserver_framed server.cpp)
target_compile_features(tcpserver_framed PRIVATE cxx_std_23)
target_link_libraries(tcpserver_framed PRIVATE Boost::corosio)
