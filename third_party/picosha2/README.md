# PicoSHA2

Unmodified `picosha2.h` and `LICENSE` from
[okdshin/PicoSHA2](https://github.com/okdshin/PicoSHA2/tree/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29),
commit `161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29`, retrieved September 14, 2026.
License: MIT; see [LICENSE](LICENSE).

| File | SHA-256 |
|---|---|
| `picosha2.h` | `b13c180161ffac8d0adc81e033e493c409457c4d1258ab9781ac80579ba3bdd8` |
| `LICENSE` | `6c30eb1f37554ec4199cb82a8c86d9e7a852da78a757832bebb03ac9ddc44ff1` |

Used privately by `eme_session` for file content fingerprints. It is not linked
into `eme_core`, the existing decoder or market-update benchmark. Session tests
include known SHA-256 vectors and an independent CMake file-hash comparison.
Keep updates pinned, preserve the upstream license, and rerun those checks.
