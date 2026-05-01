cmake_minimum_required(VERSION 3.23)
project(quickjs-java-wrapper)

set(CMAKE_CXX_STANDARD 11)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../CMakeLists.txt)

add_library( # Sets the name of the library.
        quickjs-java-wrapper

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        ${wrapper_src} ${quickjs_src})

if(MINGW)
    target_link_options(quickjs-java-wrapper PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
endif()


include_directories($ENV{JAVA_HOME}/include/)
include_directories($ENV{JAVA_HOME}/include/darwin)
include_directories($ENV{JAVA_HOME}/include/linux)
include_directories($ENV{JAVA_HOME}/include/win32)