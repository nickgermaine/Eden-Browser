#-------------------------------------------------
#
# Project created by QtCreator 2017-01-20T09:07:51
#
#-------------------------------------------------

QT       += core gui webenginewidgets winextras

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = eden-browser
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which as been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0


SOURCES += main.cpp\
        components/base/baseapplication.cpp \
    components/toolbar/addressbar.cpp \
    components/tabs/tabbar.cpp \
    components/tabs/tab.cpp \
    components/networkmanager/urlintercepter.cpp \
    components/devtools/devtoolscontainer.cpp \
    components/core/core.cpp \
    components/tabs/edenpinnedtabbar.cpp \
    components/base/baseapplication.cpp \
    components/core/core.cpp \
    components/networkmanager/urlintercepter.cpp



HEADERS  += components/base/baseapplication.h \
    components/toolbar/addressbar.h \
    components/tabs/tabbar.h \
    components/tabs/tab.h \
    components/networkmanager/urlintercepter.h \
    components/devtools/devtoolscontainer.h \
    components/json/json.hpp \
    components/core/core.h \
    components/tabs/edenpinnedtabbar.h \
    components/base/baseapplication.h \
    components/core/core.h \
    components/networkmanager/urlintercepter.h


RESOURCES += \
    resources.qrc

DISTFILES += \
    resources/stylesheets/material-dark.css

RC_ICONS = eden.ico


