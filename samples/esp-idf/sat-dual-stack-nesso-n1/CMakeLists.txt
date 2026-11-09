# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

set(EXTRA_COMPONENT_DIRS ../../../port/esp-idf/)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One lock per board, so resolving the ESP-Mosaico's display and USB
# components does not rewrite the Nesso N1's.
if(IDF_TARGET STREQUAL "esp32s31")
	idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32s31")
endif()

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(sat-dual-stack-nesso-n1 LANGUAGES C)
