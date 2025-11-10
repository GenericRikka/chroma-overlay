cmake_minimum_required(VERSION 3.16)
project(chroma_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)

pkg_check_modules(X11 REQUIRED x11 xcomposite xext xrender xfixes)
pkg_check_modules(GLFW3 REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)

add_executable(chroma_overlay
  src/main.cpp
)

target_include_directories(chroma_overlay PRIVATE
  ${X11_INCLUDE_DIRS}
  ${GLFW3_INCLUDE_DIRS}
  ${GLEW_INCLUDE_DIRS}
)

target_link_libraries(chroma_overlay PRIVATE
  OpenGL::GL
  ${X11_LIBRARIES}
  ${GLFW3_LIBRARIES}
  ${GLEW_LIBRARIES}
)

# For glfw3native.h
target_compile_definitions(chroma_overlay PRIVATE GLFW_EXPOSE_NATIVE_X11)

# Install shaders beside the binary on "make install" (optional)
install(TARGETS chroma_overlay RUNTIME DESTINATION bin)

