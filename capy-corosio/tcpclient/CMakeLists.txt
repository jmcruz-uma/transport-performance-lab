add_executable(tcpclient client.cpp)

target_link_libraries(tcpclient
    PRIVATE
    Boost::corosio
    Threads::Threads
)

target_compile_features(tcpclient PRIVATE cxx_std_23)