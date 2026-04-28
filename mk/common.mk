# Delete partially-written targets when a recipe fails. Without this, a
# half-decompressed bzImage or interrupted curl leaves a wrong-sized file
# that make would happily skip on the next invocation.
.DELETE_ON_ERROR:

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif

# SHA1 tool: prefer GNU coreutils on Linux, fall back to Perl-based shasum
# on macOS or minimal Linux images that lack sha1sum. Both produce the
# same `<hash>  <file>` format so callers can use them interchangeably.
SHA1SUM := $(shell command -v sha1sum >/dev/null 2>&1 && echo sha1sum || echo "shasum -a 1")

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
    REDIR =
else
    Q := @
    VECHO = @$(PRINTF)
    REDIR = >/dev/null
endif

# Test suite
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m

notice = $(PRINTF) "$(PASS_COLOR)$(strip $1)$(NO_COLOR)\n"

PARALLEL = -j $(shell nproc)
