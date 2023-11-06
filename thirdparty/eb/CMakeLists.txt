# set minimum cmake version
cmake_minimum_required(VERSION 3.22)

# project name and language
project(eb VERSION 1.0.0 LANGUAGES C)

set(SOURCES
    custom_unistd.h
    appendix.c
    appendix.h
    appsub.c
    bcd.c
    binary.c
    binary.h
    bitmap.c
    book.c
    booklist.c
    booklist.h
    build-post.h
    build-pre.h
    copyright.c
    cross.c
    defs.h
    eb.c
    eb.h
    endword.c
    error.c
    error.h
    exactword.c
    filename.c
    font.c
    font.h
    hook.c
    jacode.c
    keyword.c
    lock.c
    log.c
    match.c
    menu.c
    multi.c
    narwalt.c
    narwfont.c
    readtext.c
    search.c
    setword.c
    stopcode.c
    strcasecmp.c
    subbook.c
    sysdefs.h
    text.c
    text.h
    eb_utf8.c
    widealt.c
    widefont.c
    word.c
    zio.c
    zio.h
    eb.def
    )


if(WIN32)
  set(SOURCES
      ${SOURCES}
      win_dirent.h
      )
endif()

add_library(eb
  SHARED
  ${SOURCES}
  )


if(UNIX)
    find_package(ZLIB REQUIRED)
    target_link_libraries(eb ZLIB::ZLIB)
endif()

if(WIN32)
  set(ZLIB_ROOT winlibs)
  find_package(ZLIB REQUIRED)
  target_link_libraries(eb PRIVATE ZLIB::ZLIB)
  target_link_options(${PROJECT_NAME} PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/FS>)
endif()
include(GNUInstallDirs)
install(TARGETS eb
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
 
    if(WIN32)
    INSTALL (
        DIRECTORY ${CMAKE_SOURCE_DIR}/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h*" 
        PATTERN "CMakeFiles" EXCLUDE)    
    else()
    INSTALL (
      DIRECTORY ${CMAKE_SOURCE_DIR}/
      DESTINATION include
      FILES_MATCHING PATTERN "*.h*"
      PATTERN "win_dirent.h" EXCLUDE
      PATTERN "win*.h" EXCLUDE
      PATTERN ".git*" EXCLUDE
      PATTERN "winlibs" EXCLUDE
      PATTERN "CMakeFiles" EXCLUDE
      )  
    endif()