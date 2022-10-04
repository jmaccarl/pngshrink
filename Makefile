all: pngshrink

CXX = /usr/local/bin/g++-12
override CXXFLAGS += -g -std=c++20 -Wno-everything -fcoroutines
LDFLAGS = -L/usr/local/opt/libpng/lib
CPPFLAGS = -I/usr/local/opt/libpng/include

SRCS = $(shell find . -name '.ccls-cache' -type d -prune -o -type f -name '*.cpp' -print | sed -e 's/ /\\ /g')

# Get a compiler internal error when using setjmp with coroutines
pngshrink: $(SRCS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -lpng -DPNG_NO_SETJMP $(SRCS) -o "$@"

pngshrink-debug: $(SRCS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -lpng -DPNG_NO_SETJMP -O0 $(SRCS) -o "$@"

clean:
	rm -f pngshrink pngshrink-debug
