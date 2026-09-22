add_executable(udpserver server.cpp)
target_compile_features(udpserver PRIVATE cxx_std_23)

target_link_libraries(udpserver
    PRIVATE
        Threads::Threads
)
