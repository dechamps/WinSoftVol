cmake_minimum_required(VERSION 3.20)
project(WinSoftVol)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/FindWDK/cmake")

find_package(WDK REQUIRED)

find_program(OPENSSL NAMES openssl)

wdk_add_driver(WinSoftVol KMDF 1.11 driver.c)
target_compile_options(WinSoftVol PRIVATE /external:anglebrackets /WX /W4 /external:W0 /permissive- /analyze /analyze:external-)
target_link_libraries(WinSoftVol WDK::KSGUID)

add_custom_command(
	OUTPUT TestCertificate.cer TestCertificate.pvk
	# Unfortunately we can't use makecert.exe for this because it insists on interactively asking the user for a password (sigh...)
	COMMAND "${OPENSSL}" req -config "${CMAKE_CURRENT_LIST_DIR}/openssl.cnf" -batch -verbose -x509 -newkey rsa -keyout TestCertificate.pvk -out TestCertificate.cer -days 3650 -nodes
	VERBATIM
)

add_custom_command(
	OUTPUT TestCertificate.pfx
	COMMAND "${OPENSSL}" pkcs12 -export -nodes -in TestCertificate.cer -inkey TestCertificate.pvk -out TestCertificate.pfx -passout pass:
	DEPENDS TestCertificate.cer TestCertificate.pvk
	VERBATIM
)

add_custom_command(
	OUTPUT WinSoftVol_TestSigned.sys
	COMMAND ${CMAKE_COMMAND} -E copy WinSoftVol.sys WinSoftVol_TestSigned.sys
	COMMAND "${WDK_ROOT}/bin/${WDK_VERSION}/${WDK_PLATFORM}/signtool.exe" sign /v /fd SHA256 /f TestCertificate.pfx WinSoftVol_TestSigned.sys
	DEPENDS WinSoftVol.sys TestCertificate.pfx
	VERBATIM
)
add_custom_target(WinSoftVol_TestSigned ALL
	DEPENDS WinSoftVol_TestSigned.sys
)
