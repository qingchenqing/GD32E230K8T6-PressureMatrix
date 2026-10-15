@echo off
set "PYTHON_EXE=C:\Users\13659\AppData\Local\Programs\Python\Python313\python.exe"
if not exist "%PYTHON_EXE%" (
    echo Python was not found at %PYTHON_EXE%
    echo Edit this launcher if Python is installed elsewhere.
    pause
    exit /b 1
)
"%PYTHON_EXE%" "%~dp0tools\matrix_viewer.py"
if errorlevel 1 pause
