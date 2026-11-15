CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDLIBS += -lX11 -lz -lm
PREFIX ?= $(HOME)/.local
VIDEO ?= fish.mp4
FISH_URL = https://preview.redd.it/llj65a3b50ja1.gif?width=640&format=mp4&s=531009aaed79a173f412e75b181e16b646b4b3d6

.PHONY: all fetch-video check install uninstall clean
all: wmfishcpu

fetch-video:
	curl --fail --location --proto '=https' --max-time 60 --output fish.mp4.tmp '$(FISH_URL)'
	mv fish.mp4.tmp fish.mp4

fish_frames.h: generate_frames.py $(wildcard $(VIDEO))
	@test -f "$(VIDEO)" || { echo 'Run make fetch-video or supply a local clip with make VIDEO=/path/to/video.mp4.' >&2; exit 1; }
	python3 generate_frames.py "$(VIDEO)" $@

wmfishcpu: wmfishcpu.c fish_frames.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) -o $@ wmfishcpu.c $(LDLIBS)

check: wmfishcpu
	./wmfishcpu --self-test

install: wmfishcpu
	install -Dm755 wmfishcpu "$(DESTDIR)$(PREFIX)/bin/wmfishcpu"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/wmfishcpu"

clean:
	rm -f wmfishcpu fish_frames.h
