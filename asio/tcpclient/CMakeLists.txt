find_package(Threads REQUIRED)

add_executable(tcpclient client.cpp)
target_compile_features(tcpclient PRIVATE cxx_std_23)

target_compile_definitions(tcpclient
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(tcpclient
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(tcpclient
    PRIVATE
        Threads::Threads
)