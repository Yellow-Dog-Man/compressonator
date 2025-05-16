@REM Need opencv 4.2.0
set OPENCV_DIR="D:/downloads/opencv/"

mkdir -f build_cli
cd build_cli
cmake -DOPTION_ENABLE_ALL_APPS=OFF -DOPTION_BUILD_APPS_CMP_CLI=ON -G "Visual Studio 17 2022" -A x64 ..