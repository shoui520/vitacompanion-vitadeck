cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE PATH "Vita toolchain")
endif()

project(vitacompanion_install_test C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

add_executable(vitacompanion_install_test main.c)
target_link_libraries(vitacompanion_install_test SceLibKernel_stub)
vita_create_self(eboot.bin vitacompanion_install_test SAFE)
vita_create_vpk(vitacompanion-install-test.vpk VUPT00001 eboot.bin
  VERSION 01.00
  NAME "VitaCompanion Install Test")
