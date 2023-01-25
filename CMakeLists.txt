SET (FFMPEG_DIRS
	FFMPEG/include
  )

include_directories(${FFMPEG_DIRS})

set(FFMPEG_LIBRARIES
  "${CMAKE_CURRENT_LIST_DIR}/FFMPEG/lib/libavcodec.lib"
  "${CMAKE_CURRENT_LIST_DIR}/FFMPEG/lib/libavformat.lib"
  "${CMAKE_CURRENT_LIST_DIR}/FFMPEG/lib/libavutil.lib"
  "${CMAKE_CURRENT_LIST_DIR}/FFMPEG/lib/libswresample.lib"
  "${CMAKE_CURRENT_LIST_DIR}/FFMPEG/lib/libswscale.lib"
 )

add_executable(test main.cpp "wrapper.h" "wrapper.cpp")
target_link_libraries(test PUBLIC ${FFMPEG_LIBRARIES})