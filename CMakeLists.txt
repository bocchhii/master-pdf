cmake_minimum_required(VERSION 3.21)
project(MasterPDF VERSION 2.0.0 LANGUAGES C CXX)

# the version the release build puts in from its tag (v2.0.0 -> 2.0.0); "dev" built from the source code (no updates)
set(MASTERPDF_VERSION "dev" CACHE STRING "The app's version (a release's, from its tag)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # (for VS Code: where Qt and MuPDF are)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# MuPDF (static) and the libraries it is built against
find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h REQUIRED)
find_library(MUPDF_LIB mupdf REQUIRED)
find_library(MUPDF_THIRD_LIB mupdf-third)  # (MSYS2's, and MuPDF built from its source: the libraries it has inside)
# text in pictures read (OCR): Tesseract, with Leptonica - its languages copied next to the program
find_path(TESSERACT_INCLUDE tesseract/baseapi.h REQUIRED)
find_library(TESSERACT_LIB tesseract REQUIRED)
find_library(LEPTONICA_LIB NAMES leptonica lept REQUIRED)  # (older Linux systems: lept)
# a scan's letters traced into a font's outlines (Scan's custom fonts)
find_library(POTRACE_LIB NAMES libpotrace.a potrace REQUIRED)
get_filename_component(TESSDATA_DIR "${TESSERACT_INCLUDE}/../share/tessdata" ABSOLUTE)
file(GLOB TESSDATA_FILES "${TESSDATA_DIR}/*.traineddata")
# (the more accurate "best" models the app comes with - Arabic, English - in place of MSYS2's fast ones)
file(GLOB TESSDATA_BEST "${CMAKE_SOURCE_DIR}/resources/tessdata/*.traineddata")
# (Arabic's words - its model's own dictionary, as a list: a word read unsurely, a letter off one of them, is that word)
file(GLOB TESSDATA_WORDS "${CMAKE_SOURCE_DIR}/resources/tessdata/*_words.txt")
foreach(best ${TESSDATA_BEST})
    get_filename_component(name ${best} NAME)
    list(FILTER TESSDATA_FILES EXCLUDE REGEX "/${name}$")
    list(APPEND TESSDATA_FILES ${best})
endforeach()
set(MUPDF_DEPS)
foreach(dep freetype harfbuzz jbig2dec openjp2 jpeg z gumbo)
    find_library(DEP_${dep} ${dep})  # (those MuPDF was built against - Linux and macOS: the system's, where there's one)
    if(DEP_${dep})
        list(APPEND MUPDF_DEPS ${DEP_${dep}})
    endif()
endforeach()

add_executable(MasterPDF WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/app/Updates.h
    src/app/Updates.cpp
    src/app/UpdateTools.cpp
    src/app/HomePage.cpp
    src/app/OrganizeView.cpp
    src/app/OrganizeView.h
    src/app/HomePage.h
    src/ui/DocTabs.cpp
    src/ui/DocTabs.h
    src/core/Document.h
    src/core/Document.cpp
    src/core/Annotations.cpp
    src/core/TextEdit.cpp
    src/core/Content.h
    src/core/Content.cpp
    src/core/Pictures.cpp
    src/core/Drawings.cpp
    src/core/Shapes.h
    src/core/Shapes.cpp
    src/core/DocShapes.cpp
    src/core/Links.cpp
    src/core/HideText.cpp
    src/core/Structure.cpp
    src/core/Wrap.cpp
    src/core/Reader.h
    src/core/Reader.cpp
    src/core/Jobs.h
    src/core/Jobs.cpp
    src/core/Fonts.h
    src/core/Fonts.cpp
    src/core/PageText.h
    src/core/PageText.cpp
    src/view/PageView.h
    src/view/PageView.cpp
    src/view/PageTools.cpp
    src/view/ThumbnailBar.h
    src/view/ThumbnailBar.cpp
    src/view/SearchResults.h
    src/view/SearchResults.cpp
    src/app/MainWindow.h
    src/app/MainWindow.cpp
    src/app/Annotate.cpp
    src/app/TextTools.cpp
    src/app/PictureTools.cpp
    src/app/ScanTools.cpp
    src/core/ScanFont.cpp
    src/core/ScanFont.h
    src/app/SettingsDialog.cpp
    src/app/FileTools.cpp
    src/app/SignLink.cpp
    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/Classic.h
    src/ui/Classic.cpp
    src/ui/Win98Style.h
    src/ui/Win98Style.cpp
    src/ui/Icons.h
    src/ui/Icons.cpp
    src/ui/ToolButton.h
    src/ui/ToolButton.cpp
    src/ui/ClassicWindow.h
    src/ui/ClassicWindow.cpp
    src/ui/ScrollBox.h
    src/ui/ScrollBox.cpp
    src/ui/Sounds.h
    src/ui/Sounds.cpp
    src/ui/TrackBar.h
    src/ui/TrackBar.cpp
    src/ui/Widgets.h
    src/ui/Widgets.cpp
    src/ui/ColorDialog.cpp
    src/ui/WhatsThis.cpp
    src/ui/WhatsThis.h
    src/ui/FlowLayout.cpp
    src/app/ImageEditor.cpp
    src/core/Ocr.cpp
    src/core/Ocr.h
    src/app/ImageEditor.h
    src/ui/FlowLayout.h
    resources/resources.qrc
)
if(WIN32)
    target_sources(MasterPDF PRIVATE resources/app.rc)  # (the program's icon)
endif()
if(APPLE)
    set_target_properties(MasterPDF PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Master PDF"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.bocchhii.masterpdf"
        MACOSX_BUNDLE_BUNDLE_VERSION "${MASTERPDF_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${MASTERPDF_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "MasterPDF.icns")
endif()
# (only Updates.cpp has it: a new version rebuilds one file)
set_source_files_properties(src/app/Updates.cpp PROPERTIES COMPILE_DEFINITIONS MASTERPDF_VERSION="${MASTERPDF_VERSION}")

find_path(FREETYPE_INCLUDE ft2build.h PATH_SUFFIXES freetype2 REQUIRED)
find_path(HARFBUZZ_INCLUDE hb.h PATH_SUFFIXES harfbuzz REQUIRED)
target_include_directories(MasterPDF PRIVATE src ${MUPDF_INCLUDE_DIR} ${FREETYPE_INCLUDE} ${HARFBUZZ_INCLUDE} ${TESSERACT_INCLUDE})

# the free fonts the app comes with (as the Python app's), and the text readers' languages: next to the program
# (macOS: in the app's Resources folder)
if(APPLE)
    set(DATA_DIR $<TARGET_FILE_DIR:MasterPDF>/../Resources)
else()
    set(DATA_DIR $<TARGET_FILE_DIR:MasterPDF>)
endif()
add_custom_command(TARGET MasterPDF POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/fonts ${DATA_DIR}/fonts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DATA_DIR}/tessdata
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TESSDATA_FILES} ${TESSDATA_WORDS} ${DATA_DIR}/tessdata)
set(MUPDF_LIBS ${MUPDF_LIB})
if(MUPDF_THIRD_LIB)
    list(APPEND MUPDF_LIBS ${MUPDF_THIRD_LIB})
endif()
target_link_libraries(MasterPDF PRIVATE Qt6::Widgets Qt6::Network ${MUPDF_LIBS} ${MUPDF_DEPS} ${TESSERACT_LIB} ${LEPTONICA_LIB} ${POTRACE_LIB})
if(WIN32)
    target_link_libraries(MasterPDF PRIVATE dwmapi winmm)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(MasterPDF PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    set_target_properties(MasterPDF PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in)
endif()
