@echo off
set REQUIRED_VERSION=8.10.2

where gradle >nul 2>nul
if %ERRORLEVEL% neq 0 (
  echo ERROR: gradle не найден в PATH. 1>&2
  echo Установите Gradle %REQUIRED_VERSION% и повторите команду. 1>&2
  exit /b 1
)

for /f "tokens=2" %%v in ('gradle --version ^| findstr /b "Gradle "') do set CURRENT_VERSION=%%v
if not "%CURRENT_VERSION%"=="%REQUIRED_VERSION%" (
  echo ERROR: требуется Gradle %REQUIRED_VERSION%, найден %CURRENT_VERSION%. 1>&2
  exit /b 1
)

gradle %*
