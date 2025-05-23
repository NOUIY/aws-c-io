
cmake_minimum_required(VERSION 3.9...3.31)
project(aws-c-io C)

if (NOT IN_SOURCE_BUILD)
    # this is required so we can use aws-c-common's CMake modules
    find_package(aws-c-common REQUIRED)
endif()

include(AwsCFlags)
include(AwsCheckHeaders)
include(AwsSharedLibSetup)
include(AwsSanitizers)
include(AwsFindPackage)
include(CTest)
include(GNUInstallDirs)

option(BUILD_RELOCATABLE_BINARIES
        "Build Relocatable Binaries, this will turn off features that will fail on older kernels than used for the build."
        OFF)
option(BYO_CRYPTO "Don't build a tls implementation or link against a crypto interface. This feature is only for unix builds currently."
        OFF)

file(GLOB AWS_IO_HEADERS
        "include/aws/io/*.h"
        )

file(GLOB AWS_IO_TESTING_HEADERS
        "include/aws/testing/*.h"
        )


file(GLOB AWS_IO_PRIV_HEADERS
        "include/aws/io/private/*.h"
        )

file(GLOB AWS_IO_SRC
        "source/*.c"
        )

set(USE_S2N OFF)

if (WIN32)
    option(USE_IO_COMPLETION_PORTS
            "Use I/O Completion Ports to drive event-loops. \
            If disabled, a less performant implementation based on select() is used. \
            Disable this if implementing your own event-loop whose interface does not match the IOCP interface."
            ON)

    file(GLOB AWS_IO_OS_HEADERS
            )

    file(GLOB AWS_IO_OS_SRC
            "source/windows/*.c"
            )

    if (USE_IO_COMPLETION_PORTS)
        file(GLOB AWS_IO_IOCP_SRC
                "source/windows/iocp/*.c"
                )
         list(APPEND AWS_IO_OS_SRC ${AWS_IO_IOCP_SRC})

         list(APPEND EVENT_LOOP_DEFINES "IO_COMPLETION_PORTS")
    endif ()

    if (MSVC)
        source_group("Header Files\\aws\\io" FILES ${AWS_IO_HEADERS})
        source_group("Header Files\\aws\\io\\private" FILES ${AWS_IO_PRIV_HEADERS})
        source_group("Source Files" FILES ${AWS_IO_SRC})
        source_group("Source Files\\windows" FILES ${AWS_IO_OS_SRC})
    endif ()
    #platform libs come from aws-c-common transitively, so we don't specify them here, but for documentation purposes,
    #Kernel32 and wsock2 are pulled in automatically. Here we add the lib containing the schannel API.
    #Also note, you don't get a choice on TLS implementation for Windows.
    set(PLATFORM_LIBS secur32 crypt32)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    option(USE_VSOCK
	    "Build in support for VSOCK sockets"
	    OFF)

    file(GLOB AWS_IO_OS_HEADERS
            )

    file(GLOB AWS_IO_OS_SRC
            "source/linux/*.c"
            "source/posix/*.c"
            )
    set(PLATFORM_LIBS "")

    list(APPEND EVENT_LOOP_DEFINES "EPOLL")
    set(USE_S2N ON)

elseif (APPLE)

    file(GLOB AWS_IO_OS_HEADERS
            )

    file(GLOB AWS_IO_OS_SRC
            "source/bsd/*.c"
            "source/posix/*.c"
            "source/darwin/*.c"
            )

    find_library(SECURITY_LIB Security)
    find_library(NETWORK_LIB Network)

    # Enable dispatch queue if the libraries are avaliable
    if (NETWORK_LIB AND SECURITY_LIB)
        list(APPEND PLATFORM_LIBS "-framework Security -framework Network")
        list(APPEND EVENT_LOOP_DEFINES "DISPATCH_QUEUE")
    endif ()

    # Enable KQUEUE on MacOS only if AWS_USE_SECITEM is not declared. SecItem requires Dispatch Queue.
    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin" AND NOT DEFINED AWS_USE_SECITEM)
        list(APPEND EVENT_LOOP_DEFINES "KQUEUE")
    endif()

