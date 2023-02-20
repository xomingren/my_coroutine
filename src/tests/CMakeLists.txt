#add_executable(tests tests.cc)
#target_link_libraries(tests coroutine)

add_executable(echoserver echoserver.cc)
target_link_libraries(echoserver coroutine)
