# Repository Guidelines

## Project Structure & Module Organization

This repository contains a single C implementation for the OS lab shell.

- `sh.c`: shell source code, including prompt/output helpers, tokenization, parsing, and execution logic.
- `Makefile`: build, clean, submission, and OJ helper targets. The header notes that it should not be modified.
- `conf/lab.mk`: lab metadata such as `LAB` and `LAB_NUM`; do not modify for normal work.
- `conf/info.mk`: local SID, token, and server URL configuration. Treat this as private machine-specific configuration.
- `README.md`: lab setup and submission instructions.

There is no separate test directory or asset tree in the current layout.

## Build, Test, and Development Commands

- `make sh`: compile `sh.c` into the `./sh` executable with `gcc`.
- `./sh`: run the shell locally after building.
- `make clean`: remove build outputs such as `sh` and `*.o`.
- `make server-state`: check whether the configured OJ server is reachable.
- `make submit`: upload `sh.c` to the OJ. Requires valid `SID`, `TOKEN`, and `URL` in `conf/info.mk`.
- `make score`: retrieve the latest OJ score for this lab.
- `make report`: upload a report named `$(SID).pdf`.

For quick checks, build with `make sh` and exercise cases manually, for example `echo hi`, pipelines, redirection, and invalid input.

## Coding Style & Naming Conventions

Use C with 4-space indentation. Keep helper functions small and named in lower snake case, matching `free_tokens`, `init_command`, and `print_invalid_syntax`. Constants should use upper snake case, as in `MAX_ARGS` and `MAX_INPUT`.

Preserve the provided printing functions and their exact output strings; automated judging may depend on them. Avoid broad refactors of `Makefile` or `conf/lab.mk`.

## Testing Guidelines

No formal test framework is included. Validate changes by compiling cleanly with `make sh`, running `./sh`, and checking expected behavior interactively. Include edge cases for empty input, malformed pipes or redirects, background marker placement, long argument lists, and command-not-found handling.

When possible, compare behavior against the lab specification and confirm final results with `make submit` followed by `make score`.

## Commit & Pull Request Guidelines

Recent commits use short imperative or descriptive messages, for example `initialize repo` and `arguments parser done`. Keep commits focused on one change.

For pull requests, include a brief summary, verification commands, and any known limitations or unimplemented lab requirements. Mention `conf/info.mk` only if necessary, and do not expose private tokens.

## Answering Guildlines

Unless the user explicitly says “help me finish it” or “tell me the answer,” you should not complete the programming task for the user or directly state the design methodology. Instead, you should guide the user step by step, leading them to think in the right direction—for example, by reflecting on the shortcomings of current approaches and how to improve them, or by pointing out clever design tips. 