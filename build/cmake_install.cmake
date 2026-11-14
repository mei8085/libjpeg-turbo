# Install script for directory: /srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/opt/libjpeg-turbo")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/simd/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/src/spng/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/sharedlib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/src/md5/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libturbojpeg.so.0.5.0"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libturbojpeg.so.0"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHECK
           FILE "${file}"
           RPATH "/opt/libjpeg-turbo/lib64")
    endif()
  endforeach()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE SHARED_LIBRARY FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/libturbojpeg.so.0.5.0"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/libturbojpeg.so.0"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libturbojpeg.so.0.5.0"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/libturbojpeg.so.0"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHANGE
           FILE "${file}"
           OLD_RPATH "::::::::::::::::::::::::"
           NEW_RPATH "/opt/libjpeg-turbo/lib64")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE SHARED_LIBRARY FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/libturbojpeg.so")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "bin" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench"
         RPATH "/opt/libjpeg-turbo/lib64")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/tjbench")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench"
         OLD_RPATH "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build:"
         NEW_RPATH "/opt/libjpeg-turbo/lib64")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/tjbench")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/libturbojpeg.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "include" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/turbojpeg.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64" TYPE STATIC_LIBRARY FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/libjpeg.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "bin" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom"
         RPATH "/opt/libjpeg-turbo/lib64")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/rdjpgcom")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom"
         OLD_RPATH "::::::::::::::::::::::::"
         NEW_RPATH "/opt/libjpeg-turbo/lib64")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rdjpgcom")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "bin" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom"
         RPATH "/opt/libjpeg-turbo/lib64")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/wrjpgcom")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom"
         OLD_RPATH "::::::::::::::::::::::::"
         NEW_RPATH "/opt/libjpeg-turbo/lib64")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/wrjpgcom")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "doc" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/doc" TYPE FILE FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/README.ijg"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/README.md"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/example.c"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/libjpeg.txt"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/structure.txt"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/usage.txt"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/wizard.txt"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/LICENSE.md"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "doc" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/doc" TYPE FILE FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/tjcomp.c"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/tjdecomp.c"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/tjtran.c"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "man" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/man/man1" TYPE FILE FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/cjpeg.1"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/djpeg.1"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/jpegtran.1"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/rdjpgcom.1"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/doc/wrjpgcom.1"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/pkgconfig" TYPE FILE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/pkgscripts/libjpeg.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/pkgconfig" TYPE FILE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/pkgscripts/libturbojpeg.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo" TYPE FILE FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/pkgscripts/libjpeg-turboConfig.cmake"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/pkgscripts/libjpeg-turboConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake"
         "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/CMakeFiles/Export/0e6e4b477a4ae71489b37862d1c0227b/libjpeg-turboTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo/libjpeg-turboTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo" TYPE FILE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/CMakeFiles/Export/0e6e4b477a4ae71489b37862d1c0227b/libjpeg-turboTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/cmake/libjpeg-turbo" TYPE FILE FILES "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/CMakeFiles/Export/0e6e4b477a4ae71489b37862d1c0227b/libjpeg-turboTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "include" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/jconfig.h"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/jerror.h"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/jmorecfg.h"
    "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/src/jpeglib.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "/srv/fb2/mei8085/0033-libjpeg-turbo-1-1379b353/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
