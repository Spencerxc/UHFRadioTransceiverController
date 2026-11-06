FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)

# The debug console is optional (Kconfig: Radio Unit -> Enable serial debug console).
if(NOT CONFIG_RADIO_ENABLE_CONSOLE)
    list(FILTER app_sources EXCLUDE REGEX "/console/")
endif()

# The over-the-air command server runs only on the remote unit (Kconfig:
# Radio Unit -> Enable over-the-air command server). The message/client code is
# always built because the console's `cmd` command uses it.
if(NOT CONFIG_RADIO_ENABLE_COMMAND_SERVER)
    list(FILTER app_sources EXCLUDE REGEX "/command/command_server\\.cpp$")
    list(FILTER app_sources EXCLUDE REGEX "/command/telemetry\\.cpp$")
endif()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES driver console esp_timer esp_app_format esp_hw_support)
