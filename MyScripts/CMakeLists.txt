# CMakeLists.txt — сборка bt-popup, wifi-popup и dashboard (C++ / FTXUI).
#
# Самодостаточный вариант: FTXUI скачивается и собирается автоматически
# через FetchContent, ставить его в систему не обязательно.
#
# Сборка:
#   cmake -B build -G Ninja        # либо без -G Ninja, будет Makefile
#   cmake --build build -j
#   ./build/wifi-popup
#   ./build/bt-popup
#   ./build/dashboard
#
# Обычно достаточно `make install` в соседнем Makefile: он соберёт этот проект
# и overview/ и разложит бинарники по ~/.config/hypr/.

cmake_minimum_required(VERSION 3.14)
project(hypr-popups CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v7.0.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

find_package(Threads REQUIRED)

add_executable(wifi-popup wifi-popup.cpp)
target_link_libraries(wifi-popup PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  Threads::Threads
)

add_executable(bt-popup bt-popup.cpp)
target_link_libraries(bt-popup PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  Threads::Threads
)

add_executable(dashboard dashboard.cpp)
target_link_libraries(dashboard PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  Threads::Threads
)

