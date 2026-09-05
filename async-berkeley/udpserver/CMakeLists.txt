add_executable(udpserver server.cpp)

target_link_libraries(udpserver
    PRIVATE
        AsyncBerk::asyncberk
)

target_compile_features(udpserver PRIVATE cxx_std_23)
