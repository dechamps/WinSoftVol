cmake_minimum_required(VERSION 3.20)
project(WinSoftVol)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/FindWDK/cmake")

find_package(WDK REQUIRED)

find_program(OPENSSL NAMES openssl)

wdk_add_driver(WinSoftVol KMDF 1.11 driver.c)
target_compile_options(WinSoftVol PRIVATE /external:anglebrackets /WX /W4 /external:W0 /permissive- /analyze /analyze:external-)
target_link_libraries(WinSoftVol WDK::KSGUID)

install(TARGETS WinSoftVol RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/WinSoftVol.pdb" TYPE BIN OPTIONAL)

add_custom_command(
	OUTPUT Certificate.cer Certificate.pvk
	# Unfortunately we can't use makecert.exe for this because it insists on interactively asking the user for a password (sigh...)
	COMMAND "${OPENSSL}" req -config "${CMAKE_CURRENT_LIST_DIR}/openssl.cnf" -batch -verbose -x509 -newkey rsa -keyout Certificate.pvk -out Certificate.cer -days 3650 -nodes
	DEPENDS "${CMAKE_CURRENT_LIST_DIR}/openssl.cnf"
	VERBATIM
)

add_custom_command(
	OUTPUT Certificate.pfx
	COMMAND "${OPENSSL}" pkcs12 -export -nodes -in Certificate.cer -inkey Certificate.pvk -out Certificate.pfx -passout pass:
	DEPENDS Certificate.cer Certificate.pvk
	VERBATIM
)

add_custom_command(
	OUTPUT WinSoftVol_SelfSigned.sys
	COMMAND ${CMAKE_COMMAND} -E copy WinSoftVol.sys WinSoftVol_SelfSigned.sys
	COMMAND "${WDK_ROOT}/bin/${WDK_VERSION}/${WDK_PLATFORM}/signtool.exe" sign /v /fd SHA256 /f Certificate.pfx WinSoftVol_SelfSigned.sys
	DEPENDS WinSoftVol.sys Certificate.pfx
	VERBATIM
)
add_custom_target(WinSoftVol_SelfSigned ALL
	DEPENDS WinSoftVol_SelfSigned.sys
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/WinSoftVol_SelfSigned.sys" TYPE BIN)
