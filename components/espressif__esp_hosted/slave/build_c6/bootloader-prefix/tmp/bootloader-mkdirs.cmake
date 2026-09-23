# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/jc/.espressif/esp-idf-5.5/components/bootloader/subproject"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/tmp"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/src/bootloader-stamp"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/src"
  "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/jc/esp_hosted_21213/slave/build_c6/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
