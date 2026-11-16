# Fuzzing harness

One target, on the engine's only order-entry boundary:

| Target | Source | Exercises |
|---|---|---|
| `fuzz_gateway_engine` | `fuzz_gateway_engine.cpp` | A client's byte stream through `TcpGateway`'s framer and `decodeFrame` into `MatchingEngine::submitOrder` (synchronous engine, symbols 0 and 1), then `OrderBook::validateIntegrity` on every book, before and after an expiry sweep. Engine asserts armed, UBSan halting. CI runs it for 10 minutes (`gateway-engine-fuzz`). |

It is compiled together with the engine sources (not linked against
`OrderMatcher`) so libFuzzer's coverage and the sanitizers reach the engine.
Its seeds are binary frames, generated rather than committed:

```bash
python3 fuzz/gen_gateway_seeds.py /tmp/gw-corpus
./fuzz/run_libfuzzer.sh gateway 60 /tmp/gw-corpus
./build-fuzz/fuzz/fuzz_gateway_engine_replay /tmp/gw-corpus   # no libFuzzer needed
```

On macOS with Homebrew LLVM, run it with
`ASAN_OPTIONS=detect_container_overflow=0`: Darwin's libFuzzer runtime shares
the program's `std::vector` code, and the mix of annotated and unannotated
instantiations trips ASan inside libFuzzer's own corpus bookkeeping on the
first input. Linux's runtime carries a private libc++, so CI runs with full
ASan.

## Two run modes

### Replay (always available)

A standalone driver that runs the harness against a corpus of files. Built
with ASan + UBSan; no libFuzzer required. Works on Apple Clang, system
GCC, anything reasonable.

```bash
cmake -S . -B build-fuzz -DBUILD_FUZZERS=ON
cmake --build build-fuzz -j --target fuzz_gateway_engine_replay
./build-fuzz/fuzz/fuzz_gateway_engine_replay /tmp/gw-corpus
```

### Coverage-guided libFuzzer (requires Homebrew LLVM on macOS)

Apple Clang doesn't ship `-fsanitize=fuzzer`. Install Homebrew LLVM:

```bash
brew install llvm
```

Then run the target via the wrapper script — it auto-detects a
libFuzzer-capable clang, configures a dedicated `build-libfuzzer/`
tree on first run, generates seeds if no corpus is given, and saves any
crashes under `fuzz/crashes/gateway/`:

```bash
./fuzz/run_libfuzzer.sh             # 30s default run
./fuzz/run_libfuzzer.sh gateway 0   # run until interrupted
```

## Reproducing a crash

```bash
# When libFuzzer reports a crash it writes the input to fuzz/crashes/gateway/
./build-fuzz/fuzz/fuzz_gateway_engine_replay fuzz/crashes/gateway/crash-abc123
```

The replay binary uses ASan + UBSan, so the crash output identifies the
exact undefined behavior or memory error.
