cmake_minimum_required(VERSION 2.6)

project(raspberry-control-server)

enable_language(C)

set(CMAKE_C_FLAGS_DEBUG "-DDEBUG -O0 -pipe")
set(CMAKE_C_FLAGS_RELEASE "-Wall -Wextra -std=c99 -O2 -pipe")

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(WEBSOCK  REQUIRED libwebsockets)
pkg_check_modules(JSON REQUIRED jansson)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(GIO2 REQUIRED gio-2.0)

add_definitions(${OpenSSL_CFLAGS} ${WEBSOCK_CFLAGS} ${JSON_CFLAGS} ${GLIB2_CFLAGS} ${GIO2_CFLAGS})

set(SRCS server.c)

add_executable(server ${SRCS})
target_link_libraries(server ${OpenSSL_LDFLAGS} ${WEBSOCK_LDFLAGS} ${JSON_LDFLAGS} ${GLIB2_LDFLAGS} ${GIO2_LDFLAGS})

install(TARGETS server RUNTIME DESTINATION usr/bin)
