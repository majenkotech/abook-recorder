CXXFLAGS=-Wno-deprecated-declarations -I/usr/include/x86_64-linux-gnu/sphinxbase -I /usr/include/pocketsphinx

all: abook-recorder

ARCH=$(shell uname -m)

LIBS=-lm -lasound -lSDL2 -lSDL2_ttf -lSDL2_image -lpocketsphinx -lsphinxbase

ifeq ($(ARCH), armv7l)
	LIBS += -lpigpio
endif



abook-recorder.o: LiberationSans-Regular.h
abook-recorder: abook-recorder.o alsa.o
	cc -o $@ $^ -I . $(LIBS)

clean:
	rm abook-recorder *.o

LiberationSans-Regular.h: LiberationSans-Regular.ttf
	bin2h 16 < $< > $@

install: abook-recorder
	install -m 755 abook-recorder ${DESTDIR}/usr/bin
