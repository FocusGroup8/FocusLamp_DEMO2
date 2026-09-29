@echo off
set DOCKER_PATH=D:\Docker\docker\Docker\resources\bin
set PATH=%DOCKER_PATH%;%PATH%

echo ========================================
echo Docker Multi-Container System
echo ========================================
echo.

if "%1"=="" goto help
if "%1"=="build" goto build
if "%1"=="up" goto up
if "%1"=="down" goto down
if "%1"=="logs" goto logs
if "%1"=="ps" goto ps
if "%1"=="test" goto test
goto help

:build
echo Building Docker containers...
docker-compose build
goto end

:up
echo Starting Docker containers...
docker-compose up -d
echo.
echo Containers started!
echo Main client: http://localhost:8000
echo Head pose detector: http://localhost:8001
echo Gesture detector: http://localhost:8002
echo Focus detector: http://localhost:8003
echo Emotion detector: http://localhost:8004
echo Fatigue detector: http://localhost:8005
goto end

:down
echo Stopping Docker containers...
docker-compose down
goto end

:logs
echo Showing logs...
docker-compose logs -f
goto end

:ps
echo Container status:
docker-compose ps
goto end

:test
echo Testing algorithm containers...
echo.
echo Testing head-pose-detector...
curl -X GET http://localhost:8001/health
echo.
echo.
echo Testing gesture-detector...
curl -X GET http://localhost:8002/health
echo.
echo.
echo Testing focus-detector...
curl -X GET http://localhost:8003/health
echo.
echo.
echo Testing emotion-detector...
curl -X GET http://localhost:8004/health
echo.
echo.
echo Testing fatigue-detector...
curl -X GET http://localhost:8005/health
echo.
goto end

:help
echo Usage: start.bat [command]
echo.
echo Commands:
echo   build  - Build Docker containers
echo   up     - Start all containers
echo   down   - Stop all containers
echo   logs   - Show container logs
echo   ps     - Show container status
echo   test   - Test algorithm containers
echo.

:end
