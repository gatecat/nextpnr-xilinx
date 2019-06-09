find_package(Java)
include(UseJava)

if (NOT DEFINED RAPIDWRIGHT_PATH)
    message(FATAL_ERROR "CMake variable RAPIDWRIGHT_PATH must be set, see https://www.rapidwright.io/docs/Manual_Install.html")
endif()

if (NOT DEFINED GSON_PATH)
    message(FATAL_ERROR "CMake variable GSON_PATH must be set to a downloaded gson JAR")
endif()


FILE(GLOB RAPIDWRIGHT_JARS ${RAPIDWRIGHT_PATH}/jars/*.jar)
set(CMAKE_JAVA_INCLUDE_PATH ${RAPIDWRIGHT_PATH} ${RAPIDWRIGHT_JARS} ${GSON_PATH})
string (REPLACE ";" " \n " RAPIDWRIGHT_JARS_STR "${RAPIDWRIGHT_JARS}")

file(WRITE rapidwright/java/bbaexport.mf "Manifest-Version: 1.0\n")
file(APPEND rapidwright/java/bbaexport.mf "Main-Class: dev.fpga.rapidwright.bbaexport\n")
file(APPEND rapidwright/java/bbaexport.mf "Class-Path: ${RAPIDWRIGHT_JARS_STR}\n")
add_jar(rapidwright_bbaexport SOURCES rapidwright/java/bbaexport.java MANIFEST rapidwright/java/bbaexport.mf)

file(WRITE rapidwright/java/json2dcp.mf "Manifest-Version: 1.0\n")
file(APPEND rapidwright/java/json2dcp.mf "Main-Class: dev.fpga.rapidwright.json2dcp\n")
file(APPEND rapidwright/java/json2dcp.mf "Class-Path: ${RAPIDWRIGHT_JARS_STR} \n ${GSON_PATH}")
add_jar(rapidwright_json2dcp SOURCES rapidwright/java/json2dcp.java MANIFEST rapidwright/java/json2dcp.mf)