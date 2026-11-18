QT += core gui qml quick quickcontrols2 testlib dbus svg network
# The frame tests count what the canvas has put in the scene graph, which
# only Qt Quick's own headers reach.
QT += quick-private
CONFIG += c++20 testcase no_private_qt_headers_warning
TARGET = sprawl_tests
TEMPLATE = app

# A window that loses the keys to none of the app's own, as one does when
# the desktop shows another workspace, is only made through Qt GUI's own
# interface to the platform.
QT += gui-private
CONFIG += no_private_qt_headers_warning

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
    ../src/launch.h \
    ../src/library.h \
    ../src/materials.h \
    ../src/measuring.h \
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

# Unpacks .svgz pictures, which are looked through before they are drawn.
LIBS += -lz

RESOURCES += ../src/resources.qrc
