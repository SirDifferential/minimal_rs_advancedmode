CC=g++
CXXFLAGS=-std=c++11 -g -I/home/gekko/librealsense/include -fsanitize=address -fstack-protector-all
LDFLAGS=-L/home/gekko/librealsense/build_debug_libuvc -lrealsense2 -latomic
SOURCES=main.cpp
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=minimal_realsense_advancedmode

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(CXXFLAGS) -o $(EXECUTABLE) $(OBJECTS) $(LDFLAGS)

%.o: %.cpp
	$(CC) $(CFLAGS) $(LDFLAGS) -c -o $@ $<

clean:
	rm ${EXECUTABLE}

