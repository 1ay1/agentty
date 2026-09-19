# Chapter 1: C++ Basics — Deep Mastery Edition

Welcome. You're about to deeply internalize the foundation of C++ type safety.

## Files in This Directory

| File | Purpose | Time |
|------|---------|------|
| **README.md** | Original chapter (for reference) | — |
| **MASTERY_CURRICULUM.md** | **START HERE** — Complete study guide | 10 min read |
| **SECTION_1_1_DEEP_DIVE.md** | Strong Types and Type Safety | 2 hours |
| **SECTION_1_2_DEEP_DIVE.md** | Values, References, Pointers | 2.5 hours |
| **SECTION_1_3_DEEP_DIVE.md** | const Correctness | 2 hours |
| **SECTION_1_4_DEEP_DIVE.md** | Type Deduction (auto, decltype) | 1.5 hours |

## Quick Start

**If you're new to this material:**
1. Read `MASTERY_CURRICULUM.md` (sets expectations)
2. Work through sections 1.1 → 1.2 → 1.3 → 1.4 in order
3. Complete all exercises in each section
4. Pass the mastery quiz at end of each section
5. Do the capstone project to combine all concepts

**Time Investment:** 8-10 hours total (reading + exercises)

**Expected Outcome:** Deep mastery of type system, not just surface-level syntax

## The Four Core Concepts

### 1.1: Strong Types
Make illegal states unrepresentable. `ThreadId ≠ ModelId` even if both hold strings.
- **Key insight:** Compiler prevents mixing IDs at compile time
- **Zero runtime cost** — pure compile-time safety
- **Foundation:** All other concepts build on this

### 1.2: Values, References, Pointers
Choose the right way to refer to data based on ownership semantics.
- **Key insight:** Values own, references borrow, pointers are for nullable cases
- **Lifetime rules:** Compiler enforces them automatically
- **Foundation:** Prevents dangling pointers and use-after-free

### 1.3: const Correctness
Make immutability explicit. Compiler enforces it throughout your code.
- **Key insight:** const propagates naturally when applied systematically
- **Zero runtime cost** — the compiler checks, the CPU doesn't
- **Foundation:** Required for functional architecture (like agentty's)

### 1.4: Type Deduction
Use `auto` for clarity, explicit types for intent. Know the rules of deduction.
- **Key insight:** auto is a convenience, not magic
- **Safety:** Use `const auto&` for safe iteration
- **Foundation:** Combines all previous concepts cleanly

## How They Work Together

```cpp
// Strong Type (1.1): ThreadId is distinct from ModelId
using ThreadId = Id<ThreadIdTag>;

// Reference (1.2): Borrow const reference, no copy
void process_thread(const Thread& thread);

// const Member Function (1.3): Document immutability
const std::vector<Message>& messages() const;

// Type Deduction (1.4): Let compiler figure it out
const auto& msgs = thread.messages();

// All together: Type-safe, efficient, clear intent
```

## What You'll Build

By the end of this chapter, you'll have:
- ✅ Implemented a type-safe ID system with multiple distinct types
- ✅ Built a configuration manager with proper ownership/borrowing
- ✅ Written const-correct classes and functions
- ✅ Used auto appropriately in real scenarios
- ✅ Combined all concepts in a capstone project

## Expectations

**This is NOT:**
- A quick syntax tutorial
- A "skim and move on" chapter
- A collection of tips and tricks

**This IS:**
- Deep mastery through repeated exposure
- Working, compilable code you can modify
- Real bugs from agentty that these concepts solve
- Exercises that build incrementally
- Complete understanding of the *why*, not just the *how*

## How to Use These Materials

### For Each Section:

1. **Read the deep dive** (in full, no skipping)
2. **Study the examples** (run them, modify them, understand them)
3. **Complete the exercises** (all of them, then check solutions)
4. **Pass the mastery quiz** (you should be able to answer without looking back)
5. **Only then** move to next section

### For the Capstone:

- Synthesize all four sections
- Build a real system that requires all concepts
- Verify it compiles and type-safety works
- Understand how they fit together

## Measuring Your Progress

After each section, you should be able to:

**Section 1.1:**
- Explain why compile-time type checking prevents bugs
- Implement a strong type using the template pattern
- Prove zero runtime cost

**Section 1.2:**
- Choose values vs refs vs pointers for any scenario
- Explain lifetime rules and why compiler enforces them
- Implement getters that properly borrow

**Section 1.3:**
- Apply const-correctness systematically
- Mark member functions const appropriately
- Implement both const and mutable versions

**Section 1.4:**
- Use auto for obvious types (iterators, lambdas)
- Use explicit types for intent and clarity
- Explain why `const auto&` is the safe iteration pattern

## Files to Reference

If you need to see:
- **Original chapter:** `README.md`
- **Code examples to run:** `examples/` (create this directory and copy code)
- **Solutions to exercises:** Will be available after attempting

---

## Getting Started

**You are here.** 👈

**Next step:** Open `MASTERY_CURRICULUM.md` and read the overview.

Then work through sections 1.1 → 1.2 → 1.3 → 1.4 in order.

Total time: 8-10 hours. Worth every minute for deep mastery.

Let's go.
