# Interaction and MCP performance

Measured on the Fedora 44 development host (16 cores), Chromium 156.0.8078.79. These are latency/throughput measurements, not the isolated-footprint RAM results in docs/BENCHMARKS.md.

## CLI actions and pipelines

Integrated main verification, commit 0766338:

```sh
SLIM_INTERACT_N=100 SLIM_BENCH_REPS=3 bench/interaction.sh
```

Each workload uses 100 operations and three repetitions. The benchmark checks every response, exact click deltas and final input values. The baseline is built from revision 62b1866 in a separate temporary directory; browsers and fixture servers are isolated.

### Common click/fill/eval workload

| transport | median ms/op | operations/s | correctness |
| --- | ---: | ---: | --- |
| baseline, separate CLI processes | 5.638 | 177.4 | exact |
| baseline batch | 5.634 | 177.5 | exact |
| current, separate CLI processes | 5.639 | 177.3 | exact |
| current batch | 5.638 | 177.4 | exact |
| `act` | **0.664** | **1505.0** | exact |
| persistent `pipe` | **0.708** | **1412.3** | exact |

`act` reduces median latency by **88.2%** versus current batch; `pipe` reduces it by **87.4%**.

### Mixed snapshot/find/click/fill/eval workload

| transport | median ms/op | operations/s | correctness |
| --- | ---: | ---: | --- |
| separate CLI processes | 3.308 | 302.3 | exact |
| batch | 3.303 | 302.8 | exact |
| `act` | **0.862** | **1160.3** | exact |
| persistent `pipe` | **0.933** | **1071.3** | exact |

The improvement comes from connection/attach reuse and pipelined action pairs; actionability and final page-state checks remain enabled.

## MCP

Measured by the MCP track on the same host, using 100 deterministic calls (90 eval, 5 text, 5 click). The implementation is included unchanged in main.

| transport | processes / CDP connections | total for 100 calls | overall calls/s | median call |
| --- | --- | ---: | ---: | ---: |
| MCP | 1 / 1 | **85.0 ms** | **1177.0** | 0.249 ms |
| separate CLI | 100 / 100 | 163.6 ms | 611.3 | 0.994 ms |
| one CLI batch | 1 / 1 | **82.4 ms** | **1213.8** | 0.109 ms |

MCP avoids process setup and reconnects while preserving the session's current tab. Batch remains slightly faster for a precomputed command list; MCP provides the interactive JSON-RPC tool interface. See docs/MCP.md for the harness and additional protocol measurements.

## Binary and RSS

The integrated binary with profiles, interaction, media, protocol and MCP is 241,312 bytes stripped. Ordinary commands remain around 2 MB RSS; full-schema protocol output and memory-tree accounting can require more. See the individual module docs for their detailed measurements.
