cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(WrathFallLauncher LANGUAGES NONE)
endif()

if(NOT CMAKE_HOST_WIN32)
  message(FATAL_ERROR "The WrathFall launcher requires the Windows .NET Framework compiler.")
endif()

get_filename_component(project_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(WRATHFALL_LAUNCHER_OUTPUT_DIR "${project_root}" CACHE PATH "Directory for WrathFall.exe")
set(output "${WRATHFALL_LAUNCHER_OUTPUT_DIR}/WrathFall.exe")
cmake_path(NATIVE_PATH output NORMALIZE native_output)

file(TO_CMAKE_PATH "$ENV{SystemRoot}/Microsoft.NET/Framework64/v4.0.30319" framework)
set(compiler "${framework}/csc.exe")
if(NOT EXISTS "${compiler}")
  message(FATAL_ERROR "The Windows .NET Framework 4.x C# compiler was not found at ${compiler}.")
endif()

set(sources
  Program.cs Project.cs BackgroundJob.cs GameLaunch.cs GameLaunchInputs.cs
  GameImport.cs LaunchCache.cs ProjectTools.cs FrameReport.cs
)
set(source_paths ${sources})
list(TRANSFORM source_paths PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(references
  WPF/PresentationFramework.dll WPF/PresentationCore.dll WPF/WindowsBase.dll
  System.Xaml.dll System.Web.Extensions.dll System.Windows.Forms.dll
  System.Drawing.dll System.IO.Compression.dll System.IO.Compression.FileSystem.dll
)
list(TRANSFORM references PREPEND "${framework}/")
string(REPLACE "/" "\\" reference_options "${references}")
list(TRANSFORM reference_options PREPEND "/reference:")

set(assets launcher.manifest Launcher.ico LauncherWindow.xaml)
list(TRANSFORM assets PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(resource_options "/resource:LauncherWindow.xaml,LauncherWindow.xaml")
file(GLOB hero CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Hero.jpg")
if(hero)
  list(APPEND assets "${hero}")
  list(APPEND resource_options "/resource:Hero.jpg,Hero.jpg")
endif()

add_custom_command(OUTPUT "${output}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${WRATHFALL_LAUNCHER_OUTPUT_DIR}"
  COMMAND "${compiler}" /nologo /target:winexe /platform:x64 /langversion:5 /optimize+ /codepage:65001
    "/out:${native_output}" "/win32manifest:launcher.manifest" "/win32icon:Launcher.ico"
    ${resource_options} ${reference_options} ${sources}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS ${source_paths} ${assets} ${references} "${compiler}"
  COMMENT "Building WrathFall.exe"
  VERBATIM
)
add_custom_target(wrathfall_launcher DEPENDS "${output}")
