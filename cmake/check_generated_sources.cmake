# Drivers/ and Middlewares/ (the STM32Cube HAL + ST/third-party middleware
# sources) are intentionally NOT committed to this repository - the vendor
# package is too large to be worth versioning here. They must exist on disk
# before configuring, generated locally by STM32CubeMX.
#
# If you cloned this repo fresh: open this project's .ioc file in
# STM32CubeMX and use Project -> Generate Code before running cmake.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/Drivers" OR NOT EXISTS "${CMAKE_SOURCE_DIR}/Middlewares")
    message(FATAL_ERROR
        "Drivers/ and/or Middlewares/ not found in ${CMAKE_SOURCE_DIR}.\n"
        "These folders are not stored in git (the STM32Cube package is too large).\n"
        "Open the .ioc file for this project in STM32CubeMX and click "
        "Project -> Generate Code before building.")
endif()
