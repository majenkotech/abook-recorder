CXXFLAGS=-Wno-deprecated-declarations
abook-recorder.o: LiberationSans-Regular.h
abook-recorder: abook-recorder.o 
	cc -o $@ $< -I . -lm -lasound -lSDL2 -lSDL2_ttf -lSDL2_image

clean:
	rm abook-recorder *.o

LiberationSans-Regular.h: LiberationSans-Regular.ttf
	bin2h 16 < $< > $@
