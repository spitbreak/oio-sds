add_definitions(-DG_LOG_DOMAIN="oio.m2v2")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")

include_directories(BEFORE . ..
		${CMAKE_CURRENT_BINARY_DIR}
		${CMAKE_CURRENT_BINARY_DIR}/..
		${CMAKE_CURRENT_BINARY_DIR}/../metautils/lib)

include_directories(AFTER
		${JSONC_INCLUDE_DIRS}
		${LIBRAIN_INCLUDE_DIRS})

link_directories(
		${JSONC_LIBRARY_DIRS}
		${ZK_LIBRARY_DIRS}
		${SQLITE3_LIBRARY_DIRS}
		${LIBRAIN_LIBRARY_DIRS})

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/autogen.c ${CMAKE_CURRENT_BINARY_DIR}/autogen.h
	COMMAND ${PYTHON_EXECUTABLE}
	ARGS ${CMAKE_CURRENT_SOURCE_DIR}/m2gen.py ${CMAKE_CURRENT_BINARY_DIR}
	DEPENDS m2gen.py
)

add_library(meta2v2utils SHARED
		meta2_utils_lb.c
		meta2_utils_lb.h
		generic.c
		generic.h
		${CMAKE_CURRENT_BINARY_DIR}/autogen.c
		${CMAKE_CURRENT_BINARY_DIR}/autogen.h
		meta2_utils.c
		meta2_utils.h
		meta2_bean.c
		meta2_bean_utils.c
		meta2_bean.h
		meta2_macros.h
		meta2_dedup_utils.h
		meta2_dedup_utils.c
		meta2_utils_json_in.c
		meta2_utils_json_out.c
		meta2_utils_json.h
)

target_link_libraries(meta2v2utils
		metautils gridcluster sqliteutils
		${LIBRAIN_LIBRARIES}
		${GLIB2_LIBRARIES} ${SQLITE3_LIBRARIES} ${JSONC_LIBRARIES})

add_library(meta2v2remote SHARED
		meta2_macros.h
		meta2v2_remote.h
		meta2v2_remote.c)

target_link_libraries(meta2v2remote
		meta2v2utils metautils
		${GLIB2_LIBRARIES} ${SQLITE3_LIBRARIES})

add_library(meta2v2 SHARED
		${CMAKE_CURRENT_BINARY_DIR}/autogen.h generic.h
		meta2_utils.h
		meta2_macros.h
		meta2_backend.h
		meta2_backend.c)

target_link_libraries(meta2v2
		metautils sqliterepo
		meta2v2remote meta1remote meta0remote meta0utils
		meta2v2utils meta2v2remote
		sqlxsrv
		${GLIB2_LIBRARIES} ${SQLITE3_LIBRARIES})

add_executable(meta2_server
		meta2_gridd_dispatcher.c
		meta2_filters_extract.c
		meta2_filters_check.c
		meta2_filters_misc.c
		meta2_filters_action_url.c
		meta2_filters_action_container.c
		meta2_filters_action_content.c
		meta2_filters_action_events.c
		meta2_filter_context.c
		meta2_server.c)
bin_prefix(meta2_server -meta2-server)

target_link_libraries(meta2_server meta2v2
        hcresolve metautils gridcluster server
		${GLIB2_LIBRARIES} ${SQLITE3_LIBRARIES})

install(TARGETS meta2v2 meta2v2remote meta2_server meta2v2utils
		LIBRARY DESTINATION ${LD_LIBDIR}
		RUNTIME DESTINATION bin)

