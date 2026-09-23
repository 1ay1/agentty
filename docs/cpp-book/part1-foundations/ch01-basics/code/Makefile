# Chapter 1 examples.
#
#   make        build all
#   make run    build and run all, in order
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined

PROGS := 01_types \
         02_integers \
         03_strong_types \
         04_value_categories \
         05_init \
         06_references \
         07_lifetime \
         08_copy_move \
         09_auto \
         10_imagecontent

all: $(PROGS)

%: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

run: all
	@for p in $(PROGS); do \
		echo ""; \
		echo "═══════════════════════════════════════════════════════"; \
		echo "  ./$$p"; \
		echo "═══════════════════════════════════════════════════════"; \
		./$$p || exit 1; \
	done

clean:
	rm -f $(PROGS)

.PHONY: all run clean
