SRCS = vmufs.c
OBJS = $(SRCS:.c=.o)
BIN = vmufs
CFLAGS = -g -D_FILE_OFFSET_BITS=64 `pkg-config fuse --libs`
LDFLAGS = -g `pkg-config fuse --cflags`


# You shouldn't need to touch anything below this line...
all: $(BIN)

$(BIN): vmufs.c
	gcc -Wall -o vmufs vmufs.c `pkg-config fuse --cflags --libs`


clean:
	-rm -f $(OBJS)
	-rm -f $(BIN)

