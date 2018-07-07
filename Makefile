# Based on an example by hvoigt on Github (https://github.com/hvoigt/simple-nautilus-extension/blob/master/Makefile)
TARGET 		= dna.so
CXXFLAGS 	= -std=c++11

INCLUDES	=
LIBRARIES	=
SOURCES 	= $(wildcard src/*.cpp)
OBJECTS 	= $(SOURCES:.cpp=.o)

LIBDIR		= /usr/lib

$(TARGET): $(OBJECTS)
	$(CXX) $(shell pkg-config --libs libnautilus-extension) $(OBJECTS) $(INCLUDES) -o $(LIBRARIES)

%.o: %.cpp
	$(CXX) $(INCLUDES) -c $(shell pkg-config --cflags libnautilus-extension) $(CXXFLAGS) $< -o $@

install:
	mkdir -p $(LIBDIR)/nautilus/extensions-3.0
	cp $(TARGET) $(LIBDIR)/nautilus/extensions-3.0

clean:
	rm -f $(TARGET) $(OBJECTS)