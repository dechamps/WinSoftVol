# WinSoftVol developer documentation

[![.github/workflows/continuous-integration.yml](https://github.com/dechamps/WinSoftVol/actions/workflows/continuous-integration.yml/badge.svg)](https://github.com/dechamps/WinSoftVol/actions/workflows/continuous-integration.yml)

WinSoftVol is designed to be built using CMake within the Microsoft Visual C++
2019 toolchain native CMake support.

Make sure to clone the Git repo *with submodules*.

You will need to have the [Windows Driver Kit][] installed. Any version will
likely work.

The build process also requires OpenSSL but that's only for driver signing
purposes. If you have [vcpkg][] set up, it will automatically build OpenSSL as a
dependency of WinSoftVol. Otherwise, you can manually provide `openssl.exe`;
just make sure it's somewhere CMake will find it.

With that out of the way, building WinSoftVol should be as simple as opening the
`src` folder in Visual Studio 2019 and hitting Build.

[vcpkg]: https://vcpkg.io/
[Windows Driver Kit]: https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
