@echo off
rem Builds steaminput-harness with MSVC (x64 Native Tools prompt) or falls
rem back to MinGW-w64 when only gcc is on PATH.
setlocal
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /O2 /W4 /EHsc /std:c++17 src\steaminput_harness.cpp /Fe:steaminput-harness.exe /link advapi32.lib
) else (
    where x86_64-w64-mingw32-g++ >nul 2>nul
    if %errorlevel%==0 (
        x86_64-w64-mingw32-g++ -O2 -std=c++17 -Wall -Wextra -o steaminput-harness.exe src\steaminput_harness.cpp -ladvapi32
    ) else (
        echo Neither MSVC cl nor MinGW-w64 g++ found on PATH.
        exit /b 1
    )
)
endlocal
