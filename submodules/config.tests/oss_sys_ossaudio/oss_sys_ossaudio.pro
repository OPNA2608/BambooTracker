TARGET = test
LIBS += -lossaudio
DEFINES += __LINUX_OSS__ OSS_UNDER_SYS
SOURCES = $$PWD/../../common/test.cpp
