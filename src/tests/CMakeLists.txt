add_executable(echoserver echoserver.cc)
target_link_libraries(echoserver coroutine)
target_link_libraries(echoserver uring)
