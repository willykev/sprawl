QT += core gui qml quick quickcontrols2 testlib dbus svg network
CONFIG += c++20 testcase
TARGET = sprawl_tests
TEMPLATE = app

INCLUDEPATH += ../src
DEFINES += SPRAWL_VERSION=\\\"test\\\"

HEADERS += \
    ../src/backend.h \
    ../src/board.h \
    ../src/canvasview.h \
    ../src/cli.h \
    ../src/exporter.h \
    ../src/filepicker.h \
    ../src/geometry.h \
    ../src/imagestore.h \
    ../src/item.h \
    ../src/library.h \
    ../src/materials.h \
    ../src/offline.h \
    ../src/spatialgrid.h \
    ../src/textlayout.h \
    ../src/theme.h \
    ../src/translator.h

SOURCES += \
    sprawl_tests.cpp \
    ../src/backend.cpp \
    ../src/board.cpp \
    ../src/canvasview.cpp \
    ../src/cli.cpp \
    ../src/exporter.cpp \
    ../src/geometry.cpp \
    ../src/imagestore.cpp \
    ../src/library.cpp \
    ../src/materials.cpp \
    ../src/scene.cpp \
    ../src/spatialgrid.cpp \
    ../src/textlayout.cpp \
    ../src/theme.cpp \
    ../src/translator.cpp

RESOURCES += ../src/resources.qrc
