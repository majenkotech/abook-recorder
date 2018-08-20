CXXFLAGS=-Wno-deprecated-declarations

all: abook-recorder

ARCH=$(shell uname -m)

LIBS=-lm -lasound -lSDL2 -lSDL2_ttf -lSDL2_image

ifeq ($(ARCH), armv7l)
	LIBS += -lpigpio
endif



abook-recorder.o: LiberationSans-Regular.h
abook-recorder: abook-recorder.o 
	cc -o $@ $< -I . $(LIBS)

clean:
	rm abook-recorder *.o

LiberationSans-Regular.h: LiberationSans-Regular.ttf
	bin2h 16 < $< > $@

install: abook-recorder
	install -m 755 abook-recorder ${DESTDIR}/usr/bin
