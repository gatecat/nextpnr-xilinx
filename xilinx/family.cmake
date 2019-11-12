if (DEFINED RAPIDWRIGHT_PATH)
	find_package(Java)
	include(UseJava)

	if (NOT DEFINED GSON_PATH)
		message(FATAL_ERROR "CMake variable GSON_PATH must be set to a downloaded gson JAR")
	endif()


	FILE(GLOB RAPIDWRIGHT_JARS ${RAPIDWRIGHT_PATH}/jars/*.jar)
	set(CMAKE_JAVA_INCLUDE_PATH ${RAPIDWRIGHT_PATH}/src ${RAPIDWRIGHT_JARS} ${GSON_PATH})
	string (REPLACE ";" " \n " RAPIDWRIGHT_JARS_STR "${RAPIDWRIGHT_JARS}")

	file(WRITE xilinx/java/bbaexport.mf "Manifest-Version: 1.0\n")
	file(APPEND xilinx/java/bbaexport.mf "Main-Class: dev.fpga.rapidwright.bbaexport\n")
	file(APPEND xilinx/java/bbaexport.mf "Class-Path: ${RAPIDWRIGHT_JARS_STR}\n")
	add_jar(rapidwright_bbaexport SOURCES xilinx/java/bbaexport.java MANIFEST xilinx/java/bbaexport.mf)

	file(WRITE xilinx/java/json2dcp.mf "Manifest-Version: 1.0\n")
	file(APPEND xilinx/java/json2dcp.mf "Main-Class: dev.fpga.rapidwright.json2dcp\n")
	file(APPEND xilinx/java/json2dcp.mf "Class-Path: ${RAPIDWRIGHT_JARS_STR} \n ${GSON_PATH}\n")
	add_jar(rapidwright_json2dcp SOURCES xilinx/java/json2dcp.java MANIFEST xilinx/java/json2dcp.mf)
endif()


