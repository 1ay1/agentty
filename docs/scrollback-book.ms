.\" ==================================================================
.\" The Frozen Row: How maya and agentty Achieve Scrollback Integrity
.\" A groff -ms technical book. Render: groff -ms -Tpdf -U scrollback-book.ms > out.pdf
.\" Every claim herein is drawn from the actual source of maya + agentty.
.\" ==================================================================
.\"
.\" ---- Page + font geometry ----
.nr PS 11
.nr VS 15
.nr LL 6.3i
.nr LT 6.3i
.nr HM 0.9i
.nr FM 0.9i
.nr PO 1.1i
.nr PI 2m
.nr GROWPS 3
.nr PSINCR 1.5p
.\"
.\" ---- Colors ----
.defcolor codebg rgb 0.96 0.96 0.94
.defcolor rulec  rgb 0.55 0.55 0.62
.defcolor accent rgb 0.10 0.28 0.55
.\"
.\" ---- Running header/footer ----
.ds CH
.ds CF %
.ds LH The Frozen Row
.ds RH Scrollback Integrity in maya + agentty
.\"
.\" ---- CODE macro: monospaced, no-fill block ----
.de CS
.br
.sp 0.3
.nf
.ft CR
.ps 9
.vs 11
..
.de CE
.ft
.ps
.vs
.fi
.sp 0.3
.br
..
.\"
.\" ---- Inline code convenience: .CW word ----
.de CW
\%\f[CR]\\$1\f[]\\$2
..
.\"
.\" ---- Section-intro rule ----
.de HR
.sp 0.2
.br
\D't 0.4p'\h'0'\D'l \\n[LL]u 0'
.sp 0.2
..
.\"
.\" ==================================================================
.\" TITLE PAGE
.\" ==================================================================
.pl 11i
.sp |2.2i
.ce 100
.ps 26
.vs 30
.ft B
The Frozen Row
.ft
.ps 13
.vs 18
.sp 0.6
.ft I
How maya and agentty Achieve Scrollback Integrity
.ft
.sp 0.4
.ps 11
in the Terminal
.ce 0
.sp |7.0i
.ce 100
.ps 10
.ft I
A source-grounded field manual for the inline renderer:
.br
the shadow-of-wire, the Witness Chain, and the state machine
.br
that keeps a streaming TUI from ever corrupting your scrollback.
.ft
.ce 0
.sp |9.3i
.ce 100
.ps 9
Derived entirely from the maya and agentty source trees.
.br
Code is the only source of truth.
.ce 0
.bp
.\"
.\" ==================================================================
.\" FRONT MATTER
.\" ==================================================================
.ps 11
.vs 15
.SH 1
About this book
.LP
This is a book about a single, deceptively narrow engineering problem:
how a program that draws in your terminal
.I inline
\(em sharing the screen with your shell instead of taking over an
alternate buffer \(em can update that screen thousands of times per
second, over a long streaming AI response, without ever corrupting the
lines that scroll up out of view into the terminal's native scrollback.
.PP
The program is
.B agentty ,
a terminal AI coding assistant written in C++26, and its rendering
engine
.B maya .
The problem sounds small. It is not. Getting it wrong produces the
most infuriating class of TUI bug there is: a duplicated composer one
screen up, a card cut in half, your shell history silently drifting,
or \(em worst \(em an entire turn of a conversation stamped
permanently into scrollback where it will remain until you close the
terminal, because once a row scrolls off the top edge it is
.I frozen ,
owned by the emulator, unreachable by any escape sequence the
application can send.
.PP
Every claim in this book is drawn from the actual source. Where the
source and a comment or a design doc disagree, the source wins. Function
names, field names, and the specific escape sequences are quoted as they
appear in the tree so you can open the file and check. The relevant
files are:
.CS
maya/include/maya/render/serialize.hpp   InlineFrameState, the witnesses
maya/src/render/serialize.cpp            compose_inline_frame_impl (the diff)
maya/src/render/frame_bytes.cpp          the Witness Chain entrypoint
maya/include/maya/app/app.hpp            Runtime::render + coherence machine
maya/src/render/scrollback_ledger.cpp    seal_measured (freeze accounting)
agentty/src/runtime/app/update/picker.cpp  reset_inline on thread swap
agentty/src/runtime/app/update/frozen.cpp   trim + commit_scrollback
.CE
.PP
The book is meant to be read front to back once, then kept open beside
the code. Chapters 1\(en3 build the mental model of a terminal and state
the invariant. Chapters 4\(en7 are the algorithm. Chapters 8\(en10 are
the type-system armour \(em the Witness Chain \(em that makes the invariant
un-violatable at compile time. Chapters 11\(en13 are the recovery state
machine and how agentty drives it.
.bp
.\"
.\" ==================================================================
.SH 1
Contents
.LP
.nf
.ft CR
.ps 10
.vs 13
 1   The medium: what a terminal actually is
 2   Inline mode and the frozen-row problem
 3   The one invariant: shadow-of-wire
 4   The shadow: InlineFrameState and prev_cells
 5   The diff: compose_inline_frame_impl, step by step
 6   Growing, scrolling, and the will_scroll_off defence
 7   The incremental hash: proving the shadow cheaply
 8   The Witness Chain, part I: ShadowWitness
 9   The Witness Chain, part II: ScrollbackProof
10   The Witness Chain, part III: FrameBytes and commit_to
11   Coherence: the render-time state machine
12   Recovery primitives: the chooser and its traps
13   agentty as a client: trim, swap, and the ledger
14   Epilogue: why this shape, and what it buys
.ft
.ps
.vs
.fi
.bp
.\"
.\" ==================================================================
.\" CHAPTER 1
.\" ==================================================================
.SH 1
Chapter 1 \(em The medium: what a terminal actually is
.HR
.LP
Before any of the renderer makes sense, you have to hold an accurate
model of the machine it is talking to. Almost every scrollback bug in
this codebase's history was, at root, a place where the code's model of
the terminal diverged from the terminal's real behaviour.
.PP
A terminal emulator maintains a grid of character cells \(em
.CW W " columns wide by some number of rows tall. It has a"
.I cursor
at some (row, column). The application writes a byte stream to a file
descriptor; the emulator interprets that stream, mutating cells and
moving the cursor. Printable characters write a cell at the cursor and
advance the cursor one column right. Control sequences \(em the ANSI /
VT escapes, mostly starting with the byte
.CW 0x1b " (ESC) \(em move the cursor, erase regions, and change colors."
.PP
Three behaviours of this machine are load-bearing for everything that
follows.
.PP
.B "1. There is no read-back of cell contents."
The application cannot ask the terminal "what character is at row 4,
column 12?" nor "where is the cursor right now?" in any way it can rely
on across all terminals. It writes bytes; it never reads the grid. So
the renderer cannot
.I check
its work against the screen. It must
.I model
the screen perfectly and trust the model.
.PP
.B "2. Cursor moves are relative to a viewport whose top drifts."
The viewport is the visible window into a taller virtual grid. When the
cursor is on the bottom row and a line feed
.CW ( \[rs]n )
arrives, the emulator does not move the cursor down \(em there is no down
\(em it
.I scrolls :
every row shifts up one, the top row is pushed out of the viewport, and
a fresh blank row appears at the bottom. The cursor stays on the (new)
bottom row.
.PP
.B "3. A scrolled-off row is frozen."
The row that got pushed out is captured into the emulator's
.I "native scrollback"
\(em the buffer you reach by scrolling your mouse wheel up. Its bytes are
whatever was on the wire for that row at the instant it scrolled. The
application cannot address it any more. There is no cursor position that
reaches it. The single escape that can affect it,
.CW \[rs]x1b[3J
("erase saved lines"), can only
.I delete
the entire scrollback \(em including your shell history from before the
program launched \(em not selectively rewrite a row.
.PP
That third fact is the whole game. The rendering engine gets exactly
.I one
chance to put the correct bytes on a row: the moment before that row
scrolls off. After that, whatever it emitted is permanent. If the
engine's model was wrong at that instant, the wrong bytes are frozen
into your scrollback forever.
.PP
This is why the book is called
.I "The Frozen Row."
The entire architecture exists to guarantee that at the instant any row
freezes, the bytes on it are provably correct.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 2
.\" ==================================================================
.SH 1
Chapter 2 \(em Inline mode and the frozen-row problem
.HR
.LP
A full-screen TUI \(em vim, htop, less \(em has it comparatively easy. It
switches the terminal into the
.I "alternate screen buffer"
(the escape
.CW \[rs]x1b[?1049h ),
a separate grid that has no scrollback at all. It owns every visible
cell. When it exits, the alternate buffer is discarded and your original
screen \(em shell prompt, history \(em reappears untouched. A full-screen
app can repaint any cell at any time because it owns the whole grid and
nothing ever scrolls off into a shared buffer.
.PP
agentty deliberately does
.I not
do this. It runs
.B inline :
it draws below your shell prompt, in the same buffer your shell uses, so
that when a conversation grows past the screen it scrolls into the same
native scrollback as everything else, and you can scroll up through your
whole session \(em shell commands and AI turns interleaved \(em as one
continuous history. This is a far better user experience. It is also
enormously harder to get right, because now the application
.I shares
the scrollback with the host shell, and every row that scrolls off is a
row it can never touch again.
.PP
Consider what a streaming AI response does to this arrangement. The
model emits text token by token. Each token can grow the rendered
content by a fraction of a line to several lines. agentty re-lays-out
and re-renders on every batch of tokens \(em potentially dozens of frames
per second, for a response that ends up hundreds of rows tall. Every one
of those frames pushes rows off the top of the viewport into frozen
scrollback. If any frame's model is off by a row, the corruption is
immediate and permanent.
.PP
The failure modes are not hypothetical; they are catalogued in the
source and in agentty's own memory of past bugs. Among them:
.IP \(bu 2m
The composer (the input box at the bottom) duplicated one screen up,
because a shrink parked the physical cursor somewhere the model didn't
expect.
.IP \(bu 2m
An entire finished turn stamped a duplicate copy into scrollback,
because an oversized redraw emitted bottom-edge newlines that scrolled
still-visible rows off.
.IP \(bu 2m
The user's pre-launch shell history silently
.I drifting
upward every few frames, because a redraw path pushed host content into
scrollback it had no right to touch.
.IP \(bu 2m
A card cut in half at the viewport top, because rows already in
scrollback could not be rewritten to match a late layout change.
.PP
Every one of those is the same underlying event: the renderer's model of
the screen drifted from reality, and a wrong row then froze. The rest of
this book is the machinery that makes that event impossible.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 3
.\" ==================================================================
.SH 1
Chapter 3 \(em The one invariant: shadow-of-wire
.HR
.LP
Here is the entire correctness argument in one sentence, quoted from the
comment above the debug verifier at the end of
.CW compose_inline_frame_impl
in
.CW serialize.cpp :
.PP
.QP
.I
"prev_cells[y][x] must, at the end of every compose, equal
canvas.cells()[y*W + x] for every (x, y) the wire is about to show \(em
i.e. rows in [updatable_start, content_rows)."
.PP
That is the
.B shadow-of-wire invariant .
.CW prev_cells
is the renderer's private buffer holding its belief about what the
terminal is currently displaying \(em its
.I shadow
of the
.I wire .
The invariant says: at the end of every frame, the shadow must equal the
freshly-laid-out canvas over every cell the viewport shows. Because the
next frame computes the bytes to emit as a
.I diff
between the new canvas and this shadow, the shadow being correct is
exactly the precondition for the emitted bytes being correct.
.PP
The same comment then states the thesis of the whole design \(em that
every historical corruption bug was a violation of precisely this
invariant:
.PP
.QP
.I
"Every historical 'corrupted rows' bug in this file's git log was a
version of THIS invariant being temporarily violated: over-commit
scrollback shift, cells_max_y heuristic stale, streaming-markdown cache
aliasing, empty-CodeBlock ghost, partial-write residue inflation \(em all
of them showed up downstream as either a simd::bulk_eq returning
\[oq]unchanged\[cq] for a row that did change (frame freezes) or returning
\[oq]changed\[cq] for a row that didn't (cursor walks redundantly,
emitting bytes that drift the wire away from the shadow)."
.PP
Read that carefully, because it defines the two and only two ways the
invariant can fail, and therefore the two and only two symptom classes:
.IP "\fBFalse-negative diff\fP" 2m
The diff says a row is unchanged when it actually changed. The renderer
emits nothing for it; the screen keeps showing stale content. Symptom:
the frame appears
.I frozen ,
not updating.
.IP "\fBFalse-positive diff\fP" 2m
The diff says a row changed when it didn't. The renderer walks the
cursor and emits bytes for it \(em and every emitted byte is an
opportunity for the wire to drift away from the shadow, because the emit
math assumes a cursor position that may now be wrong. Symptom: cursor
drift, and eventually a wrong row freezes.
.PP
So the design has exactly one job, stated three equivalent ways:
.IP \(bu 2m
Keep the shadow equal to the wire at all times.
.IP \(bu 2m
Never let the diff run against a shadow that is not equal to the wire.
.IP \(bu 2m
Before any row freezes, guarantee its wire bytes match its shadow.
.PP
The next four chapters are how the diff maintains the shadow. The four
after that are how the type system makes it impossible to run the diff
when the shadow is
.I not
provably equal to the wire.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 4
.\" ==================================================================
.SH 1
Chapter 4 \(em The shadow: InlineFrameState and prev_cells
.HR
.LP
The shadow lives in one move-only value type,
.CW InlineFrameState ,
declared in
.CW serialize.hpp .
Its type-level annotation states its purpose:
.PP
.QP
.I
"InlineFrameState is the renderer's only handle to its shadow-of-wire \(em
dropping the value loses the model of what the terminal is displaying."
.PP
The storage, verbatim:
.CS
AlignedBuffer prev_cells_;
int           prev_width_       = 0;
int           prev_rows_        = 0;
bool          decawm_off_       = false;
bool          cursor_hidden_    = false;
int           ghost_rows_above_ = 0;
int           wire_cursor_rows_ = 0;
uint64_t      shadow_hash_      = static_cast<uint64_t>(-1);
uint64_t      gen_              = 0;
std::vector<uint64_t> row_hashes_;
.CE
.LP
Walk the fields, because each one is load-bearing:
.IP "\f[CR]prev_cells_\fP" 2m
The shadow proper: a packed
.CW "prev_rows_ \(mu prev_width_"
array of 64-bit packed cells. This
.I is
maya's belief about the wire.
.IP "\f[CR]prev_rows_\fP, \f[CR]prev_width_\fP" 2m
Dimensions of the shadow. The renderer
.I assumes
the physical cursor sits at row
.CW "prev_rows_ - 1"
\(em the last row it painted. That assumption is the hinge of the whole
diff; every operation that mutates
.CW prev_rows_
must preserve it.
.IP "\f[CR]decawm_off_\fP, \f[CR]cursor_hidden_\fP" 2m
Whether the renderer has already told the terminal to disable
autowrap
.CW ( \[rs]x1b[?7l )
and hide the cursor
.CW ( \[rs]x1b[?25l ).
These modes persist across frames so the escapes are emitted once, not
per frame.
.IP "\f[CR]ghost_rows_above_\fP" 2m
Used only by the recovery path (Chapter 12): the full content height of
the frame that was on screen when a soft failure happened, so the
recovery emit can walk the correct upward erase distance.
.IP "\f[CR]wire_cursor_rows_\fP" 2m
.CW "min(content_rows, term_h)"
\(em how many rows of the frame are actually on screen versus scrolled
off.
.IP "\f[CR]shadow_hash_\fP, \f[CR]row_hashes_\fP" 2m
The cryptographic-cheap proof that the shadow is intact. One
.CW uint64_t
hash per row, XOR-folded into a single
.CW shadow_hash_ .
Chapter 7 is entirely about these.
.IP "\f[CR]gen_\fP" 2m
A monotonic generation counter, bumped by every content-advancing
operation. It stamps scrollback-commit tokens so a stale token from a
superseded frame cannot be applied. Chapter 12 shows why this matters.
.PP
.B "Move-only, by design."
The type deletes its copy constructor and copy assignment:
.CS
InlineFrameState(const InlineFrameState&)            = delete;
InlineFrameState& operator=(const InlineFrameState&) = delete;
InlineFrameState(InlineFrameState&&) noexcept            = default;
InlineFrameState& operator=(InlineFrameState&&) noexcept = default;
.CE
.LP
The reason is stated at the type: a copy would create
.I "two parallel shadows of the same wire region."
There is exactly one terminal, so there must be exactly one shadow. If
you could copy the state, two parts of the program could each believe
they own the model of the screen, diff against their own copy, and emit
conflicting bytes. Move-only makes the shadow a
.I linear
resource: it flows from one owner to the next, never duplicated.
.PP
.B "Pure advancing operations."
Every state transition is an
.CW "&&" -qualified
method that consumes
.CW "*this"
by rvalue and returns the successor by value. You cannot call them on an
lvalue; you must write
.CW "std::move(state).reset_state()" ,
and the source
.CW state
is gone afterward. The transitions are:
.CS
reset_state()            \(-> fresh, capacity retained
finalize()               \(-> emit mode-restore bytes, drop claims
scrollback_marker(rows)  \(-> mint a commit token (const read)
committed(marker)        \(-> shift `rows` off the top of the shadow
abandoned_for_recovery() \(-> the post-soft-failure shape
.CE
.LP
Because each returns a fresh value and consumes the old, the state
machine is effectively pure \(em
.CW "(canvas, state) \(-> (bytes, state\(fm)"
\(em even though the byte-emitter writes into its moved-in local for
implementation convenience. This purity is what lets the Witness Chain
(Chapters 8\(en10) reason about correctness with the type system instead
of runtime asserts.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 5
.\" ==================================================================
.SH 1
Chapter 5 \(em The diff: compose_inline_frame_impl, step by step
.HR
.LP
The heart of the engine is one function in
.CW serialize.cpp :
.CS
std::pair<std::string, InlineFrameState>
compose_inline_frame_impl(const Canvas& canvas,
                          int content_rows,
                          int term_h,
                          const StylePool& pool,
                          InlineFrameState&& state_in,
                          bool synchronized_output,
                          std::string_view reset_prefix);
.CE
.LP
It takes the new canvas and the old shadow, and returns
.I "(bytes to write, new shadow)."
It is a pure transformation in the type system's eyes: it takes the
state by
.CW "&&" -move,
mutates a function-local copy, returns the successor. Let us walk it.
.PP
.B "Step 0 \(em take ownership, handle degenerate input."
It moves the incoming state into a local, and bails immediately on a
zero-size canvas. Then, if the terminal width changed since last frame,
it resets the state \(em a width change invalidates every row layout, so
the shadow is meaningless and must be rebuilt from scratch:
.CS
if (state.prev_width_ != W) {
    state = std::move(state).reset_state();
    state.prev_cells_.shrink_to_fit_if_oversized(...);
}
.CE
.PP
.B "Step 1 \(em compute the diff geometry."
Four integers define the frame's relationship to the viewport:
.CS
const int prev_rows       = state.prev_rows_;
const int prev_on_screen  = std::min(prev_rows, term_h);
const int updatable_start = prev_rows - prev_on_screen;
const int common          = std::min(content_rows, prev_rows);
.CE
.LP
.CW updatable_start
is the crucial one. If the previous frame was taller than the viewport,
some of its top rows already scrolled into frozen scrollback.
.CW updatable_start
is the first row index that is
.I still on screen
\(em i.e. still addressable. Rows below it
.CW ( "y < updatable_start" )
are frozen and must never be touched by the diff. This single quantity
is what keeps the diff from trying to rewrite the unrewritable.
.PP
.B "Step 2 \(em find the first changed row."
Starting at
.CW updatable_start
(never below it), scan down comparing shadow to canvas row by row, using
a SIMD bulk equality, and stop at the first difference:
.CS
int first_changed = common;
if (have_prev) {
    for (int y = updatable_start; y < common; ++y) {
        if (!simd::bulk_eq(cells + y * W, prev + y * W, W)) {
            first_changed = y;
            break;
        }
    }
}
.CE
.LP
On a steady streaming frame only the tail grows, so
.CW first_changed
lands near the bottom and the whole emit is a handful of rows. This is
why a growing 14,000-row transcript still costs a few rows of work per
frame, not 14,000.
.PP
.B "Step 3 \(em the no-op early return, and the cursor-hide re-assertion."
If nothing changed and the row count is identical, there is nothing to
emit \(em but the function still emits
.CW \[rs]x1b[?25l
(hide cursor) first, unconditionally, on every tick. The comment
explains why this cannot be latched: anything outside maya can re-show
the cursor between frames \(em a sandboxed subprocess writing to fd 1, an
SSH reconnect, a mobile terminal opening its soft keyboard. Since inline
mode never uses the hardware cursor (the composer's caret is a painted
cell), the real cursor must stay hidden at all times, so the hide is
re-asserted every tick, even the idle ones. It is idempotent \(em six
bytes, no flicker.
.PP
.B "Step 4 \(em open the frame, position the cursor."
For a normal (non-first) frame, the renderer must move the physical
cursor from where it believes it is \(em row
.CW "prev_rows - 1"
\(em to
.CW first_changed .
This is the single most delicate arithmetic in the engine:
.CS
const int cursor_row_start = prev_rows - 1;
const int delta = first_changed - cursor_row_start;
if (delta < 0) {
    int up = std::min(-delta, prev_on_screen - 1);
    if (up > 0) ansi::write_cursor_up(out, up);
    out += '\[rs]r';
} else if (delta == 0) {
    out += '\[rs]r';
} else {
    out += "\[rs]r\[rs]n";              // grow: SCROLLS at bottom edge
    if (delta > 1) ansi::write_cursor_down(out, delta - 1);
}
.CE
.LP
Three cases. Moving
.I up
(the change is above the last painted row) uses cursor-up, clamped so it
can never walk into frozen scrollback. Staying put uses a carriage
return. Moving
.I down
past the previous bottom \(em i.e. the frame is growing \(em uses
.CW "\[rs]r\[rs]n" ,
whose newline scrolls the terminal at the bottom edge. That scroll is
intentional and is exactly how an inline frame grows past the viewport.
It is also the moment a row freezes, which is why Chapter 6 exists.
.PP
.B "Step 5 \(em the per-row, per-span emit."
From
.CW first_changed
down to the last row, the loop advances one row at a time with
.CW "\[rs]r\[rs]n" ,
and for each changed row finds the changed column span and emits only
those cells. Crucially, after emitting a row's cells it
.I "writes those same cells back into the shadow"
so the shadow stays equal to the wire:
.CS
if (is_new_row) {
    std::memcpy(prev_w + y*W, cur_row, W * 8);   // whole new row
} else {
    // only [x_first_diff, x_last_diff+1) could differ
    std::memcpy(prev_w + y*W + lo, cur_row + lo, (hi-lo) * 8);
}
.CE
.LP
This is the shadow-maintenance step. The invariant from Chapter 3 holds
inductively: rows before
.CW first_changed
were identical and untouched; changed rows have their changed span
copied from canvas into shadow as they are emitted. At loop exit, shadow
equals canvas over the whole visible range.
.PP
.B "Step 6 \(em shrink handling."
If the new frame is shorter than the old
.CW ( "content_rows < prev_rows" ),
rows below the new content still show stale bytes. The renderer
re-anchors to column 0, re-emits the bottom row's visible content
(idempotent), then erases downward with
.CW \[rs]x1b[J .
There is a subtle DECAWM-off correction here: if the bottom row filled
to the last column, the cursor is stuck
.I at
the right edge (autowrap is off, so it doesn't advance to a phantom
column
.CW W ),
and a naive
.CW \[rs]x1b[J
would erase the rightmost real cell. The code detects this and instead
walks down a row, erases from there, and walks back up \(em preserving the
cursor-row invariant the next frame depends on. This one edge case was
the "last column corruption on full-width content" bug (code-block right
borders, full-width rules).
.PP
The whole function is bracketed, in synchronized-output terminals, by
.CW \[rs]x1b[?2026h
and
.CW \[rs]x1b[?2026l
so the terminal applies the entire frame atomically \(em no visible
top-to-bottom paint over a slow SSH link.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 6
.\" ==================================================================
.SH 1
Chapter 6 \(em Growing, scrolling, and the will_scroll_off defence
.HR
.LP
Chapter 5's Step 4 and Step 5 both emit
.CW "\[rs]r\[rs]n"
at the bottom edge to grow the frame, and each such newline scrolls one
row off the top into frozen scrollback. This is the exact instant the
frozen-row problem bites: whatever bytes are on the wire for that row
become permanent. If the shadow was even slightly stale for that row \(em
a wide-character boundary mis-snap, a layout shift between frames, a
subprocess that wrote to the tty \(em the stale bytes freeze.
.PP
The defence is a targeted, unconditional full-row re-emit for exactly
the rows about to freeze. From the per-row loop:
.CS
const int new_visible_top  = std::max(0, content_rows - term_h);
const int prev_visible_top = std::max(0, prev_rows   - term_h);
const bool will_scroll_off =
    (content_rows >= term_h)
    && (y >= prev_visible_top)
    && (y <= new_visible_top);
...
const bool whole_row =
    will_scroll_off || (full_row && row_first_diff < W);
const int x_first_diff = whole_row
    ? 0
    : snap_first_diff_left(row_first_diff, cur_row, prev_row, W);
.CE
.LP
Read the window carefully. The rows this frame will scroll off are
.CW "[prev_visible_top, new_visible_top)" .
The topmost still-visible row,
.CW new_visible_top ,
is the one
.I next
frame's growth will scroll off. The union \(em
.CW "[prev_visible_top, new_visible_top]"
\(em is the set of rows at risk. For every row in it,
.CW whole_row
becomes true, forcing
.CW x_first_diff " to"
.CW 0 :
the entire row is re-emitted from column 0,
.I "even if the diff said it was unchanged."
.PP
The comment states the rationale precisely:
.PP
.QP
.I
"If prev_cells says 'matches canvas' but the wire actually shows old
content (stale shadow, wide-char boundary mis-snap, layout shift between
frames), that stale content commits. Defence: force a full-row re-emit
for every row that this frame will scroll off, plus the topmost
still-visible row."
.PP
This is the belt to the shadow-invariant's braces. The shadow
.I should
always equal the wire. But scrollback corruption is so unrecoverable
that the engine spends a few extra bytes to
.I re-assert
the correct bytes on precisely the rows about to become permanent \(em so
that even if some upstream bug left the shadow stale for those rows, the
wire is corrected in the last instant before it freezes. The history
note records that an earlier version protected only the single topmost
about-to-commit row; if the frame grew by more than one row in a frame,
the lower about-to-commit rows still leaked. The union window closed
that gap.
.PP
There is a second, orthogonal full-row trigger:
.CW full_row ,
auto-enabled under Zed's terminal (and forceable with
.CW "MAYA_COMPAT_REPAINT=1" ).
Some terminals \(em Zed's notably \(em mis-track mid-row cursor moves
(cursor-forward / CHA), landing sub-span updates at the wrong column and
tearing the frame. Under those terminals, every
.I changed
row is re-emitted whole from column 0 using only
.CW "\[rs]r"
+ content, never a mid-row horizontal move. The guard
.CW "&& row_first_diff < W"
matters: it must not force-emit rows the diff calls identical, or a
one-cell change would balloon into a whole-frame re-emit (the source
cites a bench of 3137 bytes/frame versus 96 bytes/frame). Note the
difference in scope: the Zed compat repaint is about
.I horizontal
correctness on quirky terminals; the
.CW will_scroll_off
repaint is about
.I "scrollback-freeze"
correctness on
.I every
terminal.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 7
.\" ==================================================================
.SH 1
Chapter 7 \(em The incremental hash: proving the shadow cheaply
.HR
.LP
The shadow-of-wire invariant is only useful if the engine can
.I check
that the shadow has not been corrupted out-of-band before it trusts it
for a diff. The naive check \(em re-read every cell of the shadow and
compare \(em is
.CW "O(prev_rows \(mu W)"
per frame. On a 7,000-row transcript that is millions of cell reads
every frame, and it was a real source of CPU on tall turns. The engine
replaces it with an incrementally-maintained hash.
.PP
Two fields cooperate:
.CW row_hashes_
holds one 64-bit hash per shadow row (an FNV-style fold of the row's
cells, mixed with the row
.I index
so a reordering is detectable), and
.CW shadow_hash_
is their XOR-fold:
.CS
inline uint64_t combine_rows(const std::vector<uint64_t>& rows) {
    uint64_t h = 0;
    for (uint64_t r : rows) h ^= r;
    return h;
}
.CE
.LP
The key property is that XOR is its own inverse, so the fold can be
maintained incrementally. At the end of each compose, only the rows the
frame actually changed need re-hashing. The engine XORs each changed
row's
.I old
hash out of the fold, re-hashes the row, and XORs the
.I new
hash in:
.CS
uint64_t h = state.shadow_hash_;
for (int y = content_rows; y < prev_rows; ++y)   // shrink: fold out tail
    h ^= state.row_hashes_[y];
state.row_hashes_.resize(content_rows);
for (int y = begin; y < content_rows; ++y) {
    h ^= state.row_hashes_[y];                   // XOR OUT old
    uint64_t nh = hash_row(shadow_base + y*W, W, y);
    state.row_hashes_[y] = nh;
    h ^= nh;                                      // XOR IN new
}
state.shadow_hash_ = h;
.CE
.LP
By XOR-associativity the result is bit-identical to a full re-fold, at
.CW "O(changed rows)"
cost instead of
.CW "O(content_rows)" .
On a long streaming turn, where only the tail changes each frame, this
collapses a per-frame linear cost to effectively constant.
.PP
The checker,
.CW verify_shadow ,
is the symmetric reader. On the hot path (release builds) it does not
re-fold at all \(em the incremental maintenance already keeps
.CW shadow_hash_
correct by construction \(em so it returns in
.CW O(1) :
.CS
#ifndef NDEBUG
    const uint64_t h = combine_rows(state.row_hashes_);
    if (h != state.shadow_hash_) return std::nullopt;   // poisoned
    return ShadowWitness{&state, h};
#else
    return ShadowWitness{&state, state.shadow_hash_};    // trust maintained hash
#endif
.CE
.LP
It also rejects the one realistic out-of-band corruption cheaply: if the
row-hash array's size does not match
.CW prev_rows_ ,
something cleared or resized it outside the compose path, and
.CW verify_shadow
returns
.CW std::nullopt
\(em "the shadow is poisoned; do not diff." That
.CW nullopt
is the entry point to the recovery machine, and the return type
.CW "std::optional<ShadowWitness>"
is the beginning of the Witness Chain \(em the subject of the next three
chapters.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 8
.\" ==================================================================
.SH 1
Chapter 8 \(em The Witness Chain, part I: ShadowWitness
.HR
.LP
Everything so far is
.I discipline :
the diff maintains the shadow, the hash proves it, the caller is
.I supposed
to check the hash before diffing. But "supposed to" is exactly how bugs
happen. The source is explicit that every scrollback bug in the
codebase's history was a path that
.I forgot ,
or short-circuited, the check. So the engine lifts the obligation out of
discipline and into the type system. It is called the
.B "Witness Chain" ,
and it makes "diff without proving the shadow matches the wire" a
.I "compile error."
.PP
The first link is
.CW ShadowWitness ,
declared in
.CW serialize.hpp .
Its type annotation:
.PP
.QP
.I
"ShadowWitness is single-use proof that the shadow matches the wire \(em
dropping it forces the next render to re-verify or fall through to
recovery."
.PP
It is a move-only token with a
.I private
constructor. There is exactly one producer:
.CW verify_shadow .
And the compose entrypoint
.I requires
a
.CW "ShadowWitness&&"
as an argument. Chain the two facts together and the conclusion is
airtight: the only way to obtain the argument compose demands is to run
the verification that produces it. You cannot fabricate a witness \(em the
default constructor is deleted:
.CS
ShadowWitness() = delete;
private:
    ShadowWitness(const InlineFrameState* s, std::uint64_t h) noexcept
        : state_(s), hash_at_issue_(h) {}
    friend std::optional<ShadowWitness>
        verify_shadow(const InlineFrameState&) noexcept;
.CE
.PP
.B "Provenance binding."
The witness carries a pointer to the exact state it was minted against,
plus the hash value at issue time:
.CS
const InlineFrameState* state_;
std::uint64_t           hash_at_issue_;
.CE
.LP
When compose consumes the witness it re-derives the hash and compares
against
.CW hash_at_issue() .
This closes the window between
.I "verify returned"
and
.I "compose ran."
If the cells were mutated in that span, the re-hash will not match. The
response, from
.CW frame_bytes.cpp ,
is deliberately severe:
.CS
if (h != witness.hash_at_issue()) {
    // Shadow corruption between verify and consume.
    // Cannot emit bytes safely; abort the process.
    std::abort();
}
.CE
.LP
Why abort rather than recover? Because the renderer is single-threaded
and the witness was taken immediately upstream. A mismatch here means
the cell buffer changed between two consecutive function calls within
one tick \(em which implies a thread-safety bug or a hardware fault. The
source's reasoning:
.I "continuing to run is worse than stopping."
Recovery is for
.I expected
divergence; this is
.I impossible
divergence, and the only honest response is to stop.
.PP
.B "Single-use via move-only."
The witness is move-only, and consumption moves it. A second consumption
is a use-after-move, producing a witness with a null state pointer that
fails the
.CW valid()
assertion. Compilers warn on it under
.CW -Wmoved-from .
So a witness cannot be reused across frames \(em each frame must mint a
fresh proof. The type system enforces
.I freshness ,
not just existence.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 9
.\" ==================================================================
.SH 1
Chapter 9 \(em The Witness Chain, part II: ScrollbackProof
.HR
.LP
.CW ShadowWitness
proves the shadow matches the wire's
.I "visible viewport."
It says nothing about the rows that already scrolled
.I off
into frozen scrollback. Those rows are the other half of the
correctness theorem, and they get their own token:
.CW ScrollbackProof .
.PP
The problem it solves is specific. When a frame has overflowed
.CW ( "prev_rows > term_h" ),
some rows are in scrollback. The diff must never rewrite them. But there
is a subtler hazard: what if the
.I content
of those committed rows in the new canvas differs from what physically
scrolled off? That happens on a wholesale model swap (loading a
different conversation), or if a layout shift moved a block. The diff,
scanning only the visible range, would skip those rows \(em leaving the
old bytes stranded in scrollback while the model believes they hold the
new content. The result is a duplicated or mismatched prefix.
.PP
The guard is a memcmp of the canvas's top overflow rows against the
shadow's committed prefix,
.CW "InlineFrameState::scrollback_prefix_matches" .
Historically this was a bare
.CW bool
the render path was
.I disciplined
to call. The type annotation on
.CW ScrollbackProof
records what went wrong with that:
.PP
.QP
.I
"Every scrollback-corruption bug in this codebase's history was a frame
that reached the diff with a SHIFTED committed prefix because some path
forgot, or short-circuited, that check. ScrollbackProof lifts the
obligation into the type system, exactly as ShadowWitness did for the
shadow."
.PP
So, identical pattern: move-only token, private constructor, sole
producer
.CW ( "check_scrollback" ),
consumed by value in
.CW "InlineFrame<Synced>::render" .
"Render an overflowed frame without checking the committed prefix" is a
compile error. There are two discharge shapes, both carried by the same
type so the render signature is uniform:
.IP "\fBVacuous\fP" 2m
The frame fits the viewport
.CW ( "prev_rows \(<= term_h" ).
Nothing overflowed, there is no committed prefix, the obligation is
trivially true.
.CW check_scrollback
returns a vacuous proof.
.IP "\fBWitnessed\fP" 2m
The frame overflowed
.I and
the committed prefix is byte-identical to what physically scrolled off.
The memcmp ran and matched.
.PP
When the prefix has
.I shifted ,
.CW check_scrollback
returns
.CW std::nullopt ,
and the caller must recover (commit the overflow, then soft-repaint)
instead of rendering. This is the same control flow the old bool-gate
expressed \(em now unforgeable.
.PP
The proof carries the same provenance binding as
.CW ShadowWitness :
the address of the state it was minted against and the overflow row
count it validated, so render can assert (in debug) that the proof
matches the state it is rendering. Two witnesses, two halves of the
theorem: the visible viewport and the frozen prefix. Together they cover
every cell.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 10
.\" ==================================================================
.SH 1
Chapter 10 \(em The Witness Chain, part III: FrameBytes and commit_to
.HR
.LP
The witnesses prove the
.I precondition .
The last link handles the
.I postcondition :
that the state advance and the byte delivery are
.I inseparable.
This is
.CW FrameBytes ,
a capsule holding the bytes to write and the successor state, returned
by the witness-chain entrypoint
.CW compose_inline_frame
in
.CW frame_bytes.cpp .
.PP
The hazard it closes is a torn commit. Suppose compose produced bytes
and a new shadow, the code updated the shadow to reflect the new frame,
and
.I then
the write to the terminal failed partway. Now the shadow describes a
frame the wire never fully received. Every future diff runs against a
lie. The
.CW FrameBytes
capsule makes this impossible by
.I fusing
the two operations:
.CS
CommitOutcome FrameBytes::commit_to(Writer& writer) && noexcept {
    if (bytes_.empty())
        return commit::Synced{std::move(successor_)};

    Status st = writer.write_or_buffer(bytes_);
    if (st)
        return commit::Synced{std::move(successor_)};

    // Hard I/O error: drop the residue and surface HardReset.
    writer.discard_residue();
    return commit::HardReset{st};
}
.CE
.LP
The successor state is
.I only
handed out \(em wrapped in the
.CW commit::Synced
arm \(em when the write actually succeeded. If the write fails hard, the
in-flight successor is
.I dropped ,
and the outcome is
.CW commit::HardReset :
the wire is in an unknown state, so the next render will emit the full
wipe-and-repaint. The source is explicit that dropping the successor is
the point:
.I "none of its invariants hold any more (prev_cells now reflects bytes
the wire never received)."
.PP
There are two more consumption shapes, each a typed transition:
.IP "\f[CR]abandon()\fP \(-> \f[CR]commit::Stale\fP" 2m
The bytes are discarded without writing (e.g. a higher-level decision
not to send this frame). The wire still shows the prior frame, but the
successor was already computed for a frame that never shipped. So the
state is run through
.CW abandoned_for_recovery()
and returned as
.CW Stale
\(em the next render takes the case-(B) soft redraw, repainting in place
against what the wire plausibly still shows.
.IP "\f[CR]abandon_to_hard_reset(reason)\fP \(-> \f[CR]commit::HardReset\fP" 2m
The runtime observed, from a parallel path, that the wire is genuinely
unknown. Drop the bytes, surface
.CW HardReset .
.PP
Notice the shape of the whole chain now.
.CW verify_shadow
returns
.CW "optional<ShadowWitness>" ;
.CW check_scrollback
returns
.CW "optional<ScrollbackProof>" ;
.CW compose_inline_frame
consumes both witnesses and returns
.CW FrameBytes ;
.CW "FrameBytes::commit_to"
consumes the bytes and returns a typed
.CW CommitOutcome
that
.I is
the next coherence state. At no point can you obtain the next state
except by discharging every proof and successfully writing the bytes.
The corruption modes are not defended against at runtime \(em they are
.I unrepresentable .
.bp
.\"
.\" ==================================================================
.\" CHAPTER 11
.\" ==================================================================
.SH 1
Chapter 11 \(em Coherence: the render-time state machine
.HR
.LP
The witnesses gate a single frame. The
.I "coherence state machine"
decides, each render, which kind of frame to produce. It lives in
.CW Runtime
in
.CW app.hpp
as a variant of typed states. For inline mode the states are:
.CS
inline_frame::InlineFrame<Empty>       // never rendered
inline_frame::InlineFrame<Fresh>       // first paint pending
inline_frame::InlineFrame<Synced>      // shadow matches wire
inline_frame::InlineFrame<Stale>       // needs soft redraw (case B)
inline_frame::InlineFrame<HardReset>   // needs wipe + repaint
inline_frame::InlineFrame<Sealed>      // finalized
.CE
.LP
The tag is a phantom type: the same underlying state, but the tag
selects which
.CW render
overload is legal, so illegal transitions are compile errors. The normal
life of a frame is
.CW "Fresh \(-> Synced \(-> Synced \(-> ..." ,
each
.CW Synced
render running the full Witness Chain and returning a new
.CW Synced
via
.CW "commit::Synced" .
.PP
The interesting transitions are the recoveries, and they are exactly the
.CW Cmd
handlers a host can dispatch:
.CS
void commit_inline_overflow() noexcept {
    ...
    const int overflow_rows = s->rows() - term_h;
    if (overflow_rows <= 0) return;
    in_coherence_ = std::move(*s).commit(
        s->scrollback_marker(overflow_rows));
}

void force_redraw() noexcept {
    fs_coherence_ = coherent::Divergent{};
    if (auto* s = get_if<InlineFrame<Synced>>(&in_coherence_))
        in_coherence_ = std::move(*s).demote_to_stale();
}

void reset_inline() noexcept {
    fs_coherence_ = coherent::Divergent{};
    if (auto* s = get_if<InlineFrame<Synced>>(&in_coherence_))
        in_coherence_ = std::move(*s).demote_to_hard_reset();
    else if (auto* st = get_if<InlineFrame<Stale>>(&in_coherence_))
        in_coherence_ = std::move(*st).escalate_to_hard_reset();
}
.CE
.LP
Three primitives, three severities. They are
.I not
interchangeable, and picking the wrong one is its own class of bug. That
is important enough to get the whole next chapter.
.PP
One subtlety worth noting here: both
.CW commit_inline_overflow
and its sibling
.CW commit_inline_prefix
.I "shift the shadow while remaining Synced."
That is, they mutate
.CW prev_rows_
and
.CW prev_cells_
without changing the coherence
.I tag .
The render path has a canvas-preservation optimization gated on the
coherence index, and these two structural events are invisible to it \(em
so they set a one-shot inhibitor
.CW ( "scrollback_recovery_count_" -adjacent)
to suppress that optimization for the frame after the shift. This is the
kind of second-order interaction that makes the "just remain Synced"
shortcut subtle, and it is documented at the field in
.CW app.hpp .
.bp
.\"
.\" ==================================================================
.\" CHAPTER 12
.\" ==================================================================
.SH 1
Chapter 12 \(em Recovery primitives: the chooser and its traps
.HR
.LP
Three host-callable primitives interact with the inline render state.
The source is emphatic that they are not interchangeable. Here is the
chooser, then the trap that motivates it.
.PP
.B "commit_scrollback_overflow \(em bookkeeping only."
Calls
.CW commit_inline_overflow ,
which advances
.CW prev_cells_
by
.CW "max(0, prev_rows - term_h)"
rows and recomputes the hash.
.B "Zero bytes are written to the wire."
After it,
.CW "prev_rows \(<= term_h"
and
.CW "updatable_start == 0" ,
so the next diff scans every visible row. It is safe because the rows
dropped from the shadow were
.I already
emitted to the wire by earlier streaming and are already in native
scrollback \(em the commit just acknowledges that fact. Use it for a
bounded-frozen trim (dropping the oldest N entries from the transcript).
.PP
.B "force_redraw \(em soft viewport repaint."
Demotes
.CW Synced
to
.CW Stale ;
the next compose enters case (B) \(em cursor walks up, every visible row
is re-emitted in place, then
.CW \[rs]x1b[J
erases below. Use it for ghost cells
.I inside
the live viewport, and for a Ctrl-L user redraw hotkey. But it carries a
hazard: case (B)'s "new frame taller than the cursor's offset from
viewport top" branch emits bottom-edge newlines to make room, and
.I "each such newline permanently scrolls one row of host content into
scrollback."
Under streaming that is correct (the rows scrolling off belong to the
current turn). Under a
.I "model swap"
those rows are unrelated host history, and the scroll destroys them.
.PP
.B "reset_inline \(em hard wipe, host-callable."
Demotes to
.CW HardReset ,
whose next render emits
.CW "\[rs]x1b[2J\[rs]x1b[3J\[rs]x1b[H"
\(em wipe viewport, wipe saved lines, home cursor \(em then repaints fresh.
The
.CW \[rs]x1b[3J
is the
.I only
host-callable escape that can reach off-viewport frozen rows, and it does
so by destroying
.I all
of them, including pre-agentty shell history. It is reserved for an
explicit, destructive, user-initiated content swap.
.PP
.B "The trap: model swap into shorter content."
This is worth internalizing because two "obvious" fixes are both wrong.
When you switch to a different conversation (thread swap / new thread),
the old thread may have overflowed, committing dozens of its rows to
frozen scrollback. Now:
.IP \(bu 2m
.B force_redraw
seems right \(em "my shadow no longer matches what the user should see,
force a repaint." But case (B)'s bottom-edge newlines scroll the old
thread's tail
.I "and the host's shell history"
into oblivion. Wrong.
.IP \(bu 2m
.B commit_scrollback_overflow
seems right \(em it advances
.CW prev_rows
down to
.CW term_h
and fixes the mid-viewport seam the diff would otherwise leave. But it
writes
.I "zero bytes" ,
so the old thread's rows already in frozen scrollback
.I "stay physically on the wire."
If the new thread is
.I shorter ,
the next frame shrinks within the viewport and leaves a copy of the old
transcript stranded above the new one. Wrong \(em and intermittent,
because it only bites when the new content is shorter than the old
overflow.
.PP
Only
.CW reset_inline
is correct here: its
.CW \[rs]x1b[3J
is the sole primitive that reaches those stranded off-viewport rows. The
source's comment at
.CW reset_inline
in
.CW app.hpp
states it plainly \(em neither
.CW force_redraw
nor
.CW commit_scrollback_overflow
"can erase those rows; they strand a copy of the old transcript above
the new one. demote_to_hard_reset is the only coherent fix." And it adds
the guardrail:
.I "Do NOT wire this to a per-frame or per-turn path"
\(em the scrollback wipe is acceptable
.I only
for an explicit user-initiated swap.
.PP
This is why agentty's
.CW picker.cpp ,
handling
.CW ThreadListSelect
and
.CW NewThread ,
dispatches
.CW reset_inline
and nothing else. The chooser table, from the source:
.CS
Ghost cells inside the live viewport   -> force_redraw
Ctrl-L user redraw hotkey              -> force_redraw
Wholesale model swap (thread switch)   -> reset_inline
Bounded-frozen trim (drop oldest N)    -> commit_scrollback_overflow
Terminal genuinely corrupted (external) -> (none - resize only)
.CE
.bp
.\"
.\" ==================================================================
.\" CHAPTER 13
.\" ==================================================================
.SH 1
Chapter 13 \(em agentty as a client: trim, swap, and the ledger
.HR
.LP
maya provides the mechanism; agentty is the policy layer that drives it
correctly. Three interactions are worth studying because they show the
chooser from Chapter 12 in real use.
.PP
.B "1. Submit is not a swap."
When you send a message,
.CW "submit_message"
in
.CW modal.cpp
appends to the existing transcript. It deliberately does
.I not
call
.CW commit_scrollback_overflow :
.PP
.QP
.I
"Submit is not a wholesale model swap \(em it appends to the existing
transcript, so maya's normal row diff handles the composer-shrink +
new-turn-rows transition correctly. Past versions fired
commit_scrollback_overflow on every submit to flush a suspected
composer-shrink seam, but the call advances prev_cells past whatever the
renderer thinks has overflowed."
.PP
The lesson: reach for a recovery primitive only when the normal diff
genuinely cannot handle the transition. Appending is the diff's home
turf; intervening only creates drift.
.PP
.B "2. Trim fires commit_scrollback."
When the transcript grows past a soft cap, agentty freezes and drops the
oldest entries.
.CW "trim_frozen_if_oversized"
in
.CW frozen.cpp
computes exactly how many rows it dropped and returns a
.CW "Cmd::commit_scrollback(N)"
\(em the row-counted commit that shifts the shadow to match. The header
comment records the whole history of this being the drift-prone seam:
.PP
.QP
.I
"[the trim pipeline] kept in lockstep across four parallel vectors, and
summed into the exact commit_scrollback count each trim sent maya. Every
historical trim-corruption bug was drift between that pipeline and what
maya actually painted."
.PP
The word
.I exact
is the whole point. The commit must drop
.I precisely
the rows that were rendered and scrolled off \(em not an estimate.
Estimating this count (from a reconstructed row height) was the source
of the classic "phone-over-SSH duplication ghost," which brings us to
the third interaction.
.PP
.B "3. The ledger and seal_measured."
The deepest fix in the tree moves the
.I measurement
of a frozen block's height out of the host and into maya. Previously
agentty reconstructed the width a block would wrap at \(em "terminal
columns minus the chrome paddings I believe wrap my fragment" \(em and any
mistake in that arithmetic (the famous
.CW "-2 vs -4"
padding-stack comment) made wrapped blocks under-measure on narrow
terminals, producing a duplicated ghost. The
.CW ScrollbackLedger::seal_measured
function in
.CW scrollback_ledger.cpp
eliminates the reconstruction entirely:
.PP
.QP
.I
"the paint pass records the ACTUAL width constraint compute() handed the
ledger's fragment (record_paint_width), and seal_measured lays the new
block out at exactly that width. There is nothing left for the host to
reconstruct; the chrome can change shape without any host code knowing."
.PP
This is the same philosophy as the Witness Chain, applied one level up:
do not let the host
.I estimate
a quantity that must be
.I exact .
Make the party that
.I knows
the true value (the renderer, which actually performed the layout)
record it, and have everyone else read it. The ledger's
.CW seal_measured
runs a real layout pass at the paint-recorded width, inside a scoped
render context pinned to that width, so the freeze frame lays out at the
true height and the seam is byte-stable \(em no gate recovery, no ghost.
.bp
.\"
.\" ==================================================================
.\" CHAPTER 14
.\" ==================================================================
.SH 1
Chapter 14 \(em Epilogue: why this shape, and what it buys
.HR
.LP
Step back from the mechanism and look at the shape of the whole design,
because the shape is the real lesson.
.PP
The problem was: a program must maintain a perfect model of an
external, un-readable, partly-immutable device, and any drift between
model and device is permanent and user-visible. The solution has four
strata, each a different kind of guarantee:
.IP "\fB1. A single linear model.\fP" 2m
.CW InlineFrameState
is move-only so there is exactly one shadow of the one wire. No parallel
models can disagree.
.IP "\fB2. An inductive maintenance invariant.\fP" 2m
The diff writes every emitted cell back into the shadow, so shadow
equals wire by induction over frames. The
.CW will_scroll_off
defence re-asserts the correct bytes on precisely the rows about to
become permanent \(em spending a few bytes to make the one irreversible
moment safe.
.IP "\fB3. A cheap continuous proof.\fP" 2m
The incremental XOR-fold hash lets the engine
.I verify
the model in
.CW O(1)
on the hot path, so verification is affordable enough to do every frame.
.IP "\fB4. Type-system enforcement.\fP" 2m
The Witness Chain \(em
.CW ShadowWitness ,
.CW ScrollbackProof ,
.CW FrameBytes
\(em turns "you must verify before you diff" and "state advance must be
atomic with byte delivery" from
.I discipline
into
.I "compile errors."
The corruption modes are not caught at runtime; they are
.I unrepresentable .
.PP
That progression \(em from a data-structure invariant, to a runtime
proof, to a type-level impossibility \(em is the template. The comments in
the source keep returning to the same confession:
.I "every historical corruption bug was a path that forgot the check."
The Witness Chain is the answer to that confession. You cannot forget a
check that the compiler will not let you skip.
.PP
And the recovery machine is the humility layer. Not every divergence can
be prevented \(em an external process can write to the tty, an SSH link
can drop bytes, a terminal can mangle a resize. So the engine has a
small, sharp set of recovery primitives, each matched to exactly one
class of divergence, with a documented trap for the wrong choice. The
severity ladder \(em bookkeeping commit, soft viewport repaint, hard wipe
\(em maps onto how much of the wire's state has become unknown. The
cheapest recovery that restores correctness is always the right one, and
the source spells out, primitive by primitive, when each applies.
.PP
What all this buys is a single, quiet property: you can stream a
thousand-row AI response into a shared terminal, scroll up through your
whole interleaved session, resize your window, switch conversations,
and never once see a duplicated composer, a drifting prompt, or a
half-frozen card. The frozen row is always correct, because the instant
before it froze, the engine had proven \(em not hoped \(em that it was.
.sp 2
.ce 100
.ft I
\(em fin \(em
.ft
.ce 0
.\"
.\" ==================================================================
.\" COLOPHON
.\" ==================================================================
.bp
.SH 1
Colophon
.LP
This book was written directly from the maya and agentty source trees.
Every quoted comment, function signature, field name, and escape
sequence appears in the code as shown; where prose in the repository's
own design docs disagreed with the code, the code was taken as
authoritative. The primary sources are:
.CS
maya/include/maya/render/serialize.hpp
maya/src/render/serialize.cpp
maya/src/render/frame_bytes.cpp
maya/src/render/scrollback_ledger.cpp
maya/include/maya/app/app.hpp
agentty/src/runtime/app/update/picker.cpp
agentty/src/runtime/app/update/frozen.cpp
agentty/src/runtime/app/update/modal.cpp
agentty/src/runtime/app/update/meta.cpp
.CE
.LP
Typeset with groff (\f[CR]ms\fP macros), rendered to PDF via
\f[CR]gropdf\fP.
