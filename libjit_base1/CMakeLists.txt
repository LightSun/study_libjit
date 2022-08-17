cmake_minimum_required(VERSION 3.5)

project(libjit_base1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -lm -ldl -lrt")
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -lpthread -lm -ldl -lrt")
link_directories(/usr/local/lib)
link_libraries(jit jitdynamic jitplus)

add_executable(libjit_base1
    main.cpp
)
add_executable(t1
    sample/t1.c
)
add_executable(t2
    sample/t2.c
)
add_executable(t3
    sample/t3.c
)
add_executable(t4
    sample/t4.cpp
)
add_executable(t5
    sample/t5.c
)
