# Share logging and node-side share validation

These options are for solo miners who need to see what happens to each
share, and to confirm the local node would accept the constructed block
even when the share is below block difficulty.

## Node check order

`submitblock` runs `CheckBlockHeader` / `CheckProofOfWork` first. A share
that is valid work but below the block target is rejected as `high-hash`
and later checks (merkle, coinbase, witness, Blake2b headline) are skipped.

To validate *everything except proof-of-work*, use BIP22 GBT proposal mode:

```
getblocktemplate {"mode":"proposal","data":"<block hex>"}
```

That calls `TestBlockValidity(..., check_pow=false)`.

- `null` — structurally valid (would be a block if PoW met nBits)
- a string — reject reason (`bad-txnmrklroot`, `stale-prevblk`, …)
- HTTP/transport failure — node did not return JSON

## Config

```json
"logger": {
  "log_level_console": 2,
  "log_shares": true,
  "log_share_hashes": false,
  "debug_blake2b_pow": false
},
"mining": {
  "blake2b_force_version_high_bit": true,
  "dump_submitblock_path": "",
  "validate_shares_on_node": true,
  "share_node_check": "proposal",
  "share_node_check_every": 16,
  "share_node_check_missingzeros": -1
}
```

| key | default | meaning |
|---|---|---|
| `logger.log_shares` | false | INFO line per accepted/rejected share that passes the missingzeros gate + `missingzeros` (leading bits short of the block target; `0` is a block candidate) |
| `logger.log_share_hashes` | false | Append `hash=` to accepted share lines. Requires `logger.log_shares`. |
| `logger.log_share_hashes_missingzeros` | -1 | With `logger.log_share_hashes` on, only append the hash when `missingzeros <=` this. `-1` = every accepted share. Does nothing while `log_share_hashes` is off. |
| `logger.debug_blake2b_pow` | false | INFO dump of BLAKE2b H1 (119 bytes) and final LE hash |
| `mining.blake2b_force_version_high_bit` | true | OR `0x80000000` onto header/H1 version. Leave **true** unless `job` version already has the v2 bit *and* GBT did not strip it (the common GBT path clears `0x80000000` from `version` before serialization). |
| `mining.dump_submitblock_path` | `""` | If set, write each submitblock JSON to this file (detached thread) |
| `mining.validate_shares_on_node` | false | Ask the node about sampled accepted shares |
| `mining.share_node_check` | `"proposal"` | `"proposal"` skips PoW; `"submitblock"` checks PoW first |
| `mining.share_node_check_every` | 16 | Only 1 of every N accepted shares is *considered*. At most one node-check RPC is in flight at a time; others are skipped. Blocks are always submitted separately. |
| `mining.share_node_check_missingzeros` | -1 | `-1` = infinity (log every share; use `share_node_check_every`). `>= 0` overrides that sampler and the share log: only print SHARE / node-check lines when `missingzeros <=` this value. `2` ≈ 4 per block, `4` ≈ 16. |

Recommended solo-debug set:

```json
"logger": { "log_level_console": 2, "log_shares": true },
"mining": {
  "blake2b_force_version_high_bit": true,
  "dump_submitblock_path": "/tmp/datum_last_submitblock.json",
  "validate_shares_on_node": true,
  "share_node_check": "proposal",
  "share_node_check_missingzeros": 2
}
```

Then look for:

```
SHARE accepted user=... host=... reason=ok diff=... missingzeros=2
SHARE accepted user=... host=... reason=block diff=... missingzeros=0
SHARE <64 hex> mode=proposal d=... => null (structurally valid; PoW not required)
```

If proposal returns anything other than `null`, that string is the real
template/header/coinbase mismatch — not “the miner is too weak.”

## Share hashes

`missingzeros` is a power-of-two bucket, so it cannot say what difficulty a
share actually achieved. `logger.log_share_hashes` appends the share's own
hash to accepted share lines:

```
SHARE accepted user=... reason=ok diff=1024/1024/71371438 missingzeros=7 hash=<64 hex>
```

Same reversed big-endian order as the `BLOCK FOUND` line. Achieved difficulty
is `difficulty_1_target / hash`; the block target it was measured against is
`difficulty_1_target / blockdiff`, where `blockdiff` is the third component of
`diff=` already on the line.

The hash costs 70 characters, so `logger.log_share_hashes_missingzeros` can
limit it to shares at or below a given tier. Nothing is lost by doing so — the
tiers are monotonic, so a deeper share can never have achieved more difficulty
than a shallower one. It defaults to `-1` (no limit) because the fraction of
lines affected is `2^N / (blockdiff / vardiff)`, which varies far too much
between gateways for any one value to be a sensible default.