elseif (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "NetBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    file(GLOB AWS_IO_OS_HEADERS
            )

    file(GLOB AWS_IO_OS_SRC
            "source/bsd/*.c"
            "source/posix/*.c"
            )

    list(APPEND EVENT_LOOP_DEFINES "KQUEUE")
    set(USE_S2N ON)

endif()

if (BYO_CRYPTO)
    set(USE_S2N OFF)

    if (APPLE OR WIN32)
            message(FATAL_ERROR "BYO_CRYPTO is only for use with unix systems. It cannot be used on your current platform target")
    endif()
endif()

if (USE_S2N)
        file(GLOB AWS_IO_TLS_SRC
                "source/s2n/*.c"
                )
        # Prefer find_package() because it's the normal CMake way to do dependencies.
        # But fall back on aws_use_package() because some projects still need to do an IN_SOURCE_BUILD of S2N.
        # (e.g. aws-crt-java until this is resolved: https://github.com/awslabs/aws-crt-java/pull/817)
        find_package(s2n QUIET)

        if (s2n_FOUND)
            list(APPEND DEP_AWS_LIBS AWS::s2n)
        else()
            # Set flag to use in-source path to  <s2n/unstable/*.h> headers if we do an IN_SOURCE_BUILD.
            aws_use_package(s2n)
            add_definitions(-DAWS_S2N_INSOURCE_PATH)
        endif()
endif()

file(GLOB IO_HEADERS
        ${AWS_IO_HEADERS}
        ${AWS_IO_OS_HEADERS}
        ${AWS_IO_PRIV_HEADERS}
        )

file(GLOB IO_SRC
        ${AWS_IO_SRC}
        ${AWS_IO_OS_SRC}
        ${AWS_IO_TLS_SRC}
        )

add_library(${PROJECT_NAME} ${LIBTYPE} ${IO_HEADERS} ${IO_SRC})
aws_set_common_properties(${PROJECT_NAME})
aws_prepare_symbol_visibility_args(${PROJECT_NAME} "AWS_IO")
aws_check_headers(${PROJECT_NAME} ${AWS_IO_HEADERS})

aws_add_sanitizers(${PROJECT_NAME})

# We are not ABI stable yet
set_target_properties(${PROJECT_NAME} PROPERTIES VERSION 1.0.0)

if (NOT EVENT_LOOP_DEFINES)
    message(FATAL_ERROR "Event Loop is not setup on the platform.")
endif()
foreach(EVENT_LOOP_DEFINE IN LISTS EVENT_LOOP_DEFINES)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DAWS_ENABLE_${EVENT_LOOP_DEFINE}")
endforeach()

if (AWS_USE_SECITEM)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DAWS_USE_SECITEM")
endif()

if (BYO_CRYPTO)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DBYO_CRYPTO")
endif()

if (USE_S2N)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "-DUSE_S2N")
endif()

if (BUILD_RELOCATABLE_BINARIES)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "-DCOMPAT_MODE")
endif()

if (USE_VSOCK)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DUSE_VSOCK")
endif()

if (AWS_USE_APPLE_NETWORK_FRAMEWORK)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DAWS_USE_APPLE_NETWORK_FRAMEWORK")
endif()

if (AWS_USE_APPLE_DISPATCH_QUEUE)
    target_compile_definitions(${PROJECT_NAME} PUBLIC "-DAWS_USE_APPLE_DISPATCH_QUEUE")
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

aws_use_package(aws-c-common)
aws_use_package(aws-c-cal)
target_link_libraries(${PROJECT_NAME} PUBLIC ${DEP_AWS_LIBS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${PLATFORM_LIBS})

aws_prepare_shared_lib_exports(${PROJECT_NAME})

install(FILES ${AWS_IO_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/aws/io" COMPONENT Development)
install(FILES ${AWS_IO_TESTING_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/aws/testing" COMPONENT Development)

if (BUILD_SHARED_LIBS)
   set (TARGET_DIR "shared")
else()
   set (TARGET_DIR "static")
endif()

install(EXPORT "${PROJECT_NAME}-targets"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/${TARGET_DIR}"
        NAMESPACE AWS::
        COMPONENT Development)

configure_file("cmake/${PROJECT_NAME}-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
        @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/"
        COMPONENT Development)

if (NOT CMAKE_CROSSCOMPILING)
    if (BUILD_TESTING)
       add_subdirectory(tests)
    endif()
endif()
