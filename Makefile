#
# heartbeat.so Makefile
#
CC       = gcc
CFLAGS   = -Wall -g -O -fPIC -lpthread
CXXFLAGS =
#INCLUDE  = -I ./inc -I ../comm/inc
TARGET   = heartbeat.so
LIBPATH  = .libs/

vpath %.h ./inc

OBJS     = heartbeat.o
SRCS     = heartbeat.c heartbeat.h list.h

$(OBJS):$(SRCS)
	@$(CC) $(CFLAGS) $(INCLUDE) -c $^

all:$(OBJS)
	@$(CC) -shared -fPIC -o $(TARGET) $(OBJS) -lpthread
	@if [ ! -d $(LIBPATH) ]; then mkdir -p $(LIBPATH);fi;
	@mv $(TARGET) $(LIBPATH)

clean:
	@rm -f *.o
	@rm -f $(LIBPATH)*
