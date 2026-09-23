[← chapter index](README.md) · [next: what a type actually is →](01-what-is-a-type.md)

# 0. The model you need first

**Time:** 20 minutes. No code. Read it twice if you come from Python, Java,
JavaScript, or Go.

---

## the thing most languages hide

In Python:

```python
a = [1, 2, 3]
b = a
b.append(4)
print(a)        # [1, 2, 3, 4]
```

In Java:

```java
List<Integer> a = new ArrayList<>(List.of(1,2,3));
List<Integer> b = a;
b.add(4);
System.out.println(a);   // [1, 2, 3, 4]
```

Both languages made a choice for you: `b = a` copies a *handle*, and both
names now reach the same list. There is exactly one list on the heap and
the garbage collector deletes it when the last name stops pointing at it.
You never think about it.

C++ makes you say what you mean:

```cpp
std::vector<int> a{1, 2, 3};
std::vector<int> b = a;         // a full, separate copy. two vectors now.
b.push_back(4);
// a is still {1,2,3}
```

```cpp
std::vector<int>& b = a;        // b IS a. one vector, two names.
b.push_back(4);
// a is {1,2,3,4}
```

```cpp
std::vector<int> b = std::move(a);   // b takes a's buffer. a is gutted.
```

Three lines that look almost identical do three completely different
things. That's not C++ being awkward. That's C++ refusing to guess.

---

## objects, names, storage

Three words, used precisely from here on.

**Object** — a region of storage with a type, a value, and a lifetime. Not
"instance of a class". An `int` is an object. A `std::string` is an object.
An element of an array is an object.

**Name** — an identifier you wrote that reaches an object. A variable name,
a reference, a member access. Names are a compile-time thing. Objects are
a runtime thing.

**Storage** — where the bytes live and who is responsible for freeing them.
Four kinds:

| storage | lives | freed by |
|---------|-------|----------|
| automatic | a local variable in a function | the closing brace |
| static | a global, or a `static` local | program exit |
| dynamic | `new`, or what a smart pointer holds | you, or the smart pointer |
| thread | `thread_local` | thread exit |

Most C++ bugs are a name outliving its object. Section 7 is entirely about
that.

---

## the thing C++ gives you that GC languages can't

Because the compiler knows exactly when a scope ends, it knows exactly
when to run a destructor. That is *deterministic destruction* and it's the
foundation of the whole language:

```cpp
{
    std::ofstream log("run.log");
    log << "started\n";
}   // the file is closed HERE. not eventually. here.
```

No `finally`, no `with`, no `defer`, no waiting for a collector. The
closing brace is the cleanup. Chapter 2 builds everything on this.

The cost is that you must know when things die. That's what makes lifetime
worth 75 minutes of your attention later.

---

## what "value semantics" buys

In agentty, a `Thread` is a plain value:

```cpp
// include/agentty/domain/conversation.hpp
struct Thread {
    ThreadId             id;
    std::string          title;
    std::vector<Message> messages;
    // ...
};
```

Copy it and you get a real, independent thread. There is no aliasing, no
shared mutable state, no "did someone else change this while I was
rendering it". The reducer can take one, transform it, and hand back a new
one, and nothing else in the process can see a half-updated version.

That's why agentty's domain types are values and its UI is a pure function
of them. It only works because C++ copies mean copies.

---

## how to hold this in your head

When you read a C++ declaration, ask three questions in order:

1. **What object does this name?** (or does it name no object — is it a
   reference to someone else's?)
2. **Who owns it?** (who runs the destructor)
3. **How long does it live?** (which closing brace)

If you can answer those three for every line you write, you will not write
a lifetime bug. Everything in this chapter is practice at answering them.

---

## on to the code

That's the last section without a program. From here on, everything has
one.

[next: what a type actually is →](01-what-is-a-type.md)
