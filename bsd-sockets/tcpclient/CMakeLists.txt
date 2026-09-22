add_executable(tcpclient client.cpp)
target_compile_features(tcpclient PRIVATE cxx_std_23)

target_link_libraries(tcpclient
    PRIVATE
        Threads::Threads
)
