# Install script for directory: C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/intern

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/install/x64-Debug")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/atomic/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/clog/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/ghost/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/guardedalloc/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/libmv/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/memutil/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/opencolorio/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/opensubdiv/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/mikktspace/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/eigen/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/sky/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/audaspace/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/dualcon/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/iksolver/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/itasc/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/cycles/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/locale/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/rigidbody/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/utfconv/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/mantaflow/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/openvdb/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/quadriflow/cmake_install.cmake")
  include("C:/Users/norea/OneDrive/Desktop/ExcaliburInstallPackageWorkspace/blender-git/blender/out/build/x64-Debug/intern/ffmpeg/cmake_install.cmake")

endif()

