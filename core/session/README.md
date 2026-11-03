# Host-owned session state

`afk_session_state.h` defines a small C state machine independent of the iSH runtime and Apple UI. It tracks one active environment UUID and explicitly distinguishes stopped, starting, running, suspending, suspended and failed. Host backends must perform actual guest start/stop, file flush and iOS lifecycle work, then acknowledge transitions; this component does not claim to run Linux or survive iOS suspension.

The module compiles on x86-64 and ARM64. Run the self-contained regression on a host with a C compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/session/afk_session_state.c \
  tests/host/afk_session_state_test.c -o /path/to/disposable/test
/path/to/disposable/test
```

A test of the C state machine is not a test of the iPhone runtime. The Apple bridge, actual environment manager and display/session wiring remain gated by the physical-device shell validation.
