FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)

# The debug console is optional (Kconfig: Radio Unit -> Enable serial debug console).
if(NOT CONFIG_RADIO_ENABLE_CONSOLE)
    list(FILTER app_sources EXCLUDE REGEX "/console/")
endif()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES driver console esp_timer)
