#common makefile header

DIR_INC = ../include
DIR_BIN = ../bin
DIR_LIB = ../libs

TARGET	= recog
BIN_TARGET = $(DIR_BIN)/$(TARGET)

CROSS_COMPILE =
CFLAGS = -g -Wall -I$(DIR_INC)

ALSA_LIB = -lasound;

ifdef LINUX64
LDFLAGS := -L$(DIR_LIB)/x64
else
LDFLAGS := -L$(DIR_LIB)/x86
endif
LDFLAGS += -lmsc -lrt -ldl -lpthread

OBJECTS := $(patsubst %.c,%.o,$(wildcard *.c))

$(BIN_TARGET) : $(OBJECTS)
	$(CROSS_COMPILE)gcc $(CFLAGS) $^ -o $@ $(LDFLAGS) $(ALSA_LIB)

%.o : %.c
	$(CROSS_COMPILE)gcc -c $(CFLAGS) $< -o $@ $(ALSA_LIB)
clean:
	@rm -f *.o $(BIN_TARGET)

.PHONY:clean

#common makefile foot
