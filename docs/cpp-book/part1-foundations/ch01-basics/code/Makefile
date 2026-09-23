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
         10_imagecontent \
         11_zero_overhead \
         12_selftest

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

# Prove the strong type is free by diffing -O2 assembly. See section 3.
proof:
	@./proof.sh

# The one target that checks the whole chapter: build everything warning
# clean, run the self-test, and prove the zero-overhead claim.
verify: clean
	@echo "building all $(words $(PROGS)) programs, warnings are errors..."
	@for p in $(PROGS); do \
		$(CXX) $(CXXFLAGS) -Werror $$p.cpp -o $$p || exit 1; \
	done
	@echo "  ok: zero warnings"
	@echo ""
	@echo "running the self-test..."
	@./12_selftest
	@echo ""
	@echo "proving zero overhead..."
	@./proof.sh
	@echo ""
	@echo "chapter 1 verified."

clean:
	rm -f $(PROGS)

.PHONY: all run proof verify clean
