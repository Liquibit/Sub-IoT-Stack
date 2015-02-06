#This file contains helper MACRO's that are only available to code from the framework itself

# Override a specific component of the framework with alternative sources. 
# This MACRO is mainly intended to be used by individual platforms but can also be used by specific chip 
# implementations (useful for MCU's).
#
#
# Usage:
#    OVERRIDE_COMPONENT(component <source_file> <source_file> ...)
#
MACRO(OVERRIDE_COMPONENT component)
    SET_GLOBAL(FRAMEWORK_OVERRIDE_LIBS "${FRAMEWORK_OVERRIDE_LIBS};FRAMEWORK_${component}")
    ADD_LIBRARY(FRAMEWORK_${component} OBJECT ${ARGN})
ENDMACRO()

# Add one or more CMAKE variables as '#define' statements to the "framework_defs.h"
# generated by the FRAMEWORK_BUILD_SETTINGS_FILE MACRO. This MACRO adopts the same
# variable types used by the GEN_SETTINGS_HEADER MACRO. See the explanation of that macro
# in utils.cmake for more explanation.
#
# Usage:
#    FRAMEWORK_HEADER_DEFINE([STRING <string_var> <string_var> ...] [ID <id_var> <id_var> ...] [BOOL <bool_var> <bool_var> ...] [NUMBER <number_var> <number_var> ...])
#
MACRO(FRAMEWORK_HEADER_DEFINE)
    PARSE_HEADER_VARS("FRAMEWORK_EXTRA_DEFS" ${ARGN})
ENDMACRO()

# Construct a "framework_defs.h" header file in the binary 'framework' directory containing 
# The various (cmake) settings for the framework. By default an 'empty' header file is generated.
# Settings can be added to this file by calling the FRAMEWORK_HEADER_DEFINES macro.
#
# Usage:
#    FRAMEWORK_BUILD_SETTINGS_FILE()
#
MACRO(FRAMEWORK_BUILD_SETTINGS_FILE)
    GEN_SETTINGS_HEADER("${CMAKE_CURRENT_BINARY_DIR}/framework_defs.h" 
			STRING "${FRAMEWORK_EXTRA_DEFS_STRING}" 
			BOOL "${FRAMEWORK_EXTRA_DEFS_BOOL}" 
			ID "${FRAMEWORK_EXTRA_DEFS_ID}"
			NUMBER "${FRAMEWORK_EXTRA_DEFS_NUMBER}" 
			)			
    SET_GLOBAL(FRAMEWORK_EXTRA_DEFS_STRING "")
    SET_GLOBAL(FRAMEWORK_EXTRA_DEFS_BOOL "")
    SET_GLOBAL(FRAMEWORK_EXTRA_DEFS_ID "")
    SET_GLOBAL(FRAMEWORK_EXTRA_DEFS_NUMBER "")
ENDMACRO()
