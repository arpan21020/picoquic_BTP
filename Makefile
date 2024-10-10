# Compiler
CC = g++


# Source files
SRC = Receiver.cpp  /home/jarvis/Desktop/picoquic/loglib/autoqlog.c 

# Output binary
PROJECT = Receiver

# Include paths (for header files)
INCLUDE_PATHS = -I. \
				-I/home/jarvis/Desktop/picoquic/picoquic \
				-I/home/jarvis/Desktop/picoquic/loglib 

# Library paths (where the linker looks for libraries)
LIB_PATHS = -L/home/jarvis/Desktop/picoquic/_deps/picotls-build \
			-L/home/jarvis/Desktop/picoquic \
			-L/lib/x86_64-linux-gnu \
            -L/usr/lib

# Libraries to link against
LIBRARIES = -lpicoquic-core \
            -lpicoquic-log \
            -lpicotls-core \
            -lpicotls-fusion \
            -lpicotls-minicrypto \
            -lpicotls-openssl \
            -lssl \
            -lcrypto \
			-lpthread \
			`pkg-config --cflags --libs opencv4` \
			-lX11

CFLAGS = -g -Og  -std=gnu++17 -O2 $(INCLUDE_PATHS)

# Linker flags
LDFLAGS = $(LIB_PATHS) $(LIBRARIES)


$(PROJECT) : $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(PROJECT) 

# Clean up the built binary
clean:
	rm -f $(PROJECT)